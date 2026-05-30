# return_value_set_primitives

## Summary
`return_value::set<T>` is a single 30-line template at `vmhook/ext/vmhook/vmhook.hpp:1147-1176` that writes the user's value into a 64-bit `return_slot::retval` cell consumed by the trampoline's `mov rax,[rsp+8] ; movq xmm0,rax` epilogue. The signed/integral/<8-byte branch was added to sign-extend `int8/16/32` so `ireturn` does not pop +255 instead of -1; everything else takes a `memcpy` zero-extend path. The implementation is mostly correct, but `char` (whose signedness is implementation-defined) is the one primitive whose behaviour silently changes between MSVC (unsigned, zero-extends) and GCC/Clang on x86_64 (signed, sign-extends) — and JVM `C` is unsigned 16-bit, so the C++ `char` path is the wrong primitive to ever feed in.

## Bugs

### [medium] `set(char{...})` produces platform-dependent retval bits because `char` signedness is implementation-defined
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:1165-1175
- **description:** The `if constexpr` predicate is `std::is_signed_v<value_type> && std::is_integral_v<value_type> && sizeof < 8`. Plain `char` is a distinct integral type from `signed char` / `unsigned char`, and `std::is_signed_v<char>` follows the ABI's default char signedness — `false` on MSVC, `true` on GCC/Clang on x86_64 by default, and the GCC default flips to unsigned on ARM/AArch64 and on `-funsigned-char`. So `rv.set(char{ '\x80' })` writes `0x0000000000000080` under MSVC and `0xFFFFFFFFFFFFFF80` under default GCC for the same source code. Because the JVM `C` (jchar) type is *unsigned* 16-bit UTF-16, the sign-extended GCC result is the wrong bit pattern for a hook on a method returning `char`; on a method returning `byte` it is the right bit pattern. Same source, two JDK-visible behaviours depending on which OS the agent was compiled on.
- **repro:** Compile `static_assert(std::is_signed_v<char> == false);` — passes on MSVC, fails on Linux/g++. Then `rv.set(char{ '\x80' });` and read `slot.retval` — 0x80 vs 0xFFFFFFFFFFFFFF80.
- **suggested_fix:** Either (a) `static_assert(!std::is_same_v<value_type, char>, "Pass jchar (char16_t / uint16_t) for a Java char return, signed char for a Java byte return; plain 'char' has implementation-defined signedness and would produce different retval bits between MSVC and GCC.")`, or (b) treat `char` deterministically — convert to `unsigned char` first inside the integral branch. Option (a) gives a compile-time diagnostic which matches the existing `static_assert` style and forces the user to disambiguate.
- **confidence:** certain

### [low] `set<long double>(...)` is rejected with a generic "return type too large" message that does not name the offender or suggest a fix
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:1151-1152
- **description:** `long double` is 16 bytes on System V x86_64 (and 12 on i686). The `static_assert(sizeof(value_type) <= sizeof(std::int64_t), "return type too large for hook slot")` correctly rejects it, but the diagnostic doesn't say what to pass instead (the JVM has no 128-bit float; the user wanted `double`). A user who innocently writes `ret.set(3.14L)` (note the `L`) gets a cryptic compile error. Same issue for any > 8-byte trivially-copyable POD.
- **repro:** `vmhook::return_value rv{ &slot }; rv.set(3.14L);` — fires `return type too large for hook slot` with no hint.
- **suggested_fix:** Expand the message: `"return_value::set: value_type is wider than 8 bytes — the JVM has no return type wider than long/double (8 bytes). For Java 'double' return, pass 'double' (not 'long double'); for Java 'long', pass 'std::int64_t'."`
- **confidence:** certain

### [low] `set(value)` silently accepts `value_type = void*` even when the hooked method's return slot is a primitive
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:1147-1176
- **description:** There is no link between the `set<T>` template and the Java return-type signature of the hooked method. `rv.set<void*>(some_ptr)` on a hook over `int getScore()` writes 8 raw pointer bytes into the slot and the trampoline returns them as a junk int. The other direction is just as silent: `rv.set(std::int32_t{ 9999 })` on a hook over `Object foo()` writes 4 zero-extended bytes that the JIT interprets as a tagged compressed oop and follows. Both are user errors, but the template currently accepts every trivially-copyable type with sizeof <= 8 with no compile-time or run-time check — and the wrapper-only `set(nullptr)` overload (lines 1196-1203) is the *only* place the API ties a C++ type to a Java type.
- **repro:** Hook `int Foo.bar()`, call `ret.set<void*>(reinterpret_cast<void*>(0xCAFEBABEDEADBEEFull));` — Java reads the truncated low 32 bits as 0xDEADBEEF (= -559038737) and continues.
- **suggested_fix:** Out of scope for a localized fix in `set()` itself (it has no type information about the hooked method); but documenting the type-to-JVM-descriptor mapping in the `set` docstring (e.g. `// jboolean=bool, jbyte=int8_t, jchar=char16_t/uint16_t, jshort=int16_t, jint=int32_t, jlong=int64_t, jfloat=float, jdouble=double, jobject/jarray=void* (encoded oop)`) would close most foot-guns without an API change. A higher-effort fix would be to plumb the hooked method's `BasicType` into the trampoline and `static_assert` it matches at hook-registration time.
- **confidence:** likely

## Improvements

### [XS] [USER_FACING] Document the C++ -> JVM type table in the `set` docstring
- **rationale:** The current docstring on `return_value` (lines 1124-1137) only shows `ret.set(std::int32_t{ 9999 });`. Users hooking a `byte/short/long/float/double/boolean/char`-returning Java method have no in-source guide for which C++ type maps to which JVM descriptor, and the only existing example uses `int32_t`. Adding a 9-line table immediately above the `set` template (or as `@par C++ <-> JVM type mapping:` in its own docstring) eliminates the most common cargo-culted mistake (`int` instead of `int32_t`, `long` instead of `int64_t`, `char` instead of `uint16_t` for jchar).
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:1138-1147
- **suggested_change:** Insert a `@par C++ -> JVM mapping:` block listing:
  - `Z (boolean)` -> `bool`
  - `B (byte)`    -> `std::int8_t`
  - `S (short)`   -> `std::int16_t`
  - `C (char)`    -> `char16_t` (or `std::uint16_t`)
  - `I (int)`     -> `std::int32_t`
  - `J (long)`    -> `std::int64_t`
  - `F (float)`   -> `float`
  - `D (double)`  -> `double`
  - `L/[`         -> `vmhook::oop_t` or `ret.set<wrapper_t>(nullptr)` for null

### [S] [USER_FACING] Add a `static_assert` rejecting plain `char` with a fix-it message
- **rationale:** Closes Bug #1 with a one-line guard. Plain `char` is one of those rare types whose meaning silently shifts across compilers, and any code path that touches it from a template is a portability landmine. The user gets a compile-time pointer to the right type instead of an at-runtime sign-extension surprise.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:1151-1156
- **suggested_change:** Add:
  ```cpp
  static_assert(!std::is_same_v<value_type, char>,
                "return_value::set: plain 'char' has implementation-defined signedness "
                "(signed under GCC default, unsigned under MSVC). For a Java 'char' return "
                "pass char16_t or std::uint16_t; for a Java 'byte' return pass std::int8_t.");
  ```
  immediately after the existing two static_asserts.

### [XS] [USER_FACING] Expand the "return type too large" diagnostic
- **rationale:** Closes Bug #2. One-line message change, no behavioural impact.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:1151-1152
- **suggested_change:** Replace `"return type too large for hook slot"` with `"return_value::set: value_type is wider than 8 bytes — JVM has no return type wider than long/double. For Java 'double' pass 'double' (not 'long double'); for Java 'long' pass 'std::int64_t'."`

### [XS] [INTERNAL] Use a single `is_signed_integer_v` helper instead of inlining the predicate
- **rationale:** The predicate `std::is_signed_v<T> && std::is_integral_v<T> && sizeof(T) < sizeof(int64_t)` appears once today but the same logic is conceptually mirrored in `field_proxy::set` and in `set_arg`. Pulling it into a named constexpr variable (or `concept`) makes the intent self-documenting and lets the `static_assert` for `char` borrow the same vocabulary.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:1165-1167
- **suggested_change:**
  ```cpp
  template<typename T>
  inline constexpr bool requires_sign_extension_for_jvm_slot_v =
      std::is_signed_v<T> && std::is_integral_v<T> && sizeof(T) < sizeof(std::int64_t);
  ```
  then `if constexpr (requires_sign_extension_for_jvm_slot_v<value_type>) ...`.

### [S] [USER_FACING] Document the `set(nullptr)` overload's restriction in the primitive `set` docstring
- **rationale:** A user who writes `ret.set(nullptr)` from a hook on `int getScore()` will get an overload-resolution error pointing at a 200-line SFINAE failure. The primitive `set` template's docstring doesn't mention that nullptr is only valid for wrapper-typed returns. One line clarifying "for nullable Java return types only" prevents the confusion.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:1124-1137
- **suggested_change:** Add `// For nullable Java returns: ret.set<sdk::wrapper_type>(nullptr);` to the example block, with a `@see set(nullptr_t)` cross-reference.

### [S] [USER_FACING] Add a deleted overload for `nullptr_t` with a diagnostic when the user forgets the wrapper type
- **rationale:** `ret.set(nullptr)` (no template argument) currently fails overload resolution because the SFINAE-constrained `set(nullptr_t)` requires `wrapper_type : object_base`. The error message is a compiler diagnostic, not a vmhook one. Adding `template<> auto set(std::nullptr_t) noexcept -> void = delete;` (outside the constraint) lets us attach a `static_assert(false, "ret.set(nullptr) needs an explicit wrapper type: ret.set<sdk::your_wrapper_type>(nullptr).")` via a tag struct, surfacing the fix immediately.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:1196-1203
- **suggested_change:** Add an unconstrained `set(std::nullptr_t)` that triggers a `static_assert(sizeof(value_type) == 0, "...")` style diagnostic naming the required usage.

## Tests

### [standalone_unit] [new] test_return_value_set_int64_long_bit_pattern_round_trip
- **description:** `int64_t` (Java `long`) takes the memcpy path because `sizeof < sizeof(int64_t)` is false. No existing test verifies that `rv.set(int64_t{ 0xDEADBEEFCAFEBABE })` lands all 8 bytes (regression guard against someone "fixing" the `if constexpr` to `<=` and accidentally wiping the high bits via `static_cast`).
- **asserts:** `slot.retval == int64_t{ 0xDEADBEEFCAFEBABE }`; `slot.cancel == true`; same for `int64_t{ INT64_MIN }`, `int64_t{ -1 }`, `int64_t{ 0 }`.
- **existing_file:** tests/test_helpers.cpp (extend `test_return_value_set_non_integer_types` or add a sibling).

### [standalone_unit] [new] test_return_value_set_uint16_jchar_round_trip
- **description:** `uint16_t` is the canonical C++ type for Java's `jchar` (UTF-16 code unit). Currently `test_return_value_set_non_integer_types` tests `uint32_t` and `uint8_t`, but not `uint16_t`. Cover the boundary value `0xFFFF` to prove the upper 48 bits stay zero (no spurious sign extension under the unsigned-integral path).
- **asserts:** `rv.set(uint16_t{ 0xFFFF }); slot.retval == 0xFFFF`; `rv.set(uint16_t{ 0x0000 }); slot.retval == 0`; `rv.set(uint16_t{ 0x8000 }); slot.retval == 0x8000` (high bit set, must NOT sign-extend).
- **existing_file:** tests/test_helpers.cpp

### [standalone_unit] [new] test_return_value_set_int8_byte_min_max_boundaries
- **description:** Existing test only covers `int8_t{-1}`. Cover the boundary values `INT8_MIN (-128)` and `INT8_MAX (+127)` — both must sign-extend correctly. Also covers `int8_t{0}` to prove the zero-write doesn't get confused with cancel-false.
- **asserts:** `rv.set(int8_t{ INT8_MIN }); slot.retval == int64_t{-128}` (= 0xFFFFFFFFFFFFFF80); `rv.set(int8_t{ INT8_MAX }); slot.retval == 127`; `rv.set(int8_t{ 0 }); slot.retval == 0 && slot.cancel == true`.
- **existing_file:** tests/test_helpers.cpp (extend `test_return_value_sign_extension`).

### [standalone_unit] [new] test_return_value_set_int16_short_min_max_boundaries
- **description:** Mirror of the int8 boundary test — exists for `-12345` but not `INT16_MIN`/`INT16_MAX`.
- **asserts:** `rv.set(int16_t{ INT16_MIN }); slot.retval == int64_t{-32768}`; `rv.set(int16_t{ INT16_MAX }); slot.retval == 32767`.
- **existing_file:** tests/test_helpers.cpp (extend `test_return_value_sign_extension`).

### [standalone_unit] [new] test_return_value_set_int32_int_min_max_boundaries
- **description:** Mirror — exists for `-1` and `42` but not `INT32_MIN`/`INT32_MAX`. `INT32_MIN` is the load-bearing case: under the sign-extension branch, `static_cast<int64_t>(INT32_MIN)` must land as `0xFFFFFFFF80000000`, not as `0x0000000080000000` (which would be `+2147483648` on the JVM side, the classic `Math.abs(Integer.MIN_VALUE)` bug).
- **asserts:** `rv.set(int32_t{ INT32_MIN }); slot.retval == int64_t{ INT32_MIN }`; same for `INT32_MAX`.
- **existing_file:** tests/test_helpers.cpp (extend `test_return_value_sign_extension`).

### [standalone_unit] [new] test_return_value_set_float_special_values
- **description:** Existing test covers `3.14f`. Special IEEE-754 bit patterns (negative zero, +inf, -inf, NaN) take the memcpy path and must round-trip. NaN especially: it must NOT be normalized or rendered as 0.
- **asserts:** `rv.set(-0.0f)` then memcpy the low 32 bits of retval to `uint32_t`, compare to `0x80000000`; same for `+inf` (`0x7F800000`), `-inf` (`0xFF800000`), and a quiet NaN. Each case also asserts `slot.cancel == true`.
- **existing_file:** tests/test_helpers.cpp (extend `test_return_value_set_non_integer_types`).

### [standalone_unit] [new] test_return_value_set_double_special_values
- **description:** Same as the float test but for `double` — `-0.0`, `+inf`, `-inf`, quiet NaN. Memcpy path, upper bits of retval must be the IEEE-754 64-bit pattern.
- **asserts:** Bit-pattern round-trip via `memcpy(&retval, &double_value, 8); slot.retval == expected_pattern`.
- **existing_file:** tests/test_helpers.cpp

### [standalone_unit] [new] test_return_value_set_bool_non_canonical_value_normalizes
- **description:** Documents (or guards) behaviour when a non-canonical `bool` (a byte whose value is e.g. 42) is forwarded into `set`. Current implementation memcpy's the raw byte. This is technically UB on the C++ side but easy to hit from `reinterpret_cast`. The test pins the current contract — either "we memcpy as-is" or "we normalize to 0/1" — so any future drift is caught.
- **asserts:** Construct a `bool` from a `uint8_t` byte of `42` via memcpy, pass to `rv.set`, assert the documented contract.
- **existing_file:** tests/test_helpers.cpp

### [standalone_unit] [new] test_return_value_set_char_signedness_diagnostic
- **description:** If the fix for Bug #1 lands as a `static_assert`, this is a "compile-fail" smoke test (lives next to existing static_assert checks, gated on `#ifdef VMHOOK_EXPECT_COMPILE_FAIL`). If the fix lands as a runtime normalization, this verifies `rv.set(char{ '\x80' })` produces the same retval bits on MSVC and GCC.
- **asserts:** Either compilation fails with the expected message, or `slot.retval == 0x80` regardless of platform.
- **existing_file:** tests/test_helpers.cpp

### [standalone_unit] [new] test_return_value_set_unsigned_long_high_bit
- **description:** `uint64_t` with the high bit set (e.g. `0x8000000000000000`) takes the memcpy path; verify the retval cell holds the same bit pattern (`int64_t{INT64_MIN}` when reinterpreted as signed). Closes the gap noted in CHANGELOG ("uint" is mentioned but only `uint32_t` is currently tested).
- **asserts:** `rv.set(uint64_t{ 0x8000000000000000ull }); static_cast<uint64_t>(slot.retval) == 0x8000000000000000ull`.
- **existing_file:** tests/test_helpers.cpp

### [jvm_integration] [new] test_hook_method_returning_each_primitive_returns_the_set_value
- **description:** End-to-end coverage that the trampoline's `mov rax,[rsp+8]; movq xmm0,rax` epilogue actually delivers the slot bits to the JVM correctly for every primitive return type. Requires a live JDK 8/17/21 fixture. For each of `boolean`/`byte`/`short`/`char`/`int`/`long`/`float`/`double`-returning Java method, hook it with `ret.set(<extreme value>)` and verify the Java caller observes the set value (not zero, not sign-flipped, not truncated).
- **asserts:** From Java: `assertEquals((byte)-128, target.getByte());`, `assertEquals((char)0xFFFF, target.getChar());`, `assertEquals(Long.MIN_VALUE, target.getLong());`, `assertEquals(Double.NEGATIVE_INFINITY, target.getDouble(), 0);`, etc.
- **existing_file:** none yet — would belong in a new `tests/test_jvm_return_primitives.cpp` (or extend `tests/test_unified_call_syntax.cpp` if it already drives a JVM fixture; quick grep shows it doesn't).

## Parity Concerns
- `set` and `set_arg` use *different* size-handling rules: `set` does explicit `static_cast<int64_t>` sign-extension for signed integrals < 8 bytes; `set_arg` does a plain `memcpy(&raw, &value, sizeof(clean_value_type))` into a `void*` slot (vmhook.hpp:7624-7629) with no sign-extension. The interpreter's local-variable array is u4/u8 per slot and treats unfilled high bits as zero, so writing `int8_t{-1}` via `set_arg` lands `0x00000000000000FF` in `locals[-index]` — not `0xFFFFFFFFFFFFFFFF`. For a `byte`-typed local this is OK on the JVM side (the next `iload` reads only the low 32 bits and treats them as signed int), but for parity with `set`'s very carefully-written sign-extension comment this should at least be documented or unified.
- `set` accepts every trivially-copyable type with sizeof <= 8 with no link to the hooked method's actual return descriptor; the *only* type-checked overload is `set<wrapper_type>(nullptr)` (object base requires-clause at line 1197). No analogous mechanism exists for primitives, so a hook on `int foo()` will silently accept `ret.set(3.14)` (double-bit-pattern reinterpreted as int) without a peep.
- `field_proxy::set` (CHANGELOG Unreleased "Fixed") gained an explicit size-mismatch guard via `jvm_primitive_byte_width`. `return_value::set` does not have an equivalent guard tying the slot to a JVM type, so it remains the one place where a wrong-width primitive gets accepted silently — by design today, because the slot is a 64-bit raw cell, but worth a comment cross-referencing the `field_proxy::set` fix so future readers don't mistakenly add the same guard here without plumbing in the hook's `BasicType`.
- The wrapper-typed `set(nullptr)` overload (line 1196-1203) is the only `set` overload that ties a C++ type to a JVM type. Extending the same `requires` style to the primitive `set` (e.g. a typed `set_bool`, `set_byte`, ..., `set_double` family) would give users sibling APIs of the same shape; that's an L-effort change and arguably API churn, but the inconsistency is worth flagging.
