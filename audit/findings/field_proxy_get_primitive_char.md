# field_proxy_get_primitive_char

## Summary
Audited `field_proxy::get()` for the JVM `C` (Java `char`, UTF-16) descriptor at vmhook.hpp:11137-11142, together with the variant-to-target conversion path in `value_t::cast_for_variant` (10943-11009) and the implicit operator (11025-11033). The 2-byte unsigned load itself is correct, but the conversion path silently narrows every UTF-16 code unit to a C++ `char` (8 bits, often signed) without a guard or diagnostic, and the documented `field_proxy::get_as<T>()` API actually does not exist — the example wrapper and round-trip test only cover ASCII (`'A'`, `'B'`), which hides the bug.

## Bugs

### [high] Silent narrowing of Java char to C++ char when target type is one byte
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:11001-11008 (cast_for_variant fallback), reached from 11025-11033 (implicit conversion) for a value loaded at 11137-11142.
- **description:** `get()` correctly loads a `std::uint16_t` from the field (11137-11142). When a wrapper writes `auto get_not_static_char() -> char { return get_field("notStaticChar")->get(); }` (see vmhook/src/example.cpp:548-552), the implicit-conversion operator (11026) instantiates `cast_for_variant<char, std::uint16_t>` and falls through to the generic `static_cast<target_type>(value)` branch at 11003. That cast is well-formed (it does not trip `-Wnarrowing` because the cast is explicit), but every Java `char` whose value exceeds 0x7F is silently truncated: e.g. `'é'` (é, 0xE9) yields a negative `char` on signed-char platforms, and any BMP character above 0xFF (`'中'` etc.) loses its high byte entirely. There is no log line and no test caught it because example.cpp:1543/1582/1592 only round-trips `'A'`/`'B'`. By contrast `value_t::append_array_value` for `[C` arrays (10856-10867) uses the same lossy `static_cast<char>` for the array case, so `std::vector<char>` from a Java `char[]` exhibits identical truncation — making this a systemic data-loss bug, not a one-off.
- **repro:** Set a Java field `public char highChar = '中';` Run `get_field("highChar")->get()` and assign the value to a C++ `char`. Read back: you get the low byte (0x2D, `'-'`) instead of the original code unit. Same for a `char[]` field round-tripped through `std::vector<char>`. No warning/log fires.
- **suggested_fix:** Either (a) document loudly that `char` target = lossy ASCII-only and require `char16_t`/`std::uint16_t` for full fidelity (the matching path the type-traits at 12449-12460 already mark as the canonical map), and emit a `VMHOOK_LOG` at runtime when the loaded `uint16_t` value > 0x7F and target is `char`; or (b) better: add an explicit `cast_for_variant<char16_t, std::uint16_t>` short-circuit that passes through losslessly, and make `cast_for_variant<char, std::uint16_t>` log a single-shot warning when the value would lose bits. Also extend `append_array_value(std::vector<char>&, …, "[C")` at 10859-10862 with the same warning, plus add a `std::vector<char16_t>` overload so users have a non-lossy path.
- **confidence:** certain

### [medium] Documented field_proxy::get_as<T>() does not exist
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:1447 and 6821 (both comments reference `field_proxy::get_as<T>()`), versus the actual class body 10786-11319 which exposes only `get()`, `set()`, `signature()`, `raw_address()`, `is_static()`, `get_compressed_oop()`.
- **description:** Two prominent doc blocks describe `field_proxy::get_as<T>()` as the API that constructs typed wrappers from the factory map. No such member is defined. Users following the docs hit a compile error and have to guess that the actual API is `static_cast<std::unique_ptr<T>>(get_field("x")->get())` via the implicit-conversion operator. This is a real user-friendliness gap (also called out by the user as a method_proxy vs field_proxy parity gap — `method_proxy::call_as<T>` exists, `field_proxy::get_as<T>` does not).
- **repro:** Try `auto p = get_field("foo")->get_as<my_wrapper>();` — compile error. Compare with the matching `method_proxy::call<T>` ergonomics in 12700-12728.
- **suggested_fix:** Add a thin templated `get_as<T>()` that forwards to the implicit operator: `template<typename T> auto get_as() const noexcept -> T { return static_cast<T>(this->get()); }`. Special-case `char` here too (log on the high-bit truncation).
- **confidence:** certain

## Improvements

### [XS] [USER_FACING] Short-circuit char16_t and uint16_t fast-paths in cast_for_variant
- **rationale:** The current dispatch for a `C` field always allocates the visitor lambda and then falls through to a generic `static_cast`. For the canonical `char16_t`/`uint16_t` targets (which the write-side type traits at 12449-12460 advertise as the right way to express Java `char`), the cast is a no-op. An explicit branch makes the lossless path obvious in code review and lets a sibling branch warn for the lossy `char` path without confusing the lossless path.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:10989-11008
- **suggested_change:** Add `else if constexpr (std::is_same_v<clean_target_type, char16_t>) { if constexpr (std::is_same_v<clean_source_type, std::uint16_t>) return static_cast<char16_t>(value); else return char16_t{}; }` immediately before the generic fallback. Document the surrogate semantics in the docstring at 10789-10808.

### [XS] [USER_FACING] Document surrogate semantics on the value_t variant docstring
- **rationale:** The doc comment at 10789-10808 lists every variant alternative but does not say that `uint16_t` is one UTF-16 code unit and that paired surrogates are NOT decoded — users converting `proxy->get()` to `std::u16string` or `std::string` from a single `char` field will be surprised. The `vector<char>` overload at 10849-10867 is also silent.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:10789-10808 and 10849-10867
- **suggested_change:** Add a single sentence to each docstring: "Java `char` is a UTF-16 code unit, not a Unicode code point — supplementary characters span two `char`s as a surrogate pair, and this proxy returns each code unit unchanged."

### [S] [USER_FACING] Add short helpers get_char()/get_string() etc. for parity with future call_as
- **rationale:** Wrappers currently look like `auto get_health() -> int { return get_field("h")->get(); }` — the implicit operator is clever but the return-type deduction silently picks `value_t` (not `int`) without a trailing return type, which has bitten users (see the prominent note at 10769-10775 in the docs). A small named-helper layer (`get_int_field("h")`, `get_char_field("h")`) would make wrapper code self-documenting and let `get_char_field` enforce the "did you mean `char16_t`?" check at compile time.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:11077-11148
- **suggested_change:** Add inline non-member helpers next to `set_str_field` (14619-14633) and `field_oop` (14532-…): `inline auto get_char_field(const field_proxy& f) -> char16_t { return static_cast<char16_t>(f.get()); }` and friends. Cross-reference from the class-level doc block.

### [XS] [INTERNAL] Reject "C" reads when the variant load returns the wrong width
- **rationale:** `get()` for `C` happily reads 2 bytes regardless of whether the JVM signature actually matches a 2-byte field — there is no parallel to the size-mismatch guard the set() side gained in v0.4.4 (11236-11246). A wrong-signature `field_proxy` (e.g. constructed with the wrong descriptor by an out-of-tree caller) silently reads junk for `C` instead of erroring.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:11087-11148
- **suggested_change:** Either add a debug-build-only `assert` that the field_pointer alignment matches the descriptor's width (using `jvm_primitive_byte_width` from 11395), or emit a one-shot log if the read is from a misaligned address. Low-effort defensive add.

## Tests

### [standalone_unit] [new] test_field_proxy_get_char_basic_ascii_roundtrip
- **description:** Construct a `field_proxy` over an 8-byte stack buffer with descriptor `"C"`, write a known `std::uint16_t` directly, call `get()`, verify the variant alternative is `std::uint16_t` and that converting to `int`, `std::uint16_t`, and `char16_t` returns 'A'.
- **asserts:** `proxy.get().data.index() == index_of<std::uint16_t>`; `static_cast<std::uint16_t>(proxy.get()) == 0x41`; `static_cast<char16_t>(proxy.get()) == u'A'`.
- **existing_file:** tests/test_helpers.cpp (sits alongside `test_field_proxy_set_size_guard` at 1108)

### [standalone_unit] [new] test_field_proxy_get_char_high_bit_narrowing_to_char
- **description:** Same setup, write `0x00E9` ('é'). Convert to `char` (the lossy path) and observe truncation; convert to `char16_t` and observe lossless. Documents and locks-in the current behaviour, so a future fix flips this from `EXPECT_TRUE(truncated)` to `EXPECT_FALSE`.
- **asserts:** `static_cast<char16_t>(proxy.get()) == u'é'`; `static_cast<unsigned char>(static_cast<char>(proxy.get())) == 0xE9` (truncation occurred — bug witness).
- **existing_file:** tests/test_helpers.cpp

### [standalone_unit] [new] test_field_proxy_get_char_bmp_above_0xff_truncates_to_low_byte
- **description:** Write `0x4E2D` (中). Confirms the high byte is silently dropped on the `char` path. Pinpoints the data-loss bug for BMP non-Latin code units.
- **asserts:** `static_cast<char16_t>(proxy.get()) == u'中'`; `static_cast<unsigned char>(static_cast<char>(proxy.get())) == 0x2D` (== `'-'`, the LSB).
- **existing_file:** tests/test_helpers.cpp

### [standalone_unit] [new] test_field_proxy_get_char_surrogate_high_unit
- **description:** Write `0xD83D` (high surrogate of 😀, U+1F600). Confirm `get()` returns the raw code unit unchanged with no decoding attempted.
- **asserts:** `static_cast<std::uint16_t>(proxy.get()) == 0xD83D`; `static_cast<char16_t>(proxy.get()) == 0xD83D`.
- **existing_file:** tests/test_helpers.cpp

### [standalone_unit] [new] test_field_proxy_get_char_null_field_pointer_returns_default_int32
- **description:** A `field_proxy{nullptr, "C", false}` should NOT crash and should fall through to the null-guard at 11090-11093. Surprise behaviour: the null branch returns `value_t{ std::int32_t{}, signature }` regardless of the actual descriptor — for `"C"` this means the variant alternative is `int32_t`, not `uint16_t`. This test pins that contract so a future refactor doesn't change it silently.
- **asserts:** `proxy.get().data.index()` equals the `int32_t` alternative index; conversion to `int` yields 0; conversion to `char16_t` also yields 0 (via the generic fallback).
- **existing_file:** tests/test_helpers.cpp

### [jvm_integration] [extend_existing] example_class_high_unicode_char_field_roundtrip
- **description:** Add a Java fixture field `public char highChar = '中';` (or static equivalent) and a wrapper `get_high_char() -> char16_t`. Round-trip through `verify_expected_values`. Today the example only ever uses `'A'` / `'B'` (vmhook/src/example.cpp:1533, 1543, 1582, 1592, 1602, 1612), which is exactly why the truncation bug has gone unnoticed.
- **asserts:** `check_equal("highChar", instance.get_high_char(), u'中');` Also assert that reading the same field into a `char` is lossy (documents the bug) until the fix lands.
- **existing_file:** vmhook/src/example.cpp and example/vmhook/Example.java

## Parity Concerns
- `method_proxy::call_as<T>()` exists as a documented API; `field_proxy::get_as<T>()` is referenced in two doc blocks (1447, 6821) but never defined (10786-11319). Users hit a compile error following the documentation.
- `field_proxy::set()` has a size-mismatch guard (11236-11246) and a special widening case for `char` -> `"C"` (11217-11222). The mirror `get()` (11087-11148) has neither a guard nor a narrowing-warning when a `"C"` value is converted back into a 1-byte `char` — asymmetric safety. Pair the two paths.
- Free helpers exist for the write side (`set_str_field`, `set_bool_array`, `set_prim_array`, `set_str_array` declared 1490-1503, defined 14619-…), but there are no matching `get_char_field` / `get_int_field` read helpers. Wrappers must rely on the implicit-conversion operator, which has bitten users in the past with `auto`-deduced return types (the docs warn about this at 10769-10775).
- Type traits used by the call/argument-matching machinery (12449-12460) explicitly call out that `char16_t`/`uint16_t` -> `"C"` is the canonical mapping for Java `char`. The `field_proxy::get()` documentation block (11077-11086) does not mention `char16_t` and shows only `uint16_t` — that inconsistency invites users to pick `char` and hit the silent-narrowing bug.
