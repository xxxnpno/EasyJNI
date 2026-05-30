# scoped_hook_raii

## Summary
`scoped_hook<T>()` + `hook_handle` are a thin RAII wrapper over `vmhook::hook<T>()` and a per-entry copy of `shutdown_hooks()`. The construction path is internally consistent and the destructor correctly locks `g_hooked_methods_mutex` and clears `_dont_inline` / `NO_COMPILE`, but the contract has three real holes around aliasing handles, the auto-repair watchdog rewriting `hm.method`, and `set_dont_inline` being called on a Method* that the verify_hooks path may already have flagged as freed. The doc-comment also advertises an API (`running()`) the class does not have.

## Bugs

### [high] Auto-repair Method* swap silently orphans a previously valid hook_handle
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:8099-8102, 8488-8498
- **description:** The auto-repair watchdog's `try_reinstall` rewrites `hm_to_repair.method = new_method` on a stale-Method* drift event. `hook_handle` cached the OLD `Method*` at scoped_hook install time. When the user later destroys the handle, `stop()` does a linear `std::find_if` for `h.method == target` (target = the OLD pointer) and finds nothing, then early-returns at line 8497 without clearing flags or erasing the entry. The hook stays armed for the rest of the process lifetime, and `installed()` already returned true to the user so they have no signal that ownership has been lost. This is a silent leak of a live trampoline plus permanent JIT inhibition on the new Method.
- **repro:** Install `scoped_hook<T>("foo", ...)`. Have another DLL trigger JVMTI `RedefineClasses` on T (Lunar / Forge mod managers do this routinely). Wait one watchdog tick — `verify_hooks` swaps `hm.method` to the new Method. Drop the handle. The hook continues firing on the live Method until `shutdown_hooks()`.
- **suggested_fix:** Either (a) store an unforgeable identifier on the handle (class+name+signature triple, same fields the watchdog uses) and have `stop()` match on that, or (b) give the handle a stable `std::shared_ptr<control_block>` token (mirroring `watch_handle`) that `verify_hooks` updates in lockstep with `hm.method`. Option (b) also fixes the aliasing issue in the next bug.
- **confidence:** likely

### [medium] Two scoped_hook calls on the same Method silently alias the same hook entry
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:7766-7772, 8549-8636
- **description:** `vmhook::hook<T>()` early-returns `true` when `found_method` is already in `g_hooked_methods` (line 7770). `scoped_hook<T>()` then re-resolves the same Method* and constructs a new `hook_handle{m}`. Both handles `installed()` report true, both point at the same `Method*`. Destroying handle A removes the global entry and clears NO_COMPILE / dont_inline; handle B is now bogus — its `installed()` still returns true (because `method != nullptr`), but `stop()` will find no matching entry and silently no-op. The first handle to die *also* tears down the detour the second handle's caller installed, so callbacks stop firing while the second handle still looks live. Worse, the second `scoped_hook` call's `user_detour` is silently discarded by the early-return in `hook<T>()` — the caller never learns their callback was thrown away.
- **repro:** `auto h1 = scoped_hook<T>("foo", cbA); auto h2 = scoped_hook<T>("foo", cbB);` — only `cbA` ever fires; dropping `h1` removes the hook from under `h2`.
- **suggested_fix:** Either reject the second install (return empty handle + diagnostic "method already hooked, drop the first handle first") or upgrade `g_hooked_methods` to a multi-detour list so the per-method entry holds N callbacks and `hook_handle` owns one. The first is far simpler; the second matches user expectations from `watch_handle`.
- **confidence:** certain

### [medium] Doc comment advertises `running()` which does not exist on hook_handle
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:6946-6948
- **description:** Class doc says "An empty hook_handle (default-constructed or after stop()) returns running() == false and does nothing on destruction." `hook_handle` only exposes `installed()`. `watch_handle` exposes `running()`. A user who reads the class doc and writes `if (handle.running())` gets a compile error. This is also a parity miss with `watch_handle` — same RAII pattern, different verb for the same query.
- **repro:** Read the class-level comment, attempt `handle.running()`, hit compile error.
- **suggested_fix:** Either rename the doc text to `installed()` (one-line fix; safest) or add a `running()` alias on `hook_handle` that forwards to `installed()` so the two sibling RAII handles share API names. The alias path improves discoverability.
- **confidence:** certain

### [low] stop() dereferences Method* without is_valid_pointer guard
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:8506-8510
- **description:** After `find_if` succeeds, `stop()` immediately calls `set_dont_inline(entry_it->method, false)` and `entry_it->method->get_access_flags()`. If `verify_hooks` has previously logged a mode-1 "Method freed" drift but failed to reinstall (`try_reinstall` returned false at line 8094), `entry_it->method` is a dangling pointer into freed slab memory. `set_dont_inline` reads `*flags` via `get_flags()` which has no validity guard either. Dereferencing freed slab memory at JVM teardown is the same crash class the long comment in `shutdown_hooks()` (lines 8442-8466) goes out of its way to avoid. Note: the existing `shutdown_hooks()` at lines 8434-8440 has the same shape, so this is a pre-existing pattern — but `stop()` is now reachable from user code at arbitrary points in the JVM lifecycle, not just from a controlled teardown sequence.
- **repro:** Class unload of T (another DLL triggers JVMTI), `try_reinstall` fails (class never reloads), one watchdog tick later the user drops the handle.
- **suggested_fix:** Gate the flag mutation behind `if (vmhook::hotspot::is_valid_pointer(entry_it->method)) { ... }` and still `hooks.erase(entry_it)` afterwards so the dead entry is collected from the global list. Keeps the same defensive shape the rest of the file uses for Method* reads.
- **confidence:** likely

### [low] scoped_hook re-resolution duplicates work and returns empty handle on the rare "found_method changed signature" path even though hook is active
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:8562-8620
- **description:** After `vmhook::hook<T>()` returns true, `scoped_hook` walks `methods_array` AGAIN to look up the same `Method*`. The original `hook<T>()` already found the Method and stored its `Method*` in `g_hooked_methods.back()`. Re-walking is wasted work AND it can disagree with the install — if `method_signature.empty()` and there are overloads, `hook<T>()` picks the first match while `scoped_hook`'s re-resolution also picks the first match (currently consistent), but any future divergence in the iteration order will silently mis-target. More directly, the re-resolution path can return `hook_handle{}` with a logged warning at line 8616 even though the hook IS installed — the user is told `installed() == false` but their callback fires forever (well, until `shutdown_hooks()`).
- **repro:** Hard to repro on current code; failure mode would surface if anyone refactored the method-walk in either site.
- **suggested_fix:** Have `vmhook::hook<T>()` write the installed `Method*` into an out-parameter (or return `std::optional<Method*>` via an internal overload), then `scoped_hook` reads it directly with no second walk. Bonus: the "found_method` is already in `g_hooked_methods`" early-return at line 7770 can also publish the existing pointer, so the second-install case has a target too.
- **confidence:** likely

## Improvements

### [S] [USER_FACING] Add `signature()` / `class_name()` / `method_name()` accessors to hook_handle
- **rationale:** Users juggling several hooks have no way to ask a handle "which method are you?". The data is already captured on every `hooked_method` entry (`expected_class_name`, `expected_method_name`, `expected_signature` at line 5719-5721). Exposing it on the handle helps debug logs and Vector<hook_handle> ownership tables ("which hook just died?").
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:6959-7016
- **suggested_change:** Make the handle store the triple directly (cheap, 3 short strings) at construction. The information also doubles as the unforgeable key the high-severity bug needs.

### [S] [USER_FACING] `installed()` only checks for a non-null cached pointer — make it actually consult the global list
- **rationale:** Right now `installed()` is purely a "we have a Method* stashed" check. If the underlying entry has been removed by another path (e.g. `shutdown_hooks()`, another aliasing handle from the medium-severity bug above, a future `vmhook::stop_hook<T>()` API), the handle reports `installed() == true` until destruction. A predicate the user can trust would do `find_if(g_hooked_methods, ...)` under the mutex.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:6996-6999
- **suggested_change:** Replace the body with a locked lookup mirroring `stop()`. Provide a separate `bool has_target() const noexcept` for the old cheap check if anyone in the codebase needs it.

### [S] [USER_FACING] Return type from scoped_hook should let success/failure be inspected without constructing-then-checking
- **rationale:** Today every failure path returns an empty `hook_handle{}` after a `VMHOOK_LOG` line. Users who write `[[nodiscard]] auto h = scoped_hook<T>(...);` get a warning but still have to call `installed()` to know the result. A `[[nodiscard]]` on the function itself would surface the "ignored handle" case loud and clear at compile time.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:8548-8646
- **suggested_change:** Add `[[nodiscard]]` to both `scoped_hook` overloads and to `hook_handle::installed()`. Sibling `vmhook::hook<T>()` returns a plain `bool` that some callers ignore today; an audit-of-an-audit is welcome.

### [XS] [INTERNAL] Make stop() use the same lock-scope pattern as shutdown_hooks for the load-and-clear of `this->method`
- **rationale:** Lines 8478-8479 do `target = this->method; this->method = nullptr;` BEFORE acquiring the lock. If a move-assignment from another thread interleaves between read and clear, two threads can both end up with `target == m` and both run the erase — though `find_if` will only succeed once, the second `set_dont_inline` runs on a freshly removed Method. This isn't a UAF (Methods outlive shutdown anyway) but it is a thread-safety wrinkle the class doc-comment claims is OK.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:8478-8479
- **suggested_change:** Document the "single owning thread per handle" rule explicitly, or move the load-and-clear under the lock and use a single `std::exchange(this->method, nullptr)` for the swap.

### [S] [USER_FACING] Document the auto-repair drift interaction in the class header
- **rationale:** The `hook_handle` thread-safety comment (lines 6950-6957) only mentions concurrent in-flight callbacks. It doesn't mention that the auto-repair watchdog can swap the Method* underneath the handle (the high-severity bug above) until that bug is fixed. Even after the fix, users should know that "this handle owns the install identity, not a particular Method pointer" so they don't try to stash `Method*` separately.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:6938-6958
- **suggested_change:** Add a paragraph: "The hook may track Method* identity changes when JVMTI agents redefine the owning class — the handle still controls the live install regardless of which underlying Method* the JVM currently exposes."

### [XS] [USER_FACING] Mention `try_emplace`/move semantics in the README example
- **rationale:** README example (README.md:514-527) shows a single handle in a single scope. Users who want `std::vector<hook_handle> hooks;` need to know move-only + destructor-removes semantics work as expected (they do). One line in the README — "stash them in a `std::vector` and they tear down on container destruction" — closes a common documentation gap.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:6938-6958 (or README.md:528-538)
- **suggested_change:** Add a sentence about container ownership patterns.

## Tests

### [jvm_integration] [exists] scopedHookInstalled / scopedHookFiredFirstProbe / scopedHookNotFiredAfterRemove
- **description:** Happy-path test already exists — installs, fires, drops the handle, fires again, asserts no further callback.
- **asserts:** `handle.installed()`, callback fired before drop, callback NOT fired after drop.
- **existing_file:** vmhook/src/example.cpp:2728-2772

### [jvm_integration] [new] scoped_hook_double_install_same_method_aliasing
- **description:** Cover the medium-severity aliasing bug. Install scoped_hook on the same method twice with two different callbacks, drop the first handle, fire the probe, and assert that the second handle's callback either still fires (if the fix is "stack the detours") or that the second call refused to install (if the fix is "reject duplicate").
- **asserts:** Either both callbacks fire while both handles are alive AND the second callback survives dropping the first, OR the second `scoped_hook` returns `installed() == false` with a diagnostic.
- **existing_file:** vmhook/src/example.cpp (extend `test_scoped_hook`).

### [jvm_integration] [new] scoped_hook_handle_outlives_method_swap
- **description:** Cover the high-severity auto-repair-orphan bug. Install a scoped_hook on a method, force a Method* swap (the easiest non-mock approach is to wait for the auto-repair watchdog to fire after deoptimisation rather than triggering JVMTI directly), then drop the handle and verify the hook is actually torn down (no further callbacks, NO_COMPILE cleared on the LIVE method).
- **asserts:** After handle destruction, the LIVE Method's `_dont_inline` and `NO_COMPILE` are cleared and the global hooked-methods list no longer contains the entry.
- **existing_file:** vmhook/src/example.cpp (new test next to `test_scoped_hook`).

### [jvm_integration] [new] scoped_hook_move_assignment_drops_old
- **description:** Move-assignment is supposed to `stop()` the LHS before stealing the RHS. Install scoped_hook A on method foo and scoped_hook B on method bar, `A = std::move(B)`, fire foo probe (callback for foo must NOT fire — the old A install must have been torn down) and bar probe (callback must still fire — moved to A).
- **asserts:** old A's callback no longer fires post-assignment; moved B's callback fires through A.
- **existing_file:** vmhook/src/example.cpp (extend `test_scoped_hook` or a new sibling).

### [jvm_integration] [new] scoped_hook_unregistered_wrapper_class
- **description:** Call `scoped_hook<unregistered_type>("foo", cb)` without a prior `register_class`. Currently `hook<T>()` throws an exception which is caught and logs, so `scoped_hook` returns an empty handle on the underlying-failure path — but the re-resolution `type_to_class_map.find` path (lines 8569-8577) handles this case independently and ALSO returns empty. Verify both paths converge on the same observable behaviour.
- **asserts:** Returned handle's `installed() == false`, no entry in `g_hooked_methods`, diagnostic logged once (not twice).
- **existing_file:** vmhook/src/example.cpp (new).

### [jvm_integration] [new] scoped_hook_missing_signature_overload_path
- **description:** Cover the signature-overload `scoped_hook<T>(name, cb)` short overload at line 8640. Confirm an empty signature picks the first matching overload and the longer-form `(name, sig, cb)` overload picks a specific one — and that on a non-matching signature both return empty handles.
- **asserts:** Short overload installs SOMETHING when the class has overloads; long overload selects deterministically by signature; non-matching signature returns `installed() == false`.
- **existing_file:** vmhook/src/example.cpp (extend `test_scoped_hook` to use `caller_probe_class` overloads or add a new dedicated probe).

### [standalone_unit] [new] hook_handle_move_only_traits
- **description:** Static-asserts on the type traits — `hook_handle` must be `is_move_constructible_v && is_move_assignable_v && !is_copy_constructible_v && !is_copy_assignable_v && is_nothrow_destructible_v`. Catches accidental future regressions where someone adds a non-noexcept member.
- **asserts:** `static_assert(std::is_move_constructible_v<vmhook::hook_handle>)` etc.
- **existing_file:** tests/test_traits.cpp (extend).

### [standalone_unit] [new] hook_handle_default_constructed_destructor_no_op
- **description:** Verify destroying a default-constructed `hook_handle{}` does nothing observable — no log spam, no lock acquisition that could deadlock against a held mutex. The check is essentially "no JVM side-effects when no JVM is present", which makes this runnable in the standalone test binary.
- **asserts:** Construction + destruction of `hook_handle{}` outside a JVM does not crash, does not throw, does not log an error.
- **existing_file:** tests/test_api_surface.cpp (extend).

## Parity Concerns
- `watch_handle::running()` vs `hook_handle::installed()` — the two RAII handles use different verbs for the same query. Pick one (preferably one alias both classes share).
- `watch_handle` is reference-counted (`std::shared_ptr<control_block>`), `hook_handle` is unique-ownership (raw `Method*`). The aliasing-bug fix would naturally bring `hook_handle` toward the `watch_handle` shape; consider whether unifying the implementation pattern is worth the churn.
- `vmhook::hook<T>()` returns `bool` and accepts no way to learn the resolved Method*; `vmhook::scoped_hook<T>()` returns a handle and re-walks the class to recover the same pointer. The internal install routine should expose the resolved Method* directly so scoped_hook can skip the second walk and so a future `vmhook::stop_hook<T>(name, sig)` API has a clean entry point.
- `shutdown_hooks()` clears `_dont_inline` and `NO_COMPILE` for every entry but `verify_hooks::try_reinstall` re-applies them later if it runs after `shutdown_hooks()`. The hook_handle::stop() path inherits the same "leave _code untouched" comment block — if the comment's justification ever stops being accurate (a future JDK version that doesn't have the nmethod sweeper hazard), both call sites need updating in lockstep.
