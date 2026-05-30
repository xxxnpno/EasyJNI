# to_vector_generic_fallback

## Summary
Audited the generic `size() + get(int)` fallback at the end of `collection::to_vector<T>()` (vmhook.hpp:13626-13654). The cascade order correctly steers ArrayList/LinkedList/HashSet/TreeSet to their specialized walkers before reaching this path, so it only runs for *unknown* List implementations (Vector, Stack, CopyOnWriteArrayList, custom List). The top concern: returned element pointers are reconstructed by passing `method_proxy::call`'s `std::uint32_t` payload through `decode_oop_pointer`, but `method_proxy::call` for an object-return method already stores either a **truncated 64-bit raw OOP** (call_stub path) or a **truncated JNI local handle** (call_jni path) — neither is a compressed OOP, so `decode_oop_pointer` produces garbage and the fallback returns nullptr (best case) or a bogus pointer that `is_valid_pointer` happens to accept (UAF/crash on element wrapping).

## Bugs

### [high] Truncated-uint32 from `call<uint32_t>` is decoded as if it were a compressed OOP, returning garbage element pointers
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:13642-13647
- **description:** The fallback dispatches `get(int)` via `get_method_opt->call<std::uint32_t>(index)`, then passes the resulting `uint32_t` to `vmhook::hotspot::decode_oop_pointer`. But `method_proxy::call` for reference-returning methods does NOT produce a compressed OOP:
  - The `call_stub` path (vmhook.hpp:12302-12325) hits the `default:` arm at line 12324, which does `static_cast<std::uint32_t>(result_holder)` where `result_holder` is the *decoded* 64-bit OOP written by HotSpot's call_stub for T_OBJECT — the cast silently truncates the upper 32 bits.
  - The `call_jni` path (vmhook.hpp:12089-12099) explicitly stores the *truncated low-32 bits of the JNI local-ref handle pointer* (the comment at 12096 admits "Truncated-uint32 already loses the upper bits of the handle").
  Feeding either of those into `decode_oop_pointer` (which computes `narrow_oop_base + (value << narrow_oop_shift)`) produces a value unrelated to any heap address. Best case: the `is_valid_pointer(element_oop)` check rejects it and the fallback returns `nullptr` slots for every element — silently wrong. Worst case on JVMs with `narrow_oop_base == 0` and a non-zero handle, the computed address happens to land in a mapped range and `make_unique<element_type>(...)` constructs a wrapper around garbage memory; the next field/method dispatch on that wrapper crashes.
- **repro:** Create a `java.util.Vector<A>` (skips all four fast paths — no `elementData`+`size` pair on Vector? actually Vector *does* have `elementData`; better: a `java.util.concurrent.CopyOnWriteArrayList<A>` or any third-party `List` impl whose layout matches none of the four). Call `get_field("listOfAs")->get().to_vector<a_class>()`. Either every entry is null, or the JVM crashes inside `a_class::get_field(...)` when the returned wrapper dereferences its `instance`. No automated test covers this — the four fixtures in `Example.java` all hit fast paths.
- **suggested_fix:** Instead of `call<std::uint32_t>(index)` + `decode_oop_pointer`, get the raw decoded OOP back. Two options:
  1. `void* const element_oop{ static_cast<void*>(get_method_opt->call(index)) };` — the `value_t::operator target_type` specialization for `void*` + `uint32_t` (vmhook.hpp:11512-11516) already routes through `decode_oop_pointer`, but it has the *same* bug for a different reason (the stored value isn't a compressed OOP). The clean fix is to make `method_proxy::call` return the *full* decoded OOP for reference returns: change the `default:` arm at 12324 to `return value_t{ static_cast<void*>(reinterpret_cast<void*>(result_holder)) };` (and add a `void*` variant alternative), and have `call_jni` resolve the JNI handle (`*reinterpret_cast<void**>(result_handle)`) before storing/releasing it.
  2. As a localised fix in `to_vector` only: bypass `method_proxy::call` for the reference-return case by reading the call_stub's `result_holder` as `void*` directly — but that means duplicating the call_stub dispatch logic here. Option 1 is cleaner and fixes every other caller (e.g. `field_proxy::value_t::operator void*()`).
- **confidence:** likely

### [medium] Unbounded `reserve(n)` from untrusted `size()` allows OOM from corrupted heap reads
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:13627-13639
- **description:** `const std::int32_t n{ size() };` calls `proxy->call()` on the live OOP's `size` method. If the OOP's klass has been corrupted, the call may return a near-`INT32_MAX` value. The code then does `result.reserve(static_cast<std::size_t>(n))` — that's a request for ~16 GiB on a 32-bit `int` (8 bytes per `unique_ptr`). The sibling fast paths in this file all wrap their iterations with sanity caps (`hash_map_walk_entries` uses `guard < (1 << 20)` at line 14130-14131; `tree_map_walk_keys` uses `(1 << 24)` at line 14394), but the generic fallback has no such cap. A failed `reserve` throws `std::bad_alloc`, but `to_vector` is documented "Exception safety: does not throw" (line 13539) — so the throw escapes the contract and crashes the calling detour.
- **repro:** Detour that reads `to_vector` from a freshly-allocated-but-not-yet-fully-constructed list whose `size` field happens to alias a poisoned slot, or any race where `size` reads garbage. Hard to trigger deterministically but a real risk on relaxed-memory JVM heaps. Easier repro: stub `method_proxy::call` to return a large value in a unit test.
- **suggested_fix:** Bound `n` against a sane upper cap before reserve/iteration, e.g. `const std::int32_t bounded_n{ (std::min)(n, std::int32_t{ 1 << 24 }) };` matching the TreeMap walker. Also wrap the body in `try`/`catch(...)` to honour the no-throw contract.
- **confidence:** likely

### [low] `get_method_by_oop_klass("get")` may latch the wrong overload signature for Map mis-wrapping
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:13465-13486, 13633
- **description:** `get_method_by_oop_klass` returns the FIRST method named `get` in the klass hierarchy. If a caller mis-wraps a `java.util.Map` instance with `vmhook::collection`, none of the four fast-path field probes match (Map has no `elementData`/`first`/`map`/`m` field at the top level — actually `HashMap` does have an `m` field on some inner classes, but `java.util.HashMap` itself doesn't). Falling through, the fallback finds `HashMap.get(Object)` — signature `(Ljava/lang/Object;)Ljava/lang/Object;`. `resolve_compatible_method<std::int32_t>` then walks the hierarchy looking for a `get(int)` overload that doesn't exist on HashMap, eventually returning `this->method` (the `(Object)` overload) as fallback — at which point `call_jni` resolves `jmethodID` against the `(I)Ljava/lang/Object;` signature it cached and `GetMethodID` returns null, and the call silently returns monostate. End result: empty vector for every Map mis-use — silent, no log line.  This isn't strictly a bug (Map shouldn't be wrapped in `collection`) but the silent failure is unfriendly given that `field_proxy::value_t::to_vector` doesn't enforce Collection-ness either.
- **repro:** `vmhook::collection{ map_oop }.to_vector<a_class>()` — returns `{}` with no diagnostic.
- **suggested_fix:** After the four fast-path probes fail, emit a single `VMHOOK_LOG(error_tag, ...)` line noting "no fast-path match and generic List fallback inactive — receiver appears to be a non-List Collection (Set without backing map?) or a Map. Klass = {klass_name}". Cost is one log per failed call but only on the slow path. Bonus: also assert in `get_method_by_oop_klass` that the resolved method's signature starts with `(I)` for `get` lookups in this context (or expose a `get_method_by_signature` overload).
- **confidence:** speculative

## Improvements

### [S] [USER_FACING] Document and enforce the "List only" contract for the generic fallback
- **rationale:** The docstring (vmhook.hpp:13527, 13626) says "Generic fallback — only valid for List impls", but the wrapper accepts any Collection (and `field_proxy::value_t::to_vector` blindly dispatches whatever OOP it finds). A user who reaches the fallback today has no signal that their type is unsupported — they just see an empty vector. Either log the mismatch (see bug #3 above) or check the klass against `java/util/List` super-interfaces before attempting the get(int) walk.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:13626-13654
- **suggested_change:** Add a "is this a List?" probe: walk `oop_klass()->get_interfaces()` for `java/util/List`, and bail with a diagnostic log if absent. Skips the fallback entirely for misused Maps and gives the user a meaningful error string instead of a silent empty.

### [XS] [USER_FACING] Surface `size()` failure (proxy missing) as a log, not a silent zero
- **rationale:** `collection::size()` at 13494-13502 returns 0 if `get_method_by_oop_klass("size")` returns nullopt. The fallback at 13627 immediately returns an empty vector on `n <= 0`. So a malformed klass (no resolvable `size` method) silently yields an empty result — indistinguishable from a genuinely empty collection. Mirroring the diagnostic style used by `call_jni` (which logs every fail) would help users debug.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:13494-13502, 13627-13631
- **suggested_change:** In `size()`, log a single error tag line when the proxy is empty. Keep the return-0 behaviour for noexcept compatibility.

### [S] [INTERNAL] Factor the per-element wrap-or-null block into a helper
- **rationale:** Lines 13573-13582 (ArrayList fast path) and 13643-13651 (generic fallback) repeat the same `decode -> validate -> push back unique_ptr or nullptr` pattern. The HashMap/TreeMap walkers do it again with slight variations. Extracting `try_emplace_decoded<T>(result, compressed_value)` would shrink the body by ~30 lines and centralise the bug-fix for #1 above.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:13571-13583, 13640-13652
- **suggested_change:** Add a `template<typename T> static void push_decoded_element(std::vector<std::unique_ptr<T>>& out, std::uint32_t compressed)` private helper to `collection`, and have both branches call it.

### [S] [INTERNAL] Add a defensive iteration cap matching the fast paths
- **rationale:** See bug #2. The sibling walkers cap at 1<<20 (HashMap) and 1<<24 (TreeMap). The generic fallback should match.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:13627
- **suggested_change:** `const std::int32_t n{ (std::min)(size(), std::int32_t{ 1 << 24 }) };`

### [M] [USER_FACING] Parity with `field_proxy` siblings: expose `iter` / `for_each` instead of forcing materialisation
- **rationale:** Today the only way to traverse a generic-fallback collection is `to_vector<T>()`, which allocates `N * sizeof(unique_ptr)` even if the caller only needs to read one or two elements. `field_proxy::value_t` exposes lazy accessors; `collection` could expose `for_each([](auto& wrapper){ ... })` that avoids the vector allocation and lets callers `break` early. Especially valuable for the generic fallback (each `get(int)` JNI call is expensive).
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:13544-13655
- **suggested_change:** Add `template<typename T, typename F> void for_each(F&& fn) const` on `collection` that runs the same cascade but invokes `fn(element_oop)` per element. Have `to_vector<T>` call it.

## Tests

### [standalone_unit] [new] test_collection_to_vector_generic_fallback_returns_valid_pointers
- **description:** Build a mock collection klass that has neither `elementData`, `first`, `map`, nor `m` field, but provides `size()` returning 3 and `get(int)` returning three distinct stub OOPs. Verify `to_vector<wrapper>()` returns three non-null wrappers whose `get_instance()` matches the stub OOPs.
- **asserts:** `vec.size() == 3`, each `vec[i]->get_instance() == stub_oops[i]`, no entry is unexpectedly nullptr.

### [jvm_integration] [new] test_to_vector_generic_fallback_with_java_util_Vector
- **description:** Add `public Vector<A> vectorOfAs = new Vector<>(Arrays.asList(new A(), new A(), null, new A()));` to `Example.java`. Add a C++ wrapper `vector_of_as()` that calls `get_field("vectorOfAs")->get().to_vector<a_class>()`. Drive from the example driver and verify the returned vector has 4 entries (3 non-null + 1 null) and that the non-null wrappers can read fields from their A. **This is the only path that exercises bug #1 end-to-end** — currently zero coverage.
- **asserts:** `vec.size() == 4`, `vec[2] == nullptr`, `vec[0]->get_field("aInt")` matches `Example.instance.vectorOfAs.get(0).aInt` from a Java probe.
- **existing_file:** example/vmhook/Example.java (add field), vmhook/src/example.cpp (add wrapper + probe)

### [jvm_integration] [new] test_to_vector_generic_fallback_with_copyonwritearraylist
- **description:** Same shape as the Vector test but with `java.util.concurrent.CopyOnWriteArrayList`. CoW lists store data in a transient `array` field rather than `elementData`, so they reliably skip the ArrayList fast path even though they are valid `List` impls. Validates the fallback on a real-world type Minecraft mods commonly use.
- **asserts:** Same as Vector test.
- **existing_file:** example/vmhook/Example.java, vmhook/src/example.cpp

### [standalone_unit] [new] test_collection_to_vector_size_overflow_capped
- **description:** Stub `size()` to return `INT32_MAX` and `get(int)` to return nullptr. Verify `to_vector` does not throw `std::bad_alloc` from `reserve`, does not iterate billions of times, and returns within (say) 100 ms.
- **asserts:** Completes within timeout; result vector size <= safety cap.

### [standalone_unit] [new] test_collection_to_vector_on_map_logs_diagnostic
- **description:** Construct a `collection` wrapping a HashMap OOP (mis-use). Verify the result is empty AND that VMHOOK_LOG has a diagnostic line mentioning the klass name or "not a List". Currently fails silently.
- **asserts:** Result is empty; log buffer contains one error-tagged entry matching the diagnostic pattern.

### [standalone_unit] [extend_existing] exercise_collection_wrappers — add generic-fallback compile probe
- **description:** The existing `exercise_collection_wrappers` in `tests/test_api_surface.cpp` only validates that `to_vector<T>` *compiles* on each wrapper type. Extend it to also instantiate `to_vector<T>` with an OOP whose klass would skip all fast paths (null OOP is fine for compile coverage). No runtime assertion — purely an SFINAE / static_assert check that the generic branch still compiles with various `element_type` shapes (default-constructible vs not, base-of `object` vs not).
- **asserts:** Compiles; no static_assert fires.
- **existing_file:** tests/test_api_surface.cpp:83-139

## Parity Concerns
- `collection::to_vector` has FOUR specialized fast paths (`ArrayList`, `LinkedList`, `HashSet`, `TreeSet`) each with explicit field-shape probing, but only ONE generic fallback. `map::to_entries` (vmhook.hpp:13727+) follows the same pattern. The asymmetry is fine, but the generic fallback's silent-on-failure behaviour contrasts sharply with `method_proxy::call_jni`'s exhaustive `VMHOOK_LOG(error_tag, ...)` diagnostics for every dispatch branch (10+ log sites in lines 11565-12107). Bring the fallback's logging up to parity.
- `field_proxy::value_t::to_vector` (vmhook.hpp:14415-14424) is the user-facing entry point; it blindly forwards any OOP to `collection::to_vector` with zero type checking. `field_proxy::value_t::operator T*()` for object types does richer validation. Consider adding a "looks like a List/Set/Collection" probe in `value_t::to_vector` so users mis-wiring a field type get a diagnostic instead of an empty vector.
- The `vmhook::list` / `vmhook::set` / `vmhook::linked_list` type-tag classes (vmhook.hpp:13673-13722) all forward to the same generic `collection::to_vector` — there is NO type-system enforcement that calling `to_vector<T>` on a `vmhook::set` won't hit the (broken) List-only generic fallback path. Either make `set::to_vector` skip the generic branch (early-return empty) or split the cascade so that `set` only attempts HashSet/TreeSet probes.
