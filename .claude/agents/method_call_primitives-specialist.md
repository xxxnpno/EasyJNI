---
name: method_call_primitives-specialist
description: Specialist that totally masters the vmhook method_call_primitives feature — finds every flaw and owns its exhaustive JVM tests.
---

# method_call_primitives specialist

I own `vmhook::method_proxy::call()` returning every JVM primitive (`Z B S C I J F D`)
and `void`, and the `value_t` that carries the result back into C++.

## Where the feature lives (vmhook/ext/vmhook/vmhook.hpp)

- `method_proxy::value_t` — the variant + its templated `operator target_type`
  conversion: **11956-12111**. The variant alternatives are
  `monostate, bool, int8_t, int16_t, int32_t, int64_t, float, double, uint16_t,
  uint32_t (compressed-OOP ref/array), std::string`. The conversion operator
  (**11987-12058**) `static_cast`s the stored alternative to the requested type,
  with special cases for `unique_ptr<wrapper>` (decode compressed OOP), `std::string`
  (eager string or `read_java_string`) and `void*` (`decode_oop_pointer`). Helpers:
  `is_void()` (**12066**), `is_string()` (**12074**), `as_string()` (**12090**).
- `call()` — the public entry + interpreter call_stub fast path: **12726-12938**.
  The primitive **result decode switch** is **12889-12937**:
  - `Z` → `(result_holder & 1) != 0`
  - `B/S/I/J` → narrowing `static_cast` to the signed width (sign-extends on read)
  - `C` → `static_cast<uint16_t>` (zero-extends)
  - `F` → `memcpy` from `int32_t` low bits; `D` → `memcpy` from the full `intptr_t`
  - `V` → `monostate`
- `call_jni()` — the JNI fallback used when `StubRoutines::_call_stub_entry` is
  absent (true on every CI JDK 8-26): **12141-12695**. The per-primitive dispatch
  slots are **12564-12643**: instance/static slot pairs Z=39/119, B=42/122,
  C=45/125, S=48/128, I=51/131, J=54/134, F=57/137, D=60/140; V=63/143 and
  the `<init>`/`<clinit>` nonvirtual reroute at slot 93 (**12533**).
- `sig_char_to_basic_type` — maps the return-type char to a HotSpot BasicType id
  (T_BOOLEAN=4 … T_VOID=14): **11877-11894**. `V`→14 is what tells the call_stub
  not to write a result.

## How it works internally

`call()` first checks `find_call_stub_entry()`. If present, it builds a
`params[8]` intptr_t slot array (receiver in slot 0 for instance calls), packs
each arg by `memcpy`, flips the JavaThread to `_thread_in_Java`, invokes the
hand-written call_stub trampoline with the BasicType return id, restores the
thread state, then decodes `result_holder` per the return char. If the call_stub
is absent it routes to `call_jni()`, which resolves the jclass+jmethodID (cached
on the proxy), packs a `jni_value[]`, and calls the matching
`Call(Static)?<Type>MethodA` slot, draining any pending Java exception via
`check_callee_exception()`. Either path yields the SAME `value_t`, which the
caller converts implicitly to the target C++ type.

The only way to drive `call()` from a test is from **inside a hook detour**:
`current_java_thread` is set only while the Java thread executes inside the
interpreter detour. So the JVM module hooks a trigger method and performs every
`call()` in that detour.

## Flaws I found (real, current)

- **[low] Silent failure on a null primitive JNI slot** — vmhook.hpp **12569,
  12579, 12589, 12599, 12609, 12619, 12629, 12639**. Every primitive branch in
  `call_jni`'s switch does `if (!fn) { return value_t{ monostate }; }` with **no
  `VMHOOK_LOG`**, whereas the sibling `V` branch (**12559**) and `L/[` branch
  (**12652**) both log a "slot N is null" diagnostic. A corrupt/unsupported JNI
  function table only surfaces for void/object returns; an `()I` method silently
  returns a `monostate` value_t that converts to `0`. The fix is two log lines per
  branch, eight branches — identical in shape to the existing 12559/12652 messages.
- **[low] `case 'F'` bit-cast routes through *signed* int32_t** — vmhook.hpp
  **12900**: `const std::int32_t bits{ static_cast<std::int32_t>(result_holder) };`
  then `memcpy`. The value is an opaque IEEE-754 bit pattern, not a signed int, and
  the upper 32 bits of `result_holder` are call_stub garbage. `std::uint32_t`
  documents intent and avoids implementation-defined narrowing. Behaviour is the
  same on the x64 target, so this is clarity not correctness — but `case 'D'`
  already uses the full width directly, so `F` is the odd one out.
- **[design] `monostate` collapses "void return" and "dispatch failure"** —
  `value_t` returns `monostate` for a legitimate `()V` success AND for every
  failure path (no env, malformed sig, null method id, null slot, Java exception).
  `is_void()` cannot tell them apart, so a failed `()I` and a successful `()V`
  both yield `int x = 0`. There is no in-band error signal on the primitive paths.
- **[design] `call()` has no `static_assert` on arg count** — vmhook.hpp
  **12726**: `call_jni` static-asserts `sizeof...(args) <= arg_cap`, but `call()`
  silently drops args past 8 via the `if (param_idx >= 8) return;` guard at
  **12799**. Worse, `long`/`double` consume TWO interpreter local slots each, so
  the real cap is signature-dependent and undocumented.

## Exhaustive JVM test angles I cover (tests/jvm/modules/method_call_primitives.cpp + example/vmhook/fixtures/MethodPrimitives.java)

All on a live JVM, all driven through `MethodPrimitives.trigger(int)` detour so a
real bytecode dispatch + the real call gate run. ~90 `ctx.check()` assertions:

- **Z**: true/false, instance + static; `bool true → int == 1` via value_t.
- **B**: 0, 1, -1, MAX(127), MIN(-128), instance + static; plus `-1` read into a
  wider `int` proves sign-extension (`== -1`, not 255).
- **S**: 0, -1, MAX(32767), MIN(-32768), instance + static.
- **C** (unsigned): 0, 'A'(65), MAX(0xFFFF=65535), instance + static; plus 0xFFFF
  read into an `int` proves **zero**-extension (`== 65535`, not -1).
- **I**: 0, -1, INT_MAX, INT_MIN, 42, instance + static; `(I)I` echo proves arg
  passthrough alongside the return (instance + static) and a Java-side side-effect
  field confirms the argument actually arrived.
- **J**: 0, -1, LONG_MAX, LONG_MIN, and a `0x0123456789ABCDEF` bit pattern
  (catches 32-bit truncation), instance + static.
- **F**: 0, 1, -1, 0.5, FLT_MAX, smallest subnormal (`MIN_VALUE`), **-0.0**
  (value 0 AND signbit set), **NaN** (`isnan`), **+Inf/-Inf** (`isinf` + sign),
  instance + static; plus `0.5f` widened to `double` is exactly `0.5`.
- **D**: 0, 1, -1, π (exact bit compare), DBL_MAX, smallest subnormal, **-0.0**,
  **NaN**, **+Inf/-Inf**, instance + static. Float/double specials are captured as
  raw bits through the atomic so NaN/Inf/-0.0 survive intact for the assertions.
- **V**: `is_void()` true for both instance and static void methods, plus the
  Java side-effect counter increments exactly once each; conversely an `int`
  return reports `is_void()==false` and `is_string()==false`.
- Records which dispatch path the live JDK uses (`call_stub` vs JNI fallback) as
  an `[INFO]` line so the same assertions document both paths.

## Known JDK-version sensitivities

- `StubRoutines::_call_stub_entry` is **absent from VMStructs on every JDK 8-26**
  tested, so in practice `call()` runs the **JNI fallback**. My tests assert the
  converted value unconditionally (both paths must agree) and only *record* the
  path for diagnostics — they never skip the value check on a missing call stub.
- The JNI primitive slot indices (39-60 instance, 119-140 static) are fixed by the
  JNI spec and stable across all versions; a wrong slot would dispatch the wrong
  type and my boundary assertions (sign/zero-extension, NaN/Inf) would catch it.
- `boolean` JNI returns are spec'd to 0/1, so the call_stub `& 1` mask and the JNI
  `r != 0` agree; my fixture never relies on high garbage bits in a `Z` return.
- Char is the only unsigned primitive; JDKs that mishandled the `uint16_t`
  alternative would fail `mcp_char_max_zero_extends_to_int_65535`.
