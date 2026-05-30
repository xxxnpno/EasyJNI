# method_proxy_call_jni_fallback

## Summary
Audited `method_proxy::call_jni()` (vmhook.hpp:11525-12075) — the JNI fallback used when `StubRoutines::_call_stub_entry` is unavailable — and its companion caches (`cached_method_id`, `cached_class_handle`, `cached_ret_char`, `cached_effective_signature` at lines 12656-12667). Top concern: the static-call path resolves the declaring class via raw `jni_find_class` (line 11619) instead of `jni_find_class_with_context_loader`, so any static method whose declaring class lives in a non-bootstrap loader (every host class on Forge, Lunar Client, LaunchClassLoader-based stacks) fails on detour threads — exactly the workloads that needed the JNI fallback in the first place. The cached class handle is also a JNI local reference rather than a `NewGlobalRef`, which makes it unsafe to reuse across threads or across long timeframes, and the cache writes are not thread-safe.

## Bugs

### [high] Static call path doesn't fall back to the context-classloader lookup
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:11591-11635
- **description:** The static branch calls `vmhook::detail::jni_find_class(name_sym->to_string())` (line 11619). `jni_find_class` is the raw `JNIEnv::FindClass` (line 8794), which uses the calling thread's classloader. The comment block at 9163-9183 spells out why this fails for host classes loaded by LaunchClassLoader / Lunar mixin loader / Forge's transformer chain on attached detour threads. The instance path doesn't hit this because `GetObjectClass` on a live receiver always returns the runtime klass, but the static path has no receiver — it relies entirely on FindClass succeeding from whatever thread the detour fires on. The instance path's error message at 11667-11673 even mentions "Ensure the context classloader is set" — the static path needs the same fallback. Real fix is to swap to `jni_find_class_with_context_loader` and then resolve the resulting `klass*` back to a `jclass` (e.g. via the class mirror) for the cached handle.
- **repro:** `static_method("getInstance")->call_jni()` on a Minecraft / Forge class from a detour thread where the host class was loaded by LaunchClassLoader. FindClass returns null, `class_handle` stays null, the error at 11627-11629 fires, the call silently returns monostate. Instance variant of the same call works because `GetObjectClass` bypasses the classloader entirely.
- **suggested_fix:** After the raw `jni_find_class` returns null, retry through `jni_find_class_with_context_loader` (which already exists at line 9031 and walks Thread.getContextClassLoader -> system loader -> LaunchClassLoader). Cache the resulting `jclass` (either by holding the `klass*` mirror and re-deriving on demand, or by calling `JNI::ToReflectedClass` / wrapping with `NewGlobalRef`).
- **confidence:** certain

### [high] `cached_class_handle` is a JNI local ref shared across threads / lifetime
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:11619, 11632, 11642, 11650, 12657
- **description:** Both code paths cache the raw return value of `jni_find_class` (FindClass) / `jni_get_object_class` (GetObjectClass) into `this->cached_class_handle`. Both functions return JNI *local* references — scoped to the JNI frame of the calling thread. Detour threads never pop a frame so the ref appears to live forever on the originating thread, but the JNI spec is explicit that local refs from one thread MUST NOT be used by another. If a `method_proxy` (e.g. one obtained from a singleton wrapper) is invoked from two different detour threads, the second thread dereferences the first thread's local-ref-table slot, which is undefined behavior — in practice on HotSpot it can read a stale slot, decode a moved-OOP, and feed garbage into `GetMethodID` / `CallStatic*MethodA`. Additionally, on JDK 8 the local ref is invalidated if `PushLocalFrame`/`PopLocalFrame` ever runs on the originating thread (it can, when nested detours fire).
- **repro:** Construct a `method_proxy` once, store it (e.g. in a `std::optional`), and call it from two different attached detour threads concurrently. Thread A populates the cache, thread B reuses A's local ref. Crashes / wrong methods get dispatched are intermittent and look like "the method runs sometimes".
- **suggested_fix:** Promote the cached handle via `JNIEnv::NewGlobalRef` (slot 21) right after the lookup, and free it via `DeleteGlobalRef` (slot 22) in a (currently missing) `method_proxy` destructor. `cached_method_id` is fine across threads since `jmethodID` is process-wide, but the jclass needs a global ref.
- **confidence:** likely

### [high] Cache writes aren't thread-safe
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:11555-11578, 11587-11677, 12656-12667
- **description:** `cached_method_id`, `cached_class_handle`, `cached_ret_char`, and especially `cached_effective_signature` (a `std::string`) are unguarded mutable members. The `cached_effective_signature = ...` assignment at line 11555 happens inside a ternary that's evaluated whenever `selected_method != this->method`; the assignment is read on line 11557 via `effective_signature` reference. If two threads enter `call_jni` concurrently on the same proxy and both pick a different overload than `this->method`, both will be writing to the same `std::string` while the reference reads from it — `std::string`'s assignment is not atomic; the reader can see a partially-relocated SSO buffer and feed garbage to `jni_get_method_id`. Same risk for `cached_method_id` / `cached_class_handle`: both threads will redo the lookup (wasteful but OK) and both will write — the loser leaks a jclass local-ref.
- **repro:** Two threads holding the same `method_proxy*` (e.g. shared `std::optional<method_proxy>` from `get_static_method`), both calling `call_jni` with arguments that trigger overload-resolution to a different signature. Hard to repro reliably but observable as intermittent "GetMethodID returned null" with the right signature in the log.
- **suggested_fix:** Either (a) require single-threaded use and document it, (b) wrap the cache-fill block in `std::call_once` or a per-proxy `std::mutex`, or (c) make the caches atomic where possible (pointer fields can be `std::atomic<void*>` with relaxed semantics — the duplicate-fill case becomes a benign races+leak that you can clean up explicitly). The `cached_effective_signature` race is the most dangerous; consider replacing the string member with a `const char*` plus length that points into an interned `string_table` keyed by signature.
- **confidence:** likely

### [medium] Failure path leaks `cached_class_handle` JNI local ref on every retry
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:11625-11653, 11655-11676
- **description:** If `jni_get_method_id` / `jni_get_static_method_id` returns null (line 11655 branch), the function returns monostate WITHOUT clearing `cached_class_handle`. On the next call, `cached_method_id` is still null so we re-enter at line 11587, re-derive a NEW `class_handle` (FindClass or GetObjectClass), and overwrite `cached_class_handle` (lines 11632 / 11650) — leaking the previous local ref. A tight call loop that consistently fails at GetMethodID (e.g. wrong signature) will fill HotSpot's default 16-entry local-ref table inside ~16 iterations and start logging "local reference table overflow". This is the same class of leak the rest of `call_jni` was just fixed for in the CHANGELOG (string-arg refs, result-handle refs).
- **repro:** Loop calling `call_jni` with a signature that triggers `GetMethodID` failure (typo'd descriptor). 16 iterations -> JNI local-ref table overflow warning, subsequent JNI calls on the thread misbehave.
- **suggested_fix:** Before returning monostate at line 11675, call `vmhook::detail::jni_delete_local_ref(this->cached_class_handle); this->cached_class_handle = nullptr;`. (And again — promote to a global ref so this isn't a per-call concern in the first place.)
- **confidence:** certain

### [medium] `is_static()` returns false for static method proxies
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:11502-11508, 12326-12330, 12649
- **description:** `method_proxy`'s only constructor hardcodes `static_field{ false }` (line 11506). `is_static()` (line 12326) returns `this->static_field`. So `proxy.is_static()` is ALWAYS `false`, even for proxies returned by `static_method()` / `get_static_method` (which pass `owning_object = nullptr` at construction sites 13062 / 13120). Inside `call_jni`, the actual static dispatch is decided by `is_static_call{ this->object == nullptr }` (line 11581), which is correct — but any user code that branches on `proxy.is_static()` to skip a receiver setup, log the call kind, or assert pre/post conditions gets the wrong answer. Compare with the corresponding `field_proxy::is_static()` (line 11293) which is set from the constructor's `is_static_flag` parameter (line 11070) and works correctly.
- **repro:** `auto m = ExampleClass::static_method("foo"); assert(m->is_static());` always fails.
- **suggested_fix:** Either drop the constructor parameter and compute `is_static()` from `this->object == nullptr` (matches the dispatch logic), or add an `is_static_flag` parameter to the constructor and pass `true` at the two static-discovery sites (13062, 13120, 13035 family). The compute-on-demand form is more robust because it stays in sync with `this->object` even if a future change makes the proxy switch receivers.
- **confidence:** certain

### [low] `<clinit>` silently routed through `CallStaticVoidMethodA` instead of being refused
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:11907-11942
- **description:** The `<init>` / `<clinit>` special-case is gated on `!is_static_call` (line 11909). `<clinit>` is the static-initializer method and would never have a receiver, so `is_static_call` would be `true` for it — meaning it falls past the special-case and into the regular static-void branch at line 11931, which calls `CallStaticVoidMethodA` (slot 143) on `<clinit>`. The JNI spec forbids this; HotSpot will either assert-fail or silently no-op. The init-call special-case correctly handles instance `<init>` via `CallNonvirtualVoidMethodA`. For `<clinit>` there's no valid JNI dispatch (only the JVM internally invokes it), so the right behavior is to refuse + log.
- **repro:** Resolve a `<clinit>` proxy via raw walk of `klass->methods` and call it. JVM may assert or no-op without a clear diagnostic.
- **suggested_fix:** Add an early branch: `if (method_name == "<clinit>") { VMHOOK_LOG(...); return value_t{ std::monostate{} }; }` before the regular void dispatch. Also, the `is_init_call` guard on `<init>` should not require `!is_static_call` — a static method named `<init>` is impossible by JVM spec so the guard is harmless, but the name check alone is enough.
- **confidence:** likely

### [low] Misleading `receiver_oop` in dispatch log for static calls
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:11776-11783
- **description:** The diagnostic dump at 11776 prints `receiver_oop=0x{...}` from `is_static_call ? nullptr : this->object`. For static calls the user sees `receiver_oop=0x0000000000000000`, which suggests "no receiver". But the actual JNI argument is `receiver = this->cached_class_handle` (a jclass indirect handle) — completely opaque to the log. For debugging a static-method failure, knowing which jclass we're dispatching on is useful (you can paste it into a debugger to inspect the Klass*).
- **repro:** Run any failing static call and look at the log — the receiver is 0, no clue what class was selected.
- **suggested_fix:** Print both: `receiver_oop=0x{}` for the instance OOP (or class mirror OOP for static), and `class_handle=0x{}` for the cached jclass. Optionally include the class name from the pool_holder.
- **confidence:** certain

## Improvements

### [S] [INTERNAL] Centralize the per-return-type dispatch as a table-driven helper
- **rationale:** The `switch (ret_char)` at lines 11894-12074 has nine near-identical cases (Z/B/C/S/I/J/F/D + L/[). Each case reads `is_static_call ? STATIC_SLOT : INSTANCE_SLOT`, casts `table[slot]` to a typed function pointer, null-checks, calls, runs `check_callee_exception()`, returns `value_t{ r }`. Lots of boilerplate per primitive type and easy to add a slot offset by accident when adding a new return type.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:11894-12074
- **suggested_change:** Pull the (slot_instance, slot_static, return_type) tuple into a constexpr table indexed by ret_char (or a small switch routing to one templated lambda). For example: `auto dispatch = [&]<typename R>(std::size_t instance_slot, std::size_t static_slot) { ... }; case 'I': return value_t{ dispatch.template operator()<std::int32_t>(51, 131) };`. Eliminates the 8x copy-paste and makes the slot-offset table the single source of truth.

### [S] [USER_FACING] Add `to_string()` / `to_vector<T>()` / typed accessors on `method_proxy::value_t`
- **rationale:** Parity with `field_proxy::value_t` (line 10809), which has `signature`, `to_vector<T>()`, `to_entries<K,V>()`, and array-element helpers. After `method_proxy::call_jni` returns a uint32_t compressed OOP for an arbitrary reference type (line 12063), the user has no helper to decode it into a `std::string` (the special-case at 12046 only handles `Ljava/lang/String;` exactly), iterate a returned List, or walk a returned Map. They have to manually call `vmhook::hotspot::decode_oop_pointer(uint32_t(v))` and pass the result to `vmhook::read_java_string` / `vmhook::list` themselves.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:11439-11495
- **suggested_change:** Add a `std::string signature{}` member to `method_proxy::value_t` (populated by `call_jni` from `effective_signature.substr(rparen + 1)`), and inherit / copy the `to_vector<T>()` / `to_entries<K,V>()` / array helpers from `field_proxy::value_t`. Better: factor the helpers into a free template `vmhook::decode_as<T>(const value_holder&)` shared by both proxies.

### [XS] [INTERNAL] Don't recompute `rparen` on the L/[ branch
- **rationale:** `ret_char` is cached on `cached_ret_char` (line 11578), but the L/[ case at line 12041-12044 reparses `effective_signature.rfind(')')` if `rparen == npos` (which it always will be on the second call onward, since the cache short-circuits the rfind). The substring at 12046 is then re-parsed every call.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:12041-12054
- **suggested_change:** Cache `is_string_return` (bool) alongside `cached_ret_char` so the L/[ branch can skip the rfind + substr + string-compare entirely on the hot path. Alternative: cache the entire `std::string return_type_descriptor` on first compute.

### [S] [USER_FACING] Clearer error message on the FindClass(name from pool_holder) failure
- **rationale:** The diagnostic at line 11627-11629 says "the declaring class is not reachable via JNI" without explaining that the root cause is almost always the classloader. Compare with the GetMethodID failure message at 11667-11673, which spells out "context classloader" and "vmhook now does this automatically at attach". Symmetry would help.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:11627-11629
- **suggested_change:** Quote the class name (`name_sym->to_string()`) in the diagnostic — currently the user sees only the method name + signature and has to guess which class. Also mention "consider whether the declaring class lives in a non-bootstrap loader (Forge / Lunar / LaunchClassLoader); raw FindClass only sees the calling thread's loader chain".

### [M] [USER_FACING] Promote `cached_class_handle` to a global ref and add a destructor
- **rationale:** Mirrors Bug 2's fix. Currently `method_proxy` has no destructor — relying on the implicit default — so even if we fixed the local-vs-global issue, there's no place to release a `NewGlobalRef`. Adding a destructor that calls `DeleteGlobalRef` on `cached_class_handle` makes the proxy properly RAII for its JNI state.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:11502-11508, 12646-12667
- **suggested_change:** Add `~method_proxy() noexcept { if (cached_class_handle) { jni_delete_global_ref(cached_class_handle); } }`, and convert `jni_find_class` / `jni_get_object_class` callsites to wrap the result in `JNIEnv::NewGlobalRef` (slot 21). Also need to update the copy/move semantics — currently the proxy uses the implicit defaults which would double-free the global ref.

### [XS] [INTERNAL] Pin the JNI table slot numbers as constexpr named constants
- **rationale:** `table[36]`, `table[15]`, `table[228]`, `table[228u]`, `table[33]`, `table[31]`, `table[170]`, etc. are sprinkled across the exception-handling helper (11814-11892) with comments inline. A user grepping for "DeleteLocalRef slot" gets nothing. The slot numbers also re-appear in `jni_function<6, ...>` / `<23, ...>` / etc. — multiple sources of truth.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:11814-11892, 8742-8908
- **suggested_change:** Define a `namespace vmhook::detail::jni_slot { constexpr std::size_t find_class = 6; constexpr std::size_t exception_describe = 16; ... }` and reference by name. Same constants used by `jni_function<>` templates and the inline `table[]` casts in `check_callee_exception`.

## Tests

### [standalone_unit] [new] test_method_proxy_value_t_static_cast_round_trip
- **description:** Verify `method_proxy::value_t` conversion-operator (lines 11471-11494) round-trips each variant alternative through `static_cast<T>` correctly, including the special compressed-OOP -> `void*` decode at line 11479-11483. Use mock variants — no JVM required.
- **asserts:** `value_t{int32_t{42}}` -> `int{42}` / `int64_t{42}` / `double{42.0}`; `value_t{std::monostate{}}` -> `int{} == 0`; `value_t{uint32_t{0x12345678}}` cast to `void*` calls `decode_oop_pointer` (need to stub `vmhook::hotspot::decode_oop_pointer` for the test).

### [standalone_unit] [new] test_method_proxy_is_static_matches_object_null
- **description:** Constructs `method_proxy{nullptr, fake_method, "()V"}` and `method_proxy{&obj, fake_method, "(I)V"}` and asserts `is_static()` returns true and false respectively. Currently both return false (Bug 5).
- **asserts:** `method_proxy{nullptr, m, sig}.is_static() == true`, `method_proxy{&obj, m, sig}.is_static() == false`.

### [standalone_unit] [extend_existing] test_method_proxy_return_type_dispatch_table
- **description:** Static-assert that the JNI table slot numbers used for each primitive return type (Z=39/119, B=42/122, C=45/125, S=48/128, I=51/131, J=54/134, F=57/137, D=60/140, V=63/143, L=36/116, Nonvirtual=93) all share the documented (static_slot - instance_slot == 80) and (object_slot < bool_slot) invariants. Catches a slot-offset typo at compile time.
- **asserts:** `static_assert(143 - 63 == 80)`, etc. Either via a constexpr table or a sequence of static_asserts.
- **existing_file:** tests/test_traits.cpp

### [jvm_integration] [extend_existing] test_call_jni_static_void_method
- **description:** Hook a no-arg method, then from inside the detour invoke a public static void method on the host class via `static_method("name")->call_jni()`. Confirm via a Java-side counter (incremented by the static method) that the JNI dispatch actually fired. Exercises lines 11591-11635 (static branch) + 11930-11942 (static-void dispatch).
- **asserts:** Java-side counter goes from N to N+1 after the call.
- **existing_file:** vmhook/src/example.cpp

### [jvm_integration] [new] test_call_jni_every_primitive_return_type
- **description:** Java fixture has nine static methods: `static boolean retZ() { return true; }`, `static byte retB()`, `static char retC()`, `static short retS()`, `static int retI()`, `static long retJ()`, `static float retF()`, `static double retD()`, `static void retV()`. Detour calls each via `static_method("retX")->call_jni()` and confirms the return value matches. Exercises every primitive case at lines 11944-12023 and the corresponding (slot, static_slot) tuples.
- **asserts:** `value_t -> bool == true`, `int8_t == 7`, `uint16_t == 'A'`, `int16_t == 1234`, `int32_t == 42`, `int64_t == 0x1234567890ABCDEF`, `float ~= 3.14f`, `double ~= 2.71828`, void leaves a Java-side counter incremented.

### [jvm_integration] [new] test_call_jni_static_method_on_non_bootstrap_class
- **description:** Resolve a static method on a host class loaded by LaunchClassLoader (simulate via a parent-last URLClassLoader in the fixture's Main.java). Verify the call fails the way Bug 1 predicts (FindClass returns null, monostate returned). Then verify the proposed fix (context-loader fallback) makes it succeed.
- **asserts:** Without fix: log contains "FindClass(name from pool_holder) returned null"; return is monostate. With fix: call returns the expected primitive.

### [jvm_integration] [new] test_call_jni_init_constructor_dispatch
- **description:** Allocate an object via `jni_make_unique` (which uses NewObjectA), then invoke `<init>` via a `method_proxy` to confirm the special-case at lines 11907-11927 routes through `CallNonvirtualVoidMethodA`. Compare with the no-special-case path: invoking `<init>` via raw `CallVoidMethodA` would either no-op or assert on HotSpot.
- **asserts:** The object's constructor side-effect (e.g. a Java-side counter increment, or an instance field set to a sentinel) is observable.

### [standalone_unit] [new] test_method_proxy_signature_no_close_paren_yields_monostate
- **description:** Pass a malformed signature like `"("` (no close paren) to a `method_proxy` and call `call_jni()`. Should hit the `rparen == npos` branch at lines 11569-11574 and return monostate without crashing.
- **asserts:** Return is `value_t{ std::monostate{} }`; log contains "malformed signature - no ')' found".

### [jvm_integration] [new] test_call_jni_cached_class_handle_thread_safety
- **description:** Two attached detour threads share a `method_proxy` and call it concurrently for ~10k iterations. Without the global-ref / mutex fix (Bug 2 + Bug 3), the test catches intermittent "GetMethodID returned null" or wrong return values; with the fix, all iterations succeed.
- **asserts:** All N*2 iterations return the expected value; no log line mentions "GetMethodID returned null".

### [jvm_integration] [new] test_call_jni_local_ref_leak_on_repeated_lookup_failure
- **description:** In a detour, call `static_method("nonexistent")->call_jni()` (guaranteed GetMethodID failure) 32 times. Before Bug 4 fix: ~16 calls in, JNI logs "local reference table overflow" because `cached_class_handle` is overwritten without DeleteLocalRef. After fix: no overflow.
- **asserts:** `vmhook` log free of "local reference table overflow" warnings after 32 iterations.

## Parity Concerns
- `method_proxy::value_t` (line 11439) has no `signature` member and no `to_vector<T>()` / `to_entries<K,V>()` / array helpers, despite `field_proxy::value_t` (line 10809) having all of these. After `call_jni` returns a Map / List / array, the user gets a bare `uint32_t` compressed OOP and has to manually call `decode_oop_pointer` + `vmhook::list{...}` themselves — vs `field_proxy::get().to_vector<T>()` which Just Works.
- `field_proxy` correctly threads `is_static_flag` through its constructor (line 11070) and `is_static()` returns the right answer; `method_proxy` hardcodes `static_field{false}` (line 11506), so `is_static()` is always wrong for static methods. Direct parity gap with a one-line fix.
- `field_proxy::set` was recently hardened with a `jvm_primitive_byte_width` size guard (CHANGELOG Unreleased). `method_proxy::call_jni`'s `write_jni_arg_to_slot` (line 11688) has no equivalent guard — passing a `int64_t` to an `"I"` parameter would silently write 8 bytes into a 4-byte JNI slot pattern. Worth auditing the slot-writer for the same hazard.
- `field_proxy` exposes `signature()` as a `std::string_view`; `method_proxy::signature()` (line 12311) does the same, BUT after `resolve_compatible_method` picks a different overload, `call_jni` uses `cached_effective_signature` internally while `proxy.signature()` still returns the ORIGINAL `signature_text`. User-facing introspection diverges silently from dispatch behavior.
- `field_proxy` has clear get/set ergonomics; `method_proxy::call()` returns a `value_t` whose underlying variant alternative depends on the JVM ret-type, but there's no `proxy.call_int()` / `proxy.call_string()` / `proxy.call_void()` shortcut to assert the type at the call site and avoid the implicit `static_cast` chain. A `call_as<T>(...)` returning `T` directly (and logging on mismatch) would close the ergonomic gap.
