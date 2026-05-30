# on_class_loaded_define_class_hook

## Summary
`vmhook::on_class_loaded` hooks the explicit-signature 5-arg variant
`ClassLoader.defineClass(String, byte[], int, int, ProtectionDomain)`
and dispatches each callback under a snapshot of the mutex-guarded
registry. The implementation is sound for the steady-state, but the
`class_load_hook_installed` flag is never cleared on
`shutdown_hooks()`, which makes class-load notifications silently dead
after any teardown/re-arm cycle; the 3-arg `ByteBuffer` overload and
the JDK 9+ `defineClass1`/`defineClass2` native fast paths are also
not hooked, so several common class-define paths (notably
`Unsafe.defineAnonymousClass`, agent-driven `Instrumentation`, and
`ClassLoader.defineClass(name, ByteBuffer, pd)`) escape unobserved.

## Bugs

### [HIGH] `class_load_hook_installed` flag is never reset on shutdown — re-arm after `shutdown_hooks()` is a silent no-op
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:15106, 15195-15208, 8405-8470
- **description:** The static flag `detail::class_load_hook_installed`
  (line 15106) is flipped to `true` after a successful `vmhook::hook<...>`
  install (line 15207). `shutdown_hooks()` (8405-8470) tears down every
  entry in `g_hooked_methods` — including the `ClassLoader.defineClass`
  entry — but never clears `class_load_hook_installed`. A subsequent
  `vmhook::on_class_loaded(...)` therefore takes the `if
  (!detail::class_load_hook_installed)` branch (line 15195), sees `true`,
  and skips re-installation. The new callback is registered in
  `class_load_callbacks` but the underlying detour no longer exists,
  so the callback never fires. The watch_handle silently disarms when
  dropped (the on_stop just erases from the list).
- **repro:**
  ```cpp
  {
      auto w1 = vmhook::on_class_loaded([](const std::string& n){ std::println("a: {}", n); });
      // ... triggers Class.forName("Foo") — callback fires
  }
  vmhook::shutdown_hooks();
  auto w2 = vmhook::on_class_loaded([](const std::string& n){ std::println("b: {}", n); });
  // Trigger Class.forName("Bar") — callback never fires.
  ```
- **suggested_fix:** Reset `detail::class_load_hook_installed = false`
  (and the sibling `detail::exception_hook_installed = false`) inside
  `shutdown_hooks()` after `g_hooked_methods.clear()`. Doing it under
  the relevant `class_load_mutex` / `exception_mutex` is the cleanest
  fix because the test on lines 15195 / 15309 is already mutex-guarded.
  Alternatively, expose a `detail::reset_watcher_state()` helper that
  `shutdown_hooks()` calls and that all watcher modules subscribe to.
- **confidence:** certain

### [MEDIUM] ByteBuffer overload `defineClass(String, ByteBuffer, ProtectionDomain)` is not hooked — entire NIO path is invisible
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:15165-15166, 15200-15233
- **description:** The README and the docstring acknowledge that the
  `(Ljava/lang/String;Ljava/nio/ByteBuffer;Ljava/security/ProtectionDomain;)Ljava/lang/Class;`
  overload is not hooked. This overload is the standard path used by
  modern agents that build class bytecode into a direct ByteBuffer
  (e.g. ASM's `ClassVisitor` -> `ByteBuffer`), by many ahead-of-time
  loaders, and by some `URLClassLoader` subclasses. It is also reached
  indirectly when a user calls `defineClass(name, ByteBuffer, pd)`
  from JVMTI agents. Because both overloads ultimately fall into the
  same native `defineClass1`/`defineClass2` plumbing, hooking only one
  Java entry point gives an incomplete picture and silently drops
  events the user reasonably expects.
- **repro:** From Java:
  ```java
  byte[] bytes = ...;
  java.nio.ByteBuffer bb = java.nio.ByteBuffer.allocateDirect(bytes.length);
  bb.put(bytes).flip();
  myLoader.defineClass("pkg/MyClass", bb, null); // never observed
  ```
- **suggested_fix:** After the 5-arg install succeeds, also install a
  second hook with signature
  `"(Ljava/lang/String;Ljava/nio/ByteBuffer;Ljava/security/ProtectionDomain;)Ljava/lang/Class;"`
  using a 3-arg detour `(return_value&, unique_ptr<class_loader_wrapper>&, std::string name, oop_t bytebuffer, oop_t protection_domain)`.
  Both detours can share the same dispatch helper that snapshots the
  callback list and invokes them. Treat install failure of the second
  hook as non-fatal (log and continue) so older HotSpot builds that
  omit it still get the primary path.
  Optional follow-up: also hook the package-private
  `defineClass1`/`defineClass2` native entries via `hotspot::method`
  lookup so bootstrap class loads become visible (this is what JVMTI
  `ClassFileLoadHook` uses).
- **confidence:** likely

### [LOW] `name` from `defineClass(null, ...)` becomes an empty string and is still dispatched — callback cannot tell "anonymous" from "name read failed"
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:15184-15198, 14470-14495
- **description:** Java permits a null `name` argument to
  `ClassLoader.defineClass`. `read_java_string` (14470) returns `""`
  for a null/uninitialised String OOP without distinguishing that from
  a decode failure. The detour blindly forwards the empty string to
  every callback, so users cannot tell the legitimate "anonymous class"
  case from "the name wasn't readable yet" (e.g. early init when
  `find_class("java/lang/String")` returns null and the read function
  bails). Worse, the callback may attempt `find_class(internal_name)`
  on the received string and fail confusingly.
- **repro:** From Java: `loader.defineClass(null, bytes, 0, bytes.length, null);`
  callback fires with `internal_name == ""`.
- **suggested_fix:** Either (a) skip dispatch when `name.empty()` and
  log a one-shot warning the first time it happens, or (b) wrap the
  argument as `std::optional<std::string>` / pass a second `bool
  name_was_null` parameter so the callback can branch. Document the
  behaviour in the doc-comment regardless.
- **confidence:** likely

### [LOW] Lazy `register_class<class_loader_wrapper>` ignores its return value — a clean failure leaves the flag-set path racing the missing wrapper
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:15194-15203
- **description:** `register_class<...>(...)` returns `false` when the
  class cannot be found (e.g. `find_class("java/lang/String")` is not
  ready, or the JVM was started without `java.lang.ClassLoader` in the
  shared archive — pathological but observable in CDS-disabled paths).
  The on_class_loaded code path calls it for its side-effect and does
  not check the result. If it fails, the subsequent
  `vmhook::hook<detail::class_loader_wrapper>(...)` call also fails
  because the type-to-class map entry is still missing, so the user
  sees `"ClassLoader.defineClass hook installation failed"` with no
  hint about the underlying registration failure. The user only sees
  one error; the actual root cause is logged separately and far away.
- **suggested_fix:** Check the return value:
  ```cpp
  if (!vmhook::register_class<detail::class_loader_wrapper>("java/lang/ClassLoader"))
  {
      VMHOOK_LOG("{} on_class_loaded: register_class<ClassLoader> failed; "
                 "the JVM does not have java.lang.ClassLoader resolvable yet.",
                 vmhook::error_tag);
      // bail without installing — callback is already in the list, so
      // the caller's watch_handle will still erase it cleanly.
      return watch_handle{ ... };
  }
  ```
  Same pattern applies to `on_exception` for parity.
- **confidence:** likely

## Improvements

### [S] [USER_FACING] Document the JDK signature stability of the hooked overload
- **rationale:** Users targeting JDK 8 through 25 need to know that
  `defineClass(String, byte[], int, int, ProtectionDomain)` has been
  stable since JDK 1.2 and is still present in JDK 25 (its bytecode
  signature in `ClassLoader.class` is unchanged). The current doc only
  says "ClassLoader.defineClass" without spelling out the long-term
  compatibility. Add a one-line note: "Signature has been ABI-stable
  on `java.lang.ClassLoader` since JDK 1.2 and is verified to exist on
  JDK 8 through JDK 25."
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:15125-15140
- **suggested_change:** Append a sentence to the @details block:
  "The hooked descriptor is stable across every Hotspot release from
  JDK 8 through JDK 25; the hook installs lazily on first call so the
  user pays nothing until `on_class_loaded` is invoked."

### [S] [USER_FACING] Improve the "installation failed" log line to mention which overload
- **rationale:** Once the ByteBuffer overload is added, the user needs
  to know which install failed. Even today, the message
  `"ClassLoader.defineClass hook installation failed"` does not tell
  the user whether `register_class` succeeded, whether the method
  symbol was missing, or whether the trampoline patch failed. The
  underlying `vmhook::hook<>` already logs a richer message, but the
  user reads the surface-level one first.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:15211-15212
- **suggested_change:** Replace with: `"on_class_loaded: hook install
  failed for ClassLoader.defineClass(String,byte[],int,int,ProtectionDomain)
  — see the preceding hook<> error for the root cause."`

### [S] [INTERNAL] Hold the dispatch lock for the snapshot copy only, not the dispatch
- **rationale:** The current code already releases `class_load_mutex`
  before invoking callbacks (15211-15213). That is correct. However,
  the snapshot is a `std::vector<std::shared_ptr<...>>` copy, which
  bumps every shared_ptr refcount under the lock. For high-fan-out
  registrations (e.g. 50+ watchers across plugins), the inner loop
  becomes O(N) atomic incs per dispatch. Replace the snapshot with
  `std::shared_ptr<const std::vector<std::shared_ptr<class_load_callback_t>>>`
  rebuilt on each register/unregister (copy-on-write); the detour
  reads the current shared_ptr atomically and iterates without any
  lock at all.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:15179-15183, 15217-15226
- **suggested_change:** Convert `class_load_callbacks` to
  `inline std::atomic<std::shared_ptr<const callbacks_vec_t>> active_callbacks;`
  (C++20 `atomic<shared_ptr>` is available). Mutations rebuild the
  vector under the mutex and `store()`; the detour does a
  single `load()` and iterates. Mirrors a common "RCU-lite" pattern.

### [S] [USER_FACING] Surface "no callbacks registered" as no-op fast path in the detour
- **rationale:** If the user is using `on_class_loaded` transiently
  (scoped watcher) but the hook stays installed for the lifetime of
  the process (it does — neither the flag nor the underlying hook is
  uninstalled when the last watch_handle drops), every class load
  pays the cost of lock-acquire + snapshot-copy. After the last
  callback unregisters, the detour should take a fast path that
  checks an `atomic_bool empty_flag` and returns without locking.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:15170-15198, 15217-15238
- **suggested_change:** Maintain `std::atomic<size_t>
  detail::class_load_callback_count` updated under the mutex.
  Detour first line: `if (detail::class_load_callback_count.load(std::memory_order_relaxed) == 0) return;`.
  Better: tear down the underlying `hook_handle` when the count drops
  to zero (requires storing the handle returned by `vmhook::hook<>`),
  and re-install on the next registration.

### [M] [USER_FACING] Provide a callback signature with the raw `byte[]` slice so users can read the bytecode
- **rationale:** The single biggest reason a user installs
  `on_class_loaded` is to inspect or capture the bytecode (e.g. dump
  every class loaded by a target plugin for static analysis). Today
  the detour drops `bytes`, `offset`, `length`. Exposing them via an
  overload `on_class_loaded([](const std::string& name, std::span<const std::byte> bytecode){...})`
  is far more useful than the bare name. Detect the callable arity at
  compile time (already supported by `function_traits` in detail) and
  invoke the right overload.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:15170-15198
- **suggested_change:** Add a templated dispatcher: if the user's
  callable accepts `(const std::string&)`, fall through to the
  existing path; if it accepts `(const std::string&, std::span<const std::byte>)`,
  decode the `byte[]` OOP into a `std::span` (header + length + data
  pointer; pattern already used by `read_java_string` for
  `byte[]` at 14508-14518) and pass it through. Document that the
  span is valid only for the duration of the callback.

### [S] [INTERNAL] Add VMHOOK_LOG line confirming the install succeeded
- **rationale:** The failure path logs, the success path is silent.
  For a watcher feature, "did the hook actually arm?" is the question
  the user asks first when debugging. One DEBUG-level log on
  successful install removes a class of "why isn't my callback firing"
  questions.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:15205-15208
- **suggested_change:**
  ```cpp
  if (installed)
  {
      detail::class_load_hook_installed = true;
      VMHOOK_LOG("{} on_class_loaded: ClassLoader.defineClass hook armed.",
                 vmhook::debug_tag);
  }
  ```

## Tests

### [jvm_integration] [exists] class_load_observed_late_class
- **description:** Existing test in `vmhook/src/example.cpp`
  `test_class_load_watcher` verifies the watcher observes a
  `Class.forName("vmhook/LateClass")` triggered from the Java probe.
- **asserts:** `late_seen == true` after `run_java_probe` returns,
  `classLoadProbeDone == true`.
- **existing_file:** vmhook/src/example.cpp:2947-2974, example/vmhook/Main.java:223-238

### [jvm_integration] [new] class_load_watcher_rearms_after_shutdown
- **description:** Tests the HIGH-severity bug above. Install a
  watcher, observe one class load, call `vmhook::shutdown_hooks()`,
  install a fresh watcher, trigger another `Class.forName` from
  Java, assert the second callback fires.
- **asserts:** Both `first_seen` and `second_seen` are `true`.
- **existing_file:** extend vmhook/src/example.cpp:test_class_load_watcher

### [jvm_integration] [new] class_load_watcher_bytebuffer_overload
- **description:** From Java, load a class via the 3-arg
  `defineClass(String, ByteBuffer, ProtectionDomain)` overload (use a
  small `URLClassLoader` subclass that calls the protected overload)
  and verify the watcher observes it. Currently expected to FAIL,
  which is the audit's whole point.
- **asserts:** `bytebuffer_seen == true` for the loader-defined class.
- **existing_file:** extend example/vmhook/Main.java and vmhook/src/example.cpp:test_class_load_watcher

### [jvm_integration] [new] class_load_watcher_handles_null_name
- **description:** Call `loader.defineClass(null, bytes, 0, bytes.length, null);`
  from Java. Verify behaviour matches the documented contract — today
  it dispatches with `name == ""`, which the test should pin so any
  future fix has a measurable regression target.
- **asserts:** Either `null_seen == true` with `name.empty()`, or
  (after the LOW fix) `dispatch_skipped == true`.
- **existing_file:** new — extend test_class_load_watcher

### [jvm_integration] [new] class_load_watcher_multiple_callbacks_dispatch
- **description:** Register 4 watchers concurrently from 4 threads
  before triggering `Class.forName`, then drop two mid-dispatch.
  Verifies the snapshot copy under `class_load_mutex` prevents UAF
  from a callback erasing itself, and that no callback is observed
  twice or missed.
- **asserts:** Each surviving callback's counter increments exactly
  once per class-load event; no crash; erased callbacks do not fire
  after their `watch_handle` is destroyed.
- **existing_file:** new — extend test_class_load_watcher

### [jvm_integration] [new] class_load_watcher_observes_internal_slash_form
- **description:** Trigger `loader.defineClass("pkg.Sub.Cls", ...)`
  (Java-style dotted name) and assert the callback receives
  `"pkg/Sub/Cls"` — the detour does the `.` -> `/` replacement at
  15184-15185, but no test currently asserts the form.
- **asserts:** `observed_name == "pkg/Sub/Cls"`, never `"pkg.Sub.Cls"`.
- **existing_file:** extend test_class_load_watcher

### [standalone_unit] [new] on_class_loaded_compiles_without_jvm
- **description:** Compile-only smoke test in `tests/test_api_surface.cpp`
  that instantiates `vmhook::on_class_loaded(...)` and stores the
  resulting `watch_handle`. Without a JVM the lazy install will fail
  silently (logged), but the symbol must link and the callable must
  type-check.
- **asserts:** Compiles on MSVC, GCC, Clang; the resulting
  `watch_handle` is movable and destruction does not throw.
- **existing_file:** extend tests/test_api_surface.cpp

## Parity Concerns
- `on_exception` shares the identical `_hook_installed`-flag bug with
  `on_class_loaded` (the flag is never cleared on `shutdown_hooks()`).
  Any fix for the HIGH bug above should be applied symmetrically.
- `on_exception` already passes the throwable's dynamic class name
  via `klass_from_oop(self->get_instance())`. `on_class_loaded` could
  do the same: instead of (or in addition to) the Java-string `name`
  argument, read the freshly-defined `Class<?>`'s klass from the
  return value once the original method completes. This would
  eliminate the null-name ambiguity (LOW bug above) entirely.
- `watch_static_field` returns a `watch_handle` that genuinely
  disarms its hardware breakpoint on drop. `on_class_loaded`'s
  `watch_handle` only erases the callback from the list; the
  underlying detour stays installed forever. This is documented
  nowhere and is a parity gap users will hit (memory-hot loop ->
  permanent O(callbacks) overhead per class load even after they
  drop the handle).
- `on_class_loaded` and `on_exception` both use a private detail
  wrapper class (`class_loader_wrapper`, `throwable_wrapper`) lazily
  registered on first call. A small `detail::ensure_registered<T>(name)`
  helper would deduplicate the four-line snippet at 15194-15198 and
  15341-15345.
