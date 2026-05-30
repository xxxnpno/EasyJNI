# hook_arg_decoding_primitives

## Summary
Two parallel argument-decoding paths exist in vmhook: the modern `detail::extract_frame_arg` (vmhook.hpp:7183-7259) driven by the compile-time `java_slot_offsets` table is the one used by user hook callbacks installed via `vmhook::hook<T>(...)` — it correctly handles J/D 2-slot widening, has a `static_assert` for unsupported types, and is well-documented. The legacy `frame::get_argument<T>(index)` (vmhook.hpp:5159-5202) and its variadic wrapper `frame::get_arguments<types...>()` (vmhook.hpp:5016-5022), however, treat `index` as both an *argument index* and a *slot index* simultaneously — they have no J/D 2-slot awareness, no bounds check, and silently return a default-constructed value for unsupported types. Any user calling `frame->get_arguments<int, long, int>()` from inside their hook detour gets the third arg read from the high (unused) half of the long.

## Bugs

### [HIGH] `frame::get_arguments<types...>()` does not widen J/D slots, silently misreads every arg after a Java long/double
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:5016-5022, 5142-5202
- **description:** The variadic wrapper at line 5021 expands as `this->get_argument<types>(index++)...` producing the calls `get_argument<T0>(0), get_argument<T1>(1), get_argument<T2>(2), ...` — pure tuple-index, no slot widening. `get_argument` at line 5170 then does `locals[-index]` verbatim. This is *exactly* the bug the modern `extract_frame_arg`/`java_slot_offsets` path was introduced to fix (see the explicit warning in vmhook.hpp:7147-7153: "extract_frame_arg used to be called with the TUPLE INDEX directly … every method whose argument list contains a J/D followed by anything else read the wrong slot for every following arg and silently fed the user detour garbage"). `frame::get_arguments<...>()` is still public, still documented at line 5007-5015 as "Retrieves all method arguments as a typed tuple", and still recommended by the inline docs at vmhook.hpp:10315 ("Obtain it from `frame->get_arguments<void*>()` which already calls decode_oop_pointer() internally") and vmhook.hpp:12676/12726. A caller hooking `void f(long a, int b)` and doing `auto [a, b] = frame->get_arguments<std::int64_t, std::int32_t>()` reads `b` from slot 1 (the long itself / its low half), not slot 2 — wrong value, silent. The matching `test_traits.cpp:184-187` static_assert exists *only* for the modern path; the legacy wrapper is unprotected.
- **repro:**
  ```cpp
  // Java: void f(long a, int b) { /* b == 7 */ }
  vmhook::hook<X>("f", [](vmhook::return_value&, frame* fr) {
      auto [a, b] = fr->get_arguments<std::int64_t, std::int32_t>();
      // b is read from locals[-1] (the long slot), NOT locals[-2].
      // Observed b: the low half of the Java long, not the Java int.
  });
  ```
- **suggested_fix:** Route `frame::get_arguments<types...>()` through `vmhook::detail::java_slot_offsets<std::tuple<types...>>::value` exactly as the hook dispatcher at vmhook.hpp:7815-7822 does. Implementation:
  ```cpp
  template<typename... types>
  auto get_arguments() const noexcept -> std::tuple<types...>
  {
      using arg_tuple = std::tuple<types...>;
      constexpr auto offsets{ vmhook::detail::java_slot_offsets<arg_tuple>::value };
      return [&]<std::size_t... I>(std::index_sequence<I...>) {
          return std::tuple<types...>{
              this->get_argument<std::tuple_element_t<I, arg_tuple>>(offsets[I])...
          };
      }(std::make_index_sequence<sizeof...(types)>{});
  }
  ```
  Alternatively, delete `frame::get_argument` / `frame::get_arguments<...>()` entirely and force every caller through `extract_frame_arg`, since the typed-tuple use case is already covered there.
- **confidence:** certain

### [MEDIUM] `frame::get_argument<T>(index)` and `extract_frame_arg<T>(frame, index)` have no upper bound on `index` — symmetric to the `set_arg` hazard already fixed
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:5159-5202, 7183-7195
- **description:** `return_value::set_arg` (vmhook.hpp:7510-7522) rejects `index > max_jvm_locals (0xFFFF)` to prevent `locals[-index]` from walking off the interpreter local-variable array. The read counterparts have no such guard: `get_argument` at line 5170 does `locals[-index]` for any caller-supplied `std::int32_t` (including 0x10000+ or even `std::numeric_limits<std::int32_t>::min()` which makes `-index` overflow to a huge positive offset). `extract_frame_arg` at line 7195 has the same hole; today it is only reached with constant template-derived offsets, but the public signature accepts any `std::int32_t` and `frame::get_argument` is callable from user detour code. An out-of-bounds dereference here returns whatever lives on the operand stack / saved registers / adjacent thread-local storage — silently garbage rather than crashing the JVM, which is harder to diagnose than a clean failure. (Cross-reference: audit/findings/return_value_set_arg_max_locals_bound.md flagged the same gap in the read path.)
- **repro:** Inside a hook detour: `frame->get_argument<int>(0x100000);` reads `locals[-1048576]` — undefined memory access; on Windows this typically lands inside a guard page and AVs the JVM, but on Linux glibc-aligned arenas it can quietly return adjacent thread-state bytes.
- **suggested_fix:** Add the same `if (index < 0 || index > 0xFFFF) return base_t{};` guard to both `get_argument` and `extract_frame_arg`. Even better, hoist the bound into a private constexpr `vmhook::hotspot::frame::max_local_slot_index_v` constant and reference it from all three sites (read, read-detail, write).
- **confidence:** likely

### [MEDIUM] `frame::get_argument` silently returns `argument_type{}` for unsupported types — the bug `extract_frame_arg`'s `static_assert` was added to prevent
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:5198-5202
- **description:** `extract_frame_arg`'s `else` branch (line 7250-7257) intentionally produces a long, descriptive `static_assert` — the comment at line 7251-7257 explicitly calls out that "The previous behaviour silently returned a default-constructed value (all zeros) for any type larger than a pointer that wasn't std::string / unique_ptr<T> / a raw pointer - user hook callbacks saw a hidden zero arg with no compile-time or runtime warning." The sibling `frame::get_argument` at line 5198-5201 still has that exact silently-returns-default behaviour: `else { return argument_type{}; }`. Pass `std::array<int, 4>` or anything `sizeof > sizeof(void*)` and the detour quietly receives all-zeros with no diagnostic. The two implementations of the same operation have opposite UX about misuse.
- **repro:**
  ```cpp
  struct big_pod { std::int64_t a, b; };
  // Java method takes a single long arg (signature "(J)V").
  auto [val] = frame->get_arguments<big_pod>();  // val is zero-initialised
  ```
- **suggested_fix:** Mirror `extract_frame_arg`: replace `return argument_type{};` with `static_assert(vmhook::detail::dependent_false_v<argument_type>, "frame::get_argument: argument type must be primitive (sizeof <= sizeof(void*)), pointer, std::string, or std::unique_ptr<T>. Larger or non-trivially-copyable types are not representable in a single JVM interpreter local-variable slot.");`. Bonus: the `std::is_pointer_v` and primitive paths could both `static_assert(std::is_trivially_copyable_v<argument_type>)` for symmetry.
- **confidence:** certain

### [MEDIUM] `extract_frame_arg` and `frame::get_argument` duplicate the same OOP-decode heuristic with no shared helper
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:5172-5191, 7197-7210, 5084-5097
- **description:** Three distinct sites implement the same "if low 32 bits, decompress; else treat as direct pointer" logic for an OOP slot, all using the same magic constant `0xFFFFFFFFull`: (a) `frame::get_argument` pointer branch at line 5185-5188, (b) the descriptor-walking `frame::get_arguments` at line 5088-5096, (c) `extract_frame_arg`'s local `decode_oop` lambda at line 7206-7209. Any future change to the compressed/uncompressed discrimination (e.g. tightening the threshold to `narrow_oop_base + (UINT32_MAX << narrow_oop_shift)`) must be applied three times in lockstep or the read paths drift. The same flag is identified in audit/findings/return_value_set_arg_primitives.md for the write path (`set_arg::store_oop` reimplements the same heuristic at vmhook.hpp:7542-7563). Four sites of the same heuristic is a maintainability hazard, not just an aesthetic one — a JDK update that shifts compressed-OOP semantics will misfire in some paths but not others.
- **repro:** N/A — drift hazard, not a runtime bug today.
- **suggested_fix:** Extract `static auto vmhook::hotspot::decode_oop_slot(void* raw_slot) noexcept -> void*` that implements the heuristic once, and have all three read sites + `set_arg::store_oop` call it. Keep `decode_oop_pointer` (the pure-compressed decompressor) as-is.
- **confidence:** certain

### [LOW] `frame::get_argument` pointer-branch null check happens BEFORE the OOP-compress check, hiding a corner case
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:5172-5191
- **description:** `if (!raw_value) return nullptr;` at line 5174-5177 handles the case where the slot literally contains 0. But a compressed OOP value of zero (which `decode_oop_pointer(0)` returns nullptr for anyway, see vmhook.hpp:4226-4234) is also valid and would also need to return nullptr. In practice this isn't a bug because both paths converge on nullptr, but the early-return reads as "raw_value is the pointer" rather than "raw_value is a slot, possibly compressed". `extract_frame_arg`'s `decode_oop` lambda handles both cases uniformly with one early-return at line 7201-7204. Worth aligning to keep the mental model consistent across read paths.
- **repro:** N/A — paths converge on nullptr today.
- **suggested_fix:** Remove the `if (!raw_value) return nullptr;` shortcut from `get_argument`'s pointer branch and let the `decode_oop_pointer` (when shared with the helper from the previous bug) handle the zero case. Drop-in replacement once the shared helper exists.
- **confidence:** speculative

### [LOW] `frame::get_arguments()` (descriptor-walker, no template args) skips primitive args in the returned `method_args` vector — argument indices no longer match the user's mental model
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:5038-5140
- **description:** The descriptor walker at line 5070-5137 only pushes reference-type (`L...;`) arguments into `result.arguments` (line 5101, 5105). Primitive args (`I`, `J`, `D`, `F`, `B`, `S`, `Z`, `C`) and array types are *skipped* — `slot_index` is advanced correctly (line 5132-5136 has the J/D widening) but no entry is created. So for a signature `(ILjava/lang/String;J)V`, `args.size()` returns 1 (just the String) and `args.as<my_str>(0)` is the String. A user expecting "index N == argument N" (the same convention `vmhook::hook<T>` uses for callback parameters) would index the wrong way. The docstring at line 5025-5036 says "one entry per argument" then later "only reference-type args produce entries" — the two statements contradict. A user grepping for the first sentence and writing `args.as<T>(slot_for_first_arg)` would get nullptr or a wrong wrapper.
- **repro:** Java `void f(int x, java.lang.String s, long y, java.lang.Integer z)`. `auto args = frame->get_arguments(); args.size()` returns 2, `args.as<my_string>(0)` is `s`, `args.as<my_integer>(1)` is `z`. A user reading "0 = first arg" thinks index 0 is the int — they get the String. Worse, if they call `args.as<int_wrapper>(0)`, they construct an `int_wrapper` from a Java String OOP — type confusion.
- **suggested_fix:** Two-line fix: clarify the docstring to say explicitly "only reference-type args appear in the returned container; index N is the Nth *reference* argument, not the Nth method argument", and rename the method to `get_reference_arguments()` (with a deprecation alias for the old name) so the type-erased path can't be mistaken for the typed path. Alternatively, push *all* args into the vector including primitives (with `class_name` set to the JVM signature char like `"I"`) so indices match argument position 1:1.
- **confidence:** likely

## Improvements

### [S] [USER_FACING] Add `[[nodiscard]]` to `frame::get_argument` / `frame::get_arguments<...>()`
- **rationale:** Both return the decoded value; discarding it is almost always a bug (the only reason to call them is to use the result). Adding `[[nodiscard]]` catches "I forgot to capture it" mistakes at compile time. Parity with the proposed `[[nodiscard]] set_arg` from audit/findings/return_value_set_arg_primitives.md:122-129.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:5017-5022, 5160-5202
- **suggested_change:** Prefix both with `[[nodiscard]]`. No ABI impact, no source-break unless the user was deliberately discarding (in which case the warning is the point).

### [M] [USER_FACING] Promote `extract_frame_arg` to the public API and deprecate `frame::get_argument`
- **rationale:** The legacy `frame::get_argument` is the *un*-fixed version of the same operation that `extract_frame_arg` got right (J/D widening, `static_assert` on bad types, std::string support, unique_ptr factory dispatch). Today users have no way to invoke `extract_frame_arg` directly — it's in `vmhook::detail`. The README walkthrough recommends `frame->get_arguments<...>()` (the buggy path). Expose `vmhook::extract_arg<T>(frame, slot_offset)` as the supported entry point, mark `frame::get_argument` `[[deprecated("use vmhook::extract_arg<T> — frame::get_argument has no J/D slot widening")]]`, and update the README example.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:5159-5202, 7183-7259
- **suggested_change:** Add `template<typename T> [[nodiscard]] auto extract_arg(vmhook::hotspot::frame*, std::int32_t slot_index) noexcept -> std::remove_cvref_t<T> { return vmhook::detail::extract_frame_arg<T>(...); }` in the public `vmhook` namespace. Update inline docs at vmhook.hpp:10315 / 12676 / 12726 that today point users at `frame->get_arguments<...>()` to point at the new entry point.

### [S] [INTERNAL] Replace the magic `0xFFFFFFFFull` threshold with a named constant
- **rationale:** The "is this slot a compressed OOP?" threshold appears at vmhook.hpp:5089, 5185, 7207, and 7554 (set_arg). Each site re-writes the literal `0xFFFFFFFFull`. The threshold encodes a JVM-specific assumption (compressed OOP fits in 32 bits) — naming it makes the intent obvious and makes future tuning (e.g. wider compressed-OOP encodings) a one-liner.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:5089, 5185, 7207, 7554
- **suggested_change:** In `vmhook::hotspot`: `constexpr std::uintptr_t narrow_oop_slot_bits_max{ 0xFFFFFFFFull };`. Replace each `<= 0xFFFFFFFFull` with `<= vmhook::hotspot::narrow_oop_slot_bits_max`.

### [S] [USER_FACING] Diagnose primitive type/slot-width mismatch at compile time
- **rationale:** A user writing `auto [x] = frame->get_arguments<std::int32_t>()` against a Java `(D)V` method gets a half-double silently. The library knows the C++ tuple at compile time and could, given the Java signature string (already cached in `hooked_method::expected_signature`), check that each C++ type's slot width matches the Java type at hook-install time and fail loudly. This is a much stronger guarantee than the current runtime "you got garbage" failure mode.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:7820-7821 (hook install site), 7130-7134 (`is_java_double_slot_v`)
- **suggested_change:** In the `vmhook::hook<T>(...)` install path that already has both `method_arg_tuple_t` (the C++ tuple) and the method's Java signature string, walk both in parallel and `VMHOOK_LOG` (or `throw vmhook::exception`) on the first mismatch — `(J)V` vs `<std::int32_t>`, `(D)V` vs `<float>`, `(I)V` vs `<std::int64_t>`. The compile-time tuple is fixed; the signature string is known at install time; this check costs O(N) once at install and prevents silent type-punning at every hook fire.

### [XS] [INTERNAL] Hoist the `index++` pack expansion comment and rename `index` to `arg_index` in `frame::get_arguments`
- **rationale:** Line 5020-5021 reads `std::int32_t index{ 0 }; return std::tuple<types...>{ this->get_argument<types>(index++)... };` — the C++17 guarantee that braced-init-list elements evaluate left-to-right is load-bearing here (otherwise the fold would be ill-defined). A one-line comment plus renaming `index` to `arg_index` (since it IS used as an argument index — until the J/D fix is in) helps the next reader. After the J/D fix lands per the HIGH bug above, the variable goes away entirely.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:5019-5022
- **suggested_change:** Apply the J/D fix above, which removes the pack-expansion side-effect entirely. If the fix is deferred, add: `// C++17 P0145R3 guarantees left-to-right evaluation of braced-init-list elements,` `// so the index++ side-effect across the pack expansion is well-defined.` to the line above.

## Tests

### [standalone_unit] [extend_existing] test_frame_get_arguments_typed_widens_jd_slot
- **description:** Mirrors `test_traits.cpp:184-187`'s static_assert but exercises the runtime `frame::get_arguments<int, long, int>()` path. Construct a fake locals array of 6 `void*` entries (`locals_buf[5]` = arg0, `locals_buf[4]` = arg1, etc., reversed because of `locals[-index]`), seed slot 0 with `0x11111111`, slot 1 with `0x2222222233333333` (the long), slot 3 with `0x44444444` (note: slot 2 left as garbage to detect the bug). Cast to `vmhook::hotspot::frame*` (the fake), then call `frame->get_arguments<std::int32_t, std::int64_t, std::int32_t>()` and assert tuple = `(0x11111111, 0x2222222233333333, 0x44444444)`. With the current code the third element would equal slot 2's garbage; after the fix it equals slot 3.
- **asserts:** Returned tuple matches the seeded values across all three slots, including the slot-3 trailing int (not slot 2).
- **existing_file:** tests/test_helpers.cpp (new function alongside the existing `frame`/`locals` mocks; register in main runner)

### [standalone_unit] [new] test_frame_get_argument_rejects_out_of_range_index
- **description:** Sanity check the new `index < 0 || index > 0xFFFF` guard proposed under the MEDIUM bug above. Set up a valid fake frame, then call `frame->get_argument<int>(-1)`, `get_argument<int>(0x10000)`, `get_argument<int>(std::numeric_limits<std::int32_t>::min())`. Each must return `int{}` without dereferencing memory. Today (no guard) the calls would AV or return garbage; after the fix they return default-constructed.
- **asserts:** All three out-of-range calls return `int{ 0 }`; no crash.
- **existing_file:** tests/test_helpers.cpp (new)

### [standalone_unit] [new] test_extract_frame_arg_unsupported_type_static_assert
- **description:** Compile-fail test confirming that `extract_frame_arg<std::vector<int>>` triggers the `static_assert` at vmhook.hpp:7250-7257. Use a `requires { ... }`-expression in a SFINAE context or a manual compile-fail harness (CMake's `try_compile`) to assert that the static_assert message contains the expected substring.
- **asserts:** Compilation fails with the expected error message; no link-time fallthrough.
- **existing_file:** tests/test_traits.cpp (extend with a `static_assert(!requires { extract_frame_arg<std::vector<int>>(frame, 0); }, "must reject vector")` or similar — concept-based negative test).

### [standalone_unit] [new] test_frame_get_argument_default_construct_for_oversized_type
- **description:** Today, `frame::get_argument<big_pod>(0)` where `sizeof(big_pod) > sizeof(void*)` silently returns `big_pod{}`. The MEDIUM bug above proposes converting this to a `static_assert`. After the fix, this test should be a compile-fail negative test (mirror of the extract_frame_arg one). Before the fix, lock in current silent-default behaviour as a deprecation-pending test so anyone working on the area sees it.
- **asserts:** Either: silent default-construct returns zero-initialised `big_pod` (today), OR compilation fails with the expected static_assert (after fix).
- **existing_file:** tests/test_traits.cpp (new)

### [jvm_integration] [new] test_arg_decoding_long_followed_by_int
- **description:** Add a Java probe method `int nonStaticLongIntDecodeMe(long a, int b)` whose body writes `b` to a probe field and returns `a + b`. Hook with a typed callback `(self, std::int64_t a, std::int32_t b)`. Inside the hook, assert `a == EXPECTED_LONG` and `b == EXPECTED_INT`. The hook arg-decoder used by `vmhook::hook<T>` should already be correct (uses `java_slot_offsets`). To prove the *legacy* `frame::get_arguments<...>()` is broken, do a SECOND assertion inside the hook: `auto [a2, b2] = frame->get_arguments<std::int64_t, std::int32_t>(); ASSERT(b2 == EXPECTED_INT);` — this fails today, passes after the HIGH fix.
- **asserts:** `a == EXPECTED_LONG` (typed-hook path); `b == EXPECTED_INT` (typed-hook path); `b2 == EXPECTED_INT` (legacy frame->get_arguments path, post-fix).
- **existing_file:** vmhook/src/example.cpp (alongside `test_arg_mutation` at line 2288)

### [jvm_integration] [new] test_arg_decoding_double_in_signature
- **description:** Java `double nonStaticDoubleDecodeMe(double a, double b, int c)`. Hook with `(self, double a, double b, std::int32_t c)`. Assert all three arrive correctly. The c-slot at position 5 (after two doubles at slots 1-2 and 3-4 on instance method) is the regression target — `java_slot_offsets<std::tuple<oop, double, double, int>>` should equal `{0, 1, 3, 5}`.
- **asserts:** `a` and `b` are bit-exact (compare `std::bit_cast<std::uint64_t>(a)`); `c` matches the expected int.
- **existing_file:** vmhook/src/example.cpp (new test alongside `test_arg_mutation`)

### [jvm_integration] [new] test_arg_decoding_all_primitive_types_round_trip
- **description:** Cover boundary primitives that the existing tests don't: `boolean`, `byte`, `char`, `short`, `float`. Java method `void allPrimitivesDecodeMe(boolean a, byte b, char c, short d, int e, long f, float g, double h)`, hook with the matching C++ types, assert each arrival. Specifically exercises `extract_frame_arg<bool>`, `<std::int8_t>`, `<char16_t>` (or unsigned short), `<std::int16_t>`, `<float>` — all of which should round-trip through the `sizeof <= sizeof(void*)` memcpy branch at vmhook.hpp:7242-7246.
- **asserts:** Each primitive arrives bit-exact; signed types preserve sign (e.g. `-1` byte → `int8_t{-1}` not `255`); `float` bit pattern preserved via `std::bit_cast`.
- **existing_file:** vmhook/src/example.cpp (new)

### [jvm_integration] [new] test_arg_decoding_static_vs_instance_method_slot_0
- **description:** For instance methods, slot 0 is `this`. For static methods, slot 0 is the first explicit arg. Verify both: hook one instance method `void instanceMethodSlot0(int x)` with `(self, std::int32_t x)` — `self` reads slot 0, `x` reads slot 1. Hook one static method `static void staticMethodSlot0(int x)` with `(std::int32_t x)` — `x` reads slot 0. Both must succeed and produce the right values.
- **asserts:** Instance hook: `self != nullptr`, `x == EXPECTED`; static hook: `x == EXPECTED`. Ensures the slot-0 dispatch differs correctly between the two cases.
- **existing_file:** vmhook/src/example.cpp (new)

### [standalone_unit] [extend_existing] test_java_slot_offsets_descriptor_round_trip
- **description:** The compile-time `java_slot_offsets<std::tuple<...>>::value` table and the runtime descriptor walker in `frame::get_arguments()` (line 5070-5137) implement the same J/D 2-slot rule via different mechanisms. Add a parameterised test that walks a list of representative signatures (`(IJI)V`, `(DI[J)V`, `(JJD)V`, `(I)V`, `(JD)V`, `()V`, `(Ljava/lang/String;ID)V`) and asserts that the runtime walker's per-arg slot positions match what `java_slot_offsets` would produce for the corresponding C++ tuple.
- **asserts:** For each signature, the slot positions produced by the descriptor walker equal the offsets in `java_slot_offsets<corresponding_tuple>::value`.
- **existing_file:** tests/test_traits.cpp (extend the existing `java_slot_offsets` block at line 173-205)

## Parity Concerns
- Two argument-decoding paths exist (`frame::get_argument` / `frame::get_arguments<types...>` vs `detail::extract_frame_arg` via `java_slot_offsets`) that should be one. The newer path is correct; the legacy path is buggy *and* still referenced from inline docs at vmhook.hpp:10315, 12676, 12726 — pointing users at the broken API.
- `return_value::set_arg` has an `index > max_jvm_locals` bound; `frame::get_argument` and `extract_frame_arg` do not. Read and write sides of the same `locals[-index]` access should share one bound check, ideally a single helper.
- The `0xFFFFFFFFull` "is this a compressed OOP slot?" threshold is repeated in five places (vmhook.hpp:5089, 5185, 7207, 7554, and conceptually in `decode_oop_pointer` itself at vmhook.hpp:4226-4234). Any future JDK change to compressed-OOP semantics needs all five sites updated in lockstep.
- The `static_assert(dependent_false_v<T>, "...")` pattern used in `extract_frame_arg` for unsupported types is *not* used in `frame::get_argument` (which silently returns `T{}`) or `return_value::set_arg` (which logs at runtime via `VMHOOK_LOG` per audit/findings/return_value_set_arg_primitives.md:117-120). All three should agree on compile-time failure as the canonical UX for "unsupported type".
- The descriptor-walker `frame::get_arguments()` correctly advances `slot_index` by 2 for `J`/`D` (line 5132-5136) but the typed wrapper `frame::get_arguments<types...>()` does not. The descriptor walker proves the library *knows* the rule; the typed wrapper just forgot to apply it.
