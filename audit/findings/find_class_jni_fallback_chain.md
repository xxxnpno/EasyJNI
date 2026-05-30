# find_class_jni_fallback_chain

## Summary
`vmhook::detail::jni_find_class_with_context_loader` (vmhook/ext/vmhook/vmhook.hpp:9031-9162) is the fallback path used by `vmhook::find_class()` (vmhook/ext/vmhook/vmhook.hpp:6202-6284) when the ClassLoaderDataGraph walk misses. It tries three loaders in order — the current thread's context classloader, the system classloader, and Forge LaunchWrapper's `net.minecraft.launchwrapper.Launch.classLoader` — falling through to the next on each miss and returning the resolved Klass*. The chain is solid for the common HotSpot/Forge case, but it has several real correctness gaps (pending-exception leaks across path boundaries, array-class lookup failures, ignored repackaged/modern loaders) and a number of user-friendliness issues worth fixing.

## Bugs

### [HIGH] Array-class names cannot be resolved through this fallback
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:9070-9108
- **description:** `load_with_loader` converts the internal name's `/` to `.` and calls `ClassLoader.loadClass(String)`. The JLS / Javadoc for `loadClass` explicitly states it cannot resolve array class names (`[I`, `[Ljava/lang/String;`, etc.) — only `Class.forName` (or `JNIEnv::FindClass`, which the *primary* JNI helper already wraps) accepts JVM-internal array names. So if `vmhook::find_class("[Ljava/lang/String;")` ever falls through to this helper, every one of the three loader paths fails for an avoidable reason. Callers that walk `for_each_loaded_class`, generate reflection wrappers from method descriptors, or look up element types of `objArrayKlass`es will see spurious lookup failures.
- **repro:** Call `vmhook::find_class("[I")` or `vmhook::find_class("[Ljava/lang/Object;")` on a JDK where the array klass is not (yet) reachable via the ClassLoaderDataGraph walk. Each path inside `load_with_loader` returns null with a ClassNotFoundException, gets cleared, and the function returns null even though `JNIEnv::FindClass` (already called as `jni_find_class` elsewhere) would have succeeded.
- **suggested_fix:** Detect array names (`class_name.starts_with('[')`) at the top of `jni_find_class_with_context_loader` and route them directly through `jni_find_class(class_name)` + `jni_klass_from_class_mirror`, skipping the loader chain. Alternatively, in `load_with_loader` switch to `Class.forName(String, boolean, ClassLoader)` for names starting with `[`.
- **confidence:** certain

### [MEDIUM] Pending JNI exception leaks across loader-path boundaries
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:9110-9137
- **description:** `jni_find_class("java/lang/Thread")` and `jni_find_class("java/lang/ClassLoader")` (vmhook/ext/vmhook/vmhook.hpp:8794-8811) do NOT clear a pending exception on failure — they only return nullptr. The context-loader block (9110-9124) only calls `jni_exception_clear()` AFTER the block (line 9125) when control falls through. But there is no `jni_exception_clear()` *before* `jni_find_class("java/lang/ClassLoader")` is invoked on line 9127, and none before `jni_find_class("net/minecraft/launchwrapper/Launch")` either (the only clear before that is the one at 9137 inside the *next* failed path). A pending exception left over from, say, a `loadClass`-thrown ClassNotFoundException inside the lambda is normally cleared inside the lambda at line 9106 — but if `jni_call_object_method` itself returns null with no exception cleared (e.g. for an OOM-killed call), the next `jni_get_static_method_id`/`jni_get_static_field_id` will fail because JNI rejects calls with pending exceptions.
- **repro:** Force `getSystemClassLoader` to be unresolvable (or simulate a pending exception left from the context-loader path) and observe that the LaunchWrapper path fails to resolve `Launch.classLoader` even when the field exists, because `jni_get_static_field_id` short-circuits under a pending exception.
- **suggested_fix:** Call `vmhook::detail::jni_exception_clear()` *unconditionally* at the start of each loader-path block (before each `jni_find_class`/`jni_get_static_*` chain), not only on the tail-failure side. The cheapest fix is to call it before line 9110, before line 9127, and before line 9139.
- **confidence:** likely

### [MEDIUM] `jni_get_static_field_id` failure does not clear its own pending exception
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:8924-8930, 9150-9151
- **description:** `jni_get_static_field_id` (8924-8930) is intentionally minimal — unlike `jni_get_method_id` (8871-8883), it never wraps the call in `jni_exception_clear()` on entry and never clears on failure. When `Launch.classLoader` is absent (e.g. a recompiled LaunchWrapper that renamed the field, or a non-Forge JVM where the class is loaded but the field signature differs), `GetStaticFieldID` throws `NoSuchFieldError`. The pending exception is left dangling; `jni_get_static_object_field` is then called with `field_id == nullptr` (it short-circuits), and on the tail at 9157 `jni_exception_clear()` finally runs — but only after the function has already returned to `find_class`. If `find_class` itself is being called from a JNI re-entry context (e.g. inside a hook), the lingering pending exception between the field-id failure and the final clear could be observed by intermediate consumers.
- **repro:** Build with a doctored LaunchWrapper jar that exposes `Launch` without `classLoader`; trace JNI exception state across the lookup.
- **suggested_fix:** Mirror `jni_get_method_id`'s pattern in `jni_get_static_field_id`: clear before, clear after on failure. Same for `jni_get_static_method_id` and `jni_get_static_object_field`. (This is a sibling-API fix that benefits more than just this audit area.)
- **confidence:** likely

### [LOW] Misleading warning message when LaunchWrapper is the only fallback that wasn't tried
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:9143-9147
- **description:** The warning at 9143-9147 (`"all class loader paths failed (thread context, system, Minecraft Launch)"`) is logged when `jni_find_class("net/minecraft/launchwrapper/Launch")` returns nullptr — i.e. when LaunchWrapper is absent. The message reads as though the Launch path was *tried and failed*, which is false: LaunchWrapper is genuinely missing. Users debugging an injection into a vanilla JVM (no Forge) will read the message and think their installer is broken, when in fact LaunchWrapper was never in scope.
- **repro:** Inject into a vanilla `java -jar foo.jar`, run any `vmhook::find_class("my/Class")` that misses the graph; observe the warning that claims LaunchWrapper was tried.
- **suggested_fix:** Change the message to "thread context and system loaders both missed; LaunchWrapper is not present on this JVM (Forge not installed) — class is not reachable from any visible loader." Skip "Minecraft Launch" from the failed-paths list when `launch_class` was nullptr.
- **confidence:** certain

### [LOW] Redundant `class_loader_class` lookup wastes a JNI local ref per call
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:9078, 9127
- **description:** Both `load_with_loader` (9078) and the outer system-loader block (9127) call `jni_find_class("java/lang/ClassLoader")` separately and `bag.track` each result. When the chain reaches the system-loader path, the local-ref table now holds two distinct jclass refs for the same class. Local ref budget on attached worker threads is ~16 by default (JNI 1.2 spec) — this fallback already burns 6-8 refs per call; the duplicate ClassLoader ref pushes it closer to the cliff before `EnsureLocalCapacity` is even relevant.
- **repro:** Strace JNI calls; observe two `FindClass("java/lang/ClassLoader")` invocations per fallback that exercises both the context and system paths.
- **suggested_fix:** Hoist `class_loader_class` to the top of `jni_find_class_with_context_loader` and pass it into `load_with_loader` (or store it in a captured `void* class_loader_class_cached{}`). Same JNI ref drives both `loadClass` and `getSystemClassLoader`.
- **confidence:** certain

### [LOW] LaunchWrapper detection is hard-coded to legacy 1.7/1.8 Forge path
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:9139-9151
- **description:** The probe only checks `net/minecraft/launchwrapper/Launch` (LegacyLauncher / Forge ≤ 1.12, Lunar Client, Optifine standalone). Forge 1.13+, NeoForge, Fabric, Quilt, and ModLauncher all use different bootstrap classes — `cpw.mods.modlauncher.Launcher`, `net.fabricmc.loader.launch.knot.Knot`, `org.quiltmc.loader.impl.launch.knot.Knot`, etc. Each of these holds the real application classloader on a static field with a different name. The "Minecraft" name in the comment promises broader coverage than the code delivers.
- **repro:** Inject into 1.16+ Forge / Fabric, look up a mod-loaded class that is not yet in the graph; the LaunchWrapper probe misses and the function returns null even though the loader exists.
- **suggested_fix:** Either (a) walk a small table of `(class_name, field_name, field_signature)` triples covering LaunchWrapper, ModLauncher, FabricLoader, Knot, etc., or (b) document the LaunchWrapper-only scope clearly in the comment and at the public `vmhook::jni::find_class_with_context_loader` (9956-9960) docstring so users on modern modloaders know they need to call `vmhook::find_class` with a loader they've stashed themselves.
- **confidence:** certain

## Improvements

### [SMALL] [USER_FACING] Promote one-shot warnings to per-class throttle
- **rationale:** When a downstream consumer repeatedly calls `vmhook::find_class("com/foo/Bar")` for a class that genuinely doesn't exist, the cache stores nothing (only successes are cached — see 6265-6270), and every retry re-walks the graph AND the JNI fallback, emitting two warnings per call (one inside `jni_find_class_with_context_loader`, possibly one from the caller). This swamps logs in tight loops.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:6256-6263, 9143-9160
- **suggested_change:** Optionally negative-cache misses in `klass_lookup_cache` (with a small TTL or eviction on the next successful `for_each_loaded_class` notification), or add a `static thread_local std::unordered_set<std::string> already_warned` inside `jni_find_class_with_context_loader` to throttle the warning per name.

### [SMALL] [USER_FACING] Surface which loader path actually resolved the class
- **rationale:** When `vmhook::find_class` succeeds via the fallback chain, callers have no idea whether the class was found via the context loader, system loader, or LaunchWrapper. This makes diagnosing classloader-isolation bugs (the most common failure mode for injection-based tooling) much harder than it needs to be.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:9119, 9132, 9152
- **suggested_change:** When `load_with_loader` succeeds, `VMHOOK_LOG` an info-tag line that names the path: `"resolved '{}' via context loader"`, `"... system loader"`, `"... LaunchWrapper loader"`. Behind a debug-only category if log volume is a concern.

### [SMALL] [INTERNAL] Cache `loadClass` jmethodID and `Launch` jfieldID once
- **rationale:** `loadClass` method-id, `currentThread` static-method-id, `getContextClassLoader` method-id, `getSystemClassLoader` static-method-id, and `Launch.classLoader` static-field-id are all immutable for the lifetime of the JVM. Today they are re-resolved on every call that misses the graph cache. The hot path on first-use of N distinct classes does N JNI symbol lookups for the same thing.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:9085, 9113-9114, 9130, 9150
- **suggested_change:** Promote each id to a `static std::atomic<void*>` (or a `std::call_once` initializer) and reuse across calls. Mirror the caching strategy used in `method_proxy` (per the CHANGELOG entry at 180-182).

### [SMALL] [USER_FACING] Document that `find_class_with_context_loader` returns a Klass*, not a JNI handle
- **rationale:** The public name `vmhook::jni::find_class_with_context_loader` lives in a namespace alongside `vmhook::jni::find_class` (9948-9951) which returns a `void*` JNI handle. The fallback variant returns a HotSpot `klass*`. The 1-line comment at 9953-9955 does mention this, but the contrast is easy to miss when scanning the API surface — and the public detail's `noexcept` signature looks JNI-shaped. A new user could easily try to `DeleteLocalRef` the return value.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:9953-9960
- **suggested_change:** Beef up the doc-comment: explicitly call out that the return is a HotSpot metaspace pointer (not a JNI ref, do not delete), and that callers needing a JNI jclass should pass the result to a `klass_to_class_mirror_handle` helper (or just use `vmhook::jni::find_class` for the JNI-ref form).

### [MEDIUM] [INTERNAL] Replace `std::string{ class_name }` + replace with `std::string_view`-aware path
- **rationale:** Line 9092 allocates a heap `std::string` from the `string_view`, then iterates to replace `/` with `.`. Inside the lambda this happens every loader path that runs. With caching of method-ids and a small stack buffer (or `std::pmr::string`), the dotted name could be built once and reused — both the `name_string` jstring AND the dotted std::string can be hoisted out of the lambda.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:9092-9094
- **suggested_change:** Hoist `dotted_name` (and the resulting jstring) into `jni_find_class_with_context_loader` so it is created once per call rather than once per loader-path.

### [SMALL] [INTERNAL] Add `static_assert` documenting JNI slot numbers
- **rationale:** The chain depends on slots 6 (FindClass), 23 (DeleteLocalRef), 31 (GetObjectClass), 33 (GetMethodID), 36 (CallObjectMethodA), 113 (GetStaticMethodID), 116 (CallStaticObjectMethodA), 144 (GetStaticFieldID), 145 (GetStaticObjectField), 167 (NewStringUTF), 228 (ExceptionCheck). These slots are stable in the JNI spec, but the helpers spread them across many `jni_function<N>` call sites with no central reference. A regression where one slot was off-by-one would be a nightmare to debug.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:8794-9162
- **suggested_change:** Add a comment block or `inline constexpr` table (with a `static_assert(sizeof(JNIInvokeInterface_) == ...)` style sentinel) listing each slot and the JNI 1.1+ function it maps to, anchoring all the call sites.

## Tests

### [INTEGRATION] [MISSING] test_find_class_fallback_context_loader
- **description:** Inject into a Java test fixture that loads a custom class with a custom URLClassLoader installed as the thread's context classloader. The class is NOT registered in the ClassLoaderDataGraph at the time of the test (or rather, is reachable only through the context loader). Call `vmhook::find_class("my/Custom")` and assert it succeeds via the context-loader path.
- **asserts:** non-null klass*; klass name symbol equals `"my/Custom"`; subsequent call returns same klass* from cache.

### [INTEGRATION] [MISSING] test_find_class_fallback_system_loader
- **description:** Same fixture, but with the thread's context classloader cleared to null (e.g. set to bootstrap). Class is loaded by the application classloader. Assert resolution succeeds via the system loader path.
- **asserts:** non-null klass*; warning for context-loader miss is NOT emitted (system loader resolves silently); cache populated after.

### [INTEGRATION] [MISSING] test_find_class_fallback_launchwrapper
- **description:** Spawn a JVM with `net.minecraft.launchwrapper.Launch.main()` (a real LaunchWrapper jar on the classpath) and let it install its `classLoader` static. Call `vmhook::find_class` on a class that's only visible to the LaunchClassLoader. Assert resolution succeeds via the LaunchWrapper path.
- **asserts:** non-null klass*; if loader-path logging is added per the improvement above, the LaunchWrapper-tag line is emitted.

### [INTEGRATION] [MISSING] test_find_class_array_name_via_fallback
- **description:** Call `vmhook::find_class("[I")` and `vmhook::find_class("[Ljava/lang/String;")` after forcing the graph walk to miss (e.g. mock the cache to point at a never-loaded array name then evict). Today this returns null even though FindClass would succeed.
- **asserts:** non-null klass* for both names; lifecycle: cache then second call returns same pointer.

### [UNIT] [MISSING] test_find_class_fallback_returns_null_on_missing
- **description:** Call `vmhook::find_class("definitely/not/a/Class")` with no loader able to resolve it. Verify graceful null return AND that no JNI local refs leak (snapshot the local-ref slot count via `EnsureLocalCapacity` probing or via a Java-side `Thread.currentThread().getStackTrace()` reflection check).
- **asserts:** returns nullptr; warning message logged once; no pending JNI exception left on the thread (`ExceptionCheck` returns false on exit).

### [UNIT] [MISSING] test_find_class_fallback_exception_isolation
- **description:** Pre-set a pending JNI exception on the thread before calling `vmhook::find_class`. Assert the fallback chain does not corrupt or surface the pre-existing exception, and that it clears it before its own JNI work (or alternatively, documents that callers must clear).
- **asserts:** function returns a klass* (or nullptr) without throwing; `ExceptionCheck` on exit returns false.

### [UNIT] [MISSING] test_find_class_fallback_no_double_warning_on_repeat_misses
- **description:** Call `vmhook::find_class("missing/Cls")` 100 times; assert that only one warning is logged (or that warnings are throttled to a small bounded number).
- **asserts:** log line count for `'missing/Cls'` ≤ 2.

### [STRESS] [MISSING] test_find_class_fallback_local_ref_pressure
- **description:** From a JNI-attached worker thread with `EnsureLocalCapacity(16)` (the JNI minimum), call `vmhook::find_class` for 1000 distinct never-loaded class names back-to-back. The bag releases refs on each return; assert the local-ref slot is not exhausted (no `OutOfMemoryError: GC overhead limit` from JNI handle table).
- **asserts:** all calls complete; thread remains attached; no JNI handle-table OOM.

### [UNIT] [MISSING] test_find_class_fallback_modlauncher_path_documented_missing
- **description:** With a Forge 1.16+ JVM that uses `cpw.mods.modlauncher.Launcher`, call `vmhook::find_class` on a mod class and assert today's behavior (null). Pin the gap so a future improvement that adds ModLauncher support flips the assertion.
- **asserts:** today: returns nullptr; future: returns non-null klass.

## Parity Concerns
- `vmhook::jni::find_class` (vmhook/ext/vmhook/vmhook.hpp:9948-9951) returns a JNI `void*` handle while `vmhook::jni::find_class_with_context_loader` (9956-9960) returns a HotSpot `klass*`. Same namespace, similar names, very different return types and lifetime contracts (one needs `DeleteLocalRef`, the other does not). Either rename one (`klass_with_context_loader` for clarity) or provide matching JNI-handle / Klass* variants of both.
- The HotSpot-only `vmhook::find_class` and the JNI-fallback `vmhook::jni::find_class_with_context_loader` have no shared "explain what happened" diagnostic — callers cannot ask "which path resolved this?" or "which paths were tried?". A small `find_class_result` struct (klass*, resolution_path enum, std::string error_detail) would unify the two with a parallel `try_find_class` API.
- `jni_get_method_id` (8871-8883) clears pending exceptions on entry AND on failure; `jni_get_static_method_id` (8902-8908) and `jni_get_static_field_id` (8924-8930) do neither. This inconsistency is the root of the pending-exception bugs noted above and surprises anyone reading the helpers as a family.
- The LaunchWrapper detection at 9139-9151 is the only place in `vmhook.hpp` that probes for a host-application-specific class. There is no extensibility hook (no `vmhook::register_fallback_loader_probe(...)`) for downstream users to plug in their own bootstrap classloader (Fabric Knot, ModLauncher, Bukkit/Spigot CauldronClassLoader, etc.). Today, a user on a non-Forge modloader must call `vmhook::find_class` with a loader they stashed by hand from a hooked entry point.
