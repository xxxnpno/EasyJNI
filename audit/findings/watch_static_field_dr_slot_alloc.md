# watch_static_field_dr_slot_alloc

## Summary
DR0-DR3 slot allocation is correctly serialised under `dr_mutex` (`find_free_slot` + slot population + `in_use.store(true)` all happen under the same lock), so the "two threads competing for the last slot" race is well-behaved — the loser cleanly receives the "all 4 slots in use" error. However the surrounding install/release machinery contains a slot-recycle race (an in-flight VEH trap can be dispatched to the *next* watch installed on the same slot), silently-broken DR writes on running threads (no `SuspendThread` around `SetThreadContext`), a silent default `LEN=8` for any field size other than 1/2/4 bytes, and uninformative slot-exhaustion / partial-failure diagnostics.

## Bugs

### [high] In-flight trap dispatched to the wrong (newly-installed) watch after slot reuse
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:14850-14906, 15104-15116
- **description:** `dr_exception_handler` decodes the slot from `Dr6`, then *blocks on `dr_mutex`* before reading the callback. If between the trap firing and the handler acquiring the lock another thread calls `stop()` on the original handle (releases slot N, decrements `in_use` to false) and a third thread calls `watch_static_field` (re-acquires slot N for a *different* field with a *different* callback), the handler will observe `in_use == true`, copy the new callback, and invoke it with the new address. The new user's callback is fired spuriously from a trap that targeted the old watch. There is no generation counter or address comparison to detect the recycle.
- **repro:** Install watch A on slot 0, force a write to A's address from thread T1 (trap pending dispatch on T1). On thread T2 drop A's handle and immediately install watch B (also lands on slot 0). When T1's VEH finally acquires `dr_mutex`, it reads B's `callback` and `address` and calls B with A's trap. Empirically reproducible with a short busy-loop between the write and the destructor.
- **suggested_fix:** Add a `std::uint64_t generation` field to `dr_slot`, bump it on every release (or every install), and stamp the generation into something the handler can recover. Simplest: have `watch_static_field` push the generation into a side table and embed it into the trap context by having the VEH compare the captured `address` against `eptrs->ContextRecord->Dr{0..3}` for the matching slot — if the slot's stored address no longer matches the DR register's address, drop the trap. Alternative: keep an `epoch` per slot and have the lambda check `slot[i].epoch == captured_epoch` before invoking the user callback.
- **confidence:** likely

### [high] SetThreadContext / GetThreadContext called without SuspendThread — DR writes silently dropped on running threads
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:14797-14823, 14825-14848, 1053-1084
- **description:** `refresh_thread_drs` and `clear_thread_drs` iterate threads via `for_each_thread`, call `GetThreadContext`/`SetThreadContext` directly, and never `SuspendThread`/`ResumeThread`. Per the Windows API contract `SetThreadContext` is only well-defined on a suspended thread; on a running thread the kernel may merge or discard the context update, leaving DR0-DR3/DR7 unchanged or partially applied. The `THREAD_SUSPEND_RESUME` access right is even requested in `OpenThread` (line 1072) but never used. Result: a `watch_static_field` install can return a "running" `watch_handle` while the trap is in fact *not armed* on most of the JVM's threads — a silent failure with no diagnostic.
- **repro:** Install a watch on an int field hammered by a busy JIT-compiled Java thread; observe that no callbacks fire even though the field is written. Confirm by manually re-reading `Dr0` on that thread after install — it is often 0.
- **suggested_fix:** Surround the `GetThreadContext`/`SetThreadContext` pair with `::SuspendThread(thread)` / `::ResumeThread(thread)`, and skip the current thread (`::GetCurrentThreadId() == te.th32ThreadID`) since suspending self deadlocks. Log when either call fails so silent partial-arm is no longer silent.
- **confidence:** certain

### [medium] Silent default to 8-byte DR length when sizeof(field_type) is not in {1,2,4,8}
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:15064-15068
- **description:** The ternary chain ends with an unconditional fallthrough to `eight_bytes`. Any field type whose size is 3, 5, 6, 7, 16, etc. (a packed struct, a `__int128`, an oop wrapper compiled to a different size on a non-default `-XX:+UseCompressedOops` build, etc.) silently configures DR7 with `LEN=10` covering 8 bytes — possibly misaligned and possibly straddling the next field. The trap will appear to work but report writes to neighbouring memory or skip writes that don't touch the lower 8 bytes.
- **repro:** Instantiate `watch_static_field<C, std::array<std::uint8_t, 5>>`; the watch arms with LEN=8 and traps on every write to the adjacent field. No compile-time error, no log message.
- **suggested_fix:** Replace the ternary with `static_assert(sizeof(field_type) == 1 || sizeof(field_type) == 2 || sizeof(field_type) == 4 || sizeof(field_type) == 8, "watch_static_field: field size must be 1, 2, 4, or 8 bytes (hardware DR LEN limitation).");`. Also assert at runtime that `address % sizeof(field_type) == 0` since the CPU silently mis-traps on unaligned watch ranges.
- **confidence:** certain

### [medium] `refresh_thread_drs` partial-failure goes unreported — install can succeed while no threads are actually armed
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:14797-14823, 15101
- **description:** `for_each_thread` silently skips threads that `OpenThread` could not open (insufficient privilege, protected-process threads, etc.) and `GetThreadContext` failures are swallowed inside the lambda. `refresh_thread_drs` returns `void` with no count. `watch_static_field` therefore returns a `running()`-true handle even if zero threads were successfully armed — making it impossible for the user to tell whether the watch actually works.
- **repro:** Run in a sandboxed/PPL process where most JVM threads are inaccessible. `watch_static_field` returns a handle with `running() == true`, but no callback ever fires.
- **suggested_fix:** Have `refresh_thread_drs` return `{success_count, failure_count}` (or just a count) and log at warning level when *any* thread failed to arm. Either return an empty handle when zero threads were armed, or expose the partial-arm state on `watch_handle` so users can `assert(handle.armed_thread_count() > 0)`.
- **confidence:** likely

### [low] No protection against double-installing on the same field — wastes a DR slot
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:14785-14795, 15050-15057
- **description:** `find_free_slot` returns the first slot with `in_use == false`; it does not check whether some other slot is already watching the same `address`. A user that accidentally calls `watch_static_field<C, int>("x", cb)` twice consumes two of the four DR slots for the same field — and both watchers fire on every write, doubling callback dispatch cost.
- **repro:** Call `watch_static_field` twice with the same wrapper + name; both succeed silently, and `running()` returns true on both handles.
- **suggested_fix:** Before returning the free slot, scan `dr_slots[i].address == address` for `i < 4`. If found, either share the slot (push the new callback into a vector hung off the slot) or log a clear warning and refuse the second install. Sharing is the user-friendlier option since DR slots are scarce.
- **confidence:** likely

## Improvements

### [S] [USER_FACING] Slot-exhaustion error should list the four addresses that are currently held
- **rationale:** "all 4 hardware breakpoint slots in use" gives the user nothing to act on. Listing the four addresses (or, if a registry of `(field_name -> slot)` were kept, the four field names) lets the user identify which prior watch they forgot to release.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:15052-15056
- **suggested_change:** Hang an `std::string` (or `std::string_view`) `debug_label` field on `dr_slot`, populated from `field_name` at install time. On exhaustion, log: `"all 4 hardware breakpoint slots in use (slot0=<label>, slot1=<label>, slot2=<label>, slot3=<label>) — drop one of these watch_handles before installing another"`.

### [XS] [USER_FACING] Expose `watch_static_field_slots_free()` / `watch_static_field_capacity()` helpers
- **rationale:** Callers that opportunistically install many watches need a way to ask "do I have a slot?" without spamming attempts and inferring exhaustion from a returned empty handle. A two-line constexpr / inline helper avoids the boilerplate.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:14785-14795, 14963-15028
- **suggested_change:** Add `inline auto watch_static_field_slots_free() noexcept -> int { std::lock_guard g{detail::dr_mutex}; int free{0}; for (auto& s : detail::dr_slots) if (!s.in_use.load(std::memory_order_relaxed)) ++free; return free; }` and the matching `watch_static_field_capacity()` returning 4 (or 0 when `!VMHOOK_HAS_HW_DATA_BREAKPOINTS`).

### [XS] [USER_FACING] Document `length` mapping in the function comment
- **rationale:** Users reading the public docs at line 14963-15027 do not learn that field sizes other than 1/2/4/8 will produce wrong behaviour (see bug above). Even after fixing with a static_assert, the requirement should be stated explicitly so users size their probe types correctly.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:14963-15027
- **suggested_change:** Add a bullet to the `Limits:` block: "The DR LEN field can only encode {1, 2, 4, 8} byte windows; `field_type` must have a matching `sizeof`. A `static_assert` fires at compile time if not. The address must also be naturally aligned (LEN-byte alignment) — the trap silently mis-fires on unaligned watches."

### [S] [INTERNAL] Skip the current thread in `for_each_thread`-driven DR refreshes
- **rationale:** `SuspendThread(self)` deadlocks. Once the SuspendThread fix above lands, the iteration must skip the calling thread (or be reworked to set the current thread's DRs without suspending it, e.g. via direct `Wr*` intrinsics). Today the current thread's DRs are also not configured because the iteration runs sequentially from the install path — meaning a watch installed *from* a target thread never traps on that same thread until it next context-switches.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:1053-1084, 14797-14823
- **suggested_change:** In `for_each_thread`, fetch `const DWORD self_tid{ ::GetCurrentThreadId() }` and `continue` when `te.th32ThreadID == self_tid`. Add a one-line path that writes the current thread's DRs via inline asm / intrinsics. Document the new behaviour on `watch_static_field`.

### [S] [INTERNAL] Use a constexpr `kSlotCount = 4` instead of literal 4
- **rationale:** `find_free_slot`, the `dr_slots[4]` array, the `< 4` loop bound, the doc strings, and the exhaustion message all hard-code 4. A single constant centralises the assumption and makes any future port to a different DR count (e.g. ARM's 4-16 hardware watchpoints) much easier to audit.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:14765-14795
- **suggested_change:** `static constexpr int kDrSlotCount = 4;` inside `detail`. Replace `dr_slots[4]` with `dr_slots[kDrSlotCount]` and the loop bound similarly.

### [M] [USER_FACING] Hook thread-creation so threads spawned *after* install also get armed
- **rationale:** The doc explicitly calls this out as a known limitation ("Threads that exist when the watch is installed get the trap armed; threads created later do NOT"). For long-running JVMs that spin up GC/JIT/Reference-Handler threads on demand, the watch effectively becomes useless after the first GC cycle.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:14980-14982, 14797-14823
- **suggested_change:** Install a tiny `NtCreateThreadEx` / `CreateThread` hook (via the existing hooking machinery) that, after each new thread is constructed, re-applies the active DR slot configuration to that thread. Gate it behind a single `dr_install_thread_create_hook()` call from the first `dr_arm_one()` 0->1 transition and tear it down on the 1->0 transition.

### [S] [INTERNAL] Drop the dead `ensure_dr_handler_installed` legacy alias
- **rationale:** The comment at lines 14948-14959 says the alias is kept "so external consumers that might have built on the old name don't break at upgrade time", but the symbol lives inside `namespace detail` (i.e. is implementation-only and not part of the documented public API). External consumers cannot legitimately have been calling it. The alias is dead code that adds a non-obvious unbalanced-refcount footgun if a downstream developer reads "ensure" as "install once".
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:14948-14959
- **suggested_change:** Delete `ensure_dr_handler_installed`. If you want belt-and-braces, leave a `[[deprecated]]` `inline auto ensure_dr_handler_installed() = delete;` for one release.

## Tests

### [standalone_unit] [new] test_dr_slot_find_free_returns_first_free
- **description:** Drive `detail::find_free_slot()` directly with a fixture that pre-marks slots `[true, false, true, true]` and asserts the returned index is 1; then `[true, true, true, true]` returns -1; then `[false, true, false, true]` returns 0.
- **asserts:** Returned slot index matches the lowest-indexed free slot; -1 returned only when all four are taken.
- **existing_file:** tests/test_os_layer.cpp (extend; the file already covers DR helpers)

### [standalone_unit] [new] test_dr_slot_exhaustion_returns_empty_handle
- **description:** Mock `dr_slots[]` to all in_use=true and call `watch_static_field<W, int>("f", cb)`. The returned handle must satisfy `running() == false` and a single VMHOOK_LOG entry must contain "all 4 hardware breakpoint slots in use".
- **asserts:** `handle.running() == false`; log captures the exhaustion message exactly once.
- **existing_file:** new file tests/test_watch_static_field.cpp

### [standalone_unit] [new] test_dr_slot_release_makes_slot_available_again
- **description:** Pre-fill three slots, install a fourth, observe `find_free_slot` returns -1 for any further install, drop the fourth handle, observe `find_free_slot` returns its index.
- **asserts:** Slot index returned after release matches the slot used at install; `dr_armed_count` returns to 0 only after the last release.
- **existing_file:** new file tests/test_watch_static_field.cpp

### [standalone_unit] [new] test_dr_slot_alloc_two_threads_race_for_last_slot
- **description:** Pre-fill three slots, then race two threads each calling `watch_static_field`. Exactly one must succeed (non-empty handle); the other must get an empty handle. Repeat in a 1000-iteration loop to flush schedules.
- **asserts:** Across N iterations, success count == N and failure count == N (one win, one loss per race); no slot is ever doubly-installed; no crash / data race detected by TSAN where supported.
- **existing_file:** new file tests/test_watch_static_field.cpp

### [standalone_unit] [new] test_dr_slot_recycle_does_not_dispatch_old_trap_to_new_callback
- **description:** Install watch A on slot 0 with callback that signals event_a. Block A's callback on a barrier so it's "in flight". Drop A and install watch B on slot 0 (different address, different callback). Release the barrier — verify B's callback is *not* called with A's trap.
- **asserts:** event_b counter stays at 0 across the recycled-trap window; event_a counter reflects only legitimate triggers.
- **existing_file:** new file tests/test_watch_static_field.cpp (this is the regression test for the slot-recycle bug above)

### [standalone_unit] [new] test_dr_slot_field_size_static_assert_fires
- **description:** Compile-time check that `watch_static_field<W, std::array<std::uint8_t, 5>>` fails to compile after the static_assert lands. Use the test_traits.cpp pattern of `static_assert(!std::is_invocable_v<...>)` or an explicit SFINAE probe.
- **asserts:** Static assert message contains "field size must be 1, 2, 4, or 8 bytes".
- **existing_file:** tests/test_traits.cpp (extend) or new test_watch_static_field_static_asserts.cpp

### [standalone_unit] [new] test_dr_slot_double_install_same_address_is_diagnosed
- **description:** Call `watch_static_field<W, int>("x", cb1)` then again `watch_static_field<W, int>("x", cb2)`. After the fix, this should either share the slot (both callbacks fire) or log a warning and refuse the second install.
- **asserts:** Two slots are not consumed by the same address; at most one slot's `address` equals the field's address.
- **existing_file:** new file tests/test_watch_static_field.cpp

### [jvm_integration] [new] test_dr_slot_arm_survives_new_thread_creation
- **description:** Install a watch on a static `int`. Spawn a new Java thread that writes the field in a loop. Assert the callback fires at least N times (>= write count). Currently expected to fail until the post-install thread-tracking improvement lands.
- **asserts:** Callback fire count from threads created *after* install > 0.
- **existing_file:** new tests/integration/test_watch_static_field_new_thread.cpp

### [jvm_integration] [new] test_dr_slot_arm_uses_suspend_thread
- **description:** End-to-end check: install a watch on a busy field hammered by an already-running Java thread. Assert callback fires within 10ms. Today this is racy because `SetThreadContext` on a running thread is non-deterministic; passes deterministically once the `SuspendThread` fix lands.
- **asserts:** Callback fired within the deadline on a running JIT thread.
- **existing_file:** new tests/integration/test_watch_static_field_running_thread.cpp

### [standalone_unit] [exists] test_dr7_build_mask_per_slot
- **description:** Already covered indirectly via `os::detail_dr::build_dr7` in test_os_layer.cpp (the file already references the DR helpers per the grep). Confirm slot 0..3 each set the right local-enable / RW / LEN bits — extend with a parameterised case covering all four slots × write/read_write × all four lengths to lock the bit layout against accidental drift.
- **asserts:** For each (slot, kind, length) tuple, the returned mask has exactly the expected bits set and no others.
- **existing_file:** tests/test_os_layer.cpp

## Parity Concerns
- `watch_static_field` is the *only* event-driven watcher that has a hard capacity limit (4). Sibling `on_class_loaded` / `on_exception` use an unbounded `std::vector` of callbacks. Users discovering vmhook through `on_class_loaded` will be surprised that `watch_static_field` cannot watch a fifth field; documenting the cap on the function summary (already done) is good, but consider mentioning the workaround (poll the value from inside `on_class_loaded` for low-priority watches) directly in the slot-exhaustion log line.
- The non-Windows / non-x86_64 `#else` branch returns an empty handle and logs an error — but there is no polling fallback in `field_proxy::get()`-loop form that ships with the library. Sibling watchers don't have this gap. Either offer a `watch_static_field_polling(name, interval, cb)` for non-Windows, or surface the unsupported state via a `static constexpr bool watch_static_field_supported()` so callers can branch at compile time without needing to know the `VMHOOK_HAS_HW_DATA_BREAKPOINTS` macro.
- `watch_handle::on_stop` is invoked from `~watch_handle` *or* explicit `stop()`. The DR `on_stop` lambda captures only `slot` (an int) — fine — but never re-validates that the slot still belongs to *this* install. If a user moves a `watch_handle` into a vector that is destroyed in the wrong order (e.g. a vector of two handles where slot 0 was installed first but the second-installed handle was already moved-from), this is currently safe because `in_use.store(false)` is unconditional. Capture an `epoch` along with `slot` and compare on stop — same fix as the trap-recycle race.
- The `old_value` argument the doc promises is always zero (`field_type{}`) because the lambda's `on_change(field_type{}, current)` cannot recover the pre-write value. Sibling watchers either give the user useful args or omit them entirely. Consider changing the signature to `void(field_type new_value)` (one arg) to stop misleading users — or shadow-copy the field on each install and update it inside the trap to give a genuine old/new pair.
