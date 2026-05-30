# field_proxy_raw_address

## Summary
Audited `vmhook::field_proxy::raw_address()` at vmhook.hpp:11279-11282 (the one-line getter), its sole sibling `get_compressed_oop()` at 11303-11313, the field_pointer construction sites at 12844 / 12854 / 13451 / 13458, and the only public consumer `watch_static_field` at 15009. The getter itself is a trivial pointer return and is correct for both compressed-oop and uncompressed-oop layouts (because HotSpot field offsets are always byte offsets into the in-heap object layout, regardless of `UseCompressedOops`). The real concerns are not in the body of `raw_address` itself but in the documented contract: the doc claims watch_static_field is the only intended caller yet the pointer is happily handed out for instance fields too (where it is far more dangerous), and there is no warning that a GC compaction (G1/Shenandoah/ZGC) silently invalidates the pointer — particularly for the static case where the java.lang.Class mirror is a movable heap OOP.

## Bugs

### [medium] raw_address() returns silently stale pointer after GC compaction; doc gives no warning
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:11271-11282
- **description:** raw_address returns `mirror + offset` (static) or `instance + offset` (instance). On every compacting collector — G1 (default since JDK 9), Parallel, Shenandoah, ZGC — both the Class mirror and the instance are heap OOPs and can be relocated during the lifetime of the returned pointer. After a GC cycle the address points into freed/reused heap memory. watch_static_field installs a hardware DR breakpoint on that exact word; once the GC moves the mirror, the DR is now armed on memory that belongs to whatever object now occupies that address, so the user's callback fires on writes to unrelated fields (false positive) and silently misses writes to the actual static field (false negative). The library is already GC-aware elsewhere (see for_each_instance comments at line 6636-6647) so the omission here is inconsistent.
- **repro:** Run watch_static_field<C, int32_t>("counter") on a JDK with -XX:+UseG1GC, then force a full GC (e.g. by triggering memory pressure or calling System.gc() while -XX:-DisableExplicitGC). Class mirrors are not in the permgen on JDK 8+; they are regular movable heap OOPs. Writes to `counter` will no longer trigger the callback; writes to the new occupant of the old address will.
- **suggested_fix:** Two layers. (1) Document the staleness clearly in the raw_address comment (the current comment is silent on GC, unlike for_each_instance which is explicit). (2) Either pin the Class mirror via a JNI global reference inside watch_static_field for the lifetime of the watch_handle, or document that the watch is "best-effort across GC cycles". Pinning is the user-friendly fix because hardware DRs already have a 4-slot limit, so the extra JNI global ref is cheap.
- **confidence:** likely

### [low] raw_address() returns nullptr without a log when field_pointer is null, deferring the diagnostic to the caller
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:11279-11282
- **description:** raw_address has no null check and no log — it just returns whatever field_pointer is. In practice a null field_pointer means the constructing path (get_field) detected null mirror/instance and *should* have already returned nullopt before constructing the proxy (see 12839, 12848, 12901, 13447, 13454). So a null here implies someone hand-constructed a `field_proxy{nullptr, ...}` (which test_helpers.cpp:1187 does deliberately, and test_api_surface.cpp:125 does for compile-time API exercising). For those cases silent return is fine; but the public docstring claims "Exposed so that watch_static_field can install a hardware breakpoint on the slot" and doesn't tell users that the function can return null. The caller in watch_static_field does check (15010-15014), but third-party callers building their own DR/MMIO tooling on top of raw_address have no contract to guide them.
- **repro:** `vmhook::field_proxy{nullptr, "I", true}.raw_address()` returns nullptr; the trailing "/// @return" line in the doc block omits the null case.
- **suggested_fix:** Add a `@return` clause: `@return Direct pointer to the field bytes, or nullptr if the proxy was constructed with a null base (e.g. instance was null at field-resolution time).` Optionally add `Complexity: O(1). Exception safety: noexcept.` for parity with the surrounding accessors (signature(), is_static(), get_compressed_oop() all have or used to have similar tags).
- **confidence:** certain

## Improvements

### [XS] [USER_FACING] Add Complexity / Exception-safety tags for parity with sibling accessors
- **rationale:** `is_static()` at 11293-11297 has explicit "Complexity: O(1). Exception safety: noexcept." tags; `signature()` at 11265-11269 and `get_compressed_oop()` at 11303-11313 are described but missing the tags. raw_address is the most "advanced" / unsafe accessor and conspicuously omits any safety information. Adding the tags makes the API surface consistent and signals to users that the function itself is cheap and noexcept (the danger is in what they do with the pointer).
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:11271-11282
- **suggested_change:** Extend the doc comment with `Complexity: O(1).  Exception safety: noexcept.  @return The raw pointer, or nullptr if the proxy holds a null base.  @warning The returned address is invalidated by any GC that may relocate the backing object/Class mirror.  Pin the receiver (JNI global ref or DisableExplicitGC) if the pointer must outlive arbitrary user code.`

### [S] [USER_FACING] Add raw_method() to method_proxy for symmetry, or document why only field_proxy exposes raw_address
- **rationale:** Sibling method_proxy at 11430 has `name()` (12298), `signature()` (12311), `is_static()` (12326), `get_compressed_oop()` (12341) — almost the same accessor set as field_proxy at 11265-11313 — but no `raw_method()` / `underlying_method_ptr()` returning the HotSpot `vmhook::hotspot::method*`. Users who want to install an interpreter / c2i hook themselves (without going through hook<>()) cannot reach the Method* through the public API. The user explicitly called out method_proxy vs field_proxy parity. Either add raw_method() or note in raw_address's doc that the same hatch is intentionally not exposed for methods (and why — e.g. "use hook<>() instead").
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:11430-12495 (method_proxy class body)
- **suggested_change:** Add `auto raw_method() const noexcept -> vmhook::hotspot::method* { return this->method; }` to method_proxy, with a doc comment that mirrors raw_address's tone ("exposed so users can drive low-level HotSpot APIs; most code should prefer call()").

### [S] [USER_FACING] Expose a name() accessor on field_proxy for parity with method_proxy::name()
- **rationale:** method_proxy::name() at 12298-12306 lets users print the method they have a handle on. field_proxy has signature() but no name() — debugging "the wrong field" bugs is harder than it should be. The field name is known at construction time (it's the argument passed to get_field). Storing one extra std::string per proxy is cheap. Without this, a user logging "raw_address of <something>" has to remember which name they used to look it up.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:11070-11075 (constructor), 11315-11318 (members)
- **suggested_change:** Add a `std::string name_text` member, capture the field name in get_field's three return sites, and expose `auto name() const noexcept -> std::string_view { return this->name_text; }`. This also makes the diagnostic messages in set() (11241-11244) and watch_static_field (15005, 15012) much more useful, because they can now log "field 'counter' size mismatch" instead of "field_proxy::set: size mismatch".

### [S] [INTERNAL] watch_static_field hardcodes DR length from sizeof(field_type) instead of cross-checking the field's signature
- **rationale:** watch_static_field at 15031-15035 picks the DR breakpoint width from `sizeof(field_type)`, the template parameter. If a user calls `watch_static_field<C, std::int64_t>("intField")` on a 4-byte JVM `int` field, the DR is armed for 8 bytes and the next 4 bytes (whatever adjacent field follows) will also trigger spurious callbacks. The field_proxy has the JVM signature available (it's stored on the proxy), and jvm_primitive_byte_width (11395-11410) already exists to map signature -> bytes; the same guard that protects field_proxy::set could protect watch_static_field. This is not a raw_address bug per se, but it's the bug that raw_address's primary consumer most obviously enables.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:15031-15037
- **suggested_change:** After resolving the proxy, compute `const std::size_t field_size{ vmhook::detail::jvm_primitive_byte_width(proxy->signature()) };` and if `field_size != 0 && field_size != sizeof(field_type)` log a diagnostic and return an empty watch_handle, exactly the same pattern as field_proxy::set's size guard.

## Tests

### [standalone_unit] [extend_existing] test_field_proxy_raw_address_returns_constructor_pointer
- **description:** Sanity check that raw_address echoes back exactly what the constructor was given, for both static and instance proxies, and for primitive and reference signatures.
- **asserts:** `field_proxy{ptr, "I", false}.raw_address() == ptr`; same for `"Ljava/lang/String;"`, true; same for `[I"`, true; same for nullptr; same for `(void*)1` (a deliberately bogus pointer to make sure no internal validation strips it). Also check that two proxies built from the same pointer return the same address.
- **existing_file:** tests/test_helpers.cpp (add alongside test_field_proxy_set_size_guard at 1108-1190, invoked from main at 1364).

### [standalone_unit] [extend_existing] test_field_proxy_raw_address_offset_matches_get
- **description:** Build a raw 16-byte storage buffer, plant a known int32 at offset 8, construct a field_proxy{storage+8, "I", false}, and verify that `raw_address()` points at the same byte where `get()` reads from and `set()` writes to. Catches accidental future refactors where one of the three accessors starts applying an internal adjustment.
- **asserts:** `*static_cast<int32_t*>(proxy.raw_address()) == 0xDEADBEEF`; after `proxy.set(int32_t{0xCAFEBABE})`, `*static_cast<int32_t*>(proxy.raw_address()) == 0xCAFEBABE`; `static_cast<int32_t>(proxy.get()) == *static_cast<int32_t*>(proxy.raw_address())`.
- **existing_file:** tests/test_helpers.cpp

### [standalone_unit] [extend_existing] test_field_proxy_raw_address_is_noexcept
- **description:** Compile-time check that raw_address is declared noexcept (so callers can safely use it inside a VEH handler or other noexcept context). Mirrors the signature() / is_static() guarantees.
- **asserts:** `static_assert(noexcept(std::declval<vmhook::field_proxy>().raw_address()));`
- **existing_file:** tests/test_helpers.cpp

### [standalone_unit] [new] test_field_proxy_raw_address_null_base
- **description:** Construct a field_proxy with a null field_pointer (the path that mirrors a get_field on a null instance) and confirm raw_address returns nullptr without crashing. Documents the contract the audit doc-fix above introduces.
- **asserts:** `vmhook::field_proxy{nullptr, "I", false}.raw_address() == nullptr`; `vmhook::field_proxy{nullptr, "Ljava/lang/String;", true}.raw_address() == nullptr`.
- **existing_file:** tests/test_helpers.cpp (extend test_field_proxy_set_size_guard or add a small sibling)

### [jvm_integration] [new] test_watch_static_field_survives_gc
- **description:** Exercise watch_static_field on a tiny Java fixture that has a static int counter, run System.gc() in a tight loop between two writes, and confirm the callback fires for both. Catches the GC-staleness bug noted above. Today this test would document the broken behaviour; after the suggested fix it would lock in the fix.
- **asserts:** Callback fires at least twice; final observed value matches the last write; no SIGSEGV from a stale Class mirror.

### [jvm_integration] [new] test_raw_address_static_field_matches_mirror_plus_offset
- **description:** From a JVM driver, look up a static field, fetch raw_address, then independently compute `klass->get_java_mirror() + entry->offset` via the public hotspot helpers, and assert byte equality. Confirms the static branch of get_field (12844) and raw_address agree.
- **asserts:** `proxy->raw_address() == reinterpret_cast<uint8_t*>(klass->get_java_mirror()) + offset_from_VMStructs("MyClass", "counter")`.

### [jvm_integration] [new] test_raw_address_instance_field_matches_oop_plus_offset
- **description:** Same as above but for an instance field — confirms raw_address works for instance proxies even though the only documented public consumer (watch_static_field) is static-only.
- **asserts:** `proxy->raw_address() == reinterpret_cast<uint8_t*>(instance_oop) + offset`; reads via memcpy from raw_address agree with `proxy->get()`.

## Parity Concerns
- method_proxy has no `raw_method()` analogue to field_proxy::raw_address (vmhook.hpp:11430 onwards exposes name/signature/is_static/get_compressed_oop but nothing that hands out the underlying HotSpot Method*). Users hooking outside the hook<>() macro path cannot get to the Method*.
- field_proxy has no `name()` accessor; method_proxy does (12298). Means diagnostics from field_proxy::set (11241) and watch_static_field (15005, 15012) cannot include the field name without the caller threading it through.
- watch_static_field exists but `watch_instance_field` does not, even though field_proxy::raw_address is already correct for instance fields (12854). The DR machinery (single global slot table) is global, so an instance watch would work as soon as the wrapper supplies the receiver OOP. Documenting that this is the intentional Phase-1 scope (and that instance proxies' raw_address pointer is unstable across any allocation) would help.
- get_compressed_oop (11303-11313) is documented as "for reference/array types" but doesn't validate the signature — calling it on an "I" field silently reads the first 4 bytes of the int as if it were a compressed OOP. raw_address has no such "wrong type, wrong interpretation" failure mode itself, but together the two accessors invite confusion. A sibling helper `get_decoded_oop() -> void*` that combines get_compressed_oop + decode_oop_pointer (and asserts signature starts with 'L' or '[') would be the natural user-friendly companion to raw_address.
- field_proxy lacks any `is_reference() -> bool` predicate; users have to inspect signature()[0] manually. Method_proxy has the same gap. A trivial `bool is_reference() const noexcept { return !signature_text.empty() && (signature_text.front() == 'L' || signature_text.front() == '['); }` would tighten the API for both proxies and make the get_compressed_oop / raw_address use-cases self-documenting.
