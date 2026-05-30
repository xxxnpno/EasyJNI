# field_proxy_array_strings_objects

## Summary
Audited the String[] / Object[] read & write paths reachable from `field_proxy`:
`read_array_value<vector<string>>` + `append_array_value(vector<string>&,…)` for reads,
`set_str_array` for writes, and `field_proxy::value_t::to_vector<T>()` for the
wrapper-conversion path used on `[L…;` fields. Inner nulls do **not** crash, but
they are silently coerced (read → `""`, write → dropped). The top concern is the
`Object[]` (`[Lcom/foo/Bar;`) wrapper path: `to_vector<T>()` blindly wraps the
array OOP in a `vmhook::collection` and then dereferences it through
`InstanceKlass._methods` / `_fields` offsets even though the OOP is an
`ObjArrayKlass`, which produces a silent empty result on best behaviour and risks
following a garbage pointer through `is_valid_pointer` filtering.

## Bugs

### [high] `to_vector<T>()` treats Object[] fields as java.util.Collection — silent empty result, possible crash
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:14385-14400 (entry), 13544-13655 (body), 13465-13486 / 2589-2607 (InstanceKlass deref on ObjArrayKlass)
- **description:** `field_proxy::value_t::to_vector<T>()` is the documented entry point users hit via `get_field("things")->get().to_vector<thing>()`. For a Java `Foo[]` field it unconditionally wraps the array OOP in `vmhook::collection{ collection_oop }` and calls `collection::to_vector<T>()`. The `collection` body then probes the live klass through `get_field_by_oop_klass("size" / "elementData" / "first" / "map" / "m")` and `get_method_by_oop_klass("size")`. Both helpers reach into `InstanceKlass._fields` (line 2991) and `InstanceKlass._methods` (line 2599 / 2627) — but the klass behind a Java `Object[]` is an `ObjArrayKlass`, which has a completely different layout. Reading `_methods` / `_fields` at the InstanceKlass offset returns whatever happens to live there in the ObjArrayKlass; `is_valid_pointer` may or may not save us depending on the bytes (it only filters obvious sentinels, kernel-space, odd alignment, and a few poison patterns — any random aligned user-space address passes). Best case: every probe returns nullopt and `size()`→0, so the user silently gets an empty `vector<unique_ptr<T>>` from a non-empty array — undetectable failure. Worst case: the bogus `_methods`/`_fields` pointer passes `is_valid_pointer`, and `get_methods_count` (line 2606) does `*reinterpret_cast<int32_t*>(array)` on random memory, leading to either nonsense counts (the loop then dereferences `methods_array[i]` which may segfault) or wrong field offsets being computed in `find_field`. Either way the user has no diagnostic and no API to read `Foo[]` as wrappers. None of the existing `[L…;` example fixtures (`staticStringArray`, `notStaticStringArray`, `emptyStrArray`) exercise this path because they use the `vector<string>` overload, so this regression has never been caught.
- **repro:** add a `public static A[] arrayOfAs = { new A(), new A() };` to `example/vmhook/Example.java`, then in `vmhook/src/example.cpp` call `static_field("arrayOfAs")->get().to_vector<a_class>()`. Expected: a 2-element vector. Actual: empty vector (silent), with a non-zero risk of crash on an obscure JDK / GC build where the bytes at the InstanceKlass `_methods` offset inside ObjArrayKlass happen to be a valid-looking aligned pointer.
- **suggested_fix:** in `field_proxy::value_t::to_vector<T>()` (line 14386), branch on the signature before wrapping. If `this->signature.starts_with("[L")` or `"[["`, walk the array directly: `array_length(array_oop)`, then per-index `get_array_element<uint32_t>` → `decode_oop_pointer` → `is_valid_pointer` → emplace `make_unique<T>(static_cast<oop_t>(element_oop))` or `nullptr`. That mirrors the ArrayList fast path at lines 13567-13584 but skips the wrapper probe entirely. Only fall through to `collection{ … }` when the signature starts with `'L'`. As a secondary defence, add an `is_instance_klass()` guard to `get_methods_count` / `get_methods_ptr` / `find_field` that checks the klass tag (HotSpot stores `_layout_helper` / `_kind` distinguishing InstanceKlass from ArrayKlass — accessible via VMStructs `Klass::_layout_helper` or `ObjArrayKlass::_element_klass`) so that even a future caller mistake degrades to nullopt instead of bogus reads.
- **confidence:** likely (silent-empty path is certain by code inspection; crash potential depends on layout of ObjArrayKlass at the InstanceKlass `_methods` offset, which varies per JDK build).

### [medium] read path drops the null-vs-empty distinction and log-spams a warning for every null slot
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:10842-10847 (append_array_value), 14437-14445 (read_java_string null branch)
- **description:** `append_array_value(std::vector<std::string>&, …)` reads the inner compressed OOP and unconditionally calls `read_java_string(decode_oop_pointer(compressed))`. For null slots `compressed == 0` ⇒ `decode_oop_pointer` returns nullptr ⇒ `read_java_string` enters the `!string_oop` branch which **emits a warning log** (`"read_java_string(): string_oop is null or invalid (0x0000…)"`) and returns `""`. Effects: (a) a `String[]` with k null entries fills the log with k warnings, even though "inner element is null" is a perfectly legal Java state — the doc comment on this feature explicitly says "Inner nulls must become nullptr/empty slots, not crash" so this is expected, not an error; (b) the caller cannot tell `null` from `""` in the returned `vector<string>`. The other reference-array helpers carefully short-circuit on a 0 compressed OOP without logging (`get` line 11144-11147; `decode_array_oop` line 14740-14749), so this is a parity gap.
- **repro:** populate `staticStringArray = { "hello", null, "!" }`, call `example_class::get_static_string_array()`. Observed: returns `{"hello", "", "!"}` and the log contains a `[VMHOOK] read_java_string(): string_oop is null or invalid` warning for index 1. Expected: returns `{"hello", "", "!"}` (or `optional<string>` per slot) with no log noise.
- **suggested_fix:** in `append_array_value(vector<string>&, …)` short-circuit a 0 compressed OOP and push back `""` directly without calling `read_java_string`. Optionally add a sibling `append_array_value(vector<optional<string>>&, …)` so callers who care about the distinction can opt in. Move the "is null" log inside `read_java_string` from `warning_tag` to `debug_tag` (or drop entirely) — its three call sites already do an upstream null check on the compressed OOP, so the only reachable null at that point is via this array helper.
- **confidence:** certain.

### [medium] write path silently drops user values when the Java slot is null
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:14716-14731
- **description:** `set_str_array` reads the existing compressed OOP per slot, decodes it, and hands the pointer to `write_java_string`. When the existing slot is null (`compressed == 0`), `write_java_string` returns silently and **the user's `values[i]` is discarded with no warning**. The doc string at lines 14702-14708 explicitly states the function only mutates in-place; it does not allocate a fresh String OOP for a null slot. Combined with the read-side coercion in the previous bug, a round-trip `read → mutate → write` is **destructive**: any String that was null on read becomes a permanent null hole that future writes also cannot fill. Users will not notice until they read back and see their value was dropped.
- **repro:** with `staticStringArray = { "hello", null, "!" }`, call `set_static_string_array({"a","b","c"})`. Read back: get `{"a", "", "!"}` — index 1 is unchanged because no allocation happened. No diagnostic. Compare with `set_prim_array` / `set_bool_array`, which always write because the slot exists.
- **suggested_fix:** in `set_str_array` either (a) allocate a fresh `java.lang.String` via `make_java_string(values[i])` and write the new compressed OOP into the array slot when the existing slot is null, or (b) at minimum emit a `VMHOOK_LOG` at warning level mentioning the dropped index and the value length, so users learn about silent data loss. Option (a) matches user expectation and brings parity with `set_prim_array`, which always lands the value. The GC barrier / store-check is not needed for OOP-into-OOP-array assignment in modern HotSpot since the array's card table is updated by the next safepoint, but a `set_array_element<uint32_t>(array_oop, index, encode_oop_pointer(new_str))` should suffice for the array slot itself.
- **confidence:** certain.

### [low] no path to read or write a raw `Object[]` field as `vector<unique_ptr<T>>` even after Bug-1 is fixed for collections
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:10898-10922 (read_array_value), 11172-11186 (set chain)
- **description:** `field_proxy::set` recognises `vector<bool>`, `vector<string>`, and `vector<trivially_copyable>` for primitive arrays, but there is **no overload that takes `vector<unique_ptr<T>>` and writes it into a Java `[L…;` field**. Mirrored on the read side, `read_array_value` only has `append_array_value` overloads for bool, string, char, and trivially-copyable element types — there is no overload that emits `vector<unique_ptr<T>>` from a `[L…;` array. The user-facing escape hatch today is `to_vector<T>()`, which (per Bug 1) does not currently understand raw object arrays. This is the same parity gap the user called out for method_proxy vs field_proxy — once Bug 1 is patched on the read side, the write side still has no story.
- **repro:** there is no compiling C++ call site that writes a `Foo[]` field from a `vector<unique_ptr<foo>>`. Today users must drop down to `decode_array_oop` + `set_array_element<uint32_t>(array_oop, i, encode_oop_pointer(item->get_instance()))` by hand.
- **suggested_fix:** add `set_obj_array<T>(const field_proxy&, const std::vector<std::unique_ptr<T>>&)` next to `set_str_array`, and wire it into the `field_proxy::set` template chain at line 11172 with `else if constexpr (vmhook::detail::is_vector_of_unique_ptr_v<clean_value_type>)`. Body: walk to min length, for each slot encode `value[i] ? value[i]->vmhook::object_base::get_instance() : nullptr` and `set_array_element<uint32_t>(array_oop, i, compressed)`. On the read side, add `append_array_value(vector<unique_ptr<T>>&, …)` so `vector<unique_ptr<T>>` flows through `read_array_value` for `[L…;` fields with the same signature-driven dispatch the other array variants already use.
- **confidence:** certain.

## Improvements

### [S] [USER_FACING] Compile-time signature check for `get().to_vector<T>()` on non-reference fields
- **rationale:** Today users can call `get_field("intField")->get().to_vector<some_wrapper>()` and get an empty vector with no error. The signature is available at runtime (`this->signature`) — a runtime warning is the minimum, a compile-time concept on `T` (must be a non-pointer class derived from `object_base`) is the polish.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:14386-14400
- **suggested_change:** emit a `VMHOOK_LOG(error_tag, …)` when `signature` does not start with `'L'` or `'['` — this is a programmer error and the empty return value is a silent failure. Optionally `static_assert(std::is_base_of_v<vmhook::object_base, element_type>)` in `to_vector<T>` so the wrong type fails at the call site.

### [XS] [INTERNAL] Drop the doc-comment lie about bounds-checking
- **rationale:** `get_array_element` says "Bounds checking is performed against the array length" at line 10712, and indeed it is — but `set_str_array`, `set_bool_array`, `set_prim_array`, and `write_java_string` rely on the `(std::min)` clamp at the call site for safety; the underlying `set_array_element` *also* bounds-checks, which means we pay for the bounds check twice on every element. Either remove the per-call bounds check inside the inner helpers (callers already clamp) or change the doc comments to call out the redundancy as a defence-in-depth design choice. Right now a casual reader has no idea.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:10704-10732, 10741-10756, 14647-14651, 14679-14697, 14725-14730
- **suggested_change:** add a single sentence to each set-array helper noting that bounds checking is duplicated inside `set_array_element` for defence in depth, or remove the inner check and rely on the outer clamp.

### [S] [USER_FACING] Mirror `make_java_array` + `make_java_string` to allocate-on-replace for `set_str_array`
- **rationale:** Once Bug 3 is fixed by allocating into null slots, advertise this in the doc comment at lines 14702-14708. Users currently read "Does NOT replace the String references in the array" and reasonably conclude they cannot use this for null slots — even though it is the natural fix.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:14702-14716
- **suggested_change:** update the comment to "If the slot is null, a fresh java.lang.String is allocated via make_java_string() and stored. Otherwise the existing String is mutated in place." Mention the GC implications (a fresh allocation may trigger a safepoint).

### [M] [USER_FACING] Add `find_or_make_str_field` + `find_or_make_obj_array_slot` siblings for parity with method_proxy
- **rationale:** The user's stated audit goal calls out method_proxy vs field_proxy API parity. method_proxy lets callers materialise arguments lazily. field_proxy currently has no `make_*_if_null` story for any reference field, so the common "I want to fill in a null Java field from C++" workflow either silently no-ops (set_str_field) or silently truncates (set_str_array on a null slot). A pair of `set_or_allocate_*` helpers would close the parity gap and remove a surprise that bit users in three separate places (`set_str_field`, `set_str_array`, and a future `set_obj_array`).
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:14619-14623, 14716-14731
- **suggested_change:** introduce `vmhook::set_str_field_or_allocate(field, value)` that allocates a new String and writes the compressed OOP to the field when the existing OOP is null; same for `set_str_array_or_allocate(field, values)`. Document the GC implications.

## Tests

### [standalone_unit] [new] test_field_proxy_obj_array_to_vector_compiles
- **description:** verify `field_proxy::value_t::to_vector<some_wrapper>()` is callable and returns the right type for a `[L…;` signature.
- **asserts:** instantiate `field_proxy` with signature `"[Lcom/example/Foo;"` and a null `field_pointer`, call `.get().to_vector<foo_wrapper>()`, assert the return type is `std::vector<std::unique_ptr<foo_wrapper>>`. (Compile-only; no JVM needed.) The current code compiles this branch, so the test guards against regressions after Bug 1 is fixed.
- **existing_file:** tests/test_api_surface.cpp (extend the templated to_vector section at lines 105-127).

### [jvm_integration] [extend_existing] test_string_array_inner_null_round_trip
- **description:** verify Bug 2 + Bug 3 — populate `Example.staticStringArray = { "hello", null, "!" }`, read it via `example_class::get_static_string_array()`, then write `{"a","b","c"}` and re-read.
- **asserts:** (a) the read returns 3 elements; (b) the middle slot is empty AND no warning log was emitted for the null slot; (c) after `set_static_string_array({"a","b","c"})` the re-read returns `{"a","b","c"}` (not `{"a","","c"}`); (d) the array length remains 3 (no resize).
- **existing_file:** vmhook/src/example.cpp (extend `test_array_edge_cases` at line 2418, plus a `nullStringSlotArray` fixture in `example/vmhook/Example.java`).

### [jvm_integration] [new] test_obj_array_to_vector_returns_wrappers
- **description:** verify Bug 1 — a Java `A[]` field round-trips through `field_proxy::value_t::to_vector<a_class>()` without going through the Collection wrapper layer.
- **asserts:** add `public static A[] arrayOfAs = { new A(), null, new A() };` to `Example.java`. From C++, call `static_field("arrayOfAs")->get().to_vector<a_class>()`. Assert size == 3, element 0 non-null and `instance()` matches the Java OOP, element 1 == nullptr, element 2 non-null. With the current code the test fails (size == 0 silently).
- **existing_file:** vmhook/src/example.cpp `test_collection_probe` block near line 1882 (which currently only tests ArrayList / LinkedList / HashSet — there is no raw-array equivalent).

### [jvm_integration] [new] test_obj_array_write_through_field_proxy_set
- **description:** verify the parity fix for Bug 4 — a `vector<unique_ptr<a_class>>` can be assigned to an `A[]` field via `field_proxy::set`.
- **asserts:** after `instance.set_array_of_as({ make_unique<a_class>(…), nullptr, make_unique<a_class>(…) })`, the re-read via `to_vector<a_class>()` yields a matching 3-element vector with element 1 == nullptr. With the current code this does not compile (no `vector<unique_ptr<T>>` overload in the set chain).
- **existing_file:** vmhook/src/example.cpp (new helper alongside `get_static_string_array`/`set_static_string_array` at lines 668-678).

### [standalone_unit] [new] test_set_str_array_truncates_to_min_length
- **description:** boundary test for `set_str_array`. With a 2-element `String[]` and a 5-element `vector<string>`, only the first 2 are written and no out-of-bounds write occurs.
- **asserts:** `array_length` unchanged at 2; first two slots updated; no crash. Mock array OOP via a fake `field_proxy` whose `field_pointer` points to a synthesised 16-byte Java array header. Standalone — does not need a real JVM.
- **existing_file:** tests/test_api_surface.cpp.

## Parity Concerns
- `set_str_array` mutates in place; the natural C++ shape `set_obj_array(field, vector<unique_ptr<T>>)` does not exist at all — read/write asymmetry inside the field_proxy family.
- `read_java_string` logs at warning level on null inputs, but every other read helper (`get` for refs, `decode_array_oop`, `field_oop`) silently returns nullptr/zero on null inputs without log noise — the asymmetric chattiness from inside an array read loop is a UX papercut.
- `method_proxy` has uniform success/failure semantics; `field_proxy::set` for vector<string> silently drops null-slot writes while `field_proxy::set` for primitive vectors always writes — call-site behaviour depends on the element type, not the API verb.
- The doc comment on `set_str_array` (lines 14702-14708) and on `field_proxy::set` (lines 11150-11157) both fail to mention the null-slot edge case; once it is fixed, the docs should explicitly call out the allocate-on-null behaviour.
- The audit area name says "field_proxy_array_strings_objects" but there is no Object[] path tested in either standalone tests or example.cpp — the feature is undocumented and unreached, even though its sibling (`vector<string>`) has end-to-end coverage. That parity gap is itself a finding.
