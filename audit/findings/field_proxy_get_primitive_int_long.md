# field_proxy_get_primitive_int_long

## Summary
Audited `field_proxy::get()` in `vmhook/ext/vmhook/vmhook.hpp:11087-11148`, focused on the primitive Z/B/S/I/J/F/D/C signature branches. The integer widths and sign-extension semantics are correct (fixed-width `std::intN_t` types are read with `memcpy` of exactly `sizeof(T)`). Top concerns: (1) the "Z" branch reads `sizeof(bool)` bytes — implementation-defined width that can exceed the JVM's 1-byte boolean slot on exotic ABIs; (2) the null-`field_pointer` fallback unconditionally synthesises an `int32_t{}` variant alternative regardless of the declared signature (silently lying about the field's type); (3) `get()` has no size or signature sanity check matching the `set()` size-mismatch guard added in v0.4.4, so a corrupted/unknown signature silently falls through to a 4-byte compressed-OOP read.

## Bugs

### [low] "Z" branch reads `sizeof(bool)` rather than 1 byte
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:11095-11100
- **description:** `bool value{}; std::memcpy(&value, this->field_pointer, sizeof(value));` reads `sizeof(bool)` bytes — C++ leaves this implementation-defined. On MSVC/GCC/Clang for x64 it is 1 (matches JVM "Z" slot width), but the standard does not guarantee it. If `sizeof(bool) > 1` on any target ABI, this will read past the 1-byte boolean slot into the adjacent field's bytes; the resulting `bool` value is also undefined if the bit pattern isn't a valid representation. Sibling branches all use fixed-width `std::intN_t`, so they don't have this issue. The companion `set()` codepath (line 11248) has the same `sizeof(value_type)` pattern but is gated by the `jvm_primitive_byte_width` guard (11239); `get()` has no such guard.
- **repro:** Static-assert breaks on a hypothetical ABI with `sizeof(bool) == 4`. Practically: build for a platform where `sizeof(bool) != 1` and read a "Z" field that is followed in heap layout by a non-zero byte.
- **suggested_fix:** Replace with explicit 1-byte read: `std::uint8_t raw{}; std::memcpy(&raw, this->field_pointer, 1); return value_t{ raw != 0, this->signature_text };` — mirrors how the array reader handles "Z" elements at line 10833, and matches the JVM spec width unambiguously.
- **confidence:** likely (portability bug; unobservable on all currently-supported toolchains, but real per the standard)

### [low] Null-`field_pointer` fallback returns wrong variant alternative for the declared signature
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:11090-11093
- **description:** When `field_pointer` is null, `get()` always returns `value_t{ std::int32_t{}, signature_text }`, regardless of whether `signature_text` is "J" (long), "D" (double), "F" (float), "L..." (reference), etc. The `signature` member is preserved, but the variant alternative is `int32_t{0}` for every type. This is silently wrong in two observable ways: (a) `std::holds_alternative<std::int64_t>(proxy->get().data)` will be `false` for a "J" field that was never bound — surprising for callers that introspect the variant; (b) `cast_for_variant<std::string>` and `cast_for_variant<std::unique_ptr<T>>` (lines 10950, 10972) only convert from `std::uint32_t`, so a null-pointer reference field with sig "Ljava/lang/String;" returns `int32_t{0}` which then falls through to `{}` / `nullptr` correctly — but only by accident. The signature dispatch in `get()` already handles every JVM primitive; the null-path should reuse that table or fall through into it.
- **repro:** `vmhook::field_proxy fp{ nullptr, "J", false }; auto v = fp.get(); assert(std::holds_alternative<std::int64_t>(v.data));` — fails (alternative is `int32_t`).
- **suggested_fix:** Either (1) drop the early-return and let the existing signature dispatch run with `field_pointer == nullptr` after adding a null-check before each memcpy, or (2) pick the correct variant alternative based on `signature_text` in the early-return (a switch on signature[0]). Option (1) is shorter — wrap the memcpys in `if (field_pointer) memcpy(...);` and the zero-init `value{}` becomes the natural fallback.
- **confidence:** certain (easy to observe; user impact is small in practice because numeric casts collapse to 0)

### [low] No diagnostic / silent fall-through on unknown or malformed signature
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:11095-11147
- **description:** The if-chain handles "Z/B/S/I/J/F/D/C" then falls through to a uint32_t (compressed-OOP) read for "anything else". A typo'd or corrupted signature like "Q" or an empty string would silently get a 4-byte read and a uint32_t variant alternative, which then routes through the string / vector / unique_ptr path and may either decode garbage or return empty. Compare to `method_proxy::call_via_stub` (line 11571-11574) which logs `"malformed signature - no ')' found."` on a bad descriptor. Compare also to `set()` line 11252-11258 which has a `static_assert`-driven loud failure for unsupported value types. The get path is the quietest of the three.
- **repro:** `vmhook::field_proxy fp{ ptr, "WhateverGarbage", false }; auto v = fp.get();` — silently reads 4 bytes and produces a uint32_t alternative.
- **suggested_fix:** After the primitive branches, check whether `signature_text` actually starts with `'L'` or `'['` before assuming reference. If neither, emit `VMHOOK_LOG("{} field_proxy::get: unknown signature '{}' - treating as reference", vmhook::error_tag, signature_text)` and then take the reference path. Mirrors the `set()` diagnostic style.
- **confidence:** likely (silent failure mode the user wouldn't notice until decoded data is wrong)

## Improvements

### [S] [INTERNAL] Replace the linear `if`-chain with a `switch` on `signature_text[0]`
- **rationale:** Eight `if (signature_text == "X")` string-equality comparisons run in series for every read. Each comparison constructs / compares a `std::string` against a 1-char literal. A switch on `signature_text[0]` (after a length check) is one branch and gives the compiler a jump table. Also makes the parallel between get/set/method_proxy more obvious (method_proxy already uses `switch (ret_char)` — line 12272).
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:11087-11148
- **suggested_change:** `if (signature_text.size() == 1) { switch (signature_text[0]) { case 'Z': ... case 'B': ... } }`. Falls through to the reference path on length != 1 or unknown char. Halves the cost of `get()` for primitives and tightens the symmetry with the rest of the codebase.

### [XS] [INTERNAL] Hoist the memcpy-then-wrap pattern into a tiny helper
- **rationale:** Eight near-identical four-line blocks (lines 11095-11142) differ only in target type. A `template<typename T> auto read_into() -> value_t { T v{}; memcpy(&v, field_pointer, sizeof(T)); return value_t{ v, signature_text }; }` collapses the chain to one line per case.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:11095-11147
- **suggested_change:** Add a private member helper `template<typename T> auto read_typed() const noexcept -> value_t`, then dispatch returns `read_typed<std::int32_t>()` etc.

### [S] [USER_FACING] Document the variant-alternative table next to `get()`
- **rationale:** The doc-comment at 11078-11086 names the descriptor-to-variant mapping but readers wanting to use `std::holds_alternative<T>(v.data)` or `std::get<T>(v.data)` have to grep around to confirm "J" → `int64_t` not `uint64_t`. A 9-line table in the comment makes the mapping self-documenting and matches `value_t`'s variant declaration (10811-10821).
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:11077-11086
- **suggested_change:** Expand the existing 4-line comment into a table: `"Z" → bool / "B" → int8_t / "S" → int16_t / "I" → int32_t / "J" → int64_t / "F" → float / "D" → double / "C" → uint16_t / "L..." or "[..." → uint32_t (compressed OOP)"`. Add: "Null field_pointer returns int32_t{0} regardless of signature — see bug note."

### [XS] [USER_FACING] Add `static_assert`-style compile-time helpers `is_signed_primitive(sig)` for callers
- **rationale:** Users who want to template over field signatures (e.g. "all integer fields, do thing X") have to hand-roll the table. The header already defines `jvm_primitive_byte_width` (line 11395) which gives width — a sibling `jvm_primitive_is_signed_integer` returning true for B/S/I/J would let users compose. Low-effort, parity with the existing helper.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:11378-11410
- **suggested_change:** Add `inline auto jvm_primitive_is_signed_integer(std::string_view sig) noexcept -> bool` next to `jvm_primitive_byte_width`. Returns true iff `sig.size() == 1 && sig[0] is one of "BSIJ"`.

## Tests

### [standalone_unit] [new] test_field_proxy_get_primitive_int_long
- **description:** Mirror `test_field_proxy_set_size_guard` (test_helpers.cpp:1108). Allocate a stack buffer, place known byte patterns, construct `vmhook::field_proxy` with each integer signature, call `.get()`, and assert the returned variant holds the expected alternative and value.
- **asserts:**
  - "B" with byte `0xFF` → `value_t.data` holds `std::int8_t` and value is `-1` (sign extension on cast to int)
  - "B" with byte `0x80` → `std::int8_t` value is `INT8_MIN` (-128)
  - "B" with byte `0x7F` → `std::int8_t` value is `INT8_MAX` (127)
  - "S" with bytes `{0xFF, 0xFF}` → `std::int16_t{-1}`; `{0x00, 0x80}` → `INT16_MIN`; `{0xFF, 0x7F}` → `INT16_MAX`
  - "I" boundaries: `INT32_MIN`, `INT32_MAX`, `0`, `-1`
  - "J" boundaries: `INT64_MIN`, `INT64_MAX`, `0`, `-1` — verify all 8 bytes are read (sentinel bytes at offsets 8..15 stay 0xAB)
  - "I" sentinel bytes at offset 4..7 stay 0xAB (proves `get()` reads exactly 4, not 8, bytes)
  - "S" sentinel bytes at offset 2..7 stay 0xAB (proves narrow read)
  - "B" sentinel bytes at offset 1..7 stay 0xAB
  - Null-field_pointer: `vmhook::field_proxy{ nullptr, "J", false }.get()` — currently returns `int32_t{0}` (this captures the bug above; either flip the assertion when the fix lands or assert via `holds_alternative<int64_t>` after the fix)
  - Cross-cast: `int64_t x = proxy_int.get();` for an "I" holding `INT32_MIN` should produce `int64_t(INT32_MIN)` (verifies sign-extension through `cast_for_variant<int64_t>` of an `int32_t` alternative)
  - Cross-cast: `uint32_t y = proxy_byte.get();` for a "B" holding `0xFF` should produce `0xFFFFFFFF` (signed-then-widened-then-unsigned semantics) — pins down current cast behaviour
- **existing_file:** tests/test_helpers.cpp (add alongside `test_field_proxy_set_size_guard` and register near line 1364)

### [standalone_unit] [new] test_field_proxy_get_bool_size
- **description:** Verify the "Z" branch reads exactly 1 byte. Allocate `std::array<std::uint8_t, 2> storage; storage[0] = 1; storage[1] = 0xAB;` and assert `get()` returns true and `storage[1]` stays `0xAB`. After the suggested fix, this test will pin the behaviour explicitly; before the fix it passes on all current platforms but documents the assumption.
- **asserts:**
  - storage[0]=1 → get() returns variant with `bool{true}`
  - storage[0]=0 → variant `bool{false}`
  - storage[1] still equals 0xAB (no over-read of the second byte even on ABIs where `sizeof(bool) > 1`)
- **existing_file:** tests/test_helpers.cpp

### [standalone_unit] [new] test_field_proxy_get_null_pointer_per_signature
- **description:** Construct `vmhook::field_proxy{ nullptr, sig, false }` for each of "B","S","I","J","F","D","Z","C","Ljava/lang/String;","[I" and assert that `.get()` returns a value whose conversion to the natural C++ type is the zero/false/empty value. Also assert `.get().signature == sig` so the signature round-trips.
- **asserts:**
  - For each sig in the list: `T x = fp.get();` produces `T{}` (zero/false/empty)
  - `fp.get().signature == sig`
  - For "Ljava/lang/String;": `std::string{ fp.get() } == ""` (verifies the null-path doesn't decode a garbage OOP)
  - For "[I": `std::vector<int>{ fp.get() }.empty()`
- **existing_file:** tests/test_helpers.cpp

### [jvm_integration] [new] test_field_proxy_get_int_long_boundary_jvm
- **description:** End-to-end via a fixture Java class with `static int s_int_min = Integer.MIN_VALUE; static int s_int_max = Integer.MAX_VALUE; static long s_long_min = Long.MIN_VALUE; static long s_long_max = Long.MAX_VALUE; static byte s_byte = (byte)-1; static short s_short = (short)-1;`. Use `static_field("s_int_min")->get()` etc. and assert correct sign-extended values are returned in C++.
- **asserts:**
  - `int  v = obj.static_field("s_int_min")->get();` equals `std::numeric_limits<int>::min()`
  - `long v = obj.static_field("s_long_max")->get();` equals `std::numeric_limits<long long>::max()`
  - `int  b = obj.static_field("s_byte")->get();` equals -1 (sign extension through the variant)
  - `int  s = obj.static_field("s_short")->get();` equals -1
- **existing_file:** new fixture file example/vmhook/FieldGetBoundary.java + new driver hook in vmhook/src/example.cpp

## Parity Concerns
- `field_proxy::get()` falls through to the reference branch on unknown signatures with no log, while `field_proxy::set()` emits `VMHOOK_LOG` for size mismatches and `static_assert` for unsupported types — get/set logging policy is asymmetric.
- `field_proxy::value_t::data` does not include `std::string` as a variant alternative (only `std::uint32_t` for compressed OOPs), while `method_proxy::value_t::data` does (vmhook.hpp:11452). String fields therefore go through a deferred `read_java_string` in `cast_for_variant`, while string return values are eagerly decoded. Not a bug, but the asymmetry forces field-vs-method callers into different `std::holds_alternative` patterns.
- The null-`field_pointer` early-return always uses `std::int32_t{}` as the variant alternative regardless of signature; `method_proxy` value_t uses `std::monostate` for "no value" and `value_t{ std::monostate{} }` for early-return errors (e.g. line 11942). Field path could adopt the same convention — but that would require adding `std::monostate` to the field variant, which is a wider change.
- `field_proxy::get_compressed_oop()` (line 11303) special-cases the reference read path with an explicit accessor; there's no equivalent `get_raw_int64()` / `get_raw_int32()` for users who already know the signature and want to skip the variant. Compare with `method_proxy` which has no such bypass either — but if `get_compressed_oop` is justified, parallel primitive accessors might be too.
