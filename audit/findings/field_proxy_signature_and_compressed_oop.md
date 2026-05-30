# field_proxy_signature_and_compressed_oop

## Summary
Audited `field_proxy::signature()`, `field_proxy::is_static()`, `field_proxy::get_compressed_oop()` and the narrow-klass header read at offset 8 used by sibling helpers. The introspection getters themselves are tiny and correct, but `get_compressed_oop()` silently misbehaves on two real configurations — primitive-typed fields (it returns 4 bytes of garbage with no signature check) and JVMs run with `-XX:-UseCompressedOops` (where the slot is 8 bytes wide and the upper 32 bits are truncated). The narrow-klass reads at three sites (12376, 9865, 13361) hardcode the 8-byte mark-word offset and assume `UseCompressedClassPointers=true`; they will produce a 4-byte read of a 64-bit pointer's low half on JVMs run with that flag disabled.

## Bugs

### [medium] get_compressed_oop silently returns garbage on primitive fields
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:11299-11313
- **description:** The doc-comment says "Returns the compressed OOP stored in this field (for reference/array types)" but the body unconditionally memcpys 4 bytes from `field_pointer` regardless of `signature_text`. If a user calls `get_compressed_oop()` on an "I" / "F" field (4 bytes — looks plausible) they receive the raw int/float bit pattern reinterpreted as a compressed OOP; passing that to `decode_oop_pointer()` then dereferences `narrow_oop_base + (garbage << shift)` and either returns a bogus pointer or, more dangerously, a pointer that happens to land inside the heap and gets dereferenced downstream. For "J" / "D" (8-byte) primitives the function reads only the low 4 bytes, which is wrong in a different way. There is no `VMHOOK_LOG` warning and no guard that the descriptor starts with `L` or `[`.
- **repro:** `vmhook::field_proxy fp{ &some_int_storage, "I", false }; auto raw = fp.get_compressed_oop(); auto* p = vmhook::hotspot::decode_oop_pointer(raw); // p is bogus, no warning logged`
- **suggested_fix:** Add an early signature check:
  ```cpp
  if (this->signature_text.empty() ||
      (this->signature_text[0] != 'L' && this->signature_text[0] != '['))
  {
      VMHOOK_LOG("{} field_proxy::get_compressed_oop: called on non-reference field "
                 "(sig='{}') - returning 0. Use get() for primitive fields.",
                 vmhook::error_tag, this->signature_text);
      return 0;
  }
  ```
- **confidence:** certain

### [medium] get_compressed_oop truncates to 4 bytes when UseCompressedOops is disabled
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:11303-11313
- **description:** When the JVM runs with `-XX:-UseCompressedOops` (forced for heaps > 32 GB, or when the user disables it explicitly), reference fields are 8 bytes wide and store a raw 64-bit heap pointer instead of a compressed value. `get_compressed_oop()` memcpys `sizeof(value) == 4` and silently truncates the high 32 bits. `decode_oop_pointer()` then treats the truncated value as compressed and re-applies base/shift, producing a pointer that is neither the original nor a valid heap address. The same truncation exists in `field_proxy::get()` at lines 11144-11147 (the reference-type branch) — both paths share the assumption. The header is documented to be HotSpot-only and the README repeatedly says "compressed OOP", but no compile-time or runtime check rejects the misconfiguration.
- **repro:** Run the host with `-XX:-UseCompressedOops` and read any reference field via `field_proxy::get_compressed_oop()`. On a heap that lives above the 4 GB boundary (common), the returned value is the low half of a 64-bit pointer, decoded back to a wild address.
- **suggested_fix:** Query a cached `use_compressed_oops()` helper (analogous to the existing CompressedOops VMStruct lookups in `decode_oop_pointer`) at first call, and either (a) read 8 bytes and return the full pointer encoded back via `encode_oop_pointer`, or (b) log a one-time error with `vmhook::warning_tag` explaining that the library currently requires `UseCompressedOops=true`. Adding the same guard to `get()` (lines 11144-11147) prevents the same bug there.
- **confidence:** likely

### [medium] narrow-klass read hardcodes offset 8 and assumes UseCompressedClassPointers=true
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:12367-12384
- **description:** `klass_from_object_header` (and its public twin `klass_from_oop` at 13354-13368, plus the inline copy in `jni_make_unique` at 9863-9866) reads a `std::uint32_t` from `oop + 8`. This is correct only when `UseCompressedClassPointers=true` (the default on x64). When that flag is off — common on large-heap or 32-bit-aligned-klass deployments — the slot at offset 8 is a full 8-byte `Klass*` and the 4-byte read returns only the low half. The decoded "klass" then points into nowhere. There is no VMStruct lookup of `oopDesc::_metadata` offset or of `UseCompressedClassPointers`; the value is hard-coded. Since `field_proxy::get_compressed_oop()` is the front door to this whole pipeline (most users decode it and then call `klass_from_oop` to introspect the runtime type), the bug propagates into every wrapper-resolution path.
- **repro:** Run with `-XX:-UseCompressedClassPointers`. Any `vmhook::klass_from_oop(field_oop(...))` call returns a bogus klass pointer; `is_valid_pointer()` may or may not flag it depending on where the truncated value lands.
- **suggested_fix:** At startup, cache `use_compressed_class_pointers` from VMStructs (`Universe::_narrow_klass` exists only when the flag is on; absence is a reliable signal). When the flag is off, return `*reinterpret_cast<vmhook::hotspot::klass**>(oop + 8)` directly without decoding. The offset itself (8) is stable on 64-bit HotSpot regardless of the flag.
- **confidence:** likely

## Improvements

### [XS] [USER_FACING] Add field_proxy::name() for parity with method_proxy::name()
- **rationale:** `method_proxy` exposes `name()` (line 12298) but `field_proxy` only has `signature()`. Users who pass a `field_proxy` through a callback chain currently have no way to recover the originating field name for diagnostics, log messages, or generic visitor code. The name is already known at construction time inside `get_field` — just plumb it through.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:11070-11075 (ctor), 11315-11319 (members), 11265-11269 (parity neighbour), plus all 4 construction sites: 12845, 12855, 12908, 13452, 13459, 13786, 13793
- **suggested_change:** Add a `std::string field_name;` member, take `std::string_view name` as a ctor parameter, store it, and expose `auto name() const noexcept -> std::string_view`. Update each `field_proxy{ ..., entry->signature, ... }` site to pass the field name (which is already in scope as the `name` parameter of `get_field`).

### [XS] [USER_FACING] Document and warn that signature()'s string_view lifetime tracks the proxy
- **rationale:** `signature()` (line 11265-11269) returns `std::string_view` pointing into `signature_text`. The current docblock says nothing about lifetime. A user who writes `auto sig = obj->get_field("x")->signature();` on an rvalue optional accidentally dangles. Documenting it cheaply, and even cheaper: return `const std::string&` which makes the lifetime model explicit (no rvalue temptation) and matches `method_proxy::signature()` which has the same issue at 12311-12315.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:11262-11269
- **suggested_change:** Either change return type to `const std::string&`, or add an `@warning` to the docblock: "Returned view is invalidated when the field_proxy is destroyed; copy to std::string if storing".

### [S] [USER_FACING] Add field_proxy::is_reference() / is_primitive() / is_array() / byte_width() helpers
- **rationale:** Users who get a `field_proxy` of unknown type currently have to either compare `signature()` to literal strings ("L", "[", "I", ...) themselves or call `get()` and inspect the variant. Both are error-prone and duplicate logic that already exists in `jvm_primitive_byte_width` (line 11395-11410). Three one-liners would round out the API and let callers safely guard `get_compressed_oop()` themselves (preventing the bug above).
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:11262-11313 (sibling getters), 11395-11410 (width helper to reuse)
- **suggested_change:**
  ```cpp
  auto is_reference() const noexcept -> bool {
      return !signature_text.empty() &&
             (signature_text[0] == 'L' || signature_text[0] == '[');
  }
  auto is_primitive() const noexcept -> bool {
      return vmhook::detail::jvm_primitive_byte_width(signature_text) != 0;
  }
  auto is_array() const noexcept -> bool {
      return !signature_text.empty() && signature_text[0] == '[';
  }
  auto byte_width() const noexcept -> std::size_t {
      const auto prim{ vmhook::detail::jvm_primitive_byte_width(signature_text) };
      return prim ? prim : std::size_t{ 4 };  // 4 = compressed OOP; revisit when UseCompressedOops=false support lands
  }
  ```

### [XS] [USER_FACING] Add field_proxy::decoded_oop() convenience
- **rationale:** Every caller of `get_compressed_oop()` immediately pipes the result through `vmhook::hotspot::decode_oop_pointer()` (see vmhook/src/example.cpp:2826-2828, and the `field_oop` free function at 14532-14535). A one-line wrapper would remove the boilerplate and provide a natural place to consolidate the eventual `UseCompressedOops=false` fix.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:11299-11313, 14520-14535
- **suggested_change:** `auto decoded_oop() const noexcept -> void* { return vmhook::hotspot::decode_oop_pointer(get_compressed_oop()); }` plus an `@see` from the free `field_oop()` helper.

### [XS] [INTERNAL] Rename method_proxy::static_field member to static_method for clarity
- **rationale:** `method_proxy` reuses the identifier `static_field` (line 12649) as the bool flag for "this method is static". Across grep results (line 12172, 12329) this confuses readers — `static_field` reads as "a static field". `field_proxy` already uses the same name (line 11318), which is correct there. The collision is mild but real.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:12172, 12329, 12649
- **suggested_change:** Rename the `method_proxy` member to `static_method`; leave `field_proxy::static_field` alone.

## Tests

### [standalone_unit] [new] test_field_proxy_signature_returns_descriptor
- **description:** Construct a `field_proxy` directly with each JVM descriptor ("Z", "B", "S", "I", "J", "F", "D", "C", "Ljava/lang/String;", "[I", "[Ljava/lang/Object;") and assert `signature()` round-trips the exact bytes. Also assert the returned `string_view` matches `signature_text.data()` of the underlying member (no copy / re-allocation).
- **asserts:** `proxy.signature() == "I"`, `.size()`, equality across all 11 descriptors above.

### [standalone_unit] [new] test_field_proxy_is_static_round_trip
- **description:** Two trivial proxies: one with `is_static_flag=true`, one with `false`. Assert `is_static()` returns the constructor input verbatim, including on default-constructed (`field_pointer=nullptr`) instances.
- **asserts:** `static_proxy.is_static() == true`, `inst_proxy.is_static() == false`, both noexcept-callable.

### [standalone_unit] [new] test_field_proxy_get_compressed_oop_null_returns_zero
- **description:** Pin down the documented null-pointer contract by constructing a `field_proxy{ nullptr, "Ljava/lang/String;", false }` and asserting `get_compressed_oop()` returns 0 without crashing or logging an error. Already partially covered by the API-surface test, but no value is asserted there.
- **asserts:** `proxy.get_compressed_oop() == 0u`.
- **existing_file:** tests/test_api_surface.cpp:116-130 (could extend, or add fresh test in test_helpers.cpp).

### [standalone_unit] [new] test_field_proxy_get_compressed_oop_reads_4_bytes_at_pointer
- **description:** Allocate a 16-byte stack buffer with a known 4-byte sentinel at the start and 0xDEADBEEF after the slot. Construct `field_proxy{ buf.data(), "Ljava/lang/String;", false }` and assert `get_compressed_oop()` returns exactly the first 4 bytes (little-endian). Confirms no over-read into adjacent bytes.
- **asserts:** Returned value bit-equal to sentinel; sentinel after the slot untouched.

### [standalone_unit] [new] test_field_proxy_get_compressed_oop_on_primitive_field_warns_or_zeros
- **description:** Targets bug #1 above. With the suggested fix applied, construct `field_proxy{ &storage, "I", false }`, call `get_compressed_oop()`, and assert it returns 0 and emits a `VMHOOK_LOG` line containing "non-reference field". Without the fix this would currently silently return the raw int bits.
- **asserts:** Return == 0; log capture (via existing `VMHOOK_LOG_FILE` opt-in) contains the warning substring.

### [jvm_integration] [extend_existing] test_field_proxy_get_compressed_oop_decodes_to_string
- **description:** Wraps and tightens the existing `readJavaStringValue` block in `example.cpp:2810-2834` into an explicit named test: read `notStaticString` via `get_compressed_oop()`, decode via `decode_oop_pointer`, assert non-null decoded pointer AND non-empty `read_java_string` AND exact string match.
- **asserts:** `raw != 0`, `decoded_ptr != nullptr`, decoded string == "cppwins!".
- **existing_file:** vmhook/src/example.cpp:2810-2835

### [jvm_integration] [new] test_field_proxy_get_compressed_oop_static_field
- **description:** Resolve a static String field through the `static get_field(type_index, name)` overload (line 12873-12909), confirm `is_static()` returns true, confirm `get_compressed_oop()` returns a non-zero value, and decode it. Ensures the static path (field_pointer = mirror + offset) produces correct compressed OOPs.
- **asserts:** `proxy.is_static() == true`, `proxy.get_compressed_oop() != 0`, decoded String round-trips.

### [jvm_integration] [new] test_field_proxy_signature_matches_jvm_descriptor_for_all_primitive_types
- **description:** Walks every field of a fixture class containing one field per primitive plus a String and an int[]. For each, assert `signature()` equals the exact JVM descriptor that `javap -s` would print.
- **asserts:** `Z` / `B` / `S` / `I` / `J` / `F` / `D` / `C` / `Ljava/lang/String;` / `[I` all match.

## Parity Concerns
- `method_proxy` has `name()` (line 12298); `field_proxy` does not — once a proxy is in hand the field name is irrecoverable.
- `method_proxy::signature()` (line 12311) and `field_proxy::signature()` (line 11265) both return `std::string_view`; neither documents lifetime. Fix both together.
- `method_proxy::get_compressed_oop()` (line 12341) reads the receiver's first 4 bytes; `field_proxy::get_compressed_oop()` (line 11303) reads the field's first 4 bytes. They share the same `-XX:-UseCompressedOops` truncation bug and the same lack of signature/null-receiver guard nuance — any fix should land in both.
- `method_proxy::is_static()` (line 12326) is backed by member `static_field` (misnamed — it's a method flag); `field_proxy::is_static()` (line 11293) is correctly backed by `static_field`. The collision makes greps noisy.
- `field_proxy` has no `is_primitive()` / `is_reference()` / `is_array()` introspection helpers; `method_proxy` has `name()`, `signature()`, `is_static()` and is reasonably complete. Field-side feels half-finished by comparison.
