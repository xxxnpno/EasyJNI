---
name: method_explicit_signature-specialist
description: Specialist that totally masters the vmhook method_explicit_signature feature (get_method(name, signature) selecting an overload by exact JVM descriptor).
---

# method_explicit_signature specialist

I own ONE feature end-to-end: **`object::get_method(name, signature)` selecting a
single overload by EXACT JVM descriptor**, and the guarantee that a wrong / absent
/ empty signature yields **no method** (so the resulting call is a safe no-op).

## Where it lives (vmhook/ext/vmhook/vmhook.hpp)

There are FOUR signature-aware entry points plus their portable aliases:

- **Instance overload** — `object_base::get_method(name, sig)`
  - `vmhook.hpp:13678-13720`. Walks the klass superclass chain; for each method
    computes `current_signature = current_method->get_signature()` and matches with
    the **exact** compare `current_method->get_name() == method_name &&
    current_signature == method_signature` at **`vmhook.hpp:13709`**. On match returns
    `method_proxy{ this->instance, current_method, current_signature }`
    (**`vmhook.hpp:13711`**). On miss logs and returns `std::nullopt`
    (`vmhook.hpp:13716`).
- **Static overload** — `object_base::get_method(type_index, name, sig)`
  - `vmhook.hpp:13788-13830`. Same hierarchy walk; exact compare at
    **`vmhook.hpp:13819`**; returns `method_proxy{ nullptr, current_method,
    current_signature }` (**`vmhook.hpp:13821`** — note the **null** owning object,
    the correct choice for a static accessor).
- **Deducing-this forwarder** — `get_method(this object_base const& self, name, sig)`
  at `vmhook.hpp:13968-13972` (only under `VMHOOK_HAS_DEDUCING_THIS`).
- **Static-context fallback** — `derived::get_method(string_view, string_view)`
  at `vmhook.hpp:14007-14011` (only under deducing-this).
- **Portable alias** — `derived::static_method(name, sig)` at
  **`vmhook.hpp:14035-14039`**, always available (the name GCC users MUST use from a
  static wrapper method, since the deducing-this static fallback is not emitted there).

The exact-match comparison has **zero looseness**: no empty-string short-circuit, no
prefix match, no case folding. `defineClass(String,[B,I,I,ProtectionDomain)` cannot be
confused with any sibling `defineClass` overload. The match logic itself is solid.

### Interaction with call-time overload re-resolution (subtle, important)

`method_proxy::call(args...)` does NOT blindly dispatch the Method* that
`get_method(name,sig)` latched. On the **call_stub** path it runs
`resolve_compatible_method<args_t...>()` (`vmhook.hpp:13303-13345`) and on the
**call_jni** path the same (`vmhook.hpp:12167`). That helper FIRST checks
`signature_matches_arguments<args_t...>(this->signature_text)` (`vmhook.hpp:13307`)
and, if the proxy's OWN signature already matches the C++ arg types, returns
`this->method` unchanged — so the explicit selection survives. It only re-resolves to
a sibling when the proxy's own descriptor does NOT match the supplied C++ args. Net
effect for this feature: **no-arg and uniquely-typed-arg explicit selections are
honored end-to-end**, and even the `combo(CharSequence)` vs `combo(String)` case is
honored because a single `L...;` parameter matches `std::string` (so the proxy keeps
its own Method*). The only way to "lose" an explicit selection is to call with C++
args that don't match the descriptor you asked for — which is user error, not a flaw
in the lookup.

## Flaws I found

1. **[low] Instance overload sets a phantom `this` for STATIC methods.**
   `vmhook.hpp:13711` always passes `this->instance` as the owning object, even when
   the matched Method* is `JVM_ACC_STATIC`. The static overload at `vmhook.hpp:13821`
   correctly passes `nullptr`. `field_proxy` carries an `is_static` flag and gets this
   right; `method_proxy` from the instance overload does not. Calling
   `obj.get_method("smap", "(I)I")` (an instance handle to a static method) yields a
   proxy whose `object != nullptr`, so `call()` pushes a bogus receiver into slot 0 and
   passes the real arg one slot too high. **My module sidesteps this by using
   `static_method(name,sig)` for the static `smap` family** (the documented correct
   path), and documents the instance-static hazard. Fix: peek `get_access_flags()` for
   `JVM_ACC_STATIC` and pass `nullptr` when static.

2. **[low/certain] Empty signature is a silent strict-miss with a misleading log.**
   `get_method(name, "")` can never match (every real descriptor starts with `(`), so
   it returns `nullopt` and logs the generic "no method with this exact name+signature
   found". This diverges, silently, from `vmhook::hook<T>(name, signature)`
   (`vmhook.hpp:~7745`) which treats an empty signature as a **wildcard**. Two siblings,
   opposite empty-signature semantics, no cross-reference. **My module pins the strict
   behaviour** (`miss_proc_empty_sig`, `miss_smap_empty`) so any future "helpful"
   loosening that makes `""` a wildcard breaks a test. Suggested fix: early
   `if (method_signature.empty()) { log hint; return nullopt; }`.

3. **[low] Doc says "Exception safety: does not throw" but the functions are not
   `noexcept` and heap-allocate per candidate.** `vmhook.hpp:13672` / `13781` claim no
   throw, yet each loop does `const std::string current_signature{ ... }`
   (`vmhook.hpp:13708` / `13818`) — one allocation per method — and is not `noexcept`.
   Under memory pressure `std::bad_alloc` escapes, contradicting the contract.

4. **[low] Wasted allocation ordering.** `vmhook.hpp:13708`/`13818` allocate
   `current_signature` BEFORE the cheap `get_name()` check, so every name-mismatch
   pays a heap allocation. `resolve_compatible_method` (`vmhook.hpp:13331`) orders the
   name check first. Reorder to match.

5. **Parity gap.** `get_method_by_oop_klass(name)` (`vmhook.hpp:14166` / `14500`) — used
   by the collection wrappers that dispatch into dynamic/anonymous subclasses, exactly
   where overloads bite — has **no** signature-aware sibling. Those users must drop to
   `vmhook::hook<>` for disambiguation.

## Exhaustive JVM angles I cover (tests/jvm/modules/method_explicit_signature.cpp)

Fixture `example/vmhook/fixtures/MethodExplicitSig.java` (+ superclass
`MethodExplicitSigBase.java`) defines heavily overloaded families and records a
**distinct per-overload side effect** so the exact overload is proven three ways at
once: (a) `proxy->signature()` equals the requested descriptor, (b) only the expected
overload's Java side-effect field changes, (c) the call's return value is that
overload's result. All `get_method(name,sig)->call()` work runs inside the `trigger()`
detour (where `current_java_thread` is live).

- **process(...) family, 6 overloads** exact-selected: `(I)I`, `(II)I`, `(J)J`,
  `(Ljava/lang/String;)Ljava/lang/String;`, `(Ljava/lang/String;I)Ljava/lang/String;`,
  `()V` — each verified by sig text + side effect + return.
- **combo(CharSequence) vs combo(String)** — the discriminating case: the SAME Java
  String can go to either overload, so only the exact descriptor disambiguates. Both
  proxies resolve to DIFFERENT Method*s (`signature()` differs) and each fires its own
  side effect exactly once.
- **STATIC smap(I)I / smap(String)String** via the portable `static_method(name,sig)` —
  exercises the type_index static overload and the null-owning-object dispatch.
- **Inherited base(I)I / base(II)I** declared on the superclass — exercises the
  hierarchy walk in the signature-aware overloads (a leaf-only match would miss them).
- **~15 wrong/absent/empty-signature MISS angles**: wrong return type (`(I)J`,
  `(I)Ljava/lang/Object;`), wrong arg type (`(D)I`), swapped args, extra arg
  (`(III)I`), a CharSequence param where none exists, EMPTY signature (strict miss),
  wrong NAME, malformed descriptors (`I)I` no open-paren, `(I)IX` trailing junk),
  static `(J)J` miss, static empty miss.
- **Safe no-op proof**: a wrong-signature lookup never dispatches — the guarded
  call leaves `process(I)`'s side-effect field untouched.
- **Documented real behaviour**: the STATIC overload does NOT filter on
  `JVM_ACC_STATIC`, so `get_method(type_index, "process", "(I)I")` (an instance method)
  RESOLVES with a null owning object; the lookup is signature-exact and succeeds.
- **Global isolation invariants**: every overload's side effect fired EXACTLY the
  expected count across the whole run (combo sum == 2, each smap == 1, etc.) — proving
  no miss leaked into a dispatch and no exact selection picked a sibling.

Total: 80+ `ctx.check()` angles.

## JDK-version sensitivities

- **Dispatch path**: on JDK 8..25 `StubRoutines::_call_stub_entry` is absent from
  VMStructs, so `call()` takes the **call_jni** path; on older/other builds where the
  stub is present it takes the **call_stub** path. The module records which path is
  live (`find_call_stub_entry()`), and the lookup result (which Method* gets latched)
  is path-INDEPENDENT — only the call dispatch differs. All my asserted strings are
  pure ASCII so the `read_java_string` ('?'-for-≥0x80) vs `GetStringUTFChars`
  (modified-UTF-8) divergence never affects them.
- **Compact strings** (JDK 9+) don't matter here because every fixture string is
  LATIN1/ASCII and round-trips identically through both decoders and `make_java_string`.
- **Descriptors are JDK-stable**: I verified every overload's descriptor with
  `javap -s` against the compiled fixture; `(Ljava/lang/String;)Ljava/lang/String;`
  etc. are identical across JDK 8/11/17/21/25.
- **GCC portability**: the module uses `static_method(name,sig)` (never the
  deducing-this static fallback) for static calls and copy-init everywhere
  (`r.sval = v.as_string();`), so it compiles on GCC/Clang/MSVC. Verified the TU
  compiles clean under g++ (MSYS2) with `-Wall -Wextra -Wpedantic`.
