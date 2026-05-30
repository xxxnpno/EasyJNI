# return_value_set_arg_string_leak_fix

## Summary
The leak fix is correct on every path actually exercised by the std::string / string_view / const char* / char* overloads: the jstring handle returned by `jni_new_string_utf` is released via `jni_delete_local_ref` on success, on `store_oop` completion, and on the OOP-decode failure path. However, the surrounding logic still has two real defects: (1) when `NewStringUTF` returns null it leaves a JNI pending OutOfMemoryError that is never cleared before falling through to `make_java_string` / before returning to the interpreted method, and (2) the OOP-decode failure path (handle exists but `jni_decode_object` returns null) jumps straight to error logging instead of trying the `make_java_string` fallback, even though that fallback exists for exactly this kind of degraded JNI state.

## Bugs

### [medium] NewStringUTF failure leaves a pending JNI exception that poisons the rest of the detour
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:7587-7622
- **description:** `JNIEnv::NewStringUTF` is documented to throw `OutOfMemoryError` when the C string cannot be allocated, returning null in that case. The two string branches handle the null return by falling through to `vmhook::make_java_string`, but neither branch calls `vmhook::detail::jni_exception_clear()`. The OOM remains pending on the thread; the resumed interpreted method will observe it instead of whatever exception the original Java callee intended, and any later JNI call inside the same hook detour (e.g. another `set_arg`, a `make_java_string` -> internal JNI use, or any user-side JNI work) will misbehave. The rest of the codebase already clears after potentially-throwing JNI calls (`jni_get_method_id` at 8874/8880, `jni_exception_clear` everywhere after `find_class`/`call_*`), so this site is an outlier.
- **repro:** Force `NewStringUTF` to fail (e.g. supply a string that exceeds `JNI_MAX_STRING_LENGTH` or run under a JNI fault-injection layer), call `retval.set_arg(1, std::string{ /*huge*/ })` from a detour, observe that the next Java statement throws `OutOfMemoryError` even when `set_arg` reported success via the `make_java_string` fallback.
- **suggested_fix:** Immediately after the `jni_new_string_utf` call site, add `if (!string_handle) { vmhook::detail::jni_exception_clear(); }` (or, equivalently, always call `jni_exception_clear()` once before returning when the JNI fast path failed). Do this for BOTH the `std::string/string_view` branch and the `const char*/char*` branch.
- **confidence:** likely

### [low] OOP-decode failure on a valid handle never falls back to make_java_string
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:7587-7598, 7607-7618
- **description:** The fallback is selected by `string_handle ? jni_decode_object(string_handle) : make_java_string(value)`. If `NewStringUTF` returns a non-null handle but the handle's underlying OOP is unreadable (e.g. `is_valid_pointer` rejects it inside `jni_decode_object`), the conditional has already committed to the JNI path. `string_oop` ends up null and the function bails with a "both ... failed" error message that is misleading — `make_java_string` was never actually attempted on this path. The fallback exists precisely for the degraded JNI cases the audit comment describes ("HotSpot's default ref table capacity is 16 ... ref-table-overflow warnings"), so missing it for this sibling failure is inconsistent.
- **repro:** Pointer-validation hardening rejects the freshly allocated jstring (rare under healthy JVMs, but observable in early bootstrap or under colored-pointer GCs where `is_valid_pointer` filters more aggressively). The call fails with the misleading log even though `make_java_string` would have succeeded.
- **suggested_fix:** Restructure as `void* string_oop = string_handle ? jni_decode_object(string_handle) : nullptr; if (!string_oop) { string_oop = vmhook::make_java_string(value); }`. Keep the existing `jni_delete_local_ref(string_handle)` on both branches (it is a no-op for null). Update the error message to reflect that both paths really were tried.
- **confidence:** certain

### [low] Silent truncation parity between the JNI path and the make_java_string fallback
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:7587-7622, 10625
- **description:** `make_java_string` caps the produced Java string at 4096 chars (line 10625: `std::min<std::size_t>(value.size(), 4096u)`). `jni_new_string_utf` (line 9362-9374) imposes no such cap. Two `set_arg` calls with the same long string therefore inject different content into the interpreter local depending on which path won — JNI fast path stores the full string, fallback stores a silently truncated one. The caller has no way to detect the truncation.
- **repro:** In an environment where the JNI fast path fails (e.g. detour ran before JNIEnv attach), call `retval.set_arg(idx, std::string(5000, 'x'))`. The local now holds a 4096-char Java string instead of 5000.
- **suggested_fix:** Either (a) make `make_java_string` log a warning and fail when `value.size() > 4096`, returning null so `set_arg` can surface a clear "string too long for fallback path" error, or (b) lift the 4096 cap in `make_java_string` to match the JNI side. (a) is the safer change and aligns with the existing error-on-degradation pattern.
- **confidence:** certain

## Improvements

### [S] [INTERNAL] Collapse the std::string and const char* branches via a shared helper
- **rationale:** Lines 7577-7603 and 7604-7622 are byte-for-byte duplicates aside from the input-to-string_view adapter. Any future tweak (the bug fixes above; adding `wchar_t*` support; switching `store_oop` to a result-returning variant) must be applied in two places, and the duplication is precisely the kind of asymmetry that leaked once already (the v0.4.0 fix updated one branch and forgot the other).
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:7577-7622
- **suggested_change:** Extract a local lambda `inject_java_string = [&](std::string_view text) -> bool { ... }` (with the NewStringUTF/decode/fallback/store/release/exception-clear flow). The two `if constexpr` branches become single calls: `return inject_java_string(value);` and `return inject_java_string(value ? std::string_view{ value } : std::string_view{});`.

### [XS] [USER_FACING] Tighten the "both ... failed" log to include the underlying cause
- **rationale:** The current error messages at 7594-7596 and 7614-7616 say "both JNI NewStringUTF and make_java_string fallback failed" but don't distinguish between (a) NewStringUTF returned null, (b) jni_decode_object rejected the OOP, (c) make_java_string returned null because the String klass is missing, (d) make_java_string failed during byte[]/char[] allocation. When a user reports an injection failure, the only signal is "string injection failed" with no actionable next step.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:7594-7596, 7614-7616
- **suggested_change:** Record the intermediate failures (`string_handle == nullptr`, `decoded == nullptr`, `fallback == nullptr`) in locals and emit them in the log line, e.g. `"newstringutf={}, decode={}, fallback={}"`. Combined with the bug-#2 fallback-symmetry fix this lets users tell "JNI was unavailable" apart from "String klass was missing".

### [XS] [USER_FACING] Add `std::wstring` / `std::u16string` overloads or a clear compile-time diagnostic
- **rationale:** Today, `retval.set_arg(idx, std::wstring{...})` silently falls into the `is_trivially_copyable_v` arm (wstring isn't trivially copyable) -> hits the unsupported-type branch at 7631-7637 with a `typeid().name()` log line. For Windows-side users the natural ask is "Java String from wide chars". Either a real overload or a `static_assert` "use std::string with UTF-8" would be friendlier than the runtime log.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:7577-7637
- **suggested_change:** Either (a) add `std::is_same_v<clean_value_type, std::wstring> || std::is_same_v<clean_value_type, std::u16string> || std::is_same_v<clean_value_type, std::u8string>` branches that transcode to UTF-8 then delegate to the string helper, or (b) add a `static_assert(!std::disjunction_v<std::is_same<clean_value_type, std::wstring>, ...>, "convert to UTF-8 std::string first")` in the final `else` branch.

### [XS] [INTERNAL] `value` can be a forwarding reference but is never moved into NewStringUTF
- **rationale:** `value_type&&` is forwarded as `value` (lvalue) into `jni_new_string_utf`, which internally copies into a `std::string` to obtain a null-terminated buffer (line 9372). For an rvalue `std::string` input we could `std::move` into a local `std::string`, get its `c_str()` once, and avoid the copy.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:7587, 9362-9374
- **suggested_change:** Have `jni_new_string_utf` accept a perfect-forwarded callable or take a `const char*` plus a length, and let the caller construct a single null-terminated buffer (using `value.c_str()` directly when `clean_value_type == std::string`). Minor unless the hook is on a very hot path.

## Tests

### [standalone_unit] [extend_existing] test_return_value_set_arg_string_branch_no_jvm
- **description:** Today `test_return_value_set_arg_guards` exercises the early-exit branches (no frame / negative / above max_locals). It does not touch the string branches at all. Add a no-JVM unit test that constructs a `return_value` with a non-null fake frame whose `get_locals()` returns null, calls `set_arg` with a `std::string`, a `std::string_view`, a `const char*` and a `char*`, and verifies the call returns false without crashing AND without leaking (we can't observe the local-ref table without a JVM, but we can at least observe no fault).
- **asserts:** `set_arg(0, std::string{"x"}) == false`; same for `string_view`, `const char*` (both null and non-null), `char*`. No crash, no signal raised by sanitisers.
- **existing_file:** tests/test_helpers.cpp (extend `test_return_value_set_arg_guards`)

### [standalone_unit] [new] test_return_value_set_arg_const_char_nullptr_no_jvm
- **description:** Verify the explicit `value ? std::string_view{ value } : std::string_view{}` adapter at line 7606 handles a null `const char*` without UB. With no JVM the call returns false, but the empty-string_view fallback path must run cleanly.
- **asserts:** `set_arg(0, static_cast<const char*>(nullptr)) == false` and no crash.
- **existing_file:** tests/test_helpers.cpp (new sibling to `test_return_value_set_arg_guards`)

### [jvm_integration] [exists] string_arg_mutation
- **description:** `vmhook/src/example.cpp:2328-2362` already exercises the std::string_view happy path against a live JVM (`nonStaticStringArgMutationMe`). Confirms the fix works end-to-end for one input flavour.
- **asserts:** `stringArgMutationSetArgOk == true`, `stringArgMutationOriginalSawReplacement == "after"`.
- **existing_file:** vmhook/src/example.cpp

### [jvm_integration] [new] test_string_arg_mutation_const_char
- **description:** Mirror of `test_string_arg_mutation` but passes `retval.set_arg(1, "after")` (a `const char[6]` that decays to `const char*`) instead of `std::string_view`. The `const char*` overload is currently uncovered by integration tests despite being a documented part of the API surface.
- **asserts:** Same observables as `test_string_arg_mutation`.
- **existing_file:** vmhook/src/example.cpp (extend with a new test function and wire it into the runner)

### [jvm_integration] [new] test_string_arg_mutation_local_ref_pressure
- **description:** Long-running regression for the actual leak the fix targets. Install a hook on a string-argument Java method, invoke it 10_000+ times from Java, and verify (a) no "local reference table overflow" warning appears on stderr, (b) the JVM hasn't grown a multi-MB JNI handle table (HSDB or `-Xcheck:jni`). Without the fix, this would saturate the 16-slot default ref table on the second invocation and either trigger the warning or throw an internal error.
- **asserts:** Hook invocation count reaches the target with no JNI ref-table overflow.
- **existing_file:** vmhook/src/example.cpp (new test alongside `test_string_arg_mutation`)

### [jvm_integration] [new] test_set_arg_string_oversized_truncation_parity
- **description:** Regression for the truncation parity issue. Inject a 5000-character string via `set_arg`. Read the value back from Java. Assert the Java side sees all 5000 characters (i.e. either the JNI path won or the fallback truncation is removed). Currently this would fail when the fallback path runs.
- **asserts:** Java-side string length == 5000.
- **existing_file:** vmhook/src/example.cpp (new test)

### [standalone_unit] [new] test_jni_exception_pending_after_failed_newstringutf
- **description:** Synthetic test that wires a stub `JNIEnv` whose `NewStringUTF` slot returns null while leaving a sentinel in the "pending exception" slot, then calls `set_arg` with a `std::string`. After the call, verify the pending-exception slot is cleared. Covers bug #1 above.
- **asserts:** Pending exception slot is zero after `set_arg` returns.
- **existing_file:** none — would need a new fixture (or skip if a stub JNIEnv is too heavy in unit-test land; in that case prefer the integration variant: force OOM via a stress test with `-Xmx8m` and confirm the next Java line throws the intended exception, not `OutOfMemoryError`).

## Parity Concerns
- `set_arg` accepts `std::string` / `std::string_view` / `const char*` / `char*` but the sibling `vmhook::return_value::set` (used to override return values) goes through a different code path that may handle strings differently. Worth cross-checking that `set` also calls `jni_delete_local_ref` after stashing the OOP — the CHANGELOG entry mentions only `set_arg`, so `set` may still leak if it follows the old pattern.
- `method_proxy::call_jni` at lines 9540-9684 also uses `jni_new_string_utf` for string arguments and delegates cleanup to a `local_ref_bag`. The two patterns (RAII bag vs. explicit `jni_delete_local_ref` calls) should be reconciled — the `set_arg` site is the last place using the explicit pattern; pulling it into a `local_ref_bag` would make a future "forgot to release" regression impossible.
- The 4096-char truncation cap in `make_java_string` (line 10625) is inherited by any code that uses it as a fallback. `set_arg` is one such caller; check whether `method_proxy::call_jni` similarly bottoms out at `make_java_string` and would benefit from the same truncation-or-fail change.
