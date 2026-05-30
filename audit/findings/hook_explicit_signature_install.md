# hook_explicit_signature_install

## Summary
`vmhook::hook<T>(name, signature, callback)` (vmhook.hpp:7708-7983) walks the InstanceKlass `_methods` array linearly and accepts the first entry whose `get_name()` matches and whose `get_signature()` matches the filter (or, if filter is empty, the first name match). The resulting `found_method*` flows into `midi2i_hook` install plus `g_hooked_methods` registration. The matching itself is correct, but several user-friendliness and bug paths around it — silent duplicate-install drops, leaked `NO_COMPILE` / `_dont_inline` state on mid-install failure, no superclass walk for inherited overloads, no integration test that proves the filter actually selects one overload, and a wrapper-detour arity mismatch that goes undiagnosed at compile time — make the feature easy to mis-use.

## Bugs

### [medium] Mid-install failure leaks NO_COMPILE and g_hooked_methods entry
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:7774-7911
- **description:** The install sequence sets `_dont_inline`, OR's `NO_COMPILE` into `*flags`, and `g_hooked_methods.push_back(hm_entry)` (line 7840) BEFORE running `find_hook_location` / `midi2i_hook` allocation (lines 7855-7909). If those later steps throw — e.g. `find_hook_location` returns null (line 7858) or `midi2i_hook` allocation fails (line 7908) — the catch block at line 7978 returns `false`, but the JIT inhibitors stay set on `found_method` AND a `hooked_method` entry whose i2i trampoline was never installed is left in `g_hooked_methods`. `common_detour` will never fire for it (no trampoline), but `verify_hooks`/`shutdown_hooks` still walk it; `shutdown_hooks` at line 8434 will dutifully clear `_dont_inline` and `NO_COMPILE`, but until shutdown the method is JIT-frozen for no reason, and a user who retries the install with the same `found_method` will be silently short-circuited by the duplicate check at line 7766-7772 — second install returns `true` immediately, but no trampoline exists, so the user thinks the hook is active and it never fires.
- **repro:** Inject into a JVM where `allocate_nearby_memory` cannot find a free page within ±2 GiB of the i2i stub (highly fragmented address space, or i2i stub at the top of the kernel/user-space boundary on certain JVM forks). Call `vmhook::hook<T>("foo", "(I)V", cb)`. The first call returns `false`. Re-call it: returns `true` (duplicate check fires), but the hook never invokes the callback.
- **suggested_fix:** Either (a) move the `set_dont_inline` / `NO_COMPILE` / `g_hooked_methods.push_back` lines to AFTER the trampoline allocation succeeds (commit-or-rollback ordering), or (b) on the catch path, undo the partial state: `set_dont_inline(found_method, false)`, clear `NO_COMPILE`, and pop the last `g_hooked_methods` entry if it matches `found_method`.
- **confidence:** certain

### [medium] Duplicate install silently discards the user's new detour
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:7766-7772
- **description:** If `found_method` is already in `g_hooked_methods` the function returns `true` without logging, without replacing the existing detour, and without telling the caller that their lambda was ignored. Combined with the explicit-signature overload, this is a common foot-gun: a user calls `hook<T>("foo", "(I)V", cb1)` once during setup, then later tries `hook<T>("foo", "(I)V", cb2)` to swap behaviour. The second call returns `true`, the user believes `cb2` is now active, but `cb1` is still firing. The same problem exists in the no-signature path, but the signature overload is the one users reach for when they intend to be precise about which method they hook.
- **repro:** Install two hooks back-to-back on the same name+signature. Confirm via probe state (e.g. a static flag set inside the second lambda) that the second never fires.
- **suggested_fix:** When the duplicate is detected, either `VMHOOK_LOG` a `warning_tag` line ("hook for X already installed; new detour ignored; call scoped_hook's stop() first or use shutdown_hooks() to re-install") so silent failures stop, or replace the existing entry's `detour` field under the lock (probably what most users expect). Picking the former is safer because no in-flight `common_detour` will see a half-replaced `std::function`.
- **confidence:** certain

### [medium] No superclass walk — inherited overloads cannot be hooked by signature
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:7731-7751
- **description:** `target_klass->get_methods_count()` / `get_methods_ptr()` only return methods declared directly on the InstanceKlass (vmhook.hpp:2589-2607, 2617-2639). If the user registers `register_class<MySubclass>("com/example/MySubclass")` and then calls `hook<MySubclass>("toString", "()Ljava/lang/String;", cb)` for a `toString` inherited from `Object` (i.e. not overridden by `MySubclass`), the loop iterates the SUBCLASS's declared methods only, never finds `toString`, and throws "Method not found in class". The user is then forced to register a wrapper for `java/lang/Object` (or the actual declaring class), which defeats the user-friendly point of `hook<T>` with the subclass type. For the explicit-signature overload specifically, the user has already made the contract very explicit and expects the lookup to be more flexible, not less.
- **repro:** `register_class<MyDog>("com/example/Dog")` where `Dog` extends `Animal` and `Animal` declares `speak()V` that `Dog` does not override. Call `vmhook::hook<MyDog>("speak", "()V", cb)` — fails with "Method 'speak' not found in class 'com/example/Dog'."
- **suggested_fix:** After the declared-methods loop fails to match, walk `target_klass->get_super_klass()` chain (helper already exists for field lookup — see `find_field`'s superclass walk pattern) and retry the name/signature comparison there. Log info_tag identifying the actual declaring class so users learn where the method actually lives. Stop at `Object` / null super.
- **confidence:** likely

### [low] Empty-signature filter on an overloaded method silently picks the first declaration order
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:7739-7751
- **description:** The condition `method_signature.empty() || method_ptr->get_signature() == method_signature` (line 7745) means that when a user calls the no-filter overload `hook<T>("defineClass", cb)` against a class with 5 overloaded `defineClass` methods, the loop `break`s on the first one in declaration order. There is no warning, no log, no compile-time error to tell them that they probably wanted the signature overload. The CHANGELOG explicitly motivates v0.4.0's signature overload with the `defineClass` case, so the library knows this exact foot-gun exists, yet the no-filter call path does not detect-and-warn about ambiguity.
- **repro:** Define a Java class with two same-name methods (`foo(I)V` and `foo(J)V`). Call `vmhook::hook<MyClass>("foo", cb)`. The library hooks one of them with no diagnostic.
- **suggested_fix:** When `method_signature.empty()`, do a full pass to count name matches. If `count > 1`, log a `warning_tag` line listing every overload's signature and which one was picked: "Class X has 2 methods named 'foo' ('(I)V', '(J)V'); hooking '(I)V'. Use hook<T>(name, signature, cb) to disambiguate." Continue with the existing behaviour so existing call sites still compile and work.
- **confidence:** certain

### [low] Wrapper-detour arity mismatch on signature install is undiagnosed
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:7715-7717, 7805-7824
- **description:** `function_traits` deduces `method_arg_tuple_t` from the user's lambda parameter list (after stripping the leading `return_value&`). The wrapper_detour calls `extract_frame_arg` for each element of that tuple, reading from `frame->get_locals()`. If the user passes the wrong signature filter (e.g. selects `(II)V` but writes a lambda taking only one `std::int32_t`), the wrapper extracts one slot and ignores the second — the Java method's second int argument is silently dropped from the user's view. The reverse (lambda takes more args than the Java method) reads past the end of `locals`, producing garbage. Nothing in the install path cross-checks the user's lambda arity against the JVM descriptor encoded in `method_signature` (which is available right there in `method_signature`). The check is exactly what makes the signature overload safer than the no-filter overload, but the code does not perform it.
- **repro:** `hook<T>("addInts", "(II)V", [](return_value&, const std::unique_ptr<T>&, int a) { ... })`. The lambda expects 1 visible int. The method actually takes 2. No diagnostic; the hook fires and `a` reads `arg0` correctly, but `arg1` is silently dropped. If the user wrote a 3-int lambda, they would read frame-local-array out-of-bounds.
- **suggested_fix:** At install time, when `method_signature` is non-empty, parse it (simple state machine on the descriptor body up to the closing `)`) to count argument slots — adding 1 per primitive, 2 per `J`/`D`, 1 per `L...;` or `[`. Compare against `1 + std::tuple_size_v<method_arg_tuple_t>` (the 1 is the implicit `this` for instance methods; instance vs static can be read off `Method::_access_flags & ACC_STATIC` or trivially by checking whether the first arg of the lambda is a `unique_ptr<T>`). On mismatch, throw `vmhook::exception` with the actual JVM descriptor and the deduced C++ tuple types so the user sees both. Same logic could power a `static_assert` if the user wrote out a `std::function` typedef matching the descriptor (an `M` improvement), but the install-time runtime check is the more achievable win.
- **confidence:** likely

### [low] `expected_signature` snapshot lets verify_hooks pick the wrong overload after class redefinition
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:7839, 8086-8088
- **description:** Line 7839 stores `hm_entry.expected_signature = found_method->get_signature()`. If that call returns `""` because the Symbol was momentarily unreadable (the function has a try/catch that returns empty on any error, vmhook.hpp:2308-2312), the recovery path at line 8087-8088 sees `expected_signature.empty()` and falls back to `(name-only match)`. For an overloaded method, the recovery's `try_reinstall` then re-points the hook at the FIRST same-name method in declaration order — which on `defineClass` could be the wrong overload, and the user's callback now receives the wrong descriptor's arguments. The user's lambda continues to read `frame->get_locals()` as though the original signature were intact, with no log.
- **repro:** Hard to reproduce naturally — requires `get_signature()` to fail at exactly install time. Could be forced via fault injection. The window is small but the failure mode is silent.
- **suggested_fix:** When `found_method->get_signature()` returns empty at line 7839, prefer the user-supplied `method_signature` as the stored value (if non-empty), so recovery still picks the correct overload. If both are empty, log a warning that drift recovery on this hook may pick the wrong overload. Easier: just `throw vmhook::exception{"Failed to read method signature for drift snapshot."}` and let the user retry; that is consistent with the rest of the install path's "fail fast on missing identity" stance.
- **confidence:** speculative

## Improvements

### [S] [USER_FACING] Surface the descriptor mismatch in the error message
- **rationale:** The current "Method 'foo' not found in class 'X'" message at line 7755 is identical whether the name was wrong, the signature was wrong, or the method is inherited. Users debugging a typo in their descriptor get no hint about which dimension is off.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:7739-7755
- **suggested_change:** On miss, do a second pass: if any name match exists but no signature match, throw "Method 'foo' exists on class 'X' but no overload matches signature 'Y'. Available overloads: '()V', '(I)V', '(Ljava/lang/String;)V'." If no name match, throw the current error. Costs O(methods) on the failure path only (which already exists because we just walked the array).

### [S] [USER_FACING] Replace the silent duplicate short-circuit with a log
- **rationale:** Same as the duplicate-install bug above, but as a quality-of-life issue: even when "ignore second install" is the intended semantics, the user deserves to know.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:7766-7772
- **suggested_change:** `VMHOOK_LOG("{} hook(): method '{}.{}{}' already hooked; second install with new detour ignored. Call hook_handle::stop() or shutdown_hooks() to re-install.", vmhook::warning_tag, type_map_entry->second, method_name, method_signature);` before the `return true`.

### [S] [USER_FACING] Convert "Class not registered" exception text into actionable hint
- **rationale:** Current message at line 7722 mentions the mangled `typeid().name()` of the wrapper, which is non-obvious for users not used to RTTI. Adding the Java class name from any neighbouring registration hint would help.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:7719-7723
- **suggested_change:** Phrase as: "Wrapper class for hook<{typeid}> not registered. Call vmhook::register_class<{wrapper_type}>(\"com/example/...\") before vmhook::hook<{wrapper_type}>(name, signature, ...)." Also suggest doing this at static-init time. (typeid().name() is what it is — but the actionable sentence makes the path obvious.)

### [XS] [INTERNAL] Drop the unused `static` storage-class on the in-namespace template
- **rationale:** `static auto hook(...)` at line 7689 and 7709 in a namespace context gives the template internal linkage per-TU, which is what you want here since you have a definition in a header — but the more idiomatic spelling is `inline` for namespace-scope function templates (templates already have weak linkage). `static` reads as "I meant member-static" to readers who do not pause on the namespace context. Pure stylistic — semantically identical.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:7688, 7693, 7708
- **suggested_change:** Drop `static` from the three `hook` declarations / definition; templates in headers already have the right linkage.

### [M] [USER_FACING] Compile-time signature-arity check via constexpr descriptor parsing
- **rationale:** Catches the "lambda arity does not match descriptor" mistake at compile time when the user supplies a string-literal descriptor. C++20 has constexpr string operations; a `static_assert` inside the template using a constexpr descriptor parser would catch the mismatch the moment the user wrote the lambda + signature.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:7708-7717
- **suggested_change:** Add a helper `template<auto Descriptor> constexpr auto descriptor_arg_count()` that walks the literal at compile time. Add a `static_assert(descriptor_arg_count<Descriptor>() == std::tuple_size_v<method_arg_tuple_t>, "...")`. Requires the descriptor to be a template NTTP (a `static_string` wrapper), so it is opt-in via a sibling `hook_typed<T, vmhook::sig("(II)V")>(name, cb)` API rather than replacing the existing one. Bigger lift, but it is the right end-state.

### [S] [INTERNAL] Hoist the method lookup into a single helper used by both hook<T> and scoped_hook<T>
- **rationale:** Lines 7739-7751 and 8599-8615 implement the same name + optional-signature loop twice, with subtly different validity checks (only one calls `is_valid_pointer` on the Method*; both call `get_methods_count <= 0` then null check). If a future contributor adds the superclass walk to one, they will forget the other.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:7739-7751, 8599-8615
- **suggested_change:** Add `vmhook::hotspot::find_method_on_klass(klass*, name, signature) -> method*` in the `hotspot` namespace; both `hook<T>` and `scoped_hook<T>` (and the drift-repair `try_reinstall` at line 8082-8093, which has a third copy) call it. Three copies collapse into one. Reduces the surface area for the superclass-walk bug too.

## Tests

### [jvm_integration] [new] test_hook_with_explicit_signature_selects_correct_overload
- **description:** Add a Java fixture method pair `void compute(int)` and `void compute(long)` on `example_class`. Install `vmhook::hook<example_class>("compute", "(I)V", cb_int)` and `vmhook::hook<example_class>("compute", "(J)V", cb_long)`. Have a Java probe call both. Assert that `cb_int` only fired for the int call and `cb_long` only for the long call, and that the int arg observed by `cb_int` was the expected value (not garbage from reading slot 0 as if it were a long).
- **asserts:** intHookCalled == 1, longHookCalled == 1, intHookArgObserved == expectedInt, longHookArgObserved == expectedLong, neitherCrossFired.
- **existing_file:** vmhook/src/example.cpp (extend_existing — add to `test_overloaded_methods` or as a new sibling)

### [jvm_integration] [new] test_hook_empty_signature_picks_first_declared_overload_and_warns
- **description:** With the same `compute(int)` / `compute(long)` fixture, call `vmhook::hook<example_class>("compute", cb)` (no signature filter). Assert which overload got hooked (first in declaration order, per current code), AND assert the log captured an ambiguity warning. Pairs with the warning-on-ambiguity improvement above.
- **asserts:** Exactly one of cb_int / cb_long fires for both Java-side calls (whichever the loop picks); warning log line was emitted naming both signatures.
- **existing_file:** vmhook/src/example.cpp (extend_existing)

### [jvm_integration] [new] test_hook_signature_mismatch_returns_false_with_descriptor_in_message
- **description:** Call `vmhook::hook<example_class>("compute", "(D)V", cb)` against a class where `compute(double)` does not exist. Assert the call returns `false` AND the log line names the descriptor that was searched for plus the list of `compute` overloads that DO exist on the class. Validates the better-error-message improvement.
- **asserts:** install == false, log contains "(D)V" and the actual available descriptors.
- **existing_file:** vmhook/src/example.cpp (extend_existing)

### [jvm_integration] [new] test_hook_duplicate_install_logs_and_keeps_first_detour
- **description:** Install `hook<T>("foo", "(I)V", cb1)` then `hook<T>("foo", "(I)V", cb2)`. Assert both calls returned `true`, a warning log line fired for the second, and only `cb1` fires when Java calls `foo(int)`. Documents the current behaviour (which is reasonable) and locks the contract.
- **asserts:** install1 == true, install2 == true, cb1Called >= 1, cb2Called == 0, warning log captured.
- **existing_file:** vmhook/src/example.cpp (extend_existing)

### [jvm_integration] [new] test_hook_inherited_method_with_explicit_signature
- **description:** Add Java subclass `B extends A` and call `vmhook::hook<b_class>("methodFromA", "(I)V", cb)`. Currently fails with "method not found"; if the superclass-walk improvement lands, this passes and the log identifies `a_class` as the declaring class.
- **asserts:** Before fix — install == false, error log says "not found in class 'B'". After fix — install == true, info log identifies the actual declaring superclass.
- **existing_file:** vmhook/src/example.cpp (extend_existing — `a_class`/`b_class` already exist on lines 81-202)

### [jvm_integration] [new] test_hook_explicit_signature_install_failure_does_not_leak_no_compile
- **description:** Force `find_hook_location` to fail (e.g. by patching the i2i stub bytes with a pattern that breaks the scanner, or via a build-time test hook). Call `hook<T>("foo", "(I)V", cb)`; assert it returns `false`, then read `_access_flags` on the target Method and assert `NO_COMPILE` is NOT set, AND `g_hooked_methods` does not contain an entry for that method. Validates the rollback-on-failure fix.
- **asserts:** install == false, *flags & NO_COMPILE == 0, no g_hooked_methods entry for found_method.
- **existing_file:** vmhook/src/example.cpp (new; needs test-only injection of find-hook-location failure)

### [standalone_unit] [new] test_descriptor_arg_count_matches_tuple_arity
- **description:** Pure constexpr test: assert a hypothetical `descriptor_arg_count<"()V">() == 0`, `descriptor_arg_count<"(I)V">() == 1`, `descriptor_arg_count<"(IJ)V">() == 2` (J is one ARG even though it is 2 slots), `descriptor_arg_count<"(Ljava/lang/String;[I)V">() == 2`. Only valid once the parser exists; pairs with the M improvement.
- **asserts:** static_assert chains on every supported primitive + L + [ + nested arrays.
- **existing_file:** tests/test_traits.cpp (extend_existing — file is the right home for compile-time API checks)

### [standalone_unit] [new] test_hook_no_jvm_returns_false_on_signature_overload
- **description:** Extend test_api_surface.cpp `exercise_hooks` to call BOTH `vmhook::hook<my_class>("addScore", cb)` AND `vmhook::hook<my_class>("addScore", "(I)I", cb)`. Confirms the explicit-signature overload is callable in the no-JVM safe-default mode and returns `false` without crashing. Cheap insurance against accidentally breaking the no-JVM surface when the install body is refactored.
- **asserts:** Both calls compile and return `false`.
- **existing_file:** tests/test_api_surface.cpp (extend_existing — already has exercise_hooks at line 31)

## Parity Concerns
- The empty-signature `hook<T>(name, cb)` overload (line 7693-7698) thin-wraps the signature overload by passing `std::string_view{}`. Good. `scoped_hook` has the same pair (8548-8646). The bare `hook<T>` overload in `tests/test_api_surface.cpp` uses the no-signature form; nothing in `test_api_surface` exercises the explicit-signature path even at compile time. Adds risk that a future template-argument-deduction change breaks the new overload silently in the CI matrix that uses `test_api_surface` as its no-JVM smoke test.
- `on_class_loaded` (line 15200-15203) and `on_exception` (line 15357-15360) are the only in-header consumers of the explicit-signature overload. Both hard-code one specific overload of multi-overload methods (`defineClass` x5, `fillInStackTrace`). Documented in their respective doc comments. No parity issue, but it means the in-tree usage demonstrates only one descriptor shape; tests should cover more shapes (primitives, primitive arrays, objects, long/double slot widths) to keep the explicit-signature path's slot-offset math honest.
- The drift-recovery path's `try_reinstall` at lines 8064-8145 has its own duplicate of the name+signature matching loop. Same code shape as `hook<T>` and `scoped_hook`'s, but with `expected_signature` instead of the user-supplied filter. Worth folding all three into a single `find_method_on_klass` helper (see the S internal improvement above) so future signature-resolution fixes land in one place.
