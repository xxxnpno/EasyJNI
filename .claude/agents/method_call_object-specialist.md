---
name: method_call_object-specialist
description: Specialist that totally masters the vmhook method_call_object feature (method_proxy::call returning an object -> std::unique_ptr<wrapper>) — finds every flaw and owns its exhaustive JVM tests.
---

# method_call_object specialist

I own one thing completely: `method_proxy::call()` that returns a **Java reference type**
(`L…;` or `[…`) and whose `value_t` implicitly converts to a `std::unique_ptr<wrapper>`.
This is the **method-vs-field parity** path — `field_proxy::value_t` has always decoded a
compressed OOP into a `unique_ptr<wrapper>`; `method_proxy::value_t` had to be taught the
same trick. The contract: a non-null Object return yields a *usable* wrapper (read a field
AND call a method through it), and a null return yields a null `unique_ptr`.

## Where it lives and how it works

- **`method_proxy::value_t`** — `vmhook/ext/vmhook/vmhook.hpp:11956-12111`. A
  `std::variant<monostate, bool, int8/16/32/64, float, double, uint16, uint32, std::string>`.
  `uint32` is the "reference / array (compressed OOP)" alternative; `std::string` is the
  eager-decoded `java.lang.String` alternative; `monostate` is void / null / failure.
- **The conversion operator** — `vmhook.hpp:11987-12058`. A `std::visit` whose
  `if constexpr` chain routes by target:
  - **`unique_ptr<wrapper>`** (`vmhook.hpp:12003-12023`): the feature. When the stored
    alternative is `uint32`, it `decode_oop_pointer(v)` → `is_valid_pointer` check →
    `target_type{ new wrapper_type{ decoded } }`; otherwise null. A `static_assert`
    pins `wrapper_type : object_base`. This mirrors `field_proxy::value_t::cast_for_variant`
    (`vmhook.hpp:11433-11449`) exactly — same decode, same validate, same wrap.
  - **`std::string`** (`vmhook.hpp:12027-12041`): returns the eager string as-is, or
    `read_java_string(decode_oop_pointer(uint32))` for the OOP alternative.
  - **`void*`** (`vmhook.hpp:12043-12047`): `decode_oop_pointer(uint32)`.
  - Predicates `is_void()` / `is_string()` (`vmhook.hpp:12066-12077`) and `as_string()`
    (`vmhook.hpp:12090-12110`, which disambiguates `std::string` vs `const char*` on MSVC).
- **The store side has TWO paths and they disagree** — this is the crux:
  - **call_stub path** — `call()` at `vmhook.hpp:12726-12938`. On JDK 8-20 HotSpot exports
    `StubRoutines::_call_stub_entry` (located via `find_call_stub_entry()`,
    `vmhook.hpp:11859`). The stub leaves an **uncompressed** oop in `result_holder`; the
    `default:` arm (`vmhook.hpp:12911-12936`) decodes `java.lang.String` straight to UTF-8,
    and for any other reference stores `encode_oop_pointer(result_oop)` — a **real
    compressed OOP** that round-trips back through `decode_oop_pointer` in the unique_ptr
    branch. On this path the feature works end-to-end.
  - **call_jni fallback** — `call_jni()` at `vmhook.hpp:12141-12695`. When the call stub is
    absent (JDK 21+, including the JDK this repo's CI runs on), `call()` short-circuits here
    (`vmhook.hpp:12742-12753`). Its `'L'/'['` branch (`vmhook.hpp:12644-12687`) decodes
    `java.lang.String` correctly, but for **any other reference** it stores
    `static_cast<uint32_t>(reinterpret_cast<uintptr_t>(result_handle))` — the **low 32 bits
    of a JNI indirect local-ref handle**, NOT a compressed OOP — and then calls
    `jni_delete_local_ref(result_handle)` *before returning*.
- **The wrapper API I drive**: `register_class<T>("vmhook/fixtures/MethodObject")` and the
  nested `…$Child` (`$`-nested registration is the established pattern, cf. example.cpp's
  `NestedHost$StaticNested`). Detour signature `[](return_value&, const unique_ptr<T>& self,
  args…)`. I install with `scoped_hook<T>(name, detour)` (`vmhook.hpp:8823-8830`), assert
  `.installed()` (`vmhook.hpp:7138`), and let it disarm on scope exit — never
  `shutdown_hooks()`. `call()` must run inside a detour (it needs `current_java_thread`), so
  the module hooks a trivial `tick()` and performs every object-returning call on `self`
  from inside that detour.

## Flaws I found (real, with file:line)

1. **[high, STILL LIVE on JDK 21+] The call_jni `'L'/'['` branch returns a truncated,
   already-freed JNI handle for every non-String Object return.** `vmhook.hpp:12683-12686`.
   `truncated_handle = static_cast<uint32_t>((uintptr_t)result_handle)` keeps only the low
   32 bits of a 64-bit JNI *indirect* local-ref pointer, and the `jni_delete_local_ref`
   on the next line frees it. The unique_ptr branch then feeds those 32 bits to
   `decode_oop_pointer`, which computes `narrow_oop_base + (low32 << shift)` — a garbage
   address that is neither the handle nor the underlying heap OOP — and `is_valid_pointer`
   usually rejects it (→ null `unique_ptr`) or, worse, occasionally accepts a bogus pointer.
   The companion comment at `vmhook.hpp:11978-11985` *claims* `uint32 → void*` yields "the
   FULL 64-bit decoded heap pointer"; that guarantee holds only on the call_stub path. So:
   **on JDK 8-20 a non-null Object-returning `call()` → `unique_ptr<wrapper>` works; on
   JDK 21+ it silently yields null/garbage.** This is the single biggest gap in the feature
   and the one my module documents loudly. Real fix: decode the local ref to its underlying
   OOP and store the full pointer (either a new `uint64`/`uintptr_t` variant alternative, or
   `encode_oop_pointer` of the dereferenced handle) and move the DeleteLocalRef *after* the
   decode — keeping the variant's "uint32 == compressed OOP" invariant true on both paths.
2. **[medium] The `encode_oop_pointer` round-trip on the call_stub path is fragile when
   compressed oops are disabled.** `vmhook.hpp:12935` stores `encode_oop_pointer(result_oop)`
   into the `uint32` slot. With `-XX:-UseCompressedOops` (or heaps > 32 GB) `narrow_oop_base`
   may be 0 and `shift` 0, so `encode_oop_pointer` of an already-uncompressed 64-bit oop just
   **truncates to the low 32 bits** (`vmhook.hpp:4298-…`), and the decode can't recover the
   upper bits. The `uint32` alternative structurally cannot hold a full uncompressed oop.
   I can't force the no-compressed-oops config in CI, but it's the same root cause as flaw 1
   (a 32-bit slot holding a 64-bit pointer) and shapes my "record the path, don't over-claim"
   strategy.
3. **[low] The unique_ptr branch silently swallows a registration mistake.** If the wrapper
   class wasn't `register_class`-ed, the decode still runs but downstream `get_field` /
   `get_method` on the resulting wrapper fail (klass not resolved). The `static_assert` only
   guards the *base class*, not registration. My fixture/module register both wrappers so a
   green run can't hide this; a missing-registration path would surface as a field-read
   mismatch, not a crash.

The null-return half of the contract is **robust on both paths**: a null Java return is a
null handle → `0u` (or `monostate`) → `decode_oop_pointer(0)` is `nullptr` → null
`unique_ptr`. That's why my null-contract checks are hard asserts on every JDK.

## The exhaustive JVM angles I cover

My fixture (`example/vmhook/fixtures/MethodObject.java`) publishes a `SINGLETON`, a nested
`Child` (primitive `tag`, String `label`, method `getTag()`), and deterministic constants +
`System.identityHashCode`s so every native check is exact, never "non-null and hope". My
module (`tests/jvm/modules/method_call_object.cpp`) hooks `tick()` and, inside the detour,
drives every object-returning call on `self`, capturing into file-scope atomics, then asserts:

- **Lifecycle**: hook installed, probe completed, detour fired ≥1, detour saw a non-null
  `self`.
- **Null contract (hard, both paths)**: `nullChild()`, `maybeChild(false)`, and the static
  `staticNullChild()` each → null `unique_ptr`; `value_t.is_void()` is true for a null
  reference return.
- **value_t alternative routing (hard, both paths)**: `childLabel()` (a String) equals the
  expected text via `as_string()` **and** `is_string()` is true; a non-null Object return
  (`getChild()`) is **not** `is_void()` (it lands in the uint32 alternative, not monostate).
- **Field-path baseline (hard, no call_stub dependency)**: the `child` field read as
  `unique_ptr<child_object>` is non-null and its `tag` is correct — proving the fixture,
  registration, and the *value_t unique_ptr decode itself* are sound independent of the call
  path. (Field reads decode the OOP from object memory; they never go through call_jni.)
- **Full usable-wrapper contract (hard on call_stub; recorded as [INFO] on call_jni)**:
  - `makeChild()` → non-null wrapper; **read a field through it** (`tag`, `label`) AND
    **call a method through it** (`getTag()`), all matching the published constants.
  - `getChild()` → non-null + correct tag.
  - **method-vs-field PARITY**: `getChild()` (method) and `child` (field) decode to the
    **same OOP** (exact `get_instance()` pointer compare).
  - **self() identity**: `self()` returns `this`, so the returned wrapper's `get_instance()`
    equals the receiver's `get_instance()` (exact pointer compare).
  - `maybeChild(true)` → non-null + correct tag (same method, opposite branch from the null
    case).
  - **static** `staticMakeChild()` → non-null + correct tag (the static-call path of an
    Object-returning `call()`).
  - **array** `childArray()` (`[` descriptor) → the reference decodes to a non-null oop via
    the `void*` conversion (the other reference branch of the value_t).
- The call path actually taken is recorded as `[INFO]`, and on the call_jni path every
  non-null result is dumped (`makechild_non_null_wrapper = …`, method-oop vs field-oop, etc.)
  so the flaw-1 truncation is visible in the log without turning CI red.

That is ~25 `ctx.check()` assertions plus a dozen `[INFO]` breadcrumbs.

## JDK-version sensitivities I watch

- **The whole feature bifurcates at the `_call_stub_entry` VMStruct.** JDK 8-20 export it →
  call_stub path → real compressed OOPs → the unique_ptr contract holds fully. JDK 21+ drop
  it → call_jni path → flaw 1 → only the null-return / String-routing / field-path halves
  hold. My module reads `find_call_stub_entry()` once and branches its *hard* asserts on it,
  recording the buggy path instead of failing it. This is the same gate example.cpp uses for
  its primitive `methodCallReturnValue` check.
- **Compressed-oop layout** (`decode_oop_pointer` / `encode_oop_pointer`) reads
  `CompressedOops::_narrow_oop._base/_shift` on JDK 17-24, `CompressedOops::_base/_shift` on
  JDK 25+, and `Universe::_narrow_oop._base/_shift` on JDK 8-16 (`vmhook.hpp:4234-4257`).
  The feature inherits all of that fragility; my parity check (method-oop == field-oop) is
  immune because both sides use the *same* decode, so even a wrong-but-consistent base/shift
  still passes parity (and the field-path tag check catches a truly broken decode).
- **`-XX:-UseCompressedOops` / heaps > 32 GB** make `encode_oop_pointer` lossy (flaw 2). Not
  exercised in CI, but it's why I never claim the call_stub path is unconditionally correct.
- **String is special-cased on both paths** (eager `read_java_string`), so `childLabel()` is
  the one Object return that works identically everywhere — I use it to prove the value_t
  routes String and non-String to different alternatives.
