# method_proxy_call_return_primitives

## Summary
Audited `method_proxy::value_t` (lines 11439-11495), `call_jni()`'s primitive
return switch (11894-12074), and `call()`'s `result_holder` decode switch
(12269-12292). The primitive paths themselves are correct (BasicType ids match
HotSpot, JNI slot indices match the spec, sign-extension via narrowing static_cast
is right, float/double bitcasts work on Win-x64). Top concerns are user-facing
silent failures (every primitive `call_jni` slot-lookup miss returns monostate
with no log, unlike the `'V'`/`'L'` branches) and a significant API parity gap
with `field_proxy::value_t` (no string/vector/unique_ptr conversion path on
method results, so `std::string s = m.call()` on an `int`-returning method
silently yields `""`).

## Bugs

### [low] Silent failure when JNI function-table slot is null for any primitive return
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:11944-12023
- **description:** Every primitive branch in `call_jni`'s switch (Z/B/C/S/I/J/F/D)
  fetches the JNI dispatch function pointer from `table[slot]` and does
  `if (!fn) { return value_t{ std::monostate{} }; }` with **no diagnostic**.
  The two non-primitive branches (`'V'` at 11939 and `'L'/'['` at 12032) DO
  log a `JNI Call(Static)?VoidMethodA slot N is null` / `Call(Static)?ObjectMethodA
  slot N is null` message. The asymmetry means a corrupt or unsupported JNI
  function table only surfaces if the method happens to return void or an
  object - a method returning `int` (the common case) silently produces a
  monostate value_t that converts to `0`/`false`/`""`, and the caller has no
  idea the call gate was wrong.
- **repro:** Stub `table[51]` (CallIntMethodA) to null in a unit harness, invoke
  `method_proxy::call_jni()` on an `()I` method, observe that the log output is
  empty and the return value silently looks like a method returning 0.
- **suggested_fix:** Add the same `VMHOOK_LOG("{} method_proxy::call_jni('{}{}'):
  JNI Call(Static)?<Type>MethodA slot {} is null.", ...)` line to each primitive
  branch's `if (!fn)` arm, identical in shape to the existing 11924/11939/12032
  messages. Two lines per branch, eight branches.
- **confidence:** certain

### [low] `case 'F'` bit-cast does an unnecessary signed-narrowing detour
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:12277-12283
- **description:** The float decode does
  `const std::int32_t bits{ static_cast<std::int32_t>(result_holder) };`
  then `memcpy(&f, &bits, sizeof(f))`. Routing through a *signed* int32_t for a
  raw IEEE-754 bit pattern is gratuitous - the upper 32 bits of `result_holder`
  may be garbage (the HotSpot call_stub writes the float result by storing the
  jint top-of-stack into the low 4 bytes of `*result`), and the value is meant
  to be opaque bits, not a signed integer. Using `std::uint32_t` documents
  intent and avoids implementation-defined narrowing of a "negative" intptr
  to int32_t. Behaviour is the same on the platforms vmhook targets, so this
  is style/clarity, not a correctness bug.
- **repro:** N/A - cosmetic.
- **suggested_fix:** `const std::uint32_t bits{ static_cast<std::uint32_t>(result_holder) };`
  matches the pattern used for `'F'` in `field_proxy::get` and surrounding bit-cast
  code (see e.g. `decode_oop_pointer` uses of uint32_t).
- **confidence:** certain

## Improvements

### [S] [USER_FACING] Add string/vector/unique_ptr/void* semantic conversions on method_proxy::value_t for parity with field_proxy::value_t
- **rationale:** `field_proxy::value_t::cast_for_variant` (vmhook.hpp:10943-11009)
  decodes a compressed-OOP uint32_t into `std::string` (via `read_java_string`),
  `std::vector<T>` (via `read_array_value`), `std::unique_ptr<T>` (constructs the
  wrapper), and `void*` (via `decode_oop_pointer`). `method_proxy::value_t`
  (11470-11494) only special-cases `void*`; everything else falls into a
  `static_cast`-or-default branch. So:
  - `std::string s = m.call();` on a method returning `Ljava/lang/Object;`
    (anything *other* than `Ljava/lang/String;`, which is auto-converted on the
    Object branch at 12046-12054) silently yields `""` instead of decoding
    `Object::toString()` style content.
  - `std::vector<int> v = m.call();` on a method returning `[I` silently yields
    `{}` instead of reading the array elements.
  - `std::unique_ptr<entity> e = m.call();` on a method returning an `Lentity;`
    silently yields `nullptr` instead of wrapping the returned OOP.
  These are user-facing footguns the user explicitly called out as a parity gap.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:11439-11495
- **suggested_change:** Lift `cast_for_variant` out of `field_proxy::value_t`
  into a shared detail helper (e.g. `vmhook::detail::convert_variant_to<T>`),
  pass the *signature* alongside the variant (so vector and string decoding can
  pick the right element type for array fields). Then have both value_t
  conversion operators call it. The method_proxy::value_t already has the
  std::string alternative for String returns; the helper needs to handle that
  case (just return the stored string) in addition to the uint32_t-compressed-OOP
  case. Pass the method's return-type signature into value_t at construction so
  the conversion knows what kind of array or class to decode.

### [XS] [INTERNAL] Use std::uint32_t for the float bit-cast intermediate
- **rationale:** See bug above; identical body but framed as a cleanup.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:12280-12281
- **suggested_change:** Replace `const std::int32_t bits` with `const std::uint32_t bits`.

### [S] [USER_FACING] Distinguish "call failed" from "void return" and "false bool" in value_t
- **rationale:** `value_t` returns `monostate` on EVERY failure path (no
  current_jni_env, malformed signature, GetMethodID null, slot null, Java
  exception thrown, unhandled return type char). It also returns `monostate`
  for legitimate void returns. The conversion operator turns monostate into
  `target_type{}`, so a successful `()V` call and a failed `()I` call both
  produce `int x = 0;` at the call site - there is no in-band way for the
  user to tell them apart. `field_proxy` has the same issue but only on
  explicit `optional`-returning helpers; method calls are the case where you
  actually need to know if dispatch succeeded.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:11439-11453, 11894-12074, 12269-12292
- **suggested_change:** Add an `is_void()` predicate that returns true iff the
  return type char was `'V'`, and an `is_error()` (or `failed()`) predicate
  the failure paths set explicitly. Today both states collapse to `monostate`,
  so users must read the log to learn the difference. Could be implemented
  cheaply with a separate `bool error{false}` field on `value_t` (or by
  adopting `std::optional<value_t>` as the return type from `call()` /
  `call_jni()`, with the optional being empty only on dispatch failure and
  filled with monostate-data for legitimate `void` returns).

### [XS] [INTERNAL] Hoist the per-branch JNI slot indices into a constexpr table
- **rationale:** The eight `'Z'`/`'B'`/`'C'`/`'S'`/`'I'`/`'J'`/`'F'`/`'D'` branches
  in `call_jni` are near-identical templated boilerplate (~7 lines each x 8 =
  ~56 lines). A `constexpr std::array<{slot_instance, slot_static}, 8>` indexed
  by `ret_char` would let the body collapse to one templated helper. The
  return-type conversion in `call()` (lines 12269-12292) is similarly mechanical
  and could share the same dispatch table.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:11944-12023, 12269-12292
- **suggested_change:** Define `struct prim_jni_descriptor { std::size_t instance_slot,
  static_slot; }`, build a constexpr lookup keyed on ret_char, then make each
  primitive branch a one-liner around a templated `dispatch_primitive<T>(...)`
  that does the slot lookup + null check + log + call + checkException + return.

### [S] [USER_FACING] Document and enforce the 8-argument cap in call()
- **rationale:** `call_jni` has `static_assert(sizeof...(args_t) <= arg_cap)`
  (line 11683-11684). `call()` (line 12106) does not - it silently truncates
  to the first 8 args in the pack-fold at 12231 because `pack` checks
  `if (param_idx >= 8) return;` and bails (line 12179-12182). If the user
  passes 9+ args to `call()`, they get a silent drop with NO log AND NO
  static_assert. The cap should be enforced at compile time identically to
  call_jni.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:12106-12109, 12169-12182
- **suggested_change:** Add a matching `static_assert(sizeof...(args_t) <= 8,
  "method_proxy::call: max 8 arguments (8 interpreter local slots)");` at
  the top of `call()`. Also: long/double take TWO local slots each, so the
  REAL cap depends on the call signature. Document this somewhere visible.

## Tests

### [standalone_unit] [new] test_method_proxy_value_t_primitive_round_trip
- **description:** Construct `method_proxy::value_t` from each variant alternative
  (bool, int8_t, int16_t, int32_t, int64_t, float, double, uint16_t, uint32_t,
  std::string, monostate) and verify the implicit conversion operator returns
  the expected value when targeting the same type, and a static_cast'd value
  when targeting a different arithmetic type.
- **asserts:** `int(value_t{int8_t(-1)}) == -1`; `bool(value_t{true}) == true`;
  `double(value_t{1.5f}) == 1.5`; `int(value_t{std::string{"x"}}) == 0`
  (no static_cast path); `void*(value_t{uint32_t(0)}) == nullptr`;
  `int(value_t{std::monostate{}}) == 0`.

### [standalone_unit] [new] test_method_proxy_value_t_void_star_routes_through_decode_oop
- **description:** Verify the special-case at 11479-11483: when stored is uint32_t
  and target is `void*`, the conversion uses `decode_oop_pointer` rather than
  static_cast (which would truncate). Stub `decode_oop_pointer` if necessary
  (or assert with a value whose decoded result is known).
- **asserts:** Without the special case, `static_cast<void*>(uint32_t{42})` would
  return `(void*)42`; with the special case, it returns
  `decode_oop_pointer(42)`. Comparing the two paths differentiates the bug.

### [standalone_unit] [new] test_call_result_decode_byte_short_sign_extension
- **description:** Hits the `case 'B'` / `case 'S'` decode at lines 12272-12273.
  Pack `result_holder = 0xFFFFFFFFFFFFFFFFull` (Java byte -1, short -1) and
  verify the variant alternative holds `int8_t{-1}` and `int16_t{-1}`, NOT
  255 or 65535. Equivalent test for `case 'C'` with the same value asserting
  the variant alternative holds `uint16_t{0xFFFF}`.
- **asserts:** Synthesizes the decode-side switch (12269-12292) directly with a
  free function or a stripped-down value_t-from-result_holder helper. Asserts
  the variant alternative AND the int conversion result.

### [standalone_unit] [new] test_call_result_decode_float_bit_cast
- **description:** Pack `result_holder = 0x3F800000` (IEEE-754 1.0f) and verify
  the `case 'F'` branch returns `value_t{1.0f}` with no precision loss. Repeat
  with `0xFFC00000` (qNaN) and `0xFF800000` (-inf) to make sure narrowing
  through int32_t (12280) does not perturb the bit pattern. Same for `'D'` with
  `0x3FF0000000000000` -> 1.0.
- **asserts:** `float(value_t{...}) == 1.0f`, signbit/isnan/isinf preserved.

### [standalone_unit] [new] test_call_result_decode_bool_high_bits_ignored
- **description:** Hits `case 'Z'` at 12271 with `result_holder = 0x100`. The
  mask `& 1` should yield false even though the byte is non-zero.
- **asserts:** `bool(value_t{...}) == false` for `result_holder = 0x100`.
  Note: Java spec says bool returns are always 0 or 1, but the test documents
  the current implementation's mask semantics so a future change to
  `result_holder != 0` is caught and considered.

### [standalone_unit] [extend_existing] test_call_jni_primitive_slot_null_emits_log
- **description:** Verifies the fix for the "silent failure" bug. Stub a JNI
  function table with one of the primitive slots (e.g. table[51] = nullptr)
  and confirm that after `call_jni` returns monostate the log buffer contains
  the expected "JNI CallIntMethodA slot 51 is null" line.
- **asserts:** The captured VMHOOK_LOG output for each of Z/B/C/S/I/J/F/D
  contains the slot-null message after the fix is applied. Today this test
  would FAIL because no message is emitted.
- **existing_file:** tests/test_helpers.cpp (alongside the existing
  write_jni_arg_to_slot tests).

### [standalone_unit] [new] test_value_t_compile_time_conversions
- **description:** Compile-time assertions (`static_assert(std::is_convertible_v<
  method_proxy::value_t, int>)` etc) covering bool, int, long long, float,
  double, void*, std::string, std::uint16_t. Catches accidental conversion
  operator regressions at compile time.
- **asserts:** `is_convertible_v` for each supported target type. Optionally,
  a negative case: `static_assert(!is_convertible_v<value_t, std::vector<int>>)`
  documents the parity gap with field_proxy::value_t until the improvement is
  implemented.

### [jvm_integration] [new] test_call_returns_byte_short_char_negative_values
- **description:** Live JVM test that calls a Java `()B` returning `-1`,
  `()S` returning `-1`, `()C` returning `0xFFFF`, and verifies the C++ side
  observes the same value when assigning into the matching C++ type. Catches
  any future change that breaks sign-extension or truncation.
- **asserts:** `static_cast<int>(m.call())` matches the Java return for each
  of byte/short/char.

### [jvm_integration] [new] test_call_returns_float_nan_inf
- **description:** Java method returning `Float.NaN`, `Float.POSITIVE_INFINITY`,
  `-0.0f`; same for double. Verifies bit-cast preserves classification.
- **asserts:** `std::isnan(m.call())`, `std::isinf(m.call())`,
  `std::signbit(static_cast<float>(m.call())) == true` for -0.0.

### [jvm_integration] [new] test_call_method_with_long_double_arg_count_bug
- **description:** Calls a Java method `(JJ)J` (two longs, returns long).
  Probes whether the current `param_idx` accounting (single slot per arg)
  is correct - JVM interpreter actually expects long/double to occupy 2
  local slots each. If size_of_parameters is wrong, the interpreter reads
  garbage from the next slot.
- **asserts:** The return value matches the Java method's expected sum;
  failing this test would point at a deeper bug in argument-slot packing
  that is OUTSIDE this audit but worth recording.

## Parity Concerns
- `method_proxy::value_t` has no String / vector / unique_ptr / void*-from-non-OOP
  conversions; `field_proxy::value_t::cast_for_variant` (10943-11009) handles all
  of them. The two value_t types should share the same conversion helper.
- `call_jni` logs slot-null for void and Object branches but NOT for any of the
  eight primitive branches - inconsistent diagnostic coverage between sibling
  return-type handlers in the same switch.
- `call()` enforces no `static_assert` on argument count (silently drops args
  past 8 via the `if (param_idx >= 8) return;` guard); `call_jni` enforces it
  at compile time (line 11683-11684). The two entry points should share the
  same compile-time check.
- `value_t` does not carry the method's return signature, so it can never grow
  the array-decode parity that `field_proxy::value_t` enjoys (which carries
  `std::string signature{};` on line 10822).
