# field_proxy_array_primitives

## Summary
Audited the round-trip read/write path for Java primitive arrays via `field_proxy` — the `value_t::operator std::vector<T>()` conversion path (declarations at vmhook.hpp:1496-1504, dispatch at 10830-10921 and 11172-11186, definitions at 14638-14699). The path is functional but has three real concerns: (1) a hard `noexcept`-vs-`reserve` violation that turns a corrupted `_length` into `std::terminate`, (2) zero signature/width validation on read so a wrong `std::vector<T>` instantiation walks past the heap object end and quietly returns garbage, and (3) silent no-op writes (no log) when the OOP is null, which diverges from `field_proxy::set`'s loud size-mismatch guard added in v0.4.4.

## Bugs

### [high] `read_array_value` violates `noexcept` on bogus `_length`
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:10898-10922 (and the noexcept call site at 10898 reached from 10961-10970 and 11026-11033)
- **description:** `read_array_value` calls `result.reserve(static_cast<std::size_t>(length))` after only checking `length <= 0`. The full conversion chain — `field_proxy::get()` -> `value_t::operator target_type() const noexcept` -> `std::visit([](auto v) noexcept ...)` -> `cast_for_variant` -> `read_array_value` — is declared `noexcept`. If the array OOP is partially corrupted (or simply points to torn / racing data while the GC is moving objects) the `_length` field at +12 can hold any 32-bit value. A value like `0x40000001` produces `reserve(1073741825)`. For `std::vector<std::int64_t>` that is ~8 GiB; `std::bad_alloc` is then thrown out of a `noexcept` function, which calls `std::terminate` and kills the entire Java process. Same hazard for any moderately corrupted `_length` (or a legitimately huge Java array that doesn't fit in C++ address space).
- **repro:** Read a freshly-allocated primitive array whose `_length` slot has been smashed by a racing GC relocation, OR craft a unit test that simulates an array OOP with `_length = 0x7FFFFFFF` and call `field_proxy::get().operator std::vector<std::int64_t>()`. The harness will abort instead of returning an empty vector.
- **suggested_fix:** Either drop `noexcept` from `read_array_value` and wrap the call site in try/catch returning `{}`, OR (preferred for a `noexcept`-everywhere API) clamp `length` with a sanity ceiling consistent with the rest of the file — `vmhook::hotspot::is_valid_pointer` already trusts the OOP base; pick a per-field-width ceiling (e.g. `length * sizeof(element) <= 64 MiB` matching the spirit of the 4096-char `make_java_string` cap, or use the InstanceKlass-style 4096 thread guard from line 6612). On overflow, `VMHOOK_LOG` once and return `{}`.
- **confidence:** certain

### [high] Silent OOB read when target `vector<T>` element type mismatches the JVM array element width
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:10869-10881 (generic `append_array_value`) and 10898-10922 (`read_array_value`)
- **description:** `read_array_value` ignores the `signature` parameter for every numeric type — it only forwards it to the `vector<char>` overload to special-case `[C`. There is no check that `sizeof(element_type) == jvm_primitive_byte_width(signature.substr(1))`. So `field_proxy{ ..., "[I", ... }.get().operator std::vector<std::int64_t>()` calls `get_array_element<int64_t>` on a `[I` (4-byte stride) array. The bounds check inside `get_array_element` (line 10724) gates only on `index >= length` — it does not gate on `index * sizeof(element_type) >= length * jvm_element_width`. The N-th read therefore reaches byte offset `16 + N*8` and walks straight past the array's data area into the next heap object's header (or unmapped memory if the array is the last allocation in its TLAB). This is the read counterpart to the v0.4.4 `field_proxy::set` size-mismatch guard at lines 11236-11246, which already documents this exact class of bug for scalar fields. The array path was not given the same protection.
- **repro:** In `vmhook/src/example.cpp`, add a wrapper that returns `static_field("staticIntArray")->get().operator std::vector<std::int64_t>()`. Three int32s `{4096, 8192, 12288}` will be read as two int64s, the second containing arbitrary bytes from the next heap object. On a fragmented heap this will eventually crash; on a clean heap it just silently returns wrong values.
- **suggested_fix:** Add a one-line check in `read_array_value` before the loop:
  ```
  const std::size_t expected_width{ vmhook::detail::jvm_primitive_byte_width(signature.size() >= 2 ? signature.substr(1) : signature) };
  if (expected_width != 0 && expected_width != sizeof(element_type) && !std::is_same_v<element_type, bool> && !(signature == "[C" && std::is_same_v<element_type, char>)) {
      VMHOOK_LOG("{} field_proxy::value_t::to_vector: element width mismatch (cpp={}B, jvm={}B, sig='{}') — returning empty vector to avoid OOB read.", vmhook::error_tag, sizeof(element_type), expected_width, signature);
      return result;
  }
  ```
  The `[C` + `char` and `[Z` + `bool` cases already round-trip through 1 byte each, so guard those as special-cases (the existing append overloads already handle the data conversion correctly).
- **confidence:** certain

### [medium] `set_prim_array` / `set_bool_array` silently no-op when the array OOP is null with no log
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:14638-14652 (`set_bool_array`), 14669-14699 (`set_prim_array`), 14716-14731 (`set_str_array`)
- **description:** All three setters check `if (!array_oop) return;` with no `VMHOOK_LOG`. The same code path in `field_proxy::set` for scalar primitives (line 11241) and the static-assert at 11252-11258 took a deliberate v0.4.4 stance that silent no-op writes are user-hostile. The array setters were not converted. A user who writes `obj.get_field("possiblyNullArray")->set(std::vector<int>{1,2,3})` against a field whose Java value is `null` gets zero diagnostics — the call returns and the values vanish. (This is a Bug rather than an Improvement because the v0.4.4 commit message and the set-side static_assert explicitly position silent no-op writes as bugs.)
- **repro:** In Example.java, add `public static int[] nullIntArray = null;` and a setter wrapper. Calling `set_static_null_int_array({1,2,3})` returns with no log line. Java still sees `null`.
- **suggested_fix:** Log when `array_oop` is null. Distinguish "field's compressed OOP is 0" (Java `null`) from "decode failed" (invalid OOP), since the two have different user-recovery actions. Example:
  ```
  if (!array_oop) {
      VMHOOK_LOG("{} vmhook::set_prim_array: backing Java array is null (sig='{}') - cannot resize from C++; allocate via make_java_array() and assign the field first.",
                 vmhook::error_tag, field.signature());
      return;
  }
  ```
- **confidence:** likely

### [low] `set_prim_array` size-mismatch is also a silent truncation
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:14679 (and 14647, 14725 for the sibling setters)
- **description:** All three setters compute `length = min(array_length, values.size())` and silently write fewer elements than the user passed (or fewer than the array holds). The v0.4.4 commit changed scalar `field_proxy::set` to refuse on size mismatch precisely because silent partial writes are user-hostile. Array setters intentionally cannot resize (zero-JNI), but they should at least log when the user vector and Java array length disagree so a `std::vector<int>{1,2,3,4,5}` passed against a `int[3]` field doesn't silently drop two elements.
- **repro:** Pass a 5-element `std::vector<int32_t>` to `set_static_int_array`. The Java array still holds `{1,2,3}` from the original three slots, with no warning.
- **suggested_fix:** Add a one-line `VMHOOK_LOG(vmhook::warning_tag, ...)` when `values.size() != static_cast<size_t>(array_length(array_oop))`. Cheaper than rejecting the write since the zero-JNI layer truly cannot reallocate.
- **confidence:** likely

## Improvements

### [S] [INTERNAL] Hoist `array_length` out of the per-element loop in `read_array_value`
- **rationale:** `read_array_value` already calls `array_length(array_oop)` once at line 10909, then `append_array_value` re-enters `get_array_element` which calls `array_length` again on every element (line 10723) and re-runs `is_valid_pointer` on the OOP. For a 256-element `largeIntArray` that's 257 extra `is_valid_pointer` calls and 257 extra reads of the `_length` slot.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:10714-10732, 10898-10922
- **suggested_change:** Add an unchecked sibling `get_array_element_unchecked<T>(array_oop, index)` that skips the per-element bounds check (the outer loop already enforces `index < length`). Or inline the memcpy directly in `read_array_value` once we have `array_oop`, `length`, and `signature`.

### [S] [USER_FACING] Forward `set_bool_array` from `set` template even when the user passes `std::vector<std::uint8_t>` for a `[Z` field
- **rationale:** The set dispatcher at line 11174 picks `set_bool_array` only when `clean_value_type` is exactly `std::vector<bool>`. A user who has a `std::vector<std::uint8_t>` of 0/1 values for a `[Z` field gets routed to `set_prim_array<uint8_t>`, which writes raw bytes. That happens to work because the JVM stores boolean as uint8 anyway, but the asymmetry is undocumented and trips users coming from JNI (where `jbooleanArray` exposes `jboolean*` = `uint8_t*`).
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:11172-11186
- **suggested_change:** When `field.signature() == "[Z"`, route any `std::vector<integral>` through `set_bool_array` (normalising non-zero to 1) instead of `set_prim_array`. At minimum add a doc comment on `set_prim_array` clarifying that for `[Z` the elements are stored as bytes and any non-zero value is "true" in Java.

### [M] [USER_FACING] Add `field_proxy::is_array()`, `array_length()`, and `array_element_signature()` accessors
- **rationale:** Users currently have to write `field.signature()` and substring-check `[` to distinguish array from scalar fields, and there is no documented way to ask "how many elements does this array hold without reading them all?". Sibling `method_proxy` has a slightly richer API (return signature inspection); adding three one-liners closes the parity gap. Particularly useful for the "very large array" case — the user can decide not to allocate a `vector<int64_t>` of 256MB elements without first reading the field.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:11315-11319 (private state already has `signature_text`; add public accessors near 11265-11297)
- **suggested_change:** Add:
  ```
  auto is_array() const noexcept -> bool { return !signature_text.empty() && signature_text.front() == '['; }
  auto array_element_signature() const noexcept -> std::string_view { return is_array() ? std::string_view{signature_text}.substr(1) : std::string_view{}; }
  auto array_length() const noexcept -> std::int32_t { return is_array() ? vmhook::array_length(vmhook::field_oop(*this)) : 0; }
  ```

### [XS] [USER_FACING] Doc-clarify that `to_vector<T>` shadows `operator vector<T>`
- **rationale:** `value_t::to_vector<element_type>()` (lines 11044-11046, 14385-14400) is the OOP-array-of-wrappers path — it does NOT call `read_array_value`. The implicit `operator std::vector<T>()` is the primitive-array path. The naming overlap (`to_vector` is for object arrays via `collection::to_vector`; primitives go through implicit conversion) is genuinely confusing — users guessing at `get().to_vector<int>()` would compile to a `vector<unique_ptr<int>>` and a runtime "not a collection" log. There is no compile-time diagnostic preventing the wrong-use case.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:11035-11046
- **suggested_change:** Add a `requires` clause: `auto to_vector() const requires (!std::is_arithmetic_v<element_type>)` so `to_vector<int>()` fails at compile time with a readable hint pointing at the implicit-conversion path.

### [S] [INTERNAL] `decode_array_oop` and `field_oop` have inconsistent `noexcept` declarations
- **rationale:** `decode_array_oop` (line 14740) is NOT declared `noexcept`, but `field_oop` (line 14532) is, and forwards directly to `decode_array_oop`. The callee can technically throw (it doesn't in practice — every operation is trivial) but the asymmetry is a future foot-gun the moment someone adds logging that allocates.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:14532-14536, 14740-14749
- **suggested_change:** Add `noexcept` to `decode_array_oop` to match. The body already has nothing that could throw.

## Tests

### [jvm_integration] [new] read_null_primitive_array_returns_empty_vector
- **description:** Add `public static int[] nullIntArray = null;` to Example.java and a wrapper `get_null_int_array`. Verify the C++ read returns an empty vector and (after the fix above) a single warning is logged.
- **asserts:** `vec.empty()` and no crash. Optional: capture log buffer and assert the warning fired exactly once.
- **existing_file:** vmhook/src/example.cpp::test_array_edge_cases (line 2418)

### [jvm_integration] [new] write_to_null_primitive_array_logs_and_returns
- **description:** Using the same `nullIntArray` fixture above, call `set_static_null_int_array({1,2,3})` and confirm it does not crash, the Java field is still null, and the new diagnostic line is emitted. Pairs with bug 3.
- **asserts:** Field stays null after the call; log line contains "backing Java array is null".
- **existing_file:** vmhook/src/example.cpp::test_array_edge_cases (line 2418)

### [jvm_integration] [new] read_int_array_as_vector_int64_is_rejected
- **description:** Read `staticIntArray` (declared `[I`, three int32 elements) into a `std::vector<std::int64_t>`. With the fix from bug 2 the call returns an empty vector and logs an "element width mismatch" diagnostic. Without the fix this can read OOB and either crash or return garbage.
- **asserts:** Returned vector is empty; log line mentions "element width mismatch".
- **existing_file:** vmhook/src/example.cpp::test_array_edge_cases (line 2418)

### [jvm_integration] [extend_existing] write_oversized_vector_logs_truncation_warning
- **description:** Pass a `std::vector<std::int32_t>{1,2,3,4,5}` to `set_static_int_array` (declared as `int[3]`). After fixing bug 4, the call should log a truncation warning and write only the first 3 elements. The Java side already validates the values in `validateInJava()`, but we want the warning to surface.
- **asserts:** Java-side `staticIntArray` equals `{1,2,3}`; log line contains "size mismatch" or "truncated".
- **existing_file:** vmhook/src/example.cpp (extend wrapper test around line 1546)

### [standalone_unit] [new] read_array_value_handles_corrupted_length
- **description:** Build a fake array OOP in heap-allocated memory: a 16-byte header with `_length = 0x7FFFFFFF` followed by zero bytes. Pass its compressed encoding to `field_proxy::value_t{ ..., "[I", ... }` and convert to `std::vector<std::int32_t>`. Without bug 1's fix this terminates the test process. With the fix it returns an empty vector and logs.
- **asserts:** Test process does not abort; vector is empty.
- **existing_file:** (new file e.g. `tests/test_array_read_safety.cpp` — none of the existing standalone tests cover the `value_t` array path)

### [jvm_integration] [extend_existing] write_then_read_round_trip_for_every_primitive_array
- **description:** Existing test only verifies single-shot writes (lines 1546-1613 in example.cpp). Add a second pass that writes a different vector, reads it back via `get_static_int_array()`, and compares. This exercises the read and write halves against the same field for every `[Z [B [S [C [I [J [F [D`. Currently only the write half is checked against the Java-side validator.
- **asserts:** For each primitive type: written values are the same as read-back values.
- **existing_file:** vmhook/src/example.cpp (extend around line 1546)

### [standalone_unit] [extend_existing] vector_of_byte_compile_check_matches_J_field_width
- **description:** test_api_surface.cpp at line 127 instantiates `field.get().to_vector<element_w>()` for the collection path, but does not instantiate every primitive-array `operator std::vector<T>()` conversion. Add a compile-only block that forces instantiation of `operator std::vector<bool>`, `vector<int8_t>`, `vector<std::byte>`, `vector<int16_t>`, `vector<uint16_t>`, `vector<char>`, `vector<int32_t>`, `vector<int64_t>`, `vector<float>`, `vector<double>` from a dummy `field_proxy`. Catches signature/dispatch regressions at compile time.
- **asserts:** Compiles cleanly.
- **existing_file:** tests/test_api_surface.cpp (line 127)

## Parity Concerns
- **Silent failure parity with `field_proxy::set` for scalars.** v0.4.4 explicitly hardened scalar `set` against silent no-op (static_assert at 11252-11258) and silent size mismatch (warning at 11241). The three array setters (`set_bool_array`, `set_prim_array`, `set_str_array` at 14638/14669/14716) were not updated — they still silently no-op on null OOP and silently truncate on size mismatch.
- **Width validation parity between read and write paths.** `field_proxy::set` size-checks scalar primitives against `jvm_primitive_byte_width`. The array read path does no such check, so wrong-typed `std::vector<T>` instantiations slip through and read OOB. The trait helper at 11395+ is already in place; reusing it on the read side closes the gap with one extra branch.
- **Naming overlap with the collection path.** `value_t::to_vector<T>()` (object array → `vector<unique_ptr<T>>`) and `operator std::vector<T>()` (primitive array → `vector<T>`) coexist on the same type. There's no compile-time guard preventing users from writing `get().to_vector<int>()` and getting a `vector<unique_ptr<int>>` plus a runtime log instead of the primitive read they wanted. A `requires` clause on `to_vector` would mirror the v0.4.4 ethos of "fail loudly".
- **No `array_length()` / `is_array()` accessors on `field_proxy`.** `method_proxy` exposes return-type inspection; `field_proxy` does not expose array-shape inspection. Users with a large-array field have no way to peek at length before deciding to read it.
