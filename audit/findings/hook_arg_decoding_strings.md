# hook_arg_decoding_strings

## Summary
`extract_frame_arg<std::string>` (vmhook.hpp:7184-7259) reads the local slot, decodes a 32-bit narrow OOP via an inline lambda, and delegates to `vmhook::read_java_string` (vmhook.hpp:14437-14517) which walks the `value` field of `java.lang.String` to obtain the backing `char[]` / `byte[]` and copy bytes into a `std::string`. The decoder is functional for the common JDK 8 / 11 / 17 cases but has several real correctness, documentation, and user-friendliness bugs around the 4096-cap (returns empty instead of truncating, cap is measured in bytes for UTF-16), the LATIN1/UTF-16 paths (silently maps every non-ASCII unit to `?`), the implicit "compressed OOPs only" assumption, and the empty-string warning spam.

## Bugs

### [high] Documented "truncates at 4096" actually returns an empty string and logs a warning
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:14429-14483
- **description:** The doxygen says "Truncates strings longer than 4096 characters as a sanity check," but the implementation at line 14477-14483 returns `{}` (empty `std::string`) and logs a warning when `length > 4096`. So a 4097-char String becomes empty in the hook callback, not a 4096-char prefix. Users who hook a method receiving large strings (URLs, JSON payloads, stack traces) silently see `""` instead of either the real content or the documented prefix, with no indication beyond a one-line debug log.
- **repro:** Hook any Java method whose String argument exceeds 4096 bytes (e.g. a JSON body). The hook callback receives `std::string{}` instead of the first 4096 bytes.
- **suggested_fix:** Either (a) actually truncate by clamping `length` to 4096 before the copy loop and continuing, or (b) update the doxygen + warning message to say "rejected because too long; returned empty" so users stop being misled by the docstring. The truncate-and-continue behaviour matches the docstring intent and the symmetric `make_java_string` (line 10625: `std::min<std::size_t>(value.size(), 4096u)`) which truncates rather than rejecting.
- **confidence:** certain

### [high] 4096-character cap is measured in bytes, capping UTF-16 strings at 2048 chars
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:14475-14483, 14506-14514
- **description:** On JDK 9+ with `coder == 1` (UTF-16), the backing array is `byte[]` whose `_length` is in bytes (`length` here), and the char count is `length / 2`. The cap check at line 14477 compares the raw byte length against 4096, so a UTF-16 String of 2049 chars (= 4098 bytes) is rejected even though the docstring promises a 4096-character cap. Users get inconsistent behaviour: a 2500-char Latin-1 String passes (single-byte coder), a 2500-char Japanese String is dropped.
- **repro:** Construct a Java String of 2049 Japanese / Cyrillic characters and hook a method that receives it on JDK 9+. The hook gets `""` because `length = 4098 > 4096`.
- **suggested_fix:** Apply the cap in *characters*, not bytes: after computing `char_count = (coder == 0 ? length : length / 2)`, clamp `char_count` and the loop bound at 4096 rather than checking the raw `length`. Or alternatively make the cap configurable / removable.
- **confidence:** certain

### [medium] Non-ASCII characters silently corrupted to `?` in both JDK 8 char[] and JDK 9+ UTF-16 paths
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:14489-14497, 14506-14514
- **description:** In the `!has_coder` (JDK 8 char[]) and `coder != 0` (JDK 9+ UTF-16) branches, every char with value `>= 0x80` is silently replaced with `'?'` (lines 14495 and 14512). This destroys all non-ASCII text without any warning. The function is documented as decoding a Java String to `std::string`; users reasonably assume UTF-8 output. A hook reading user input, file paths, or log lines containing any non-ASCII data sees garbage. The Latin-1 branch (line 14503) at least preserves bytes 0x80-0xFF in the output (whether they form valid UTF-8 is a different problem), but the UTF-16 path destroys all of them.
- **repro:** Java code `"café"` → hook callback receives `"caf?"` on JDK 9+ if the JVM picks UTF-16 (any code point > 0xFF forces UTF-16 coder).
- **suggested_fix:** Encode UTF-16 → UTF-8 properly in both UTF-16 paths (a small inline encoder handling BMP + surrogate pairs is ~20 lines). At minimum, document the lossy behaviour in the doxygen so callers can choose to call `make_unique<java.lang.String>` and use `getBytes("UTF-8")` instead.
- **confidence:** certain

### [medium] Empty-string warning fires for legitimate 0-length backing arrays
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:14476-14483
- **description:** JDK 9+'s `""` literal shares a singleton `EMPTY_BYTES` byte[] of length 0. The cap check rejects `length <= 0` and logs `"array length 0 out of range (must be 1..4096) - either an empty string or the array header is corrupt"` at warning level. Any hook that receives empty strings (common — default field values, error message templates) will spam VMHOOK_LOG on every call. The line above (14457) intentionally suppresses warnings for the `value == 0` case ("Don't log here to avoid spamming the common case"), but this second equivalent case (legitimate empty backing array) was missed.
- **repro:** Hook any method whose String parameter is `""`. Each invocation produces one warning log line.
- **suggested_fix:** Treat `length == 0` separately from `length < 0` or `length > 4096`: return `{}` silently for the legitimate empty case, keep the warning only for `length < 0` (impossible for a sane array) and `length > 4096` (truncation event).
- **confidence:** certain

### [medium] `read_java_string` ignores JDK 7-style `offset` / `count` fields that `make_java_string` knows about
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:14437-14517 vs. 10661-10669
- **description:** `make_java_string` checks `string_klass->find_field("offset")` and `"count"` to populate them on legacy JVMs (JDK 6/7 layout where `String` shared a backing array with `offset`/`count` for substrings). `read_java_string` does not look at either field and always reads from offset 0 with the backing-array length. If vmhook ever runs on a HotSpot variant that still has `offset` + `count` (older OpenJDK 7 ports, some embedded HotSpot forks), `read_java_string` reads the parent string's full backing array rather than the substring window, returning the wrong content for every `String.substring()` result. The fact that `make_java_string` is asymmetric (writes offset/count) suggests the author considered this case but only one side was wired up.
- **repro:** Run on a JDK 7-style HotSpot (or any HotSpot fork where `String` retains `offset`/`count`). Hook a method receiving `someStr.substring(5, 10)`. The hook sees the entire backing string, not the 5-char window.
- **suggested_fix:** Either (a) remove the `offset`/`count` write in `make_java_string` (since the rest of the library assumes JDK 8+), document the JDK 8+ requirement clearly, and stop pretending to handle older layouts — or (b) read `offset` and `count` in `read_java_string` when those fields exist and use them in place of `length` and the data-pointer offset.
- **confidence:** likely

### [low] `find_field("coder")` called on every invocation; not cached
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:14486, 14500
- **description:** `read_java_string` calls `string_klass->find_field("coder")` on every call (`has_coder` boolean) and, when present, calls `vmhook::get_field<std::uint8_t>(string_oop, string_klass, "coder")` which also performs a field lookup internally. A hot path (e.g. a hook on `Logger.log(String)`) repeats this lookup per invocation. `string_klass` is effectively immutable across the JVM lifetime; the `coder` field's offset can be computed once and cached statically.
- **repro:** Hook a method called thousands of times per second with a String arg; profile sees `find_field` in the hot stack.
- **suggested_fix:** Cache the `coder` field's offset (or absence) in a function-local `static const std::optional<std::size_t>` populated on first invocation. The same caching opportunity exists for `value` (line 14456) and the `string_klass` itself (line 14447).
- **confidence:** likely

### [low] Hard-coded array-header offsets (12 / 16) assume x86-64 + UseCompressedOops + UseCompressedClassPointers
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:14476, 14485
- **description:** The header offsets `arr + 12` for `_length` and `arr + 16` for `_data[0]` only hold for the (mark word=8B, narrow-klass=4B, length=4B) layout, i.e. compressed klass pointers + compressed OOPs on 64-bit. If the user runs with `-XX:-UseCompressedClassPointers` or `-XX:-UseCompressedOops`, the klass pointer is 8 bytes and length moves to +16, data to +20/+24. The same magic numbers are used in `array_length` at line 10701 — but that one at least documents the assumption. `read_java_string` silently reads garbage in non-compressed configurations.
- **repro:** Run with `-XX:-UseCompressedOops -XX:-UseCompressedClassPointers`. Any hook receiving a String arg gets garbage in the callback.
- **suggested_fix:** Replace the literal `arr + 12` / `arr + 16` with `vmhook::array_length(arr_oop)` and a dynamically-computed data offset (which `array_length` could expose as a helper). At minimum, add a comment noting the compressed-OOPs / compressed-klass-pointers prerequisite and a `static_assert` / runtime check at library init.
- **confidence:** likely

### [low] `extract_frame_arg<std::string>` does not validate `read_java_string` succeeded vs. legitimate empty string
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:7212-7215
- **description:** The string branch unconditionally returns `vmhook::read_java_string(decode_oop(raw_value))`. If `read_java_string` returns `{}` because (a) the OOP was null, (b) the backing array was too long, (c) the backing array length was 0, or (d) the user actually passed `""`, the callback can't distinguish. Combined with the bugs above (the 4096 cap returning empty, the empty-array warning), a callback that branches on `s.empty()` will mis-classify legitimate failures as empty strings.
- **repro:** A hook callback writes `if (str.empty()) handle_null()` — it fires both for actual nulls *and* for >4096-byte strings *and* for 0-byte backing arrays.
- **suggested_fix:** Either change `read_java_string` to return `std::optional<std::string>` (breaking change), or add a sibling `read_java_string_or_default` that takes a fallback, or document the empty-on-failure contract loudly in the doxygen so callers know they can't differentiate.
- **confidence:** speculative

## Improvements

### [S] [USER_FACING] Make the 4096 cap configurable (or remove it entirely)
- **rationale:** Hard-coded magic numbers in a public single-header library bite users who legitimately need bigger strings (URLs, GraphQL queries, stack traces, base64 blobs). 4096 is a sanity check, not a fundamental limit. Users on modern hardware would prefer to set their own bound.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:14477
- **suggested_change:** Replace the literal `4096` with a `vmhook::config::max_decoded_string_length` `inline constexpr` (defined at the top of the file alongside `warning_tag`), or take an optional second parameter `read_java_string(void* oop, std::size_t max_length = 4096)`. Either keeps backward compatibility.

### [S] [USER_FACING] Improve the warning messages to be actionable
- **rationale:** The current warnings say things like "string_oop is null or invalid (0x...)" which tells the user *what* but not *why* or *what to do*. A user seeing this in a log has no path forward.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:14442-14443, 14450-14453, 14468-14471, 14479-14482
- **suggested_change:** Each warning should end with a one-line "what to check next" hint, mirroring the excellent error message style already used in `extract_frame_arg`'s `static_assert` (line 7250-7257). E.g. for the length-out-of-range warning, append "If your String is intentionally larger than 4096, increase `vmhook::config::max_decoded_string_length` or call `read_java_string_n(oop, your_limit)`."

### [S] [INTERNAL] Cache `string_klass` and field offsets in a function-local static
- **rationale:** Three identical `find_class("java/lang/String")` calls fan out from `make_java_string`, `read_java_string`, `write_java_string`. The result is JVM-lifetime stable; recomputing on every call is wasted work, especially given the function is called once per string argument in a potentially high-frequency hook.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:14447, 10606, 14563
- **suggested_change:** Introduce a small helper that returns a struct with `{ klass*, value_offset, coder_offset_optional }` and have all three free functions use it. Memoise the struct in a function-local `static const` (initialised lazily on first JVM-attached call).

### [S] [USER_FACING] Add proper UTF-16 → UTF-8 encoder (or document the lossy fallback)
- **rationale:** The current `(chars[i] < 0x80) ? char : '?'` silently destroys all non-ASCII text. For a tool meant to be used by JVM-app developers who reasonably expect their app's strings to round-trip, this is a footgun. A simple BMP-only UTF-8 encoder is ~15 lines; full surrogate-pair handling is ~25.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:14489-14497, 14506-14514
- **suggested_change:** Replace the lossy loop with a proper encoder, or add a `read_java_string_utf8` sibling that does it correctly and have the docstring of the existing function flag the lossy ASCII-only behaviour.

### [XS] [USER_FACING] Document and align `read_java_string` doxygen with actual behaviour
- **rationale:** The docstring claims it "truncates," but it actually rejects-and-empties. The cap claim is in "characters" but enforced in bytes for UTF-16. Users reading the header to learn the API are actively misled.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:14429-14436
- **suggested_change:** Either fix the implementation to match the docstring (preferred, see bug above) or update the docstring to say "Returns empty if longer than 4096 bytes (note: bytes, not characters — UTF-16 strings effectively cap at 2048 chars); silently maps non-ASCII to '?'."

### [XS] [INTERNAL] Lambda parameter name `raw_value` shadows outer `raw_value`
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:7195, 7198-7210
- **rationale:** Inside `extract_frame_arg`, the lambda `decode_oop` takes a parameter named `raw_value` which shadows the outer `void* const raw_value` declared three lines earlier. Compiles fine but is confusing in review and will cause warnings under `-Wshadow`.
- **suggested_change:** Rename the lambda parameter to `narrow` or `slot_bits`.

### [S] [USER_FACING] Provide a `read_java_string_view` that does not copy for the LATIN1 fast path
- **rationale:** On JDK 9+ LATIN1 strings (the majority, since modern JDK packs ASCII text as LATIN1), `read_java_string` does `result.assign(reinterpret_cast<const char*>(data), length)` — a malloc + memcpy. For read-only inspection (`if (str == "GET")`), a `std::string_view` over the heap data would skip the copy. The lifetime is bounded by the hook callback so this is safe.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:14501-14503
- **suggested_change:** Add `read_java_string_view(void* oop) -> std::string_view` returning a view into the heap. Document the lifetime constraint (must not outlive the hook call / safepoint).

### [XS] [USER_FACING] Forward declaration at line 1478 mismatches the body's `noexcept`-ness
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:1478, 14437
- **rationale:** The forward decl `inline auto read_java_string(void* string_oop) -> std::string;` is not marked `noexcept`, and neither is the definition. But everything inside is `noexcept` in practice (it returns empty on every error path). Adding `noexcept` would let callers compose it inside their own `noexcept` lambdas without `noexcept(noexcept(...))` gymnastics.
- **suggested_change:** Mark both the forward declaration and the definition `noexcept`, matching the existing pattern of `write_java_string` / `make_java_string` (both noexcept).

## Tests

### [jvm_integration] [extend_existing] test_read_java_string_truncation_4096_plus
- **description:** Hook a Java method receiving a 5000-character String and assert the hook receives the documented behaviour (currently empty; ideally a 4096-char prefix).
- **asserts:** `decoded.size() == 4096` (or `decoded.empty()` until bug fixed); a warning is emitted only on the truncation event, not per-call.
- **existing_file:** vmhook/src/example.cpp (next to `test_read_java_string` at line 2808)

### [jvm_integration] [new] test_read_java_string_utf16_large_string_silently_dropped
- **description:** Encode a 2049-char string of code points > 0xFF (e.g. Japanese), pass through a hook, assert the current bug (returns empty) so any future fix is visible as a green test flip.
- **asserts:** With 2049 non-Latin chars: hook callback receives `""` (current behaviour); with 2048 chars receives all `?` (current behaviour).
- **existing_file:** vmhook/src/example.cpp

### [jvm_integration] [new] test_read_java_string_empty_string_no_warning_spam
- **description:** Hook a method whose String argument is `""`. Invoke it 1000 times. Assert the VMHOOK_LOG file does not gain 1000 warning lines.
- **asserts:** Log line count before vs after differs by 0 (or at most 1 if first-call init logging is acceptable).
- **existing_file:** vmhook/src/example.cpp

### [jvm_integration] [new] test_read_java_string_non_ascii_round_trip
- **description:** Verify the UTF-16 → UTF-8 lossy behaviour. Write `"héllo"` to a String field, read back via `read_java_string`, observe the corruption.
- **asserts:** Current: `decoded == "h?llo"`. After fix: `decoded == "h\xC3\xA9llo"` (proper UTF-8).
- **existing_file:** vmhook/src/example.cpp

### [jvm_integration] [new] test_read_java_string_substring_offset_count
- **description:** On a JVM where `String` has `offset`/`count` (skipped if not present), pass `"abcdef".substring(2, 4)` through a hook and assert the hook sees `"cd"`, not `"abcdef"`.
- **asserts:** Skipped if `String.coder` exists (i.e. JDK 9+); on JDK 6/7 layout asserts `decoded == "cd"`.
- **existing_file:** vmhook/src/example.cpp

### [jvm_integration] [new] test_read_java_string_null_oop
- **description:** Call `vmhook::read_java_string(nullptr)` directly and assert behaviour. Existing `test_read_java_string` does not cover the null path.
- **asserts:** Returns empty string; emits exactly one warning (no crash).
- **existing_file:** vmhook/src/example.cpp

### [jvm_integration] [new] test_read_java_string_invalid_pointer_pattern
- **description:** Call `vmhook::read_java_string` with sentinel patterns `0xCCCCCCCC`, `0xDEADBEEF`, `0xCAFEBABE` to confirm `is_valid_pointer` rejects them and the function does not dereference.
- **asserts:** Returns `{}` for each pattern; does not crash.
- **existing_file:** vmhook/src/example.cpp

### [standalone_unit] [new] test_extract_frame_arg_string_decode_lambda_shadow
- **description:** Add a `-Wshadow` build configuration in the test CMakeLists and confirm `extract_frame_arg`'s lambda no longer warns about `raw_value` shadowing.
- **asserts:** Clean build under `-Wshadow` on GCC and Clang.
- **existing_file:** tests/CMakeLists.txt

### [jvm_integration] [new] test_read_java_string_uncompressed_oops_runtime_check
- **description:** Run the test JVM with `-XX:-UseCompressedClassPointers -XX:-UseCompressedOops` and assert `read_java_string` either (a) returns correct results or (b) fails loudly rather than silently returning garbage.
- **asserts:** Either the decoded string matches expectations, or an explicit error is logged and `{}` is returned (no garbage like `"`\xCC\xCC...`).
- **existing_file:** vmhook/src/example.cpp (would need a separate JVM-launch configuration)

### [jvm_integration] [extend_existing] test_read_java_string_via_extract_frame_arg
- **description:** The existing `test_read_java_string` calls the free helper directly through a field proxy, not via the hook path. Add coverage where a hook callback's String argument is observed (the actual `extract_frame_arg<std::string>` codepath).
- **asserts:** Hook callback receives the exact String value the Java caller passed.
- **existing_file:** vmhook/src/example.cpp (extend `test_read_java_string` at line 2808)

## Parity Concerns
- `make_java_string` (10603) and `read_java_string` (14437) are not symmetric: the former writes `offset` and `count` on legacy layouts, the latter ignores them. Pick one truth — either both handle pre-JDK-8 layouts or neither does.
- `make_java_string` calls `find_class` and `find_field` per invocation (same caching opportunity as `read_java_string`). A shared cached `string_class_info_t` would benefit both.
- `make_java_string` truncates oversized input to 4096 chars (10625: `std::min<std::size_t>(value.size(), 4096u)`). `read_java_string` rejects oversized input. Behavioural mismatch users will not expect from a "write then read it back" pattern.
- `read_java_string` always emits one warning per invocation on the empty/long path; the rest of the library mostly logs errors only on actual broken state (the explicit "Don't log here to avoid spamming the common case" comment at line 14460 shows the author was aware of this anti-pattern but applied it to only one of three paths).
- `extract_frame_arg<std::string>` uses the inline `decode_oop` lambda, while `cast_for_variant` (line 10954) and `append_array_value` (line 10846) call `vmhook::hotspot::decode_oop_pointer` directly with no `bits <= 0xFFFFFFFFull` guard. The three sites should share one helper to avoid the "is this an already-decoded full pointer or a narrow oop?" check drifting.
