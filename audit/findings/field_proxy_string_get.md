# field_proxy_string_get

## Summary
Audited `read_java_string()` at vmhook.hpp:14437-14517, the workhorse behind `field_proxy::value_t::cast_for_variant<std::string>()` (line 10954) and `frame::extract_frame_arg<std::string>` (line 7214). The top concern is silent data loss: every non-ASCII character (>= 0x80) is replaced with `?` on both the JDK 8 char[] and the JDK 9+ UTF-16 paths, with no log line. Secondary concerns are a documentation/behavior mismatch on the 4 KiB cap ("truncates" in doc, rejects in code), a noisy warning fired for legitimate empty Java strings, and a JDK-9+ length-vs-char-count interpretation that caps UTF-16 strings at 2048 chars instead of 4096.

## Bugs

### [high] Non-ASCII characters silently replaced with `?` (data loss, no log)
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:14489-14497, 14506-14514
- **description:** Both the JDK 8 `char[]` path (no `coder` field) and the JDK 9+ UTF-16 path (`coder != 0`) substitute `'?'` for every code unit with the high bit set: `result += (chars[i] < 0x80) ? static_cast<char>(chars[i]) : '?';`. A field holding `"café"` decodes to `"caf?"` with no warning. This is data loss — the user has no way to tell the result is degraded — and is inconsistent with the LATIN1 path (line 14503) which preserves bytes >= 0x80 verbatim. UTF-8 encoding for the supplementary chars is trivial and would round-trip cleanly through `std::string`.
- **repro:** Java static field `String s = "café";`. Call `cls.field("s")->get()` and convert to `std::string`. Result is `"caf?"`. Same with `"日本語"` -> `"???"`.
- **suggested_fix:** UTF-8 encode each `chars[i]`: emit 1 byte for `< 0x80`, 2 bytes for `< 0x800`, 3 bytes for `< 0x10000`. Optionally log once per call when degradation actually occurs, so silent lossy decoding cannot pass unnoticed. Apply identically to both UTF-16 branches so the byte-vs-char interpretation is consistent.
- **confidence:** certain

### [medium] 4 KiB cap is a hard reject, not a truncation (contradicts docstring)
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:14435 (doc), 14477-14483 (code)
- **description:** The docstring (line 14435) says "Truncates strings longer than 4096 characters as a sanity check." The code path actually does `if (length <= 0 || length > 4096) return {};` — a 4097-character String returns the empty string instead of the first 4096 characters. Users debugging a real String of length 5000 see an unrelated "array length out of range" warning and an empty result, with no indication that their data was simply too long.
- **repro:** `String s = "x".repeat(5000);` then read it via `field_proxy::get()`. Returns `""` and logs `"array length 5000 out of range"`. Sibling `make_java_string()` at line 10625 uses `std::min(..., 4096u)` to truncate — different behavior for the same cap.
- **suggested_fix:** Either (a) truncate to 4096 to match the docstring and sibling: `const std::int32_t effective_length{ std::min(length, std::int32_t{4096}) };` and log a one-line "truncated" notice, or (b) update the docstring to say "rejects" and split the diagnostic into two distinct messages so users can tell a too-long string apart from a corrupt header.
- **confidence:** certain

### [medium] JDK 9+ UTF-16 cap is 2048 characters, not 4096
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:14477, 14508
- **description:** `length` is the *byte* count of the backing `byte[]` (read at offset +12 from the array header). For a UTF-16-coded String on JDK 9+, the actual character count is `length / 2` (computed at line 14508). The 4 KiB sanity cap is applied to the byte count *before* the divide, so the effective character limit on the UTF-16 path is 2048, not 4096. JDK 8 (char[]) and JDK 9+ LATIN1 both reach 4096 chars; only UTF-16 stops at 2048. Silent platform-conditional behaviour difference.
- **repro:** Build a Java String of 3000 non-Latin1 chars (e.g. `"日".repeat(3000)`). Backing `byte[]` length is 6000. The `length > 4096` check trips and the function returns `""` even though the same source code would return 3000 chars on a JDK that happened to store the string as Latin1.
- **suggested_fix:** Move the cap check below the coder branch, comparing against the *character* count: `const std::int32_t char_count{ has_coder && coder != 0 ? length / 2 : length };` and gate the cap on `char_count`. Or simply raise the byte-cap to 8192 with a comment.
- **confidence:** likely

### [low] Empty Java String emits a warning log on every call
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:14477-14483
- **description:** `length <= 0` is treated as a failure path that emits the warning `"array length {} out of range (must be 1..4096) - either an empty string or the array header is corrupt."`. An empty string (`String s = "";`) is a valid, common state — `new String()` produces one — and a hot loop that prints every empty field will spam the log. The OR-comment ("either empty or corrupt") acknowledges the ambiguity but doesn't resolve it.
- **repro:** `String s = "";` field. Each call to `cls.field("s")->get()` followed by string conversion logs a `[!]` warning.
- **suggested_fix:** Split the check: `if (length == 0) return {}; if (length < 0 || length > 4096) { VMHOOK_LOG(...); return {}; }`. No log for the legitimate empty case.
- **confidence:** certain

### [low] Per-call `find_class` + `find_field("coder")` are unnecessary lookups
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:14447, 14486, 14500
- **description:** Every call to `read_java_string()` re-resolves `java/lang/String` and (when reading non-empty) calls `string_klass->find_field("coder")` twice — once to decide which branch (line 14486), and again to pull the coder byte via `get_field` (which itself calls `find_field` internally — line 14500). The `find_field` path is `O(F)` per lookup (per the file header). Hot paths that decode many strings (e.g., `to_vector<std::string>` walking a 1000-element String[] at line 10846) pay this cost N times. Cache `string_klass`, the `value` field offset, the `coder` field presence, and the `coder` field offset in a function-local `static` set on first successful resolution.
- **repro:** Profile `read_array_value<std::vector<std::string>>` on a 10_000-element String[]. Most CPU time will be in `klass::find_field`, not the actual byte copy.
- **suggested_fix:** `static const auto cached{ resolve_string_layout() };` where `resolve_string_layout()` returns `{string_klass*, value_offset, coder_offset_or_neg1}`. Guard with `std::call_once` or use C++ static-init guarantees. Each subsequent call becomes branch-free pointer arithmetic.
- **confidence:** likely

## Improvements

### [S] [USER_FACING] Distinguish "empty", "too long", "corrupt header" in diagnostics
- **rationale:** The single warning at line 14479 conflates three very different failure modes. Users debugging a `read_java_string` returning empty cannot tell whether their String has length 0 (legitimate), >4096 (cap), or <0 (genuinely corrupt). Splitting them lets users self-diagnose.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:14477-14483
- **suggested_change:** Three separate branches with distinct log messages, no log for empty: see "Empty Java String emits a warning log" bug above. Bonus: include the string OOP address in the log so the user can correlate.

### [XS] [USER_FACING] Make the 4 KiB cap configurable via a constexpr or macro
- **rationale:** Some users will have legitimate strings >4 KiB (URLs, JSON blobs, GUI text). A hard-coded constant forces a fork of the header. Sibling `make_java_string` shares the same cap (line 10625); both should reference one symbol.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:14477, 10625
- **suggested_change:** Add `inline constexpr std::int32_t k_max_string_chars{ 4096 };` near the top of the namespace, override-able via `#ifndef VMHOOK_MAX_STRING_CHARS`. Use it in both call sites.

### [S] [INTERNAL] Return `std::optional<std::string>` or pass an out-param for "fail vs empty"
- **rationale:** Callers cannot distinguish "field held an empty string" from "field was null / OOP invalid / find_class failed" — all three yield `std::string{}`. `field_proxy::get_or<std::string>` or `try_get` would be a clean addition. Important for unit tests and for code that needs to know whether a field was ever set.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:14437-14517 (and the variant cast at 10950-10960)
- **suggested_change:** Add a `try_read_java_string` returning `std::optional<std::string>` (existing function delegates and discards the optional for ABI). Wire a `field_proxy::try_get<std::string>()` accessor through `value_t` for parity with the planned method_proxy `try_call` direction.

### [M] [INTERNAL] UTF-8 transcode helper shared with `make_java_string` / `write_java_string`
- **rationale:** Three call sites (`read_java_string` x2, `make_java_string`, `write_java_string`) do byte-narrowing or byte-widening between Java's `char` (UTF-16) and C++ `std::string` (UTF-8 by convention). Centralising this avoids the asymmetric loss documented above and gives one place to fix encoding bugs.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:14489-14514, 10638-10657, 14592-14602
- **suggested_change:** `inline std::string utf16_to_utf8(const std::uint16_t* src, std::size_t count) noexcept` and `inline void utf8_to_utf16(std::string_view src, std::uint16_t* dst, std::size_t cap) noexcept`. Replace inline loops with calls. Same helper covers `frame::extract_frame_arg<std::string>` indirectly via `read_java_string`.

### [XS] [INTERNAL] Replace magic offsets +12 / +16 with named constants
- **rationale:** Lines 14476 and 14485 use bare `+12` and `+16`, identical to the documented HotSpot array layout at lines 10687-10691 and the offsets in `array_length`/`make_java_array`. A single named constant (`hotspot::k_array_length_offset`, `hotspot::k_array_data_offset`) would prevent drift if HotSpot ever changed (e.g., 32-bit builds where klass pointer is 4 bytes — but only for class word with the +12 then becomes +8).
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:14476, 14485
- **suggested_change:** Extract two `constexpr std::ptrdiff_t` constants in the `hotspot` namespace and use them in `array_length`, `make_java_array`, `read_java_string`, `write_java_string`, etc.

### [S] [USER_FACING] Document the "?" substitution explicitly in the function header
- **rationale:** Until the data-loss bug is fixed, users at minimum deserve a doc-comment line telling them non-ASCII becomes `'?'` on the char[] / UTF-16 paths. The current docstring (line 14430-14435) implies a lossless decode.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:14430-14435
- **suggested_change:** Add a third bullet under `@details`: "Non-ASCII characters (>= 0x80) on JDK-8 char[] and JDK-9+ UTF-16 paths are replaced with '?'. Latin-1-coded strings (JDK-9+ coder=0) are returned byte-verbatim."

## Tests

### [standalone_unit] [new] test_read_java_string_truncation_vs_rejection
- **description:** Construct a synthetic in-memory layout that mimics a HotSpot `byte[]` + minimal `String` klass so `read_java_string` can be called without a JVM. Build arrays of length 0, 1, 4096, 4097. Document whether the implementation truncates or rejects each. Confirms behavior matches the docstring.
- **asserts:** `len=0 -> ""`, `len=4096 -> 4096-char string`, `len=4097 -> "" with log` (current), or `4096-char prefix` (post-fix).

### [standalone_unit] [new] test_read_java_string_null_oop_safety
- **description:** Call `read_java_string(nullptr)` and `read_java_string` with a deliberately-invalid pointer (e.g. `0xDEAD`). Ensure the function returns `{}` without crashing and that the early `is_valid_pointer` branch logs once.
- **asserts:** No crash; returns empty; log line contains "string_oop is null or invalid".

### [standalone_unit] [new] test_read_java_string_field_caching_perf
- **description:** Spin a loop of 100_000 calls against a stubbed `string_oop` and measure the time spent in `find_class` / `find_field`. After the proposed static-cache fix, the per-call cost should drop by ~10x. Useful as a perf regression test.
- **asserts:** Total time < threshold; no per-call allocation in steady state (use a counting allocator).

### [jvm_integration] [new] test_string_field_unicode_roundtrip
- **description:** Add a Java fixture field `String unicode = "café日本";` and a `String empty = "";` and `String big = "x".repeat(5000);`. Read each via `cls.field(...)->get()` and convert to `std::string`. This is the regression test that proves the data-loss bug.
- **asserts:** `unicode` round-trips byte-for-byte (post-fix); current code yields `"caf?\?\?\?\?\?\?"`. `empty` yields `""` with NO warning in the log. `big` yields a 4096-char string (or `""` if the rejection behaviour is kept; docstring must match).
- **existing_file:** would extend `example/vmhook/Example.java` (or add a new `StringProbe.java`).

### [jvm_integration] [extend_existing] test_string_field_get_jdk8_vs_jdk9
- **description:** Run the same fixture twice — once on a JDK 8 build (char[] backing), once on JDK 9+ (byte[] + coder). Verify identical decoded results for ASCII, Latin1 (`"é"`), and supplementary plane (`"😀"` if BMP-pair handling is added). Currently only Latin1 differs (JDK 9+ returns raw bytes; JDK 8 returns `?`).
- **asserts:** Decoded `std::string` identical across JDK versions for the same source code.
- **existing_file:** wire into the JVM driver at `vmhook/src/example.cpp`.

### [standalone_unit] [new] test_read_java_string_field_proxy_dispatch
- **description:** Build a fake field_proxy with signature `"Ljava/lang/String;"` whose backing memory points to a synthetic String oop (per the truncation test). Convert `proxy.get()` -> `std::string` and verify it routes through `cast_for_variant` -> `read_java_string`. Today this path is entirely untested in standalone units.
- **asserts:** Conversion yields the same string the underlying `read_java_string` would, and signatures other than `Ljava/lang/String;` yield `""` from the `cast_for_variant` else-branch (line 10958).

## Parity Concerns
- `read_java_string` has no `try_` variant — `method_proxy::call` audit work has called out the same gap for return-string fetching. Both should converge on `std::optional<std::string>` so users can distinguish "field/return was empty" from "lookup failed".
- `make_java_string` (line 10603) caps at 4096 chars via `std::min` (truncates); `read_java_string` rejects at the same cap. Round-tripping `"x".repeat(5000)` through `make_java_string -> read_java_string` returns `""` instead of the truncated `4096`-char prefix. Same constant, opposite behaviour.
- `write_java_string` (line 14555) silently truncates to the existing array length without logging; `read_java_string` logs when it bails on length. The trio of String helpers should agree on a single "log when degrading" policy.
- `field_proxy::value_t::cast_for_variant<std::string>` at line 10950 returns `std::string{}` from the non-uint32_t branch without a log; failures inside `read_java_string` do log. A user converting a non-OOP field (`int`) to `std::string` gets `""` with no diagnostic. Method_proxy already addresses this with explicit-type errors; field_proxy could do the same.
- The JDK 9+ `coder` field is looked up via `find_field("coder").has_value()` instead of a cached predicate that `make_java_string` (line 10624) and `write_java_string` (line 14590) could share. Three call sites, three independent lookups.
