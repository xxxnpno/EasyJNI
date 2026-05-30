---
name: field_primitives_get-specialist
description: Specialist that totally masters the vmhook field_primitives_get feature — finds every flaw and owns its exhaustive JVM tests.
---

# field_primitives_get specialist

I own `field_proxy::get()` for every JVM primitive descriptor — the read side of
direct field access in vmhook. I know this code at the byte level and own its
exhaustive, JVM-only test module.

## Where the feature lives

- **`field_proxy::get()`** — `vmhook/ext/vmhook/vmhook.hpp:11548-11609`. A linear
  `if (signature_text == "X")` chain that, for each primitive descriptor, declares
  a fixed-width C++ local, `std::memcpy`s `sizeof(local)` bytes from
  `field_pointer`, and wraps it in a `value_t`. Descriptor → variant alternative:
  `"Z"`→`bool`, `"B"`→`int8_t`, `"S"`→`int16_t`, `"I"`→`int32_t`, `"J"`→`int64_t`,
  `"F"`→`float`, `"D"`→`double`, `"C"`→`uint16_t`, and the fall-through
  `"L…"`/`"[…"`→`uint32_t` (the raw compressed OOP).
- **Null-pointer fallback** — `vmhook.hpp:11551-11554`: when `field_pointer` is
  null it returns `value_t{ std::int32_t{}, signature_text }` for **every**
  signature.
- **`value_t` variant + conversion** — `vmhook.hpp:11270-11522`. The variant
  alternatives are ordered exactly bool(0), int8(1), int16(2), int32(3), int64(4),
  float(5), double(6), uint16(7), uint32(8). `cast_for_variant<T>`
  (`11404-11470`) is the implicit-conversion engine: string/vector/unique_ptr/void*
  only convert from the `uint32_t` (OOP) alternative; everything else falls into
  the generic `static_cast<T>(value)` branch (`11462-11465`).
- **Pointer resolution** — `object::get_field()` (`13517-13557`) computes
  `field_pointer = decoded_object + offset` (instance) or `mirror + offset`
  (static, read from `_fields` JVM_ACC_STATIC), then constructs the proxy. By the
  time `get()` runs the static/instance distinction is already baked into the
  pointer; `get()` itself never consults `is_static()`.

## Flaws I found (real bugs, with file:line)

1. **`"Z"` raw-memcpy into `bool` can synthesize a non-canonical bool**
   (`vmhook.hpp:11558-11560`). `bool value{}; memcpy(&value, ptr, sizeof(value));`
   copies the raw byte straight into a `bool` whose object representation only
   permits 0/1. Any other byte (mid-write race, `Unsafe.putByte`, HotSpot
   debug-fill 0xAA) yields an indeterminate bool; the subsequent widen/visit is
   UB. **Every sibling site normalizes** — `method_proxy::call` does `(raw & 1)`,
   `call_jni` does `r != 0`, the `[Z` array reader (`11294`) does `!= 0`. Only
   `get()` reads raw. Fix: read through `uint8_t` then `raw != 0`.

2. **`"Z"` reads `sizeof(bool)`, not 1 byte** (`vmhook.hpp:11558-11559`).
   Implementation-defined width; on any ABI where `sizeof(bool) > 1` this over-reads
   past the 1-byte JVM "Z" slot into the next field. Unobservable on x64
   MSVC/GCC/Clang but real per the standard, and unlike `set()` (which is gated by
   `jvm_primitive_byte_width`) `get()` has no width guard.

3. **Null-pointer fallback returns the wrong variant alternative for every
   signature** (`vmhook.hpp:11551-11554`). A null-pointer `"J"`/`"D"`/`"F"`/`"C"`
   proxy returns `int32_t{0}`, not the descriptor's alternative, so
   `std::holds_alternative<int64_t>` is false for a long, and `std::get<bool>`
   on a `"Z"` proxy throws `bad_variant_access`. Numeric/bool casts collapse to
   zero only by luck; introspecting callers see a broken invariant.

4. **`static_cast<int>(NaN/Inf)` is UB and `static_cast<bool>(NaN)` is `true`**
   (`cast_for_variant` generic branch, `vmhook.hpp:11462-11465`). Reading an
   `"F"`/`"D"` field that holds `NaN`/`±Inf` into an integer target executes
   `static_cast<int>(non-finite)` — UB per [conv.fpint]. And a NaN float read into
   a `bool` is truthy, silently reporting "no usable value" as `true`.

5. **Java `char` > 0x7F silently narrows when the target is C++ `char`**
   (`cast_for_variant` generic branch). `get()` loads the full `uint16_t`
   correctly, but `char c = proxy->get()` truncates: `'中'` (0x4E2D) becomes `0x2D`
   (`'-'`), `'é'` (0x00E9) becomes a negative char. No log, no guard. The lossless
   path (`char16_t`/`uint16_t`) is correct; only the 1-byte target loses data.

6. **No diagnostic on unknown/malformed signature** (`vmhook.hpp:11605-11608`). A
   typo'd descriptor (`"Q"`, `""`) silently falls through to a 4-byte compressed-OOP
   read and a `uint32_t` alternative, unlike `set()` which logs and guards.

7. **Doc drift**: the doc-comments at `vmhook.hpp:1447` and `6821` reference
   `field_proxy::get_as<T>()`, which does not exist (the class exposes only
   `get()/set()/signature()/raw_address()/is_static()/get_compressed_oop()`).

## Exhaustive JVM test angles I cover

My module is `tests/jvm/modules/field_primitives_get.cpp` against fixture
`example/vmhook/fixtures/FieldPrimitivesGet.java`. Everything runs on a real
HotSpot JVM via the modular harness; there are no standalone/no-JVM tests.

- **Every primitive at every boundary**, read with `static_field("name")->get()`:
  Z {true,false}; B {0,1,-1,MIN,MAX,0x7F,0x80,0xFF,0xAB}; S {0,1,-1,MIN,MAX,
  0x8000,0x7FFF,0xBEEF}; I {0,1,-1,MIN,MAX,0xDEADBEEF,0x7FFFFFFF,0x80000000};
  J {0,1,-1,MIN,MAX,0xDEADBEEFCAFEBABE,0x7FFF…,0x8000…,high-32-bits}; C {space,'A',
  MAX 0xFFFF,'é' 0xE9,'中' 0x4E2D, all four surrogate-range units}.
- **For every read I assert BOTH** the converted C++ value **and** the
  `value_t::data.index()` (proves the right signature branch and variant
  alternative were selected) **and** the round-tripped `signature()`.
- **Sign-extension semantics**: B 0xFF widens to int −1 and to `uint32_t`
  0xFFFFFFFF; S/I sign-extend into wider integers; char widens unsigned.
- **Float/double bit-exact**: every F/D value is reconstructed in Java from raw
  `intBitsToFloat`/`longBitsToDouble`, and I `memcpy`-pun the converted
  `float`/`double` back to integer bits and assert equality — covering +0.0/−0.0
  sign bit, ±Inf, canonical qNaN 0x7FC00000 / 0x7FF8…, **signaling NaN**,
  **NaN payload**, **MIN_VALUE denormal**, MIN_NORMAL, MAX_VALUE. Plus semantic
  predicates `isnan/isinf/signbit` round-tripped through `get()`.
- **char narrowing witnesses**: the same `"C"` field read as `uint16_t`/`char16_t`
  is lossless while `char` truncates to the low byte — pinning bug #5 on the JVM.
- **Instance dispatch**: a held `static instance` lets me build an instance
  wrapper and read instance fields (`iBool…iDouble`) through
  `get_field("name")->get()`, asserting value + variant index for each, and I
  cross-check that reading a `JVM_ACC_STATIC` field **through the instance
  accessor** equals the static-accessor result (proves `get()` ignores the flag).
- **Null-pointer contract**: I construct `field_proxy{nullptr, sig, false}`
  directly for Z/B/S/I/J/F/D/C/`Ljava/lang/String;`/`[I` and assert the
  (buggy-by-design) invariant — variant alternative is **always** int32, signature
  round-trips, and every numeric/bool/double conversion is zero/false without a
  crash; the String-typed null proxy decodes to empty (no garbage-OOP chase).
- **Live post-dispatch state**: the probe's `run()` performs genuine
  `putstatic`/`putfield` on `r*`/`rI*` fields via an `invokevirtual writeRuntime`,
  then after `run_probe` I read those fields back through `get()` — proving `get()`
  reflects live JVM state, not just class-initializer constants.
- **Accessor parity / purity**: `is_static()` true vs false on static/instance
  proxies; `signature()` value; reading the same field twice yields identical
  value + variant index (no side effects); `raw_address()` is non-null and
  width-aligned (4 for int, 8 for double) — the exact pointer `get()` reads from.

## JDK-version sensitivities I track

- **NaN canonicalisation**: HotSpot stores `Float.NaN` as the canonical qNaN
  0x7FC00000 (double 0x7FF8…). A signaling-NaN or payload NaN written via
  `intBitsToFloat` must survive `get()`'s `memcpy` untouched on every JDK 8..25 —
  any float→double→float promotion in a conversion path would canonicalise the
  payload, so I read F/D into the **matching** type only and bit-check.
- **char encoding**: I write every `char` constant as a numeric/`\uXXXX` literal so
  the fixture is pure ASCII and compiles identically under `javac` on Windows
  (Cp1252) and Linux/macOS (UTF-8) — CI invokes `javac` with **no `-encoding`
  flag**, so raw UTF-8 char literals would mis-decode on the Windows runner.
- **GCC portability**: I use `static_field("name")` (always available) for all
  static-context reads and reserve `get_field("name")` for true instance context,
  because the deducing-this `get_field` static overloads don't exist on GCC.
- **Compressed OOPs**: the reference/array fall-through reads a 4-byte narrow OOP;
  this only holds with compressed class pointers (the default) — out of scope for
  primitives but I keep the `instance` field a real reference so the instance
  wrapper resolves on every supported JDK.
