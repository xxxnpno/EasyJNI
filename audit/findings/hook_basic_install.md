# hook_basic_install

## Summary
`vmhook::hook<T>(name, [signature,] callback)` walks the registered class's
`_methods` array, locks the install mutex, registers a `hooked_method` entry,
patches the shared HotSpot i2i interpreter stub with a 5-byte JMP into an
allocated trampoline, and deopts JIT-compiled targets so direct callers reach
the patched entry. Several silent-failure / partial-install paths leave
`g_hooked_methods` dirty when the install aborts mid-way; the `push_back` of a
new entry also reorders against an existing trampoline so concurrent
in-flight detours on a co-tenant method can iterate a reallocating vector.
A few smaller polish items (duplicate `get_signature()`, useless redundant
`is_valid_pointer` on `methods_array`, etc.) round out the list.

## Bugs

### [high] Half-installed method permanently poisons future re-install attempts
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:7840-7912
- **description:** `g_hooked_methods.push_back(std::move(hm_entry));` runs at line 7840 BEFORE the i2i trampoline is installed (lines 7853-7912). If anything in the i2i install path throws — `find_hook_location` returns nullptr (line 7855-7858), or `midi2i_hook::midi2i_hook` fails RWX allocation and sets `has_error()` (line 7905-7908) — the catch at line 7978 returns `false`, but the `hooked_method` entry is never erased from `g_hooked_methods`. The user sees `false`, calls `vmhook::hook<T>(...)` again to retry, and at line 7766-7772 the duplicate-membership scan finds the orphan entry and returns `true` without actually installing anything. Result: the hook is reported "installed" forever but never fires, and the user has no way to recover short of `shutdown_hooks()`. The same trap applies to the deopt step at lines 7933-7969 — though deopt itself is noexcept-ish via `noexcept` setters, anything that threw before it (e.g. `find_hook_location` failure on a brand-new i2i) leaves the registry with a dangling entry. The pre-install side-effects to `Method._flags` (`set_dont_inline`, line 7774) and `Method._access_flags |= NO_COMPILE` (line 7781) also leak past the failure with no rollback.
- **repro:** Inject into a JVM build whose i2i stub layout doesn't match either `pattern_full` or `pattern_fallback` (e.g. a future JDK build, or an interpreter the user has patched themselves). `find_hook_location` returns nullptr, the first `hook<T>()` returns `false`, every subsequent `hook<T>()` for the same method returns `true` but the detour never fires.
- **suggested_fix:** Move `g_hooked_methods.push_back(...)` to the end of the function, AFTER both the i2i install and the deopt have succeeded. Alternatively, wrap the post-push_back section in a scope guard that pops the entry on exception. The same guard should also restore `_dont_inline` and clear `NO_COMPILE` if they were set during this call.
- **confidence:** certain

### [high] push_back can realloc while a sibling method's detour is iterating
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:7840, 5740-5757, 5883-5906
- **description:** The vector-iteration contract documented at lines 5743-5751 ("mutated only from the user's setup thread BEFORE detours fire") is silently violated when the user installs hook #2 on a method that shares an i2i stub with already-installed hook #1. The shared i2i has already been patched, so the trampoline is live and `common_detour` is dispatching for hook #1 right now. When `g_hooked_methods.push_back(...)` at line 7840 forces a reallocation, the lock-free iteration in `common_detour` (line 5883) is reading the OLD buffer, which has just been freed by the allocator. UAF on the `hook.detour` `std::function` cell — observed in practice as random AV in `std::_Func_impl_no_alloc::_Do_call` from the trampoline. The mutex held by the install path (line 7764) doesn't help because `common_detour` deliberately does not acquire it.
- **repro:** Hook a hot method (e.g. `Object::toString` whose i2i is shared with many other Object subclasses), then on a worker thread install a second hook on `String::length` while the first is firing. The exact race is timing-sensitive but trivial to reproduce with a tight loop in a Java thread plus a tight install loop in another C++ thread.
- **suggested_fix:** Either (a) pre-reserve the vector to a known cap inside `register_class` / library init so push_back never re-allocates, (b) switch `g_hooked_methods` to a `std::vector` of stable-address handles (e.g. `std::vector<std::unique_ptr<hooked_method>>` or a `std::list`), or (c) replace lock-free iteration with a copy of `(method*, detour&)` pairs taken under the mutex before dispatch. Option (a) preserves the current hot path; the docstring already mentions it as a workaround.
- **confidence:** likely

### [high] Auto-repair watchdog is permanently dead after shutdown_hooks + re-init
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:7974, 8315-8403, 8412
- **description:** `hook<T>()` calls `auto_repair::ensure_started()` at line 7974 which CAS-sets `g_started` to true and detaches a worker thread. The worker returns when `g_shutdown_requested` becomes true (line 8340). `shutdown_hooks()` raises that flag at line 8412 but never resets it, and `g_started` is never reset either. If the user calls `shutdown_hooks()` (e.g. between two test runs, or to reload a mod) and then re-installs hooks, the install succeeds but: (1) `g_shutdown_requested` is still true, so the very first `common_detour` fire sees the flag at line 5853 and returns immediately — detours never fire; (2) `ensure_started` sees `g_started == true` and skips re-spawning the watchdog, so even if the flag could be reset, no auto-repair runs. The library cannot be re-initialised within a process lifetime.
- **repro:** `vmhook::hook<X>("a", cb1); vmhook::shutdown_hooks(); vmhook::hook<X>("b", cb2);` — the second hook's callback is never invoked.
- **suggested_fix:** In `shutdown_hooks()` after the tear-down loop finishes and after `g_hooked_methods.clear()`, reset both `g_shutdown_requested` to false and `auto_repair::g_started` to false. Alternatively document explicitly in the `shutdown_hooks` doc that re-init is unsupported and have `hook<T>()` early-fail when `g_shutdown_requested` is set instead of silently letting installs accumulate.
- **confidence:** certain

### [medium] _dont_inline silently skipped when Method._flags VMStruct is missing
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:7774, 5935-5952, 2203-2214
- **description:** `set_dont_inline` returns silently if `Method._flags` is not exported by gHotSpotVMStructs (line 5939-5942). On every JDK from 8 to 25 the field is exported, but a future or patched JVM that drops it will leave the inline-guard half-applied: `NO_COMPILE` (later, line 7781) goes through `get_access_flags()` which DOES throw if the VMStruct is missing, but `_dont_inline` is a silent no-op. The hook may then get inlined into a JIT'd caller, completely bypassing the i2i patch even though `_code` is cleared, because the inlined IR has already inhaled the method body. No diagnostic, no warning — looks like a "the hook just doesn't fire" mystery.
- **repro:** Synthetic — build a `VMStructs` shim that excludes `Method._flags`, install a hook, observe that JIT'd callers reaching the method through inlining never trigger the detour while uninlined callers do.
- **suggested_fix:** Have `set_dont_inline` return a `bool` indicating success and log a warning at install time when it fails (mirroring the `get_access_flags` failure path at line 7777-7780). This converts the silent failure into an at-least-noticed failure.
- **confidence:** likely

### [medium] is_valid_pointer probe on raw methods_array can crash on a junk pointer
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:7732-7737, 2617-2638
- **description:** `target_klass->get_methods_ptr()` returns a pointer at offset +8 into the `Array<Method*>` body. `get_methods_ptr` does validate the array base via `is_valid_pointer`, but the pointer it returns is `array + 8`, and `hook<T>` then checks only `if (!methods_array || method_count <= 0)`. The `is_valid_pointer` check skipped on `methods_array` is fine because we already validated `array`, BUT the loop at line 7740 dereferences `methods_array[method_index]` for `method_index` up to `method_count - 1`. If the klass's `_methods` array length was corrupted (e.g. a non-InstanceKlass slipped through `find_class`, returning an arrayKlass or objArrayKlass), the loop walks `method_count` bytes past the array end and AV's. `get_methods_count` does a similar +0 read of `_length` but again with no upper-bound sanity check.
- **repro:** Race `register_class<T>` against a JVMTI agent that retransforms T to a different class layout. The cached klass may transiently point at a non-InstanceKlass.
- **suggested_fix:** Add a sanity cap in `hook<T>()`, e.g. `if (method_count > 100000) throw ...`, since real Java classes never have more than a few thousand methods. Also call `is_valid_pointer(methods_array[method_index])` before accessing fields on the Method (already done on line 7743, but only after a possibly-bad dereference earlier in the loop bounds check).
- **confidence:** speculative

### [low] Pre-install Method-flag mutations leak on exception
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:7774, 7781
- **description:** `set_dont_inline(found_method, true)` and `*flags |= NO_COMPILE` mutate live Method bookkeeping before the i2i / deopt steps that might throw. If the install fails at `find_hook_location` (line 7855), `midi2i_hook` allocation (line 7903), or even just because the deopt branch fails to recover a `c2i_entry` (line 7949 just logs — doesn't throw — so this one is OK, but the principle stands), the Method is left with `NO_COMPILE` and `_dont_inline` set even though no hook is installed on it. The JVM will refuse to JIT a method it can't actually compile, with no way for the user to restore it short of unloading the class. Minor — most users won't notice — but symmetry with `hook_handle::stop()` (which DOES clear both flags, lines 8506-8510) suggests this is unintentional.
- **repro:** Same as the "half-installed" repro above.
- **suggested_fix:** Same rollback guard as suggested for the half-install bug; if the install fails, clear `NO_COMPILE` and `_dont_inline` before returning false.
- **confidence:** likely

### [low] catch swallows all std::exception including bad_alloc — no stack trace
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:7978-7982
- **description:** The catch is too broad. `std::bad_alloc` from the `g_hooked_methods.push_back` (large-state lambda capture, see the `std::function` member of `hooked_method`) is reported as a plain `vmhook::hook() ...: bad allocation` log line and the user sees `false` returned. There's no distinction between "method not found" (a configuration bug fixable by the user) and "out of memory" (a fatal-class issue). The log message also doesn't include a stack trace, so debugging a regression is harder than necessary. Compounded by the surrounding lambda's `std::function` allocation possibly being the source of throw — a confusing log message points at the user's hook call when the root cause is allocator state.
- **repro:** Hard to trigger naturally; observed when the auditor injects a memory-pressure failpoint.
- **suggested_fix:** Split the catches: catch `std::bad_alloc` separately and re-log it as "out of memory installing hook" with the suggestion to retry; catch `vmhook::exception` separately (these are the controlled-error path) and pass the inner what() through cleanly. Keep the generic catch as a fallback.
- **confidence:** speculative

## Improvements

### [S] [USER_FACING] Surface the method count when "Method not found" fires
- **rationale:** Right now the error message at line 7755 says `Method 'foo' not found in class 'bar'`. Users have no way to know whether they typed the name wrong or the class loader handed back a stripped/empty klass. Including `method_count` and (if small) the first few available names makes the diagnostic actionable.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:7753-7756
- **suggested_change:** Build a brief listing of the first ~5 method names from `methods_array` and include them in the throw: `Method 'foo' not found in class 'bar' ({} methods declared; first names: [bar, baz, quux]). Hint: check capitalisation, descriptor mismatch, or whether you wanted a synthetic / bridge method.`

### [S] [USER_FACING] Mention the duplicate-install short-circuit in the log
- **rationale:** When the duplicate check at line 7766-7772 returns `true`, nothing is logged. Users who add a second `hook<T>(...)` to the same method (e.g. by re-running setup code) get a silent success that hides the fact their second callback is NOT replacing the first. They wire up callback B, expect B to fire, and see callback A behaviour. This is the #1 cause of "my hook isn't working" support questions.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:7766-7772
- **suggested_change:** Add a `VMHOOK_LOG(vmhook::warning_tag, ...)` line before the `return true;` saying that a duplicate install was requested and the original callback remains active. Recommend `scoped_hook` / `hook_handle::stop` if the user really wants to swap callbacks.

### [S] [USER_FACING] Surface JDK family and platform in the per-hook log
- **rationale:** The single-line log at line 7797-7801 says either "deoptimising" or "patching i2i stub" — useful, but it doesn't include the JDK version or pattern (full/fallback). Users on JDK 21+ who hit `find_hook_location` failures have no way to know which pattern was attempted. Adding `(JDK detected: 21, pattern: fallback)` triages 90% of "doesn't work on my JVM" reports in one log line.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:7795-7802
- **suggested_change:** Cache the chosen pattern in `find_hook_location` and pass it through to the log message here. Cost is one boolean.

### [S] [INTERNAL] Don't re-symbolize the signature for expected_signature
- **rationale:** Line 7839 calls `found_method->get_signature()`, which does the full ConstMethod → Symbol → to_string chain. The same call was already made at line 7745 in the filter loop (well, only if `method_signature.empty()` was false). Cache the result in a local once and re-use.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:7745, 7839
- **suggested_change:** Hoist `const std::string actual_signature{ found_method->get_signature() };` after the `found_method != nullptr` check, use it for both filter comparison (when applicable) and `hm_entry.expected_signature`.

### [S] [INTERNAL] Drop static-internal-linkage on namespace-scope hook template
- **rationale:** `static auto hook(...)` at namespace scope gives the function template internal linkage in every TU that includes the header. C++17 inline variables share state, but each TU still emits its own template instantiations for the same `hook<T>` — pure code-bloat on multi-TU projects (the typical injector + example layout). Other top-level entry points (`find_class`, `register_class`, `verify_hooks`, `shutdown_hooks`) have the same issue. `inline` is the modern equivalent and gives external linkage with inline semantics.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:7688, 7693, 7708, 6202, 6797, 8036, 8405
- **suggested_change:** Replace `static` with `inline` on every namespace-scope function / function template that is meant to be header-only. Single-token change per definition.

### [M] [USER_FACING] Add a list-overloads helper for the "method not found" path
- **rationale:** When `hook<T>` fails because of an overload mismatch, the user is left guessing the descriptor. The class has the methods, the user just doesn't know which signature to pass. A `vmhook::list_overloads<T>("methodName")` helper that returns `std::vector<std::string>` of every matching signature would let the user copy-paste the right one into their `hook<T>(name, sig, cb)` call.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:7708-7756 (logic site), header API (new symbol)
- **suggested_change:** Add `template<class wrapper_type> static auto list_overloads(std::string_view name) -> std::vector<std::string>;` defined inline near the hook overloads. Walk the methods array, collect signatures whose name matches, return them. Suggest its use in the "method not found" error message.

### [S] [INTERNAL] Replace `auto&& user_detour` + std::forward dance with a named template param
- **rationale:** `auto&& user_detour` is a forwarding parameter and works correctly, but the `[detour = std::forward<decltype(user_detour)>(user_detour)]` capture is dense and tripped up two earlier audits per the comment thread. A named template parameter (`template<class wrapper_type, class detour_t> auto hook(... detour_t&& user_detour)`) makes the type and forwarding intent explicit and lets `static_assert(std::is_invocable_v<detour_t&, vmhook::return_value&, ...>)` surface signature mismatches at the install site instead of at the lambda-call site deep inside template machinery.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:7688-7711, 7805-7824
- **suggested_change:** Add the named template parameter and a `static_assert` that checks the callable signature against `function_traits`. Mirror the change in `scoped_hook` (line 8549) so error messages stay consistent.

## Tests

### [standalone_unit] [new] hook_returns_false_when_class_not_registered
- **description:** Calling `vmhook::hook<MyType>("foo", cb)` without first calling `register_class<MyType>("my/Type")` must return `false` and log a clear error. Easy to check without a JVM because the failure happens at `type_to_class_map.find` before any HotSpot lookup.
- **asserts:** Return value is `false`; `VMHOOK_LOG` (intercepted via a custom sink) emitted the "Class not registered" message; `g_hooked_methods.size()` is unchanged (zero in a fresh test).
- **existing_file:** tests/test_api_surface.cpp (extend_existing — already has a `register_class` + `hook` exercise, mirror it without the registration)

### [standalone_unit] [new] hook_returns_false_when_class_not_loaded
- **description:** With `register_class<MyType>("does/not/Exist")` but no live JVM (so `find_class` returns nullptr), `vmhook::hook<MyType>("foo", cb)` should return false and not mutate any globals.
- **asserts:** `false` return, no entry pushed to `g_hooked_methods`, log line matches "Class '...' not found in JVM."

### [standalone_unit] [new] hook_signature_overload_compiles_and_short_circuits
- **description:** Compile-only + zero-JVM check that both `hook<T>(name, cb)` and `hook<T>(name, sig, cb)` are callable for the same wrapper, return false, and don't crash. Mirror the existing `test_api_surface.cpp::exercise_hooks` test.
- **asserts:** Both overloads compile, both return false without JVM, `shutdown_hooks()` is safe to call after.
- **existing_file:** tests/test_api_surface.cpp (extend_existing)

### [standalone_unit] [new] duplicate_hook_install_does_not_double_push
- **description:** After mocking the type_map + a fake klass that returns a single method, call `hook<T>("foo", cb1)` then `hook<T>("foo", cb2)` — the second call should return true (current behaviour) and `g_hooked_methods.size()` should still be 1.
- **asserts:** Both calls return true; `g_hooked_methods.size() == 1`; the registered detour is `cb1` (the original), not `cb2` (verifies the silent-replacement-doesn't-happen bug surfaced by the suggested log improvement).

### [standalone_unit] [new] failed_install_does_not_poison_registry
- **description:** Pinpoint the "half-installed" bug. Inject a failpoint inside `find_hook_location` that returns nullptr on first call only. Call `hook<T>("foo", cb)` (returns false). Remove the failpoint, call `hook<T>("foo", cb)` again — it should re-attempt the full install, not short-circuit to true. After fix, `g_hooked_methods.size()` after the first failed call should be 0.
- **asserts:** First call returns false; `g_hooked_methods.size() == 0` after the first call; second call goes through the full install path (verifiable by the trampoline being allocated).

### [standalone_unit] [new] auto_repair_restarts_after_shutdown_and_reinit
- **description:** Tests the watchdog-dead-after-shutdown bug. After `hook<T>(...) -> true; shutdown_hooks(); hook<T>(...) -> ???`, the second install should leave the system in a state where `common_detour` would actually dispatch (i.e. `g_shutdown_requested == false`) and a new worker thread is spawned.
- **asserts:** Post-reinit `g_shutdown_requested.load() == false`; `auto_repair::g_started.load() == true` and the worker is actually alive (heartbeat counter incremented after one interval).

### [jvm_integration] [new] hook_fires_for_interpreted_method
- **description:** Spin up a real JVM via JNI-Invocation, register a wrapper for a known class (e.g. `java/lang/String`), hook `length()`, call `"abc".length()` from Java, assert the detour ran with `self.toString() == "abc"`. Verifies the interpreter-dispatch path end-to-end.
- **asserts:** Detour invoked exactly once; `arg0` matches the receiver; original return value (3) is observed by Java if cancel was not called.

### [jvm_integration] [new] hook_deopts_jit_compiled_method
- **description:** Force-compile a target method (loop call until C2 kicks in), confirm `Method._code != nullptr` via VMStructs, install the hook, call the method once, assert (a) the detour fired and (b) `Method._code` is now nullptr and `_from_compiled_entry` points into the c2i adapter (or the fallback i2i stub when c2i recovery fails).
- **asserts:** `_code == nullptr` post-install; `_from_compiled_entry` is the c2i adapter address (validated via `validate_adapter_handler_entry`) on JDK 8/17/21/24/25; detour fired on the first post-install dispatch.

### [jvm_integration] [new] hook_install_under_concurrent_dispatch_does_not_corrupt
- **description:** Stress test for the realloc-during-iteration bug. Pre-install a hook on a hot shared-i2i method (e.g. `Object.toString`), spawn 4 Java threads in a tight `toString()` loop, then from a C++ thread install 50 additional hooks on sibling classes that share the same i2i. The trampoline already exists, so each new install lands in the `i2i_already_patched` branch and pushes a `hooked_method` entry while detours fire. Run under ASan / TSan; no AV, no race report.
- **asserts:** Test runs for N seconds without crash; final `g_hooked_methods.size() == 51`; ASan / TSan finds no issues.

### [jvm_integration] [new] hook_chain_resume_into_prior_hooker
- **description:** Simulate another DLL having patched the i2i stub before us by manually writing a `0xE9 + rel32` JMP to a known scratch trampoline at `find_hook_location(i2i)`. Install our hook, verify our trampoline's resume JMP lands on the scratch trampoline (chain detection took the `chain_resume != nullptr` branch at line 7877-7891).
- **asserts:** Log contains "chaining our hook in front of theirs"; our trampoline's resume rel32 decodes to the scratch trampoline address; both detours fire on a subsequent dispatch.

### [standalone_unit] [extend_existing] hook_with_invalid_chain_jmp_target_falls_back_to_default_resume
- **description:** Plant a `0xE9 + rel32` at the injection point whose decoded target is in kernel space (fails `is_valid_pointer`). Install our hook, verify chain_resume stayed nullptr and the trampoline resumes at `target + HOOK_SIZE` per the constructor's defence (line 5251-5253).
- **asserts:** Log contains "doesn't pass is_valid_pointer - installing without chain"; `current_chain_resume == nullptr`; trampoline resume rel32 lands on `target + HOOK_SIZE`.
- **existing_file:** tests/test_helpers.cpp (extend_existing — this file already covers `is_valid_pointer` sentinels)

### [standalone_unit] [new] hook_user_callable_signature_mismatch_compile_diagnostic
- **description:** Verify (negative compile test, gated behind `static_assert`) that passing a detour with the wrong arg count or types produces a readable error at the call site, not deep inside lambda template machinery.
- **asserts:** Compilation fails with a `static_assert` whose text mentions the expected signature; without `static_assert` the error appears inside the wrapper lambda 100+ lines deep.

## Parity Concerns
- `scoped_hook<T>` re-implements parts of the install flow (line 8567-8615 does a duplicate Method* lookup). The two paths should share helpers so any fix to `hook<T>` is automatically inherited by `scoped_hook`.
- `verify_hooks::try_reinstall` (line 8064+) duplicates the `find_class` → walk-methods logic from `hook<T>`. Extracting a `find_method_in_class(klass*, name, sig)` helper would unify the three sites and shrink the surface area for drift.
- `hook_handle::stop()` clears `_dont_inline` and `NO_COMPILE` (lines 8506-8510); a failed `hook<T>()` does not (see the "pre-install mutations leak" bug). The two paths should match.
- The auto-repair watchdog interval (`VMHOOK_AUTO_REPAIR_INTERVAL_MS`, default 1000ms) is documented at the install site (line 8306) but not from `hook<T>` itself. Mention it in the `hook<T>` doc-comment so users discover the knob from the function they're calling.
- `vmhook::hook` returns `bool`; `vmhook::scoped_hook` returns a handle. Consider returning `std::expected<hook_handle, install_error>` (or a small `install_result` struct) so the basic `hook<T>` API can also expose typed failure reasons (class-not-registered, method-not-found, i2i-pattern-missing, oom).
