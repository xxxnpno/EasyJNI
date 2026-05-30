# return_value_set_arg_primitives

## Summary
`return_value::set_arg(index, primitive)` treats `index` as a raw **slot** index into the interpreter local-variable array (`locals[-index]`), with no awareness that Java `long`/`double` consume **two** slots. The read path (`extract_frame_arg` driven by `java_slot_offsets`) already knows this and feeds the callback the right slot index, but `set_arg` forces the user to compute the slot index themselves. The documentation calls `index` an "argument index" / "local-variable slot" interchangeably, so any user copying the read-path convention (tuple index) will silently mis-target the slot whenever a `long`/`double` precedes the arg being mutated.

## Bugs

### [HIGH] No J/D 2-slot awareness — slot index vs argument index ambiguity silently corrupts adjacent locals
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:7510-7638 (impl), 1211-1231 (doc), 7115-7175 (read-path counterpart `java_slot_offsets`)
- **description:** The hook dispatcher computes `slot_offsets` via `vmhook::detail::java_slot_offsets<method_arg_tuple_t>` (vmhook.hpp:7817-7821) so the callback receives `extract_frame_arg<T>(frame, slot_offsets[i])` — the *slot index* with `long`/`double` widening accounted for. `return_value::set_arg` (vmhook.hpp:7510) has no equivalent: it does `locals[-index]` (line 7545, 7550, 7556, 7561, 7628) treating the parameter as a raw slot index. The public-facing docstring at vmhook.hpp:1211-1228 says "index 0 is 'this' for instance methods; index 0 is the first argument for static methods" — wording that strongly implies an *argument* index, but the implementation requires the *slot* index. As soon as a method signature contains a `long`/`double` before the target arg, a user who reads the docs and passes argument index N will write to slot N (which is the high half of the preceding `long`/`double`, or the wrong arg entirely). On methods like `void f(long a, int b)`, calling `set_arg(1, 42)` from the user's mental model overwrites slot 1 — the high 32 bits of `a` — and the original `b` (at slot 2) survives. Worse, on `void f(int a, long b, int c)` the user would pass `2` to update `c`, but `c` actually lives at slot 3, and slot 2 is the high half of `b`. The corruption is silent.
- **repro:**
  ```cpp
  // Java: void target(long a, int b) { /* read b */ }
  vmhook::hook<my_class>("target",
      [](vmhook::return_value& ret,
         const std::unique_ptr<my_class>& self,
         std::int64_t a,
         std::int32_t b) {
          // Intent: replace b. b is the SECOND explicit arg (argument index 2
          // including 'this'), but it lives at SLOT 3 because 'a' takes 2 slots.
          ret.set_arg(2, std::int32_t{ 99 });   // writes slot 2 = high half of 'a'
          // Original method then reads b from slot 3 unchanged, and reads 'a' as garbage.
      });
  ```
- **suggested_fix:** Two complementary fixes:
  1. (Doc) Either change the docstring at vmhook.hpp:1211-1228 to spell out "slot index, where each Java `long`/`double` consumes two slots; pass `slot_index = arg_index + (number of preceding long/double args)`", with the README example (README.md:378) updated to reflect it.
  2. (API parity) Add a typed overload `set_arg<T>(index, value)` that uses `java_slot_offsets` against the *currently-hooked method's signature* (already cached in `hooked_method::expected_signature`) so the user can pass argument-index 1, 2, ... and let the library translate. Even simpler: expose a sibling `set_arg_by_slot(slot, value)` (literal slot semantics) plus a new `set_arg_by_argument(arg_index, value)` that parses the descriptor and maps to slot, mirroring the read path. The current name `set_arg` is the ambiguous one — it should match whichever convention `vmhook::hook` uses for callback arguments (which is *argument index*, after `java_slot_offsets` mapping).
- **confidence:** certain

### [MEDIUM] `std::is_trivially_copyable_v<clean_value_type>` accepts `bool` / `char` / `short` / `float` and writes a non-sign-extended value — Java `int`/`long` interpreter expects sign-extension
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:7624-7630
- **description:** The primitive branch does `void* raw{}; std::memcpy(&raw, &value, sizeof(clean_value_type)); locals[-index] = raw;` — only the low N bytes of the slot get the value, the upper bits remain zero. Compare with `return_value::set()` at vmhook.hpp:1159-1175 which explicitly sign-extends signed integers narrower than `int64_t`. For Java types: `int`/`short`/`byte` slot values are passed and read as 32-bit signed in HotSpot's interpreter; the high 32 bits of a Java `int` slot are typically zero (since slots are 64-bit machine words but `iload` only reads 32 bits). For Java `boolean`/`byte`/`short`/`char` the existing 32-bit-zero behaviour is harmless. But for someone passing `std::int8_t{-1}` expecting the Java side to read `-1` after `iload`: the slot now holds `0x00000000000000FF` and `iload` correctly returns `-1` (because `iload` reads 32 bits and `0xFFFFFFFF` would be the byte interpreted as `int -1`, but `0x000000FF` is `int 255`). This is exactly the bug `return_value::set` documents and fixes — and `set_arg` re-introduces it for `std::int8_t`/`std::int16_t` arguments. For 8-byte primitives (`std::int64_t`, `double`), the J/D bug above bites first; for narrow signed ones, sign-extension is needed too.
- **repro:**
  ```cpp
  // Java: int target(byte b) { return b; }   // 'b' is at slot 1 (assume static)
  vmhook::hook<X>("target", [](vmhook::return_value& ret, std::int8_t b) {
      ret.set_arg(0, std::int8_t{ -1 });
      // Slot 0 now = 0x00000000000000FF.  Java reads b via iload → int 255,
      // not -1.  return value to Java caller: 255.
  });
  ```
- **suggested_fix:** Apply the same sign-extension as `return_value::set` (vmhook.hpp:1159-1175) inside the primitive branch:
  ```cpp
  else if constexpr (std::is_trivially_copyable_v<clean_value_type>
                  && sizeof(clean_value_type) <= sizeof(void*))
  {
      std::uintptr_t raw{ 0 };
      if constexpr (std::is_signed_v<clean_value_type>
                 && std::is_integral_v<clean_value_type>
                 && sizeof(clean_value_type) < sizeof(void*))
      {
          raw = static_cast<std::uintptr_t>(static_cast<std::int64_t>(value));
      }
      else
      {
          std::memcpy(&raw, &value, sizeof(clean_value_type));
      }
      locals[-index] = reinterpret_cast<void*>(raw);
      return true;
  }
  ```
- **confidence:** likely

### [MEDIUM] `long`/`double` written to a single slot — high slot retains stale bits and `lload`/`dload` reads a half-stale value
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:7624-7630
- **description:** Even if the user correctly passes the slot index of the *low* slot of a Java `long`/`double`, the primitive branch only writes `locals[-index]`. HotSpot's `lload N` reads `slot[N]` as the low 32 bits and `slot[N+1]` as the high 32 bits on x64 (the interpreter packs J/D across two adjacent locals; the exact ordering depends on `_local_words_per_jlong` — JDK 8-25 on x64 uses 2 words per long with the high word at `slot[N]` and low at `slot[N+1]` per the historical "first" half convention, or vice-versa). The single write at line 7628 leaves the other half holding whatever was there before — possibly the prior method's spill — so the resulting Java `long`/`double` is a mix of the new value and the stale half. `set_arg<std::int64_t>(slot, my_long)` thus cannot be relied on.
- **repro:** Hook a Java method `void f(long x)` (signature `(J)V`, instance method → x at slot 1+2), call `ret.set_arg(1, std::int64_t{ 0x0123456789ABCDEFLL })` from inside the hook, then have the method `return x;` — value observed by the caller will be partially clobbered because slot 2 was not updated.
- **suggested_fix:** In the primitive branch, when `sizeof(clean_value_type) == 8` and the slot is part of a `long`/`double`, split the write across `locals[-index]` and `locals[-(index+1)]` with the JVM's canonical ordering. Better: detect Java `long`/`double` at compile time (`vmhook::detail::is_java_double_slot_v<clean_value_type>`) and write both halves explicitly:
  ```cpp
  if constexpr (vmhook::detail::is_java_double_slot_v<clean_value_type>)
  {
      std::uint64_t bits{};
      std::memcpy(&bits, &value, sizeof(clean_value_type));
      // HotSpot x64 layout: high half lives at locals[-index], low half at locals[-(index+1)]
      // (the J/D occupies two slots; the interpreter convention matches the existing
      // extract_frame_arg single-word reinterpret).
      locals[-index]       = reinterpret_cast<void*>(static_cast<std::uintptr_t>(bits >> 32));
      locals[-(index + 1)] = reinterpret_cast<void*>(static_cast<std::uintptr_t>(bits & 0xFFFFFFFFull));
      return true;
  }
  ```
  Validate the exact half ordering against `frame::get_arguments<std::int64_t>` round-tripping on a live JVM (test case below).
- **confidence:** likely (the single-slot write is certain; the exact half-ordering needs JVM verification)

### [LOW] `index == max_jvm_locals (0xFFFF)` and J/D writes overrun by one slot
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:7521-7522
- **description:** The guard accepts `index == 0xFFFF`. If the proposed J/D 2-slot fix above writes `locals[-(index+1)]`, that becomes `locals[-0x10000]` — one past the JVM-spec max. Either tighten the guard to `index >= max_jvm_locals` for J/D writes, or compute the high-slot index after dispatch and re-check. Today this is benign because `set_arg` only writes one slot, but it becomes load-bearing once J/D split-writes land.
- **repro:** Cannot be exercised today without J/D split write; documented for the same fix landing.
- **suggested_fix:** Change to `if (index < 0 || index > max_jvm_locals - (is_java_double_slot_v<T> ? 1 : 0))` once J/D split-write is implemented, with the corresponding log message clarification.
- **confidence:** speculative

## Improvements

### [S] [USER_FACING] Documentation should disambiguate slot index vs argument index, mention J/D
- **rationale:** The current docstring at vmhook.hpp:1211-1228 says "index 0 is 'this'", which sounds like an argument index. The implementation uses it as a slot index. README.md:378 example happens to work because no J/D precedes the target arg. Users hitting J/D-bearing methods will quietly mis-target slots without any compile or runtime indication.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:1211-1228, README.md:368-385
- **suggested_change:** Add a paragraph: "When the Java method signature contains `long` or `double` parameters, each consumes **two** local-variable slots. Pass `slot_index = arg_position + count_of_preceding_long_or_double_args`. For example, in `void f(long a, int b)`, `b` lives at slot 3 (not slot 2): `set_arg(3, ...)`." Show the case in the README and in a new doctest under tests/.

### [M] [USER_FACING] Provide a `set_arg_by_argument` overload that resolves slot via the live method signature
- **rationale:** The library already parses the descriptor (vmhook.hpp:5049-5137 in `frame::get_arguments`) and the hook install path knows the full signature (`hooked_method::expected_signature` set at vmhook.hpp:7839). Expose a sibling overload that lets the user pass argument index (1 = first explicit arg for instance methods, mirroring the callback parameter order) and have the library compute the slot. This removes the entire class of J/D miscount bugs from the user-facing surface.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:7510-7638
- **suggested_change:** Add (and document):
  ```cpp
  // Pass the same index the callback parameter has (1 for 'self' on instance
  // methods + N for the Nth Java arg).  Slot is computed from the live signature
  // of the hooked method, so long/double counting is automatic.
  template<typename value_type>
  auto set_arg_by_argument(std::int32_t arg_index, value_type&& value) noexcept -> bool;
  ```
  Implementation: walk the `current_method->get_signature()` descriptor, counting one slot per non-J/D parameter and two per J/D, and dispatch into the existing `set_arg(slot_index, value)`.

### [S] [INTERNAL] Reuse `extract_frame_arg`'s `decode_oop`/`encode_oop` helper rather than redefining locally
- **rationale:** `set_arg`'s `store_oop` lambda (vmhook.hpp:7542-7563) re-implements the compressed/decompressed heuristic that `extract_frame_arg`'s local `decode_oop` lambda (vmhook.hpp:7198-7210) also implements. The boundary value `> 0xFFFFFFFFull` is duplicated. Promote both to a shared helper in `vmhook::hotspot` (or `vmhook::detail`) so the two paths cannot drift.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:7542-7563, 7198-7210, 5085-5097
- **suggested_change:** Introduce `vmhook::hotspot::is_compressed_oop_value(uintptr_t)` and `vmhook::hotspot::pack_oop_into_slot(void*) -> void*` helpers that encapsulate the magic constant; replace the three sites.

### [S] [INTERNAL] `typeid(clean_value_type).name()` in the unsupported-type log is implementation-defined and unreadable on GCC/Clang
- **rationale:** vmhook.hpp:7635 logs `typeid(clean_value_type).name()` which on GCC/Clang is the raw mangled name (e.g. `St6vectorIiSaIiEE`). The existing `static_assert` in `extract_frame_arg` (vmhook.hpp:7250-7257) is a much friendlier compile-time alternative for the unsupported case — use `static_assert(dependent_false_v<clean_value_type>, "...")` in the unreachable `else` branch so the bad type is caught at compile time with a clear message.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:7631-7637
- **suggested_change:** Replace the runtime `VMHOOK_LOG` + `return false` with a `static_assert(vmhook::detail::dependent_false_v<clean_value_type>, ...)`. Today, no path can hit this branch because all `is_trivially_copyable_v && sizeof <= 8` cases match the prior branch — so the only way to reach it is with a non-trivially-copyable type or one larger than a pointer, both of which should be diagnosed at compile time, not at hook fire time.

### [S] [USER_FACING] Make `set_arg(...)` `[[nodiscard]]` 
- **rationale:** The return value `bool` carries critical failure information (frame null, bad slot, JNI string allocation failed). The README example at README.md:378 ignores it. Adding `[[nodiscard]]` to the declaration at vmhook.hpp:1230 would prompt users to handle the failure path explicitly.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:1229-1231
- **suggested_change:**
  ```cpp
  template<typename value_type>
  [[nodiscard]] auto set_arg(std::int32_t index, value_type&& value) noexcept -> bool;
  ```

## Tests

### [standalone_unit] [extend_existing] test_return_value_set_arg_guards_negative_one_passes_check
- **description:** Current guard test (`test_return_value_set_arg_guards`) covers nullptr frame, negative index, and index past `max_jvm_locals`. Extend to also cover: `index = max_jvm_locals` (boundary), `index = max_jvm_locals + 1` (just past, ensure rejected), `index = std::numeric_limits<std::int32_t>::min()` (deep negative).
- **asserts:** All return false; no crash. Boundary value 0xFFFF is accepted (current code returns false because frame is null, but the guard itself does not reject 0xFFFF).
- **existing_file:** tests/test_helpers.cpp (extend `test_return_value_set_arg_guards` at lines 907-931)

### [standalone_unit] [new] test_return_value_set_arg_primitive_sign_extension
- **description:** Mirrors `test_return_value_sign_extension` for `set()`. Construct a fake stack frame where `get_locals()` returns a pointer to a small `void*` array we own; call `set_arg<std::int8_t>(0, -1)`, then read back the slot and verify it equals `static_cast<std::int64_t>(-1)` (i.e., `0xFFFFFFFFFFFFFFFF`) after sign-extension. Same for `std::int16_t{-1}` and `std::int32_t{-1}`. Demonstrates the bug today (the byte slot would be `0x00000000000000FF`) and locks the fix in.
- **asserts:** Slot value after `set_arg<int8_t>(0, -1)` equals -1 sign-extended; same for int16_t and int32_t; unsigned types remain zero-extended.
- **existing_file:** new function in tests/test_helpers.cpp (add `test_return_value_set_arg_primitive_sign_extension`, register in main runner)

### [standalone_unit] [new] test_return_value_set_arg_jd_writes_both_slots
- **description:** After J/D-aware fix lands. Construct a fake locals array; call `set_arg<std::int64_t>(1, 0x0123456789ABCDEFLL)`; read back both `locals[-1]` and `locals[-2]` and verify the 64-bit value reconstructs (in the documented JVM half-ordering). Mirror for `double` with a known bit pattern (e.g., 3.14159) and reinterpret via `std::bit_cast<std::uint64_t>(d)`.
- **asserts:** Reconstruction matches; no other slots are touched.
- **existing_file:** new in tests/test_helpers.cpp

### [jvm_integration] [extend_existing] test_arg_mutation_long_followed_by_int_uses_correct_slot
- **description:** Existing `test_arg_mutation` (vmhook/src/example.cpp:2288-2326) covers `int` mutation only. Add a probe method `void nonStaticLongIntArgMutationMe(long a, int b)` whose body writes `b` into a probe field. Inside the hook, mutate `b` via `set_arg(?, 42)` — first with the *argument* convention (the user's likely mental model), then via the slot convention with `set_arg(3, 42)` — and verify only the latter actually changes `b`. This is the primary J/D regression test.
- **asserts:** With slot-convention `set_arg(3, 42)`, probe field == 42. With (broken) arg-convention `set_arg(2, 42)`, probe field == original `b` (7) and `a` is now corrupted in its high half.
- **existing_file:** vmhook/src/example.cpp (add probe field + method + test alongside `test_arg_mutation`)

### [jvm_integration] [new] test_arg_mutation_double_argument_round_trip
- **description:** Hook a Java method `double doubleArgMutationMe(double in) { return in * 2; }` and inside the hook call `set_arg<double>(<slot>, 12.5)`. Verify the original method returns `25.0`. With the current single-slot-write bug, the result will be partially garbled.
- **asserts:** Java returns 25.0 exactly; no NaN; no infinity.
- **existing_file:** vmhook/src/example.cpp (new test method alongside `test_arg_mutation`)

### [standalone_unit] [new] test_return_value_set_arg_unsupported_type_static_assert
- **description:** After the `else`-branch is converted to `static_assert`, add a SFINAE / `requires` check that the compile-time error fires for, e.g., a `std::vector<int>` value. This is a compile-fail test (could be guarded behind a `VMHOOK_TEST_COMPILE_FAILURES` macro and use `static_assert` in a non-instantiated helper).
- **asserts:** Compile-time message contains the expected diagnostic.
- **existing_file:** new in tests/test_helpers.cpp

## Parity Concerns
- Read path (`extract_frame_arg`) accepts an *argument-index* from the user-facing `vmhook::hook<T>` callback signature and internally translates to slot index via `java_slot_offsets<arg_tuple>`. Write path (`return_value::set_arg`) requires the *slot index* directly. The two halves of the API speak different languages about the same locals array — the user's mental model from the callback parameters (which is "argument 0, argument 1, ...") does not transfer to `set_arg`.
- `return_value::set` (vmhook.hpp:1147-1176) explicitly sign-extends narrow signed integers; `return_value::set_arg` does not. The two write paths should agree on the slot-bit conventions they apply.
- `frame::get_arguments()` (vmhook.hpp:5038-5141) correctly tracks the J/D 2-slot increment when parsing a descriptor; the same descriptor-walking logic is conspicuously absent from `set_arg`. A shared private helper (e.g. `vmhook::detail::compute_slot_for_arg(const std::string& descriptor, std::int32_t arg_index, bool is_static)`) would let both share the same J/D accounting.
- The string and object-pointer branches of `set_arg` correctly delete the JNI local reference (vmhook.hpp:7601, 7621) after extracting the underlying OOP — this is the v0.4.1 fix called out in CHANGELOG.md:88-94. The primitive branch has no analogous concern, but the inconsistency with `frame::get_arguments` (which uses raw slot indices verbatim) and `extract_frame_arg` (which uses slot-via-`java_slot_offsets`) is worth a single unified slot-resolution helper to keep all three call sites honest.
