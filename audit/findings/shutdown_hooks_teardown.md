# shutdown_hooks_teardown

## Summary
`vmhook::shutdown_hooks()` flips `g_shutdown_requested`, wakes the auto-repair watchdog, then takes `g_hooked_methods_mutex` and walks both global vectors to delete every `midi2i_hook` (whose destructor restores the original i2i bytes) and clear `_dont_inline` / `NO_COMPILE` on every previously-hooked `Method*`. The teardown is well-commented and intentionally leaves `_code = nullptr` to avoid a known nmethod-flush UAF, but the shutdown flag is set permanently with no reset path — making the function effectively one-shot for the lifetime of the library — and several null-/validity-check gaps in the Method state-restore loop will fault if any captured Method was freed before teardown (which is exactly the scenario `verify_hooks()` was added to detect).

## Bugs

### [high] Permanently latched shutdown flag breaks re-install after teardown
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:8405-8470, 8355-8380, 5846-5856
- **description:** `shutdown_hooks()` does `g_shutdown_requested.store(true, …)` and never clears it. Subsequent `vmhook::hook<T>()` calls succeed in pushing entries into `g_hooked_methods`, but (a) `common_detour` early-returns on `g_shutdown_requested.load()` so the new hook silently never fires, and (b) `auto_repair::ensure_started()` short-circuits with the comment "Library has already been torn down; do not resurrect.", so the watchdog never re-spawns. From the user's perspective `hook()` returns `true` but the detour is dead — there is no warning, no diagnostic, and no way to recover short of unloading the DLL. This breaks the documented "tear everything down, then install again" pattern that's commonly used by mod loaders that re-init on world switch.
- **repro:** `vmhook::hook<Foo>("bar", cb);` → `vmhook::shutdown_hooks();` → `vmhook::hook<Foo>("bar", cb);` returns `true` but `cb` is never invoked.
- **suggested_fix:** Reset `g_shutdown_requested.store(false, std::memory_order_release)` at the very end of `shutdown_hooks()` (after the two vector clears). Also reset `detail::auto_repair::g_started` so a subsequent `hook<T>()` call re-spawns the watchdog. Document the new "shutdown_hooks() is idempotent and reversible" contract in the function header.
- **confidence:** certain

### [high] Null dereference in restore loop when Method* was freed by JVMTI before teardown
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:8432-8440, 5935-5942
- **description:** The Method-restore loop calls `set_dont_inline(hooked_method_entry.method, false)` and then `hooked_method_entry.method->get_access_flags()` without first validating `hooked_method_entry.method`. `set_dont_inline` dereferences `method_pointer->get_flags()` unconditionally with no null/validity check — it would AV if the Method* slot was zeroed, and would corrupt unrelated memory if the slot was recycled by HotSpot's allocator. `verify_hooks()` exists precisely because this aliasing happens in practice (mode-1 "Method freed" and mode-2 "Method aliased" cases described at lines 8147-8164). If the user calls `shutdown_hooks()` after a class-redefinition event but before the watchdog's next pass, the loop will either crash the JVM or scribble flags into someone else's Method object.
- **repro:** Hook `EntityRenderer.orientCamera`, have a JVMTI agent (Forge, Lunar, JDI debugger) `RedefineClasses(EntityRenderer)`, then call `shutdown_hooks()` before the auto-repair watchdog re-installs.
- **suggested_fix:** Mirror the validity guards `verify_hooks()` already uses — skip entries where `hooked_method_entry.method` is null or `!vmhook::hotspot::is_valid_pointer(hooked_method_entry.method)` or `!hooked_method_entry.method->get_const_method()`. Log a single warning per skip ("hook for 'X.y' was freed before shutdown — skipping flag restore"). Defensive `if (method_pointer)` guard inside `set_dont_inline` would be a defence-in-depth bonus.
- **confidence:** likely

### [medium] No quiescence between trampoline-free and i2i-restore, so detours mid-execution land in freed code
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:8427-8430, 5524-5545, 718-730
- **description:** The shutdown flag stops *new* `common_detour` calls, but threads already inside the trampoline's assembly prologue (the `push rax/…/push rbp; push 0; push 0; mov rcx,rbp; …; call [rip+disp32]` sequence before `common_detour` is reached, or the pop/jmp sequence after it returns) are still executing in `this->allocated`. `~midi2i_hook` first rewrites the 5-byte JMP at the i2i target (good — new entries skip the trampoline) and then immediately calls `os::release(this->allocated, this->allocated_size)` which on Windows is `VirtualFree(addr, 0, MEM_RELEASE)` — the page is gone the instant the syscall returns. Any in-flight thread executing in that page faults. Window is small but real, especially during JVM shutdown when thousands of Java frames are unwinding through hooked methods (finalizer dispatch, sweeper, etc.).
- **repro:** Hook a hot method (`Minecraft.runTick`), spawn a busy thread that keeps it hot, call `shutdown_hooks()` from a different thread under load. Crash dump lands in 0x10?????? code-cache range with `kb` showing the freed trampoline RIP.
- **suggested_fix:** Either (a) decommit the trampoline's executable bits but defer the `VirtualFree` to a small grace pool (release after, say, 1s — long enough that any thread that entered before the i2i restore has finished its 100-instruction trampoline body), or (b) leak the trampoline pages on shutdown (cheap — one page per unique i2i stub × tens of stubs max). Option (b) is simpler and matches the rationale already used for `_code` non-restoration. At minimum, document the race in the destructor comment.
- **confidence:** likely

### [low] notify_shutdown() does not join the watchdog before clearing vectors
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:8419, 8321-8353, 8368-8378
- **description:** The watchdog thread is `detach()`'d at spawn (line 8371) and `notify_shutdown()` only kicks the condvar; `shutdown_hooks()` then proceeds straight to acquire `g_hooked_methods_mutex`. If the watchdog happened to be mid-iteration of `verify_hooks()` (already holding the mutex), shutdown blocks at the lock — fine — but right after shutdown clears the vectors and releases the mutex, the watchdog re-checks the flag and exits without ever observing the cleared state. However, between releasing the mutex and the watchdog's exit, nothing prevents the watchdog from doing one more `verify_hooks()` pass if `notify_shutdown()`'s notify_all fired AFTER the watchdog had already passed the predicate check (e.g., the watchdog woke from timeout right before notify, ran verify_hooks on the still-populated vector, then re-checked the flag at line 8340 and exited cleanly). That last verify_hooks pass races nothing because shutdown is still blocked on the mutex it holds, so practically it's fine — but the design relies on this ordering and is fragile. There is no `g_watchdog_running` flag that shutdown can wait on.
- **repro:** Conceptual race only; hard to repro without instrumentation. No known user-visible crash.
- **suggested_fix:** Add a `std::atomic<bool> g_watchdog_running{ false }` set/cleared at the top and bottom of `worker_loop`. After flipping `g_shutdown_requested` and `notify_shutdown()`, spin-wait (or condvar-wait, max 2× interval) until `g_watchdog_running` is false before taking the install mutex. This also helps the dynamic-unload case where the user later DLL-unloads vmhook — currently the detached worker_loop would access a torn-down g_cv_mutex.
- **confidence:** speculative

### [low] hook_handle::stop() leaves orphaned midi2i_hook entries when the last sharing method is removed
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:8472-8533, 8427-8430
- **description:** Single-handle `stop()` removes the entry from `g_hooked_methods` but **never** removes the matching `i2i_hook_data` from `g_hooked_i2i_entries`. The comment at line 8501-8505 says this is deliberate ("other hooks may still share the same i2i entry point"). That's correct for the typical case, but if the user removes every hook one-by-one via individual handle destruction, `g_hooked_i2i_entries` keeps growing forever (well, it stops growing because the same i2i is reused, but the trampolines remain installed and `common_detour` keeps being entered for unrelated methods — it falls through harmlessly, but it's pointless overhead and unbounded memory). `shutdown_hooks()` is the only path that ever frees the trampolines, so a user who relies on RAII handles and never calls `shutdown_hooks()` (allowed by the API) leaks every trampoline page until process exit.
- **repro:** Tight loop: install N scoped_hook handles, drop them. Watch `g_hooked_i2i_entries.size()` and the executable region count climb monotonically.
- **suggested_fix:** In `hook_handle::stop()`, after erasing from `g_hooked_methods`, check whether any remaining `hooked_method` shares the same i2i. If not, find the matching `i2i_hook_data`, `delete` its `hook`, and erase from `g_hooked_i2i_entries`. Apply the same quiescence concern from bug #3.
- **confidence:** likely

## Improvements

### [S] [USER_FACING] Log a one-line shutdown summary
- **rationale:** Right now `shutdown_hooks()` is silent unless something throws (and the loops can't throw). Users debugging "did my uninject actually run?" have no breadcrumb. A single `VMHOOK_LOG("{} shutdown_hooks: released {} trampolines and re-armed {} methods.", …)` after the clears mirrors the existing logging style and costs nothing.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:8466-8469
- **suggested_change:** Capture `g_hooked_i2i_entries.size()` and `g_hooked_methods.size()` before the clears, log once with `vmhook::info_tag` after.

### [S] [USER_FACING] Document idempotency and no-op-when-empty behaviour
- **rationale:** The doxygen on `shutdown_hooks` (lines 7985-8008) explains phases 1-3 but never states that it is safe to call before any hook is installed, safe to call twice, or whether it can be called from any thread. Users reading the header have to read implementation to know. The class-level note at lines 24-25 says "hook installation and shutdown_hooks() MUST be called from a single thread" — but the implementation locks `g_hooked_methods_mutex`, so the constraint is actually weaker than stated. Either tighten the implementation to enforce the documented constraint, or weaken the docs to reflect reality.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:7985-8008, 24-25
- **suggested_change:** Add to header: "Idempotent: safe to call multiple times. Safe to call before any hook is installed. Acquires `g_hooked_methods_mutex` internally, so concurrent calls from multiple threads serialise correctly — but mixing with `vmhook::hook<T>()` calls from the same thread is the only contract verified by tests."

### [M] [INTERNAL] Extract a `reset_method_jit_inhibitors` helper shared with hook_handle::stop()
- **rationale:** The Method-flag-restore block (lines 8432-8466) and the same block in `hook_handle::stop()` (lines 8506-8519) are byte-for-byte duplicates — same `set_dont_inline(method, false)`, same `*flags &= ~NO_COMPILE` pattern. Both will need the same null-guard fix from bug #2, and both will need the same evolution if HotSpot adds new inhibitor bits. One helper keeps them in lock-step.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:8432-8466, 8506-8519
- **suggested_change:** Add `static auto restore_method_jit_state(vmhook::hotspot::method* m) noexcept -> void` next to `set_dont_inline`, containing the validity check + dont_inline clear + NO_COMPILE clear. Replace both call sites with `restore_method_jit_state(entry.method)`.

### [S] [USER_FACING] Add `is_shutdown()` / `is_hooks_installed_count()` introspection
- **rationale:** Users who want to gate their cleanup on "did shutdown already run?" have no way to ask without checking a private vector. Same for "how many hooks are currently armed?". A two-line accessor pair makes the contract self-documenting and lets users write defensive `if (!vmhook::is_shutdown()) vmhook::shutdown_hooks();` patterns.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:8405-8470
- **suggested_change:** Add `static auto is_shutdown() noexcept -> bool { return hotspot::g_shutdown_requested.load(std::memory_order_acquire); }` and `static auto installed_hook_count() noexcept -> std::size_t { std::lock_guard<std::mutex> g{hotspot::g_hooked_methods_mutex}; return hotspot::g_hooked_methods.size(); }` in the public namespace.

### [XS] [INTERNAL] Reserve vector capacity is dropped on clear() but not shrunk
- **rationale:** `clear()` keeps capacity. After repeated install/shutdown cycles a user who hooked many methods at peak has a large reserved buffer permanently. Microscopic memory waste, not worth fixing unless bug #1 is addressed (then it matters).
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:8468-8469
- **suggested_change:** Replace `.clear()` with `.clear(); .shrink_to_fit();` — or with `std::vector<>{}.swap(g_hooked_methods)` if `shrink_to_fit` is treated as non-binding by the impl. Marginal.

## Tests

### [standalone_unit] [new] test_shutdown_hooks_idempotent_when_no_hooks_installed
- **description:** Call `vmhook::shutdown_hooks()` against a fresh process with zero installed hooks. Then call it again. Then call it a third time. Verify no crash, no exception, and the two global vectors remain empty / size 0.
- **asserts:** No SEH/C++ exception escapes. `vmhook::installed_hook_count()` (if exposed) == 0. The function returns in well under a millisecond on the second/third call.
- **existing_file:** tests/test_api_surface.cpp (compile-only today — would need a small runtime-section gate)

### [standalone_unit] [extend_existing] test_shutdown_hooks_safe_without_jvm
- **description:** `tests/test_api_surface.cpp` already calls `vmhook::shutdown_hooks()` on line 47 after a failed install attempt against no JVM. Extend with a second consecutive call (idempotency) and an assertion that no log lines at `error_tag` were emitted during the second call.
- **asserts:** Compile + execute success. Capture VMHOOK_LOG output via temporary `VMHOOK_LOG_FILE` env var; verify no `[error]` lines appear in the second-call window.
- **existing_file:** tests/test_api_surface.cpp

### [jvm_integration] [extend_existing] test_shutdown_hooks_clears_for_reinstall
- **description:** Install a hook, call `shutdown_hooks()`, install the SAME hook again, invoke the Java method, verify the detour fires the second time. This is the regression test for bug #1 (permanently latched shutdown flag).
- **asserts:** Second-install detour observed via an `std::atomic<bool> fired_after_reinstall`. Counter shows the detour ran exactly once for the post-shutdown invocation.
- **existing_file:** vmhook/src/example.cpp (already has 10+ `test_method_*` helpers that follow this hook→shutdown_hooks pattern — add `test_hook_after_shutdown` next to `test_method_non_static`)

### [jvm_integration] [new] test_shutdown_hooks_under_concurrent_detour_load
- **description:** Hook a hot Java method, spawn 8 Java threads that hammer it for 200 ms, call `shutdown_hooks()` from the C++ thread mid-storm, then sleep for a quiescence window and try to invoke the method one more time from the main thread. Verify (a) the JVM did not crash and (b) the detour was not called after `shutdown_hooks` returned.
- **asserts:** No 0xC0000005 in 0x10?????? range (capture via SEH filter on the main thread or via PROCESSDUMP harness). `post_shutdown_detour_count.load() == 0`. This exercises bug #3's quiescence window.
- **existing_file:** none yet (would slot into the JVM-integration matrix near `test_method_non_static` in `vmhook/src/example.cpp`)

### [jvm_integration] [new] test_shutdown_hooks_after_class_redefinition
- **description:** Hook `Foo.bar`, have the test driver call JVMTI `RedefineClasses(Foo)` (or simulate by manually zeroing the captured Method* via a debug build hook), then call `shutdown_hooks()`. Verify no AV in `set_dont_inline` / `get_access_flags`. This is the regression test for bug #2.
- **asserts:** `shutdown_hooks()` returns cleanly. The expected drift-warning is logged once. No exception or SEH escapes.
- **existing_file:** none (new — would live in a JVMTI-aware integration test if/when one is added; gate behind a JVMTI-available preprocessor flag)

### [standalone_unit] [new] test_shutdown_hooks_notifies_watchdog_before_lock
- **description:** Whitebox: inject a stall into `verify_hooks()` (e.g., via a hook on a test-only `g_test_verify_stall_ms`), let the watchdog enter it, call `shutdown_hooks()`, time the call. Verify shutdown still completes within `2 × VMHOOK_AUTO_REPAIR_INTERVAL_MS` even with the watchdog "busy" (proves the notify-then-lock ordering at 8412-8425 works under contention).
- **asserts:** `shutdown_hooks()` completes in < 2 × interval. Watchdog log line "shutting down" appears before "shutdown_hooks complete".
- **existing_file:** none (would require a test-only `verify_hooks` stall hook)

## Parity Concerns
- The bug #1 latched-flag issue means there is no clean "restart" mode. Sibling APIs that *also* spawn lifecycle-bound resources (`scoped_hook` returning a handle that re-installs on rebuild, `auto_repair::ensure_started` which is gated on the same flag, `watch_static_field` infrastructure with its own dr-slot lifecycle) all assume a single process-wide install→teardown cycle. Either commit to that contract everywhere and document it loudly (recommended for an in-process injectable lib), or fix bug #1 and make every sibling re-init-aware.
- `hook_handle::stop()` (single-hook teardown) and `shutdown_hooks()` (bulk teardown) duplicate the Method-restore block (see Improvement: extract helper). They also differ in error handling: `hook_handle::stop()` catches `std::exception` and unknown exceptions explicitly, `shutdown_hooks()` only relies on `noexcept` and would `std::terminate` if any sub-call surprised by throwing. Pick one convention.
- The class header comment at lines 24-25 says install + shutdown_hooks must be called from a single thread, but the install mutex (`g_hooked_methods_mutex`) is acquired in both. Either remove the thread restriction from the docs or remove the mutex from the hot path — current state is "we say A but implement B" which confuses anyone trying to use the library safely from multiple threads.
