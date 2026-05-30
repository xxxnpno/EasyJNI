# hook_unhook_double_free_safety

## Summary
The ~hook_handle / shutdown_hooks split is sound with respect to literal double-`delete` of the midi2i_hook trampoline — `hook_handle::stop()` deliberately never touches `g_hooked_i2i_entries`, so only `shutdown_hooks()` calls `delete hook` once. Concurrent `stop()` across two distinct handles is safe because both serialize on `g_hooked_methods_mutex`. The real hazards are subtler: (1) two scoped_hooks on the same Java method silently share one underlying entry, so destroying either handle disarms both; (2) a small TOCTOU window in `stop()` where `this->method` is captured before the mutex is taken; and (3) the install path never re-checks `g_shutdown_requested`, so a handle whose user_detour lambda references already-torn-down state can still be inserted post-shutdown.

## Bugs

### [high] Duplicate scoped_hook on the same Method returns two handles that share one entry — destroying either disarms the hook
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:7766-7772, 8472-8533, 8599-8614
- **description:** `vmhook::hook<T>()` early-returns `true` if `found_method` already appears in `g_hooked_methods` (line 7766-7772) and *silently discards the new user_detour*. `scoped_hook<T>()` then walks the klass methods array (line 8599-8614) and produces a `hook_handle{m}` regardless. The result is two `hook_handle` objects in user code, both holding the same `vmhook::hotspot::method*`. When the first handle is destroyed, `~hook_handle -> stop()` takes the mutex, `find_if` succeeds, the entry is erased, and the (shared) hook is uninstalled. When the second handle is destroyed, `find_if` returns `end()` and `stop()` is a no-op — but the user expected their second hook to still be live. From the user's perspective: handle B's callback never fires (because hook() discarded its lambda), AND when handle A drops, handle B's "hook" is also gone. Not a memory-double-free, but a logical double-unhook that breaks the per-handle ownership contract that `class hook_handle`'s docstring (lines 6940-6957) promises.
- **repro:** `auto h1 = vmhook::scoped_hook<C>("m", "(I)V", cb1); auto h2 = vmhook::scoped_hook<C>("m", "(I)V", cb2);` — both `h1.installed() == true` and `h2.installed() == true`, but only `cb1` ever fires; on `h1` going out of scope, `cb1` also stops.
- **suggested_fix:** Either (a) make `scoped_hook` detect the duplicate and return an empty handle plus a clear `VMHOOK_LOG` ("method is already hooked; scoped_hook cannot install a second detour on the same Method* — drop the existing handle first"), or (b) make `hook<T>()` chain user detours so multiple installs on the same Method actually fire all callbacks in install order. Option (a) is the safer near-term fix because it preserves the documented "one handle owns one hook" model.
- **confidence:** certain

### [medium] hook_handle::stop() reads/zeroes this->method BEFORE acquiring the mutex — racey with a concurrent move-assignment of the same handle
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:8472-8487
- **description:** `stop()` does `target = this->method; this->method = nullptr;` and only then takes `g_hooked_methods_mutex` (line 8487). The handle's docstring (lines 6950-6957) says "remove hooks from a single thread, or guard the removal site with a mutex" — but if a user does guard `~h` and `h = std::move(other)` from two threads, two concurrent `stop()` calls on the same `this` can both read the same `target` value (no atomic, no barrier between read and the nulling write), then both try to erase the same entry under the mutex. The second `find_if` returns `end()` and silently no-ops, so this is a NO-double-free, but the silent no-op masks a real concurrency bug in user code. More importantly, neither the docstring nor the implementation makes this guarantee explicit: a reader would assume the early read of `this->method` is atomic with the mutex region, and it is not.
- **repro:** Thread 1: `h.stop();` Thread 2: `h = std::move(other);` (which internally calls `this->stop()`). Both threads may pass the `if (!this->method) return;` check (line 8474) when only one should.
- **suggested_fix:** Move `target = this->method; this->method = nullptr;` *inside* the `std::lock_guard` block, so the read/null-out happens atomically with the erase. Cheap and removes the ambiguity:
  ```cpp
  std::lock_guard<std::mutex> stop_lock{ vmhook::hotspot::g_hooked_methods_mutex };
  vmhook::hotspot::method* const target{ std::exchange(this->method, nullptr) };
  if (!target) { return; }
  ```
- **confidence:** likely

### [medium] vmhook::hook<T>() / scoped_hook can install AFTER shutdown_hooks() has run because no install path checks g_shutdown_requested
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:7758-7976, 8405-8470
- **description:** `shutdown_hooks()` sets `g_shutdown_requested = true` and clears both global vectors. There is nothing in `hook<T>()`'s install path (after acquiring `g_hooked_methods_mutex`) that checks this flag, so a user who calls `vmhook::hook<T>()` after `shutdown_hooks()` re-arms a new entry, re-allocates a midi2i_hook trampoline (because the cleared `g_hooked_i2i_entries` no longer remembers the prior patch), and the returned `hook_handle` is destroyed normally — but the watchdog thread already returned (line 8340-8342), so JIT-state drift is no longer auto-repaired. Worse, the user's destructor for the handle now runs in a process where `g_shutdown_requested == true` and the auto-repair watchdog is dead, and any further `shutdown_hooks()` call still picks the new trampoline up correctly. Not a double-free per se, but lifecycle whiplash that the docs (line 25: "hook installation and shutdown_hooks() MUST be called from a single thread") only partially address.
- **repro:** `vmhook::hook<C>("m", cb); vmhook::shutdown_hooks(); auto h = vmhook::scoped_hook<C>("m", cb);` — second install succeeds, watchdog is dead, handle's destructor runs but no further verify_hooks ever runs.
- **suggested_fix:** At the top of `hook<T>()` (and `scoped_hook<T>()`), after taking the install mutex, check `if (vmhook::hotspot::g_shutdown_requested.load(std::memory_order_acquire)) { VMHOOK_LOG("...installing after shutdown_hooks() is not supported..."); return false; }`. Mirrors the existing guard in `ensure_started()` at line 8363.
- **confidence:** certain

### [low] hook_handle does NOT clear this->method when find_if returns end() — stale pointer survives a no-op stop()
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:8472-8498
- **description:** The first `stop()` call zeroes `this->method` at line 8479 unconditionally. But if `~hook_handle()` runs *after* `shutdown_hooks()` cleared the vectors, `stop()` zeroes `this->method`, takes the mutex, `find_if` returns `end()`, returns. The handle is now safely empty. So far so good. BUT consider the scoped_hook-returns-empty-handle case at line 8636: if Method* re-resolution fails after `vmhook::hook()` already installed the hook, the handle returned has `method == nullptr` — the install is live, but the user has no way to disarm it individually. The handle's `installed()` returns false, so the user assumes the install failed, not that it leaked. (Comment at 8616-8620 acknowledges this. The comment says "The hook IS active, but the caller has no way to disarm it individually" — that is an admitted leak.) This is not a double-free, but it's the inverse: a *missing-free* that only `shutdown_hooks()` can recover.
- **repro:** Stub a klass whose `get_methods_count() <= 0` between `vmhook::hook()` returning true and the re-resolution loop. The hook is installed but the handle is empty.
- **suggested_fix:** If re-resolution fails after install succeeded, call back into the global list and erase the just-inserted entry (best effort) so the user's handle.installed() == false matches reality. Or pass the `found_method*` out of `hook<T>()` directly so `scoped_hook` does not need to re-walk the klass methods array. The latter eliminates the entire failure mode.
- **confidence:** likely

### [low] Trampoline kept alive in g_hooked_i2i_entries even after the last hook on its i2i is removed
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:8500-8519
- **description:** `hook_handle::stop()` deliberately leaves the midi2i_hook trampoline registered in `g_hooked_i2i_entries` (the inline comment at line 8500-8505 says other hooks may share the same i2i entry point). But there is no refcount: if the user removes the *last* hook sharing a given i2i, the trampoline stays patched into the JVM's shared i2i stub and `common_detour` keeps firing on every Java invocation of that stub, walking an empty `g_hooked_methods` lookup table and returning. Not a use-after-free, but a permanent runtime-cost regression for any Java method whose i2i stub our hook patched, even after the user disarms. `shutdown_hooks()` does eventually clean it up, but in long-running mods the user may install/uninstall many times.
- **repro:** Install and uninstall a hook in a loop; observe `g_hooked_i2i_entries.size()` grows monotonically (well, stays at the maximum of unique i2is ever patched) until shutdown.
- **suggested_fix:** Add a refcount to `i2i_hook_data` (`std::size_t hook_count`), bump it on install when reusing a matching i2i, decrement on stop, and `delete entry.hook; erase entry;` when it hits zero. This needs care: deleting the trampoline restores the original 5-byte stub bytes mid-execution, so do it only when no in-flight detour is on the stack — gated by the same `g_hooked_methods_mutex` already held, and only after the `g_hooked_methods` erase succeeded.
- **confidence:** speculative

## Improvements

### [S] [USER_FACING] Add a stop()-then-installed() check pattern doc to hook_handle docstring
- **rationale:** Users today have no way to tell a "stop already ran" from a "hook never installed". `installed()` returns the same false in both cases. A two-line callout in the class docstring (lines 6940-6957) clarifying that `installed() == false` after a successful `stop()` is the expected sentinel would head off confused bug reports.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:6940-6957
- **suggested_change:** Add to the docstring: "After stop() returns, installed() == false. Calling stop() again, destroying the handle, or move-assigning over it is a no-op — the handle is in a steady-state 'empty' position identical to a default-constructed handle."

### [S] [INTERNAL] Move target capture inside the lock in hook_handle::stop()
- **rationale:** Eliminates the TOCTOU window described in Bug 2 above with zero perf cost (we already pay the lock). Makes the code obviously correct rather than subtly correct, which matters because users will read it before subclassing or wrapping it.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:8472-8487
- **suggested_change:** Replace lines 8474-8487 with:
  ```cpp
  try
  {
      std::lock_guard<std::mutex> stop_lock{ vmhook::hotspot::g_hooked_methods_mutex };
      vmhook::hotspot::method* const target{ std::exchange(this->method, nullptr) };
      if (!target) { return; }
      // ... rest of stop() body using `target`
  }
  ```

### [S] [USER_FACING] Detect duplicate scoped_hook on same Method and return an empty handle with an explicit log
- **rationale:** Fixes the silent semantic-double-free described in Bug 1. Users who call scoped_hook twice on the same Method get a `installed() == false` for the second call and a log line telling them why. Cheap to add; spares a category of "my hook stopped firing for no reason" tickets.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:8552-8636
- **suggested_change:** After `vmhook::hook<wrapper_type>(...)` returns true, before re-resolving Method*, check whether `g_hooked_methods` already contained an entry with `expected_method_name == method_name` AND `expected_signature == method_signature`. If so, log "scoped_hook<{}>('{}{}'): a hook is already installed on this method; returning empty handle to keep ownership semantics consistent." and return `hook_handle{}`.

### [M] [INTERNAL] Make scoped_hook get the Method* directly from hook<T>() instead of re-walking the klass
- **rationale:** The re-walk at lines 8599-8614 duplicates ~60 lines of install logic and introduces the empty-handle-but-hook-is-live leak described in Bug 4. If `hook<T>()` returned an `optional<vmhook::hotspot::method*>` (or wrote the Method* through an out-parameter), scoped_hook could construct the handle directly with no second class lookup, no second methods-array scan, and no possibility of "install succeeded, re-resolution failed" leak.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:7689-7977, 8548-8645
- **suggested_change:** Add a non-public overload `hook<T>(name, sig, detour, vmhook::hotspot::method*& out_method)` that fills `out_method` on success. Public `hook<T>` calls it discarding the out. `scoped_hook` calls the new overload and constructs `hook_handle{ out_method }` directly. Drops the re-walk entirely.

### [S] [USER_FACING] Reject install attempts after shutdown_hooks() with a clear log
- **rationale:** Today `vmhook::hook<T>()` happily installs after `shutdown_hooks()` even though the watchdog has exited and the user's mental model is "everything is torn down". A check at the top of the install routine (mirroring ensure_started's pattern at line 8363) gives a clear error path and prevents zombie hooks from leaking past shutdown.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:7758-7765
- **suggested_change:** Immediately after taking `install_lock`, add:
  ```cpp
  if (vmhook::hotspot::g_shutdown_requested.load(std::memory_order_acquire))
  {
      VMHOOK_LOG("{} hook(): refusing to install '{}' because shutdown_hooks() has already run; "
                 "re-installs after shutdown are not supported.", vmhook::error_tag, method_name);
      return false;
  }
  ```

### [XS] [INTERNAL] Document the hook_handle move-assignment lock ordering
- **rationale:** `hook_handle::operator=(hook_handle&&)` calls `this->stop()` which takes `g_hooked_methods_mutex`. If a caller wraps move-assignment in their own lock, lock-order analysis matters. One line in the operator's body comment ("acquires g_hooked_methods_mutex via stop() — do not call while holding it") prevents the obvious deadlock.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:6977-6986
- **suggested_change:** Add a 2-line comment above the move-assignment operator noting that it takes `g_hooked_methods_mutex` transitively.

## Tests

### [standalone_unit] [new] test_hook_handle_double_stop_is_noop
- **description:** Construct a `vmhook::hook_handle` with a stub `vmhook::hotspot::method*` (bypassing the install path by friending the test or via a test-only constructor), insert a matching entry into `vmhook::hotspot::g_hooked_methods`, then call `handle.stop()` twice in a row. The first call should erase the entry and clear `handle.method`; the second should observe `handle.method == nullptr` at the top of stop() and return immediately without touching the vector.
- **asserts:** After first stop(): `g_hooked_methods.empty() == true`, `handle.installed() == false`. After second stop(): no crash, vector still empty, no exception escapes.

### [standalone_unit] [new] test_hook_handle_move_assignment_calls_stop
- **description:** Build two `hook_handle`s with distinct stub Method*s, both inserted in `g_hooked_methods`. Move-assign `h1 = std::move(h2)`. Verify that h1's prior entry was erased (proving `stop()` ran), h1 now owns h2's Method*, and h2 is empty.
- **asserts:** `g_hooked_methods.size() == 1`, the surviving entry's method matches h2's original method, `h1.installed() == true`, `h2.installed() == false`.

### [standalone_unit] [new] test_concurrent_stop_two_distinct_handles
- **description:** Build two `hook_handle`s for two different stub Method*s, both inserted in `g_hooked_methods`. Launch two `std::thread`s, each calling `stop()` on its handle, with a `std::barrier` to maximise overlap. After join, both entries should be gone with no exceptions or crashes.
- **asserts:** `g_hooked_methods.empty() == true`, both handles have `installed() == false`.

### [standalone_unit] [new] test_shutdown_hooks_idempotent
- **description:** Insert several stub entries into `g_hooked_methods` and `g_hooked_i2i_entries` (with hook=nullptr to avoid the delete touching real allocations — or with a heap-allocated dummy that records destruction in a flag). Call `shutdown_hooks()` twice in a row.
- **asserts:** First call empties both vectors and (if dummy hook was non-null) `delete` was called once. Second call observes empty vectors, no `delete` is called a second time, `g_shutdown_requested == true`, no exceptions.

### [standalone_unit] [new] test_handle_destruction_after_shutdown_is_safe
- **description:** Build a `hook_handle` referring to a stub Method* that IS in `g_hooked_methods`. Call `shutdown_hooks()` (which clears the vector but does not touch the handle's `this->method`). Let the handle go out of scope.
- **asserts:** No crash, no double-erase attempt, no exception escapes the destructor. The handle's stop() observes the entry is missing and returns silently.

### [jvm_integration] [extend_existing] test_scoped_hook_duplicate_on_same_method
- **description:** Extend the existing `test_scoped_hook` in example.cpp. Install two `vmhook::scoped_hook<caller_probe_class>("innerStep", "(I)I", cb)` calls in the same scope (cb1 increments counter A, cb2 increments counter B). Run the probe once.
- **asserts:** Document and verify which callback fires. Today: only cb1 fires (because hook<T>() at line 7770 early-returns true and silently discards cb2). This test pins the behaviour so regressions are caught, and once Bug 1's fix is applied, the test should be updated to assert h2.installed() == false plus a log line.
- **existing_file:** vmhook/src/example.cpp (test_scoped_hook around line 2731)

### [jvm_integration] [new] test_install_after_shutdown_hooks_rejected
- **description:** Install a hook, call `shutdown_hooks()`, then attempt `vmhook::hook<caller_probe_class>(...)` or `vmhook::scoped_hook<...>(...)`.
- **asserts:** Today the install returns true and the watchdog has exited; once Improvement 5 is applied, the install should return false (and scoped_hook returns an empty handle) with a clear log line. The test pins the new contract.

### [standalone_unit] [new] test_hook_handle_empty_destructor_noop
- **description:** Default-construct a `hook_handle` (so `method == nullptr`) and let it destruct without ever taking any other action.
- **asserts:** No mutex acquisition, no `find_if`, no log line. Verifies the early `if (!this->method) return;` in stop() at line 8474 is unrelied-upon by tests today.

## Parity Concerns
- `vmhook::scoped_hook` and `vmhook::hook` share the early-duplicate-return at lines 7766-7772 — scoped_hook inherits the silent-detour-discard from hook(). Any fix here must update both API entry points consistently.
- `class scoped_watcher` (referenced in the field_watch facility around line 6892, which has an analogous "idempotent stop()" docstring) likely has the same lock-ordering subtlety as `hook_handle::stop()` — worth checking with the same lens during the field_watch audit.
- The auto-repair watchdog (lines 8315-8403) and `verify_hooks` (lines 8036-8291) walk `g_hooked_methods` lock-free in some paths; any change to `hook_handle::stop()` lock semantics needs to stay compatible with `try_reinstall`'s pointer-swap at line 8102, which assumes pointer assignment is atomic on x86_64.
