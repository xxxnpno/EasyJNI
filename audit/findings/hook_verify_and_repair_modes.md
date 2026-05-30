# hook_verify_and_repair_modes

## Summary
`verify_hooks()` covers three drift modes (freed Method, aliased Method, JIT re-population) and is driven both manually and by an auto-repair watchdog thread spawned on first `hook<T>()`. Implementation is solid overall, but a handful of bugs make repair silently get stuck (`drift_logged` permanently true on `try_reinstall` failure, mode-3 not consulted after mode-1/2, `_from_interpreted_entry` not reset in mode 3 / try_reinstall, no per-i2i byte-for-byte check against a known-good snapshot, watchdog cannot restart after `shutdown_hooks()`).

## Bugs

### [HIGH] try_reinstall failure leaves drift_logged stuck and mutes future passes
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:8170-8217
- **description:** In mode 1 (freed Method) and mode 2 (aliased Method), `hm.drift_logged = true` is set BEFORE calling `try_reinstall(hm)`. `try_reinstall` only clears `drift_logged = false` on the success path (line 8130). If `find_class` fails because the JVM is still mid-redefinition, or the rescan of `methods_array` does not yet see the new Method, `try_reinstall` returns false and `drift_logged` stays `true` forever. The outer loop's gate at line 8172 (`if (!hm.method || hm.drift_logged) continue;`) then permanently skips this `hooked_method`, so the next watchdog cycle can never retry the repair even after the JVM finishes redefining. The hook is permanently dead with no recovery path.
- **repro:** Trigger a class redefinition while the watchdog wakes during the redefinition's tiny window where the old Method is freed but the new method array has not yet been published. `try_reinstall` returns false, then the JVM finishes the redefinition the next millisecond, then the watchdog wakes again — but the entry is skipped forever because `drift_logged` was never reset.
- **suggested_fix:** Reset `drift_logged` on the failure path of `try_reinstall` so repair will be retried next pass, while still avoiding a log spam by gating only the `VMHOOK_LOG` call on a separate "already logged" boolean. Either split the debounce state (`logged`) from the "dead, do not retry" state, or simply `hm.drift_logged = false;` on the failure branch and accept one log line per pass.
- **confidence:** certain

### [HIGH] Mode-3 JIT drift never re-evaluated once mode-1/2 fired earlier
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:8170-8284
- **description:** The single `drift_logged` flag is reused to debounce three independent failure modes. Once mode 1 or mode 2 has fired (even if `try_reinstall` succeeded but a subsequent JIT re-population happens), the loop's top-of-iteration gate `if (!hm.method || hm.drift_logged) continue;` skips the method without ever reaching the mode-3 detector at line 8239. Mode 3 itself sets `drift_logged = true` once and only resets when steady-state is reached on a future pass (line 8282) — but the early `continue` prevents the loop ever reaching the steady-state branch. The "drift_logged" semantic is "any drift has been logged once", but the gate uses it as "skip this method".
- **repro:** Hook a method, force-trigger a mode-2 alias via JVMTI RedefineClasses. `try_reinstall` succeeds and resets `drift_logged = false`. Then later HotSpot re-JITs the new method (mode 3). The next watchdog pass logs mode 3 and sets `drift_logged = true`. From then on, the watchdog tick gate at 8172 skips the entry; mode 3 never repairs `_code = nullptr` again even if HotSpot re-JITs again, and the steady-state reset branch is unreachable.
- **suggested_fix:** Remove `drift_logged` from the gate at line 8172 — it should be a per-message debounce, not a "skip whole method" flag. Gate only the `VMHOOK_LOG(...)` line and let every pass re-evaluate all three modes. Alternatively introduce one debounce flag per mode (`freed_logged`, `aliased_logged`, `jit_drift_logged`) or replace the gate with a "logged this exact state" hash.
- **confidence:** certain

### [MEDIUM] Mode 3 and try_reinstall do not restore _from_interpreted_entry
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:8104-8126, 8259-8274
- **description:** The install path explicitly rewrites `_from_interpreted_entry = i2i` for compiled methods (lines 7941, 7963). The mode-3 repair path and `try_reinstall` clear `_code` and rewrite `_from_compiled_entry`, but they never call `set_from_interpreted_entry(i2i)`. If during JIT drift HotSpot wrote a non-i2i value into `_from_interpreted_entry` (which it does whenever it installs an nmethod: it points the interpreted entry at the i2c adapter), interpreted callers that resolve through `_from_interpreted_entry` bypass the patched i2i stub entirely. The install-path symmetry is broken — the repair leaves the method in a state the install path would not produce.
- **repro:** Hook a method that is currently interpreted. Allow HotSpot to JIT-compile it (verify_hooks deopts it). HotSpot may re-JIT again later (mode 3). After the second JIT, `_from_interpreted_entry` is again pointing at the i2c adapter even though `_code = nullptr` has been cleared. Interpreted call sites cache `_from_interpreted_entry` and route through the adapter, skipping the i2i patch.
- **suggested_fix:** After clearing `_code` in mode 3 and `try_reinstall`, also call `hm.method->set_from_interpreted_entry(hm.method->get_i2i_entry())` (with null-check on the i2i lookup). Match the install path's three-step deopt exactly.
- **confidence:** likely

### [MEDIUM] verify_and_repair chain-detection accepts cycle back into vmhook's other trampolines
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:5593-5616
- **description:** When `verify_and_repair` finds an alien JMP at the target, it follows the rel32 and only rejects the chain target if it equals `this->allocated`. It does NOT reject a chain target that points at one of vmhook's OTHER `g_hooked_i2i_entries[i].hook->allocated` trampolines. If two i2i stubs share an injection point (rare but possible with stub deduplication across JDK versions) and both have been re-patched by another hooker, the chain could loop back through vmhook trampolines and infinitely re-enter `common_detour`. There is no global "is this address one of OUR trampolines" check.
- **repro:** Difficult in production but reasoned from code: any chain target that happens to be the start of another midi2i_hook trampoline owned by us creates a cycle when both verify-and-repair runs see the other's JMP and chain to it.
- **suggested_fix:** Before adopting `prior_trampoline` as `new_chain`, walk `vmhook::hotspot::g_hooked_i2i_entries` and reject any pointer whose `allocated`..`allocated+allocated_size` range contains `prior_trampoline`. Hold `g_hooked_methods_mutex` while doing this (verify_hooks already does).
- **confidence:** speculative

### [MEDIUM] No byte-for-byte trampoline integrity check against install-time snapshot
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:5571-5638
- **description:** The audit task explicitly asks "Trampoline integrity check, byte-for-byte vs known-good?" — `verify_and_repair` only checks the **5-byte JMP at the injection point**, never the trampoline body itself (`this->allocated`..`this->allocated + allocated_size`). If a third-party tool (anti-cheat, another hooker, page-level write that landed in the wrong place) corrupts the trampoline's `cmp byte ptr [rsp], 0` / `je` / cancel-path / resume-path bytes or, more critically, the **DETOUR_ADDRESS_OFFSET slot** that holds `common_detour`, every subsequent call jumps to garbage. Mode 1/2/3 detect Method-level drift; mode-3 fixes JIT drift; but nobody checks the assembly bytes vmhook itself wrote.
- **repro:** Manually clobber a byte in `this->allocated + DETOUR_ADDRESS_OFFSET`. `verify_and_repair` reports the hook as intact because the 5-byte JMP still passes memcmp. The next Java call dispatches through `call [rip+0x51]` into junk and AVs.
- **suggested_fix:** At install time after the final memcpy of the assembly array, take a `std::vector<std::uint8_t>` snapshot of `this->allocated[HOOK_SIZE..HOOK_SIZE+sizeof(assembly)]`. `verify_and_repair` memcmps the live trampoline body against the snapshot (skipping the 4-byte `resume_jmp_delta` slot, which the chain-resume rewriter legitimately mutates). On mismatch, restore from the snapshot under `os::protect` execute_rw, then re-apply the current `resume_jmp_delta`. Log a CRITICAL-severity warning naming the byte offset that drifted.
- **confidence:** certain

### [MEDIUM] Auto-repair watchdog cannot be restarted after shutdown_hooks()
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:8355-8412, 5775
- **description:** `shutdown_hooks()` sets `g_shutdown_requested = true` and never resets it. `ensure_started()` uses `g_started.compare_exchange_strong` so a second call is a no-op even though `g_started` was never reset. If a user calls `shutdown_hooks()` and then later installs a new hook via `vmhook::hook<T>()`, the install path's call to `ensure_started()` is a no-op, AND if it weren't, the early-return check on `g_shutdown_requested` at line 8363 would still abort spawn. Even worse, `common_detour` itself early-returns on `g_shutdown_requested` (line 5853) so the freshly-installed hook silently never fires. The implicit contract is "shutdown_hooks() is a one-way door for the process lifetime", but this contract is undocumented at the `shutdown_hooks()` and `hook<T>()` API surface — users will reasonably expect symmetry.
- **repro:** `hook<MyClass>("foo", cb);` → `shutdown_hooks();` → `hook<MyClass>("foo", cb);`. The second hook returns `true` (install succeeded), but the detour will never fire because `common_detour` returns early on `g_shutdown_requested`.
- **suggested_fix:** Either (a) reset `g_shutdown_requested = false` and `g_started = false` at the end of `shutdown_hooks()` so the cycle is restartable, OR (b) document the one-way-door semantic in the docstrings of `shutdown_hooks()` and `hook<T>()` and make `hook<T>()` return `false` with a clear log message when `g_shutdown_requested` is observed. Option (a) is the user-friendly choice and matches the implicit expectation of an injection scenario where the process keeps running after uninject + reinject.
- **confidence:** certain

### [LOW] hook<T>() rollback gap: NO_COMPILE / _dont_inline / g_hooked_methods entry stay set on midi2i_hook failure
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:7774-7912
- **description:** The install path sets `set_dont_inline(true)` (7774), `*flags |= NO_COMPILE` (7781), and `g_hooked_methods.push_back(...)` (7840) BEFORE constructing the `midi2i_hook` trampoline at 7903. If `midi2i_hook` returns error (line 7905) and the throw at 7908 is caught by the function-level `catch (const std::exception&)` at 7978, neither `set_dont_inline(false)` nor the `NO_COMPILE` clear runs, and `g_hooked_methods` is left with a stale entry pointing at a method with no trampoline. `verify_hooks` will then see a hooked_method that has nowhere to dispatch through. Mode 3's NO_COMPILE re-arm will succeed on every pass since NO_COMPILE was never cleared.
- **repro:** Force `allocate_nearby_memory(target, total_size)` to fail (e.g. through address-space exhaustion in the +/-2GB range). `hook<T>()` returns false but `g_hooked_methods` retains the orphan entry; `verify_hooks` reports drift but `try_reinstall` re-creates the same conditions (still no trampoline) and the orphan persists.
- **suggested_fix:** Catch the `midi2i_hook` failure separately, pop the just-pushed `g_hooked_methods` entry, then re-clear `NO_COMPILE` and `set_dont_inline(false)` before re-throwing or returning false. RAII the install steps so an exception unwinds the partially-applied state.
- **confidence:** likely

## Improvements

### [S] [USER_FACING] Document VMHOOK_AUTO_REPAIR_INTERVAL_MS / VMHOOK_DISABLE_AUTO_REPAIR knobs in CHANGELOG or README
- **rationale:** The two compile-time knobs at lines 8305-8306 are only discoverable by reading the header source. New v0.5.0 users have no way to know they exist. A short README section "Auto-repair watchdog" naming both macros and the default 1000ms cadence would prevent users either over-tuning the interval or rebuilding to learn the macro name.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:8294-8313
- **suggested_change:** Add a section to README.md describing the watchdog, the two macros, when to disable (single-threaded test harnesses), and the recommended override range (e.g. 250 ms for latency-sensitive hooks, 5000 ms for low-overhead workloads).

### [S] [USER_FACING] verify_hooks() should return a richer summary, not just a count
- **rationale:** `verify_hooks() -> std::size_t` collapses three semantically-distinct repair events (i2i JMP re-patch, Method* reinstall, JIT drift redeopt) into one number. Users running their own monitoring cannot tell whether their JVM is hostile (mode 2 spike) or just JIT-thrashing (mode 3 spike). The fix is cheap: return a small POD `struct verify_result { size_t i2i_repatched; size_t method_reinstalled; size_t jit_redeopted; size_t freed_unrepaired; }` and keep a `static_cast<size_t>(result)` conversion for back-compat callers.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:8036-8038
- **suggested_change:** Introduce `struct verify_hooks_result` with named counters and a `total()` member. Existing `if (verify_hooks() > 0)` callers continue to work via implicit conversion or `operator size_t`.

### [S] [INTERNAL] Pull the watchdog interval load out of the constexpr to allow runtime override
- **rationale:** Today the interval is compile-time only (`VMHOOK_AUTO_REPAIR_INTERVAL_MS`). Adding `vmhook::set_auto_repair_interval(std::chrono::milliseconds)` would let users tune cadence without recompiling. Useful when the hooked process is heavily contended and 1s pauses for the mutex are visible.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:8321-8329
- **suggested_change:** Replace the `constexpr auto interval{...}` with a `static std::atomic<std::chrono::milliseconds::rep> g_interval_ms{...};` read each loop iteration; provide a small public setter.

### [XS] [USER_FACING] Mode 3 log should name HotSpot's mechanism, not just "JIT-state drifted"
- **rationale:** Today's log says "JIT-state drifted (_code=0x..., NO_COMPILE=cleared)". For a non-HotSpot-savvy user this is opaque. Spelling out "HotSpot's tiered compiler re-promoted this method to compiled despite NO_COMPILE" makes the message actionable.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:8248-8255
- **suggested_change:** Tweak the format string to include a one-line explanation of the cause (matches the comment at 8221-8230 already in code).

### [M] [INTERNAL] Snapshot the install-time `assembly[]` bytes for byte-for-byte trampoline integrity
- **rationale:** See bug "No byte-for-byte trampoline integrity check". A `std::vector<std::uint8_t> snapshot` member on `midi2i_hook` enables true known-good comparison in `verify_and_repair` and gives the repair routine a recovery source for body corruption.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:5220-5638
- **suggested_change:** After the final memcpy at line 5497, copy `this->allocated + HOOK_SIZE` and `sizeof(assembly)` into a new `std::vector<std::uint8_t> trampoline_snapshot`. In `verify_and_repair`, after the JMP check, memcmp the body excluding the `RESUME_JMP_OFFSET+1..+5` region (the chain-resume delta that legitimately changes).

### [S] [USER_FACING] Surface watchdog start failure to the install caller
- **rationale:** `ensure_started()` swallows `std::thread` construction failure (line 8373) and rolls `g_started` back. The user's `hook<T>()` call returns true even though the watchdog never started, so they think auto-repair is working when it is not. A `VMHOOK_LOG` warning would alert them. Today the rollback is silent.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:8373-8378
- **suggested_change:** Add `VMHOOK_LOG("{} auto_repair::ensure_started: std::thread construction failed - hooks installed but JIT drift will not auto-repair. Call vmhook::verify_hooks() manually on a cadence.", vmhook::warning_tag);` inside the `catch (...)` block.

### [S] [INTERNAL] Replace the per-iteration `wait_for` lambda lookup with a member-level shutdown predicate
- **rationale:** The predicate lambda at line 8335 captures nothing and is recreated on every wait, generating an allocation-free but still trivial function-object per iteration. Hoisting it once is a one-line readability tweak.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:8335-8338
- **suggested_change:** Promote the lambda to a `static constexpr auto shutdown_predicate = []() noexcept { return ...; };` at namespace scope, reused by both `wait_for` and the explicit re-check at 8340. Cosmetic.

### [S] [USER_FACING] CHANGELOG entry for v0.5.0 mode-3 + watchdog is missing from `[Unreleased]`
- **rationale:** The audit task lists "verify_hooks modes 1/2/3 (mode 3 = repair, v0.5.0)" and "Auto-repair watchdog (v0.5.0 latest commit)" but the only CHANGELOG sections are `[Unreleased]` (v0.5.0 work — but the watchdog and mode-3 aren't called out there) and `[0.4.0]`. The README does not appear to mention either feature. Users discover them only by reading commit messages.
- **file_lines:** CHANGELOG.md:7-14
- **suggested_change:** Add an "Added" entry under `[Unreleased]` for `vmhook::verify_hooks()` mode 3 (JIT drift repair) and the auto-repair watchdog spawned from `hook<T>()`. Name the two compile-time controls.

## Tests

### [standalone_unit] [new] test_verify_hooks_no_jvm_safety
- **description:** Call `vmhook::verify_hooks()` with no JVM attached (the test_helpers fixture). Confirm it returns 0 and does not crash. Confirm `g_hooked_methods` and `g_hooked_i2i_entries` stay empty.
- **asserts:** `vmhook::verify_hooks() == 0`, no log output above `warning_tag`, internal vectors empty.
- **existing_file:** tests/test_helpers.cpp (extend_existing)

### [standalone_unit] [new] test_verify_hooks_swallows_internal_exceptions
- **description:** Exercise the no-throw contract: install a hook whose `method->get_const_method()` throws (mock or override `is_valid_pointer` via the test fixture pattern used in `test_helpers`). Confirm `verify_hooks()` returns 0 cleanly even when the per-Method probe throws.
- **asserts:** `verify_hooks()` returns without propagating exceptions.
- **existing_file:** tests/test_helpers.cpp (extend_existing)

### [standalone_unit] [new] test_auto_repair_disable_macro
- **description:** Compile a TU with `VMHOOK_DISABLE_AUTO_REPAIR` defined, install one hook (no JVM, expected to no-op-fail), confirm no watchdog thread is spawned by checking the process thread count before / after via `vmhook::for_each_thread`. Compile a second TU without the macro and confirm the watchdog DOES start (thread count differs by 1).
- **asserts:** Watchdog presence flips with the macro.
- **existing_file:** none — new test_auto_repair.cpp

### [standalone_unit] [new] test_auto_repair_interval_macro
- **description:** Confirm `VMHOOK_AUTO_REPAIR_INTERVAL_MS` overrides the compile-time interval. Statically wait `2 * interval + slack` and confirm the watchdog called `verify_hooks` at least twice (instrument via an atomic counter increment hooked inline into a wrapper macro).
- **asserts:** Counter >= 2 within `2 * interval + 500ms`.
- **existing_file:** none — new test_auto_repair.cpp

### [standalone_unit] [new] test_midi2i_hook_verify_and_repair_intact
- **description:** Construct a `midi2i_hook` over a writable scratch page (mirrors the pattern in test_os_protect_interaction.cpp). Confirm `verify_and_repair()` returns true with no log output and no byte mutations.
- **asserts:** Return value true, scratch page bytes unchanged before/after.
- **existing_file:** tests/test_os_protect_interaction.cpp (extend_existing)

### [standalone_unit] [new] test_midi2i_hook_verify_and_repair_repatches_after_clobber
- **description:** Construct a `midi2i_hook`, manually clobber the 5-byte JMP at the injection point with `0x90 0x90 0x90 0x90 0x90` (NOP sled). Confirm `verify_and_repair()` returns false (repair needed), then `true` on the next call, and the bytes are restored byte-for-byte to the expected `0xE9 + rel32` form.
- **asserts:** First call returns false, second returns true, byte-for-byte match against expected JMP.
- **existing_file:** tests/test_os_protect_interaction.cpp (extend_existing)

### [standalone_unit] [new] test_midi2i_hook_verify_and_repair_handles_alien_jmp_chain
- **description:** Clobber the JMP with a JMP to a known-valid second trampoline page. `verify_and_repair` should detect the alien JMP, accept the chain target, rewrite the resume JMP, and re-apply our 5-byte JMP. Confirm `current_chain_resume` was updated.
- **asserts:** Repair returns false, `current_chain_resume` is the second-trampoline page, our JMP is back at the injection point.
- **existing_file:** tests/test_os_protect_interaction.cpp (extend_existing)

### [standalone_unit] [new] test_midi2i_hook_verify_and_repair_rejects_self_chain
- **description:** Clobber the JMP with a JMP pointing back into our own `this->allocated` page. The chain-detection branch at line 5602 must reject the chain (avoiding cycle) and fall back to `current_chain_resume`. Confirm we do not corrupt the resume delta.
- **asserts:** `current_chain_resume` is unchanged after repair.
- **existing_file:** tests/test_os_protect_interaction.cpp (extend_existing)

### [standalone_unit] [new] test_verify_hooks_drift_logged_not_sticky_after_failed_reinstall
- **description:** Regression for the HIGH bug "try_reinstall failure leaves drift_logged stuck". Force-construct a hooked_method whose Method* is invalid AND whose expected_class_name does not resolve. Run `verify_hooks` twice and confirm the second call still logs (or at least still attempts the repair) — i.e. `drift_logged` is reset on the failure path.
- **asserts:** Second `verify_hooks` call observes the same drift and the entry was re-visited (counter / log probe).
- **existing_file:** none — new test_verify_repair_state.cpp

### [standalone_unit] [new] test_verify_hooks_mode3_reachable_after_mode2_repair
- **description:** Regression for the HIGH bug "Mode-3 JIT drift never re-evaluated once mode-1/2 fired earlier". Simulate mode 2 (alias) → successful reinstall → mode 3 (JIT drift). Confirm mode-3 repair fires.
- **asserts:** Mode-3 repair counter increments after the simulated sequence.
- **existing_file:** none — new test_verify_repair_state.cpp

### [standalone_unit] [new] test_shutdown_then_hook_returns_false_or_works
- **description:** Regression for the MEDIUM bug "Auto-repair watchdog cannot be restarted after shutdown_hooks()". Call `shutdown_hooks()` then `hook<T>("foo", cb)`. Either: (a) install fails cleanly with a clear log, OR (b) install succeeds AND the watchdog restarts. Either contract is OK; today neither holds: install returns true but common_detour is dead.
- **asserts:** Either `hook<T>()` returns false with a "shutdown has been requested" log, or `g_shutdown_requested` was reset and a fresh watchdog is alive.
- **existing_file:** none — new test_lifecycle.cpp

### [standalone_unit] [new] test_verify_hooks_byte_for_byte_trampoline_integrity
- **description:** After implementing the snapshot improvement, clobber a single byte inside the trampoline body (e.g. flip the `je` opcode 0x84 to 0x85). Run `verify_and_repair`. Confirm the byte is restored from the snapshot.
- **asserts:** Trampoline body matches snapshot after repair.
- **existing_file:** none — covered by the new improvement; add to tests/test_os_protect_interaction.cpp (extend_existing)

### [jvm_integration] [new] test_jvm_verify_hooks_mode3_redeopt
- **description:** Install a hook on a hot Java method. Let HotSpot recompile it (loop the method enough to trigger CompileBroker). Confirm `verify_hooks()` reports mode 3 once and the hook resumes firing on subsequent compiled-path calls.
- **asserts:** Hook fires on a call after JIT compilation completes.
- **existing_file:** none — new tests/test_jvm_verify_modes.cpp

### [jvm_integration] [new] test_jvm_auto_repair_watchdog_keeps_hook_alive
- **description:** Same setup as above but do NOT call `verify_hooks()` manually. Sleep `interval + slack`. Confirm the watchdog has redeopted the method (test by snapshot of `method->get_code()` returning null at the time of the next call).
- **asserts:** `method->get_code()` is null after watchdog tick.
- **existing_file:** none — new tests/test_jvm_verify_modes.cpp

## Parity Concerns
- The header advertises three repair modes, but the install path's recovery (set both `_from_compiled_entry` and `_from_interpreted_entry`) is asymmetric with mode 3 / try_reinstall (which set only `_from_compiled_entry`). API parity with the install path is a stronger contract than the comment-block at line 8232 ("Repair = redo exactly what the install path does") currently honors.
- `verify_hooks()` returns `std::size_t` — but `for_each_thread`, `for_each_loaded_class`, `for_each_instance` (the new v0.4 introspection trio in the changelog) return rich types or take callbacks. A future `verify_hooks(visitor)` overload that names each repaired entry would match that trio's design language.
- `scoped_hook` and `hook_handle::stop()` participate in the same `g_hooked_methods` vector and benefit from the watchdog, but the docstrings do not say so — users dropping a `scoped_hook` may not realise that drift repair was active during the handle's lifetime.
- The chain-resume snapshot `current_chain_resume` is not preserved across mode-2 `try_reinstall` (the new Method gets the existing trampoline, which still carries the OLD chain target). If the previous hooker who was chained in front of us was itself replaced during the redefinition window, our trampoline now chains into freed memory.
