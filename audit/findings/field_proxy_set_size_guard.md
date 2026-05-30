# field_proxy_set_size_guard

## Summary
Audited the `field_proxy::set` size-mismatch guard (vmhook.hpp:11207-11248) and its
helper `vmhook::detail::jvm_primitive_byte_width` (vmhook.hpp:11395-11410). The guard
covers all eight JVM primitive descriptors (Z B C S I J F D) correctly, and the
`C + 1-byte char` widening shortcut is still in place ahead of the guard. The
top concern is that the guard only fires inside the `is_trivially_copyable_v`
branch — earlier `if constexpr` arms (`std::string`, `std::vector<...>`,
`std::unique_ptr<wrapper>`) dispatch purely on the C++ type and will happily try
to interpret a primitive field's bytes as a compressed OOP, which is a separate
silent-corruption class that the new guard does not catch.

## Bugs

### [high] String / vector / unique_ptr branches dispatch without consulting the JVM signature
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:11158-11206
- **description:** The `set()` template dispatches on the C++ value type alone:
  `std::string` -> `set_str_field`, `std::vector<T>` -> `set_*_array`,
  `std::unique_ptr<wrapper>` -> compressed-OOP write, with no check that the
  field's `signature_text` is actually a reference / array. If a user calls
  `field.set(std::string{"42"})` on an `"I"` field, the string branch fires,
  `set_str_field` calls `field_oop(*this)` which reads the field's 4 bytes as
  a compressed OOP (`get_compressed_oop`, vmhook.hpp:11303-11309), decodes it
  via `decode_array_oop`, and hands the result to `write_java_string`. The
  bytes interpreted as a compressed OOP can decode to any heap address; the
  `is_valid_pointer` check in `write_java_string` (vmhook.hpp:14558) is the
  only thing standing between this and a wild write. The new size-mismatch
  guard explicitly does not cover this — its block is unreachable for these
  branches. Same story for `set(std::vector<int>{...})` on `"I"`,
  `set(std::unique_ptr<MyClass>{...})` on `"D"`, etc.
- **repro:**
    ```cpp
    std::array<std::uint8_t, 4> storage{ 0x12, 0x34, 0x56, 0x78 };
    vmhook::field_proxy proxy{ storage.data(), "I", false };
    proxy.set(std::string{"hi"});  // bytes 0x78563412 interpreted as cOOP,
                                   // forwarded into write_java_string
    ```
- **suggested_fix:** Mirror the trivially-copyable guard at the top of each
  non-primitive branch. The same `jvm_primitive_byte_width` helper is the
  right oracle in reverse: if it returns non-zero, the field is a primitive
  and the string / vector / unique_ptr branches should refuse the write with
  a diagnostic. e.g. inside the `std::string` arm:
    ```cpp
    if (vmhook::detail::jvm_primitive_byte_width(this->signature_text) != 0) {
        VMHOOK_LOG("{} field_proxy::set(std::string): refusing write to "
                   "primitive field (sig='{}'). Pass a numeric primitive instead.",
                   vmhook::error_tag, this->signature_text);
        return;
    }
    ```
- **confidence:** certain

### [medium] unique_ptr<wrapper> branch writes 4 bytes unconditionally — wrong on uncompressed-OOP HotSpot
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:11187-11206
- **description:** The `is_unique_ptr_v` arm always encodes the wrapper as a
  4-byte compressed OOP (`encode_oop_pointer` -> `uint32_t compressed`) and
  memcpys `sizeof(compressed) == 4` bytes into the field slot. On HotSpot with
  `-XX:-UseCompressedOops` (default once Java heap exceeds 32 GB, or with
  large object alignment) reference fields are 8 bytes wide. Writing only the
  low 4 bytes leaves the upper 4 unchanged (and `encode_oop_pointer` returns
  an invalid narrow encoding on a system that doesn't use it). The size guard
  in the primitive arm is the *only* width-aware writer in the function;
  reference writes have no equivalent.
- **repro:** Run any field-write under a JVM started with
  `-XX:-UseCompressedOops` (or any heap > 32 GB). `set_str_field` works because
  it goes through the array-element writer, but `set(unique_ptr<T>{...})`
  corrupts the upper 4 bytes of the reference slot.
- **suggested_fix:** Route the OOP write through a helper that consults
  `vmhook::hotspot::is_compressed_oops_enabled()` (or the equivalent flag this
  codebase already reads) and writes either 4 or 8 bytes. At minimum add a
  diagnostic `VMHOOK_LOG` when narrow-OOP encoding is requested on an
  uncompressed-OOP runtime so users can see why their writes look correct in
  hex but broken in Java.
- **confidence:** likely

## Improvements

### [S] [USER_FACING] Make field_proxy::set return bool so callers can detect a rejected write
- **rationale:** `method_proxy::call` returns a `value_t` and callers can
  observe failures (return-by-value or std::optional in surrounding code).
  `field_proxy::set` is `void` — the only signal the guard gives is a log
  line, which the user explicitly flagged as a parity gap. Returning `bool`
  (or `[[nodiscard]] bool`) lets callers `if (!proxy.set(value)) { ... }` and
  still keeps the noexcept signature. Existing call sites that ignore the
  return continue to compile.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:11158-11260
- **suggested_change:** Change the trailing return type to `bool`, return
  `false` from every early-return path (`!field_pointer`, size-mismatch
  rejection, unsupported branches if you ever lift the static_assert) and
  `true` after a successful memcpy / `set_*` delegate. Leave existing call
  sites unchanged — they implicitly discard.

### [XS] [USER_FACING] Diagnostic message should include field address and is_static
- **rationale:** The current log (vmhook.hpp:11241-11244) prints
  `value=NB, field=MB, sig='X'` but no field address, no static/instance
  flag, and no calling context. In a real game where dozens of fields share
  the same signature, the user can't tell *which* field they mis-typed.
  `method_proxy` logs include method name + signature on every error.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:11241-11244
- **suggested_change:** Append `field_pointer` (formatted as
  `0x{:016X}`) and `static_field ? "static" : "instance"` to the diagnostic.
  e.g.:
  ```
  "{} field_proxy::set: size mismatch at 0x{:016X} ({} field, value={}B,
   field={}B, sig='{}') ..."
  ```

### [S] [USER_FACING] Match-by-signature compile-time helper for known field types
- **rationale:** The signature is only known at runtime today, but for a
  large fraction of real use the user *does* know the field type at compile
  time (they wrote `vmhook::find_field("foo")` and immediately followed up
  with `set(int32_t{...})`). A helper like
  `field_proxy::set_as<"I">(std::int32_t{...})` could fail at compile time
  when the C++ argument type doesn't match the descriptor. Today the user
  only learns at runtime via the new guard.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:11158-11260
- **suggested_change:** Add a non-template overload-set keyed on a
  signature string-literal template parameter (`template<vmhook::sig_literal Sig>`).
  Optional; the runtime guard already prevents the worst harm.

### [XS] [INTERNAL] Move the descriptor table into a single constexpr lookup shared with sig_char_to_basic_type
- **rationale:** `jvm_primitive_byte_width` (vmhook.hpp:11402-11409) and
  `sig_char_to_basic_type` (vmhook.hpp:11362-11376) both switch on the same
  primitive descriptor characters. Today they're two independent switches
  that could drift if a future contributor adds a new primitive (none
  imminent — JVM spec is stable — but the duplication is real).
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:11362-11410
- **suggested_change:** Extract a single `constexpr std::array<{char, basic_type, byte_width}>`
  table that both functions consult. Trivial; mostly a cleanup, no
  behaviour change.

### [XS] [USER_FACING] Update the static_assert message to mention `const char*` and array-of-char
- **rationale:** The static_assert message at vmhook.hpp:11252-11258 lists
  the accepted categories as "string / string_view / std::vector /
  unique_ptr<wrapper> / trivially_copyable" — but a user reading that and
  trying `set("hello")` would think they need to call `std::string{"hello"}`
  first. The `is_convertible_v<value_type, std::string_view>` arm at
  vmhook.hpp:11168-11171 actually accepts `const char*` directly; the
  diagnostic just doesn't say so.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:11252-11258
- **suggested_change:** Tweak the message to say
  `"... string / string_view / const char* / std::vector / unique_ptr<wrapper> / trivially_copyable ..."`.

## Tests

### [standalone_unit] [exists] test_field_proxy_set_size_guard
- **description:** Exercises the guard on int32->I (right-sized), int64->I
  (mismatch refused), int32->J (mismatch refused), char->C widening,
  uint32->ref (size-0 skip), and null field_pointer. Covers all eight
  primitive widths via test_jvm_primitive_byte_width below.
- **asserts:** field bytes unchanged on rejection; trailing sentinel bytes
  preserved when the guard rejects a too-wide write; widening shortcut still
  produces a 16-bit value in the slot.
- **existing_file:** tests/test_helpers.cpp:1108-1190

### [standalone_unit] [exists] test_jvm_primitive_byte_width
- **description:** Direct exercise of the width helper for every primitive
  descriptor plus reference / array / void / empty / unknown / multichar.
- **asserts:** Z/B == 1, S/C == 2, I/F == 4, J/D == 8, every other input
  returns 0.
- **existing_file:** tests/test_helpers.cpp:1067-1096

### [standalone_unit] [extend_existing] test_field_proxy_set_string_into_primitive_is_refused
- **description:** Regression for the high-severity bug above. Create an
  "I"-typed `field_proxy` over a stack buffer, call `set(std::string{"42"})`,
  and confirm the field bytes are unchanged and a diagnostic is logged.
  Same for `set(std::vector<int>{1, 2})` and
  `set(std::unique_ptr<test_wrapper>{...})` on a primitive field. Today
  these all *try* to write a string / array / OOP through the wrong code
  path; the test would catch it once the matching guards are added.
- **asserts:** stack buffer bytes (and trailing sentinels) are unchanged
  after each mistyped set call.
- **existing_file:** tests/test_helpers.cpp (extend after the existing
  `test_field_proxy_set_size_guard`).

### [standalone_unit] [extend_existing] test_field_proxy_set_size_guard_all_widths
- **description:** The existing test covers I and J but not Z, B, S, C, F,
  D end-to-end. Add a small parameterised loop that builds a `field_proxy`
  per primitive descriptor and verifies (a) the matching-width C++ type
  writes successfully and (b) every other-width C++ type is rejected
  without touching adjacent sentinels.
- **asserts:** For each descriptor (Z, B, S, C, I, J, F, D), a memcpy of
  the exact width writes the field; all wider / narrower types are
  rejected; sentinel guard bytes are preserved on rejection.
- **existing_file:** tests/test_helpers.cpp:1108-1190

### [standalone_unit] [new] test_field_proxy_set_widening_for_byte_typed_values
- **description:** The `"C" + sizeof == 1` widening fires for any 1-byte
  trivially-copyable type, not just `char`. Verify the behaviour for
  `std::int8_t{-1}` (lands as 0x00FF, the zero-extended cast) and
  `std::uint8_t{0xFF}` (also 0x00FF). Document the intent and pin it down
  so a future contributor can't tighten the check to `is_same_v<char>` and
  silently break callers that pass `unsigned char`.
- **asserts:** A 1-byte write to a "C" field always lands as a 16-bit
  value with the high byte zero, regardless of which 1-byte arithmetic
  type was passed.

### [standalone_unit] [new] test_field_proxy_set_returns_bool_after_signature_change
- **description:** If the suggested API change to return `bool` is taken,
  add a test that the size-mismatch path returns `false`, the null-pointer
  path returns `false`, and the success path returns `true`. Without this
  the new return value would never be exercised in CI.
- **asserts:** Return values match the documented contract for each
  early-exit and the happy path.

### [jvm_integration] [new] test_field_proxy_set_uncompressed_oops
- **description:** Re-run the `example.cpp` field-write integration with
  `-XX:-UseCompressedOops` on the launched JVM. Confirms that the
  medium-severity bug (4-byte write to an 8-byte reference slot) is fixed
  once the suggested width-aware write lands. Today this scenario is not
  exercised by any test.
- **asserts:** A reference field updated via `set(unique_ptr<T>{...})` on
  an uncompressed-OOP JVM reads back through `get_as<T>()` to the same
  wrapper instance.

## Parity Concerns
- `field_proxy::set` is `void` while `method_proxy::call` returns a typed
  `value_t` that surfaces success/failure; users have no programmatic way
  to detect a rejected `set` even though the guard exists.
- Diagnostic logging in `method_proxy` includes method name + signature on
  every error path; `field_proxy::set`'s diagnostic only has value size,
  field size, and signature — no field address, no static/instance flag,
  and no caller-supplied tag.
- `method_proxy::call` performs argument-type checks against the resolved
  Java method's signature (see vmhook.hpp:11571 onward for sig parsing);
  `field_proxy::set` only checks size, not type. A `set(float{...})` into
  an `"I"` field passes the size guard (both 4 bytes) and writes the
  IEEE-754 bit pattern into the int slot — a category of bug the guard
  was never designed to catch.
- The dispatch order in `field_proxy::set` favours C++ value-type
  branches (`std::string`, `std::vector`, `std::unique_ptr<wrapper>`)
  ahead of the trivially-copyable + signature-aware branch, but only the
  last one knows about the JVM signature. `method_proxy`'s equivalent
  per-arg packer consults the JVM signature first. Aligning the
  dispatch order — or adding parallel size guards in the earlier arms —
  would match the sibling API's safety posture.
