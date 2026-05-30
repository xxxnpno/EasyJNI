# method_proxy_call_return_string

## Summary
Audited `method_proxy::call_jni()` String-return path (vmhook.hpp:11525-12075) plus the surrounding `call()` dispatcher (12106-12293), `jni_get_string_utf()` (9389-9419), and the RAII `string_handle_cleanup` (11710-11727). The 0.4.3 leak fix for the result-handle local-ref is correct in `call_jni`, but the sibling `call()` call-stub path silently returns a TRUNCATED 32-bit OOP handle for every `Ljava/lang/String;`-returning method instead of extracting UTF-8 — so on JDKs where `_call_stub_entry` is exposed (JDK 8/11/17 typically) `get_method("foo")->call()` to `std::string` returns `""` regardless of the Java return value. Secondary concerns: a `bool*` vs `jboolean*` ABI mismatch in `jni_get_string_utf`, no caching of the "is-string-return" signature decision in the hot path, and no test coverage at all for the String-return code path.

## Bugs

### [critical] call_stub path drops String returns; `get_method("...")->call()` returning `Ljava/lang/String;` silently yields ""
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:12269-12292
- **description:** When `find_call_stub_entry()` returns non-null (JDK 8/11/17 normal case), `call()` routes through the call_stub fast path. The trailing `switch (ret_char)` only special-cases primitives + `V`; every reference/array return (`L`, `[`) falls into `default: return value_t{ static_cast<std::uint32_t>(result_holder) };`. That throws away the upper 32 bits of the heap pointer AND never invokes `GetStringUTFChars`, so `value_t` ends up holding a `uint32_t` alternative. When the caller assigns to `std::string` the templated `operator target_type<std::string>` static_visits, `static_cast<std::string>(uint32_t)` is ill-formed, the `requires` arm fails, and `target_type{}` returns an empty string. Net effect: `dog->speak()` returns `""` on a healthy JDK 8/11/17 attach even though the same call via the JNI fallback (JDK 21+, where the call_stub is missing) correctly returns the UTF-8 text — the same wrapper line works on one JDK and silently breaks on another.
- **repro:** Attach to JDK 17, call `get_method("toString")->call()` on any Object; expect non-empty string, actually get `""`. Or run the existing `test_interface_and_polymorphism` (example.cpp:2485) on JDK 8/11/17 — it will fail the `animalSpeakContainsWoof` assertion. The reason the test currently appears to pass is that the CI host happens to have `_call_stub_entry` missing from VMStructs (forcing the JNI fallback), masking the bug.
- **suggested_fix:** Add a String-special case to the call_stub path mirroring `call_jni`. After `case 'D':` and before `case 'V':`, insert a `case 'L':` arm that wraps `reinterpret_cast<void*>(result_holder)` as a synthetic jstring handle (via `jni_oop_handle`), calls `jni_get_string_utf()` when `signature.substr(rparen+1) == "Ljava/lang/String;"`, returns `value_t{ std::string }`, else falls through to the truncated-uint32 default. Cache an `is_string_return` bool on the proxy on first computation. Crucially, do NOT call `jni_delete_local_ref` here — the OOP went through the stub frame and is NOT a JNI local ref.
- **confidence:** certain

### [medium] `jni_get_string_utf` declares `jboolean*` parameter as `bool*` — latent ABI mismatch
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:9397-9407
- **description:** `get_string_utf_chars_t` is typed `const char*(*)(void*, void*, bool*)` and `&is_copy` is a `bool*`. The JNI spec says the third argument is `jboolean*` which is `unsigned char*` (always 1 byte). On every supported toolchain `sizeof(bool) == 1` so this happens to work, but `bool*` and `unsigned char*` are not aliased per the C++ object model — the callee writes a `jboolean` (uint8_t) through a `bool*` lvalue. If a future MSVC/Clang upgrade enables `-fsanitize=alignment`/`-fno-strict-aliasing=off` it would flag this. Also: in the sister call site at line 11808 (`get_string_utf_chars_t = const char*(*)(void*, void*, std::uint8_t*)`) the same function is correctly typed as `std::uint8_t*`. Two declarations of the same JNI slot disagree.
- **repro:** Static analysis / UBSAN with strict aliasing finds the alias violation; on platforms where `bool` is somehow >1 byte (none of the supported tier-1 hosts but exotic ABIs exist) the callee writes 1 byte into a 2+ byte slot, leaving the other bytes garbage and a subsequent read of `is_copy` indeterminate.
- **suggested_fix:** Change line 9397 to `using get_string_utf_chars_t = const char* (*)(void*, void*, std::uint8_t*);` and pass `std::uint8_t is_copy{}; (void)is_copy;` — matching the declaration used inside `call_jni`'s `check_callee_exception` lambda. Alternatively, simply pass `nullptr` for the is_copy out-param like the lambda at vmhook.hpp:11867 does — the value is never read here anyway.
- **confidence:** likely

### [low] Cannot distinguish "method returned Java `null`" from "method returned the empty string"
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:12041-12055
- **description:** When the Java method legitimately returns `null` (e.g. `String foo() { return null; }`), `result_handle` is `nullptr`, `jni_get_string_utf(nullptr)` returns `""`, and the proxy returns `value_t{ std::string{} }`. This is indistinguishable from a method that returned `new String("")`. Java programmers routinely treat `null` and `""` as semantically distinct (e.g. JSON serialization, equals checks); silently coercing one to the other means C++ callers cannot react. The contract documented at line 12039 ("when the method returns java.lang.String, return a std::string copy") doesn't mention the null collapse.
- **repro:** `static String returnNull() { return null; }` then `get_static_method("returnNull")->call()`. Expected: some way to detect null (e.g. monostate). Actual: `value_t{ "" }`.
- **suggested_fix:** When `result_handle` is null, short-circuit to `return value_t{ std::monostate{} };` before calling `jni_get_string_utf`. Then callers using `std::optional<std::string> = value_t.try_as<std::string>()` (or a documented `value_t::is_null()` predicate) can branch. Document the new contract in the call_jni doxygen and the `value_t` doc-comment.
- **confidence:** certain

## Improvements

### [S] [USER_FACING] Add `value_t::to_string()` convenience method (parity with field_proxy::value_t)
- **rationale:** Today users have to write `std::string s = get_method("speak")->call();` and trust the implicit conversion. There is no way to write `auto s = get_method("speak")->call().as_string();` and the variant alternative is private. Sibling `field_proxy::value_t` exposes a richer surface (e.g. `to_entries<K,V>()` per the comment at example.cpp:1099) — `method_proxy::value_t` is bare-bones in comparison. The user feedback called out method_proxy vs field_proxy parity gaps.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:11439-11495
- **suggested_change:** Add `auto as_string() const noexcept -> std::optional<std::string>` that returns the variant's std::string alternative if present, else `std::nullopt`. Also add `auto is_null() const noexcept -> bool` returning `std::holds_alternative<std::monostate>(data)`. These two methods together let users distinguish "method returned String" from "method returned monostate" without relying on the lossy implicit conversion.

### [S] [INTERNAL] Cache the "return type is `Ljava/lang/String;`" decision on the proxy
- **rationale:** Every Object-returning call recomputes `effective_signature.rfind(')')` plus the `substr(rparen+1) == "Ljava/lang/String;"` string comparison (line 12041-12046). The proxy already caches `cached_ret_char`; adding a `cached_is_string_return` bool flag costs one byte and saves a substring compare + an rfind per call in a tight detour loop. Document that 0=not-computed-yet via a tristate (or compute alongside ret_char in the same `if (!cached_ret_char)` block so they're always in sync).
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:12041-12055 / 12656-12667
- **suggested_change:** Add `mutable std::int8_t cached_is_string_return{ -1 };` next to `cached_ret_char`. In the `if (!ret_char)` block (line 11566) also set `cached_is_string_return = (rparen + ... == "Ljava/lang/String;") ? 1 : 0;`. Then `if (cached_is_string_return == 1)` replaces the per-call substring compare. Same trick removes the lazy `rparen` recompute at line 12041 entirely.

### [M] [USER_FACING] Distinguish "JNI failure" from "Java returned empty string" via diagnostic
- **rationale:** When `jni_get_string_utf` fails (GetStringUTFChars returned null because of OOM or because the OOP was already collected) the proxy silently returns `""` with no log. Combined with the null-vs-empty collapse above, three completely different failure modes ("Java returned null", "Java returned empty string", "JNI OOM") all surface as `value_t{ "" }`. Add a `VMHOOK_LOG` at line 9408-9410 (`if (!chars) return {};`) so at least the JNI-side failure is observable in logs.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:9408-9411
- **suggested_change:** Replace `if (!chars) { return {}; }` with `if (!chars) { VMHOOK_LOG("{} jni_get_string_utf: GetStringUTFChars returned null for jstring=0x{:016X} - probably OOM or invalid OOP.", vmhook::error_tag, reinterpret_cast<std::uintptr_t>(string_handle)); return {}; }`. Cheap, no API change, no perf impact on success path.

### [XS] [INTERNAL] String-detection comparison uses heavyweight `std::string_view` construction over a `std::string`
- **rationale:** Line 12046 constructs `std::string_view{ effective_signature }.substr(rparen + 1)` — but `effective_signature` is already a `std::string` (it's a reference at line 11553), so just call `effective_signature.compare(rparen + 1, std::string::npos, "Ljava/lang/String;") == 0` which avoids constructing a temporary string_view AND avoids the extra-substr allocation cost. Even better with the cached bool from improvement #2 above, this disappears entirely on the hot path.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:12045-12046
- **suggested_change:** `if (rparen != std::string::npos && effective_signature.compare(rparen + 1, std::string::npos, "Ljava/lang/String;") == 0)` — one fewer temporary, clearer intent.

### [M] [USER_FACING] `call()` doc-comment doesn't mention the String-return convenience; document the contract
- **rationale:** The `call_jni` doc-comment at line 11510-11524 mentions `CallObjectMethodA for methods returning java.lang.String`, but the public `call()` doc-comment at line 12088-12105 says nothing about return-type handling other than "value_t holding the Java return value". A user reading only `call()`'s doc has no way to know that String returns get an automatic UTF-8 conversion. Once the critical-bug fix above propagates the String-conversion to the call_stub path too, both paths' contract must be documented together so users can rely on `std::string s = get_method("toString")->call()` working uniformly.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:12088-12105
- **suggested_change:** Add a `@note Return types returning java.lang.String are automatically decoded via JNI GetStringUTFChars and exposed as std::string in value_t; assign to std::string and it works directly. Other reference types return a truncated handle and are typically only used to test non-null-ness.` line to the doc-comment.

## Tests

### [standalone_unit] [new] test_jni_get_string_utf_null_handle_returns_empty
- **description:** Sanity-check that `jni_get_string_utf(nullptr)` returns an empty string without faulting AND without attempting to call the JNI table (since there is no JVM). This exercises the null-guard at line 9392.
- **asserts:** `vmhook::detail::jni_get_string_utf(nullptr).empty()`; doesn't crash.
- **existing_file:** tests/test_helpers.cpp (extend the existing JNI helper section)

### [standalone_unit] [new] test_method_proxy_value_t_string_alternative_round_trip
- **description:** Build a `method_proxy::value_t` directly from a `std::string` (this is what the call_jni path emits), then exercise the `operator std::string()` and the proposed `as_string()`/`is_null()` accessors. Catches the case where someone reshuffles the variant alternatives and the std::string alternative gets reordered out of the convertible set.
- **asserts:** `value_t v{ std::string{"hello"} }; std::string{ v } == "hello"`; `v.is_null() == false`; `v.as_string().has_value() && *v.as_string() == "hello"`. Then `value_t empty{ std::monostate{} }; empty.is_null() == true; !empty.as_string().has_value();`.
- **existing_file:** tests/test_helpers.cpp (new test function)

### [standalone_unit] [new] test_method_proxy_value_t_implicit_conversion_to_string_from_uint32_returns_empty
- **description:** Regression test pinning the *current* lossy behavior of `operator target_type<std::string>` when the variant holds `uint32_t`. Once the critical bug fix lands, this test should be updated to expect a non-empty string for the call_stub path, but the variant-level conversion itself stays lossy. This test documents the implicit-conversion contract.
- **asserts:** `value_t v{ static_cast<std::uint32_t>(0xDEADBEEF) }; std::string{ v }.empty()`.
- **existing_file:** tests/test_helpers.cpp (new test function)

### [jvm_integration] [extend_existing] test_method_call_return_string_via_call_stub
- **description:** Currently the existing `test_interface_and_polymorphism` (example.cpp:2485) exercises `animal->speak()` returning std::string but relies on a single hook context. Add a dedicated test that calls a *static* method returning `Ljava/lang/String;` (e.g. `static String staticHello() { return "hi-mom"; }`) and asserts the C++ side gets `"hi-mom"`. Critical because the static path uses the pool_holder FindClass branch instead of GetObjectClass, and the String special-case must work on both. Run with `find_call_stub_entry` both available and forcibly disabled (set a test-only override that returns nullptr) to cover both branches of `call()`.
- **asserts:** `static_method("staticHello")->call() == std::string{"hi-mom"}` regardless of call-stub availability; identical std::string returned by both code paths; no JNI local-ref table overflow after 1000 repetitions in a tight loop (tests the 0.4.3 leak fix at scale).
- **existing_file:** vmhook/src/example.cpp, example/vmhook/Example.java

### [jvm_integration] [new] test_method_call_return_string_null_distinguishable_from_empty
- **description:** Add a Java method that returns `null` and another that returns `""`. From C++, call each and confirm we can tell them apart (requires either the `is_null()` accessor improvement above, or a documented contract that null collapses). At minimum the test pins the current behavior so a future change is intentional.
- **asserts:** After fix: `static_method("returnNullStr")->call().is_null() == true`; `static_method("returnEmptyStr")->call().is_null() == false && as_string() == ""`. Before fix (current): both return `""` (regression-pin only).
- **existing_file:** none — new test in vmhook/src/example.cpp + example/vmhook/Example.java

### [jvm_integration] [new] test_method_call_return_string_local_ref_leak_under_load
- **description:** Tight loop of 10000 String-returning calls against a single proxy, asserting that the JNI local-reference table never overflows (no `"JNI local reference table overflow"` warning in the log). Directly validates the 0.4.3 fix at scale and the proposed call_stub-path fix.
- **asserts:** No log entries containing `"local reference table overflow"`; returned std::string content is identical across all 10000 iterations.
- **existing_file:** vmhook/src/example.cpp

## Parity Concerns
- `field_proxy::value_t` exposes higher-level accessors like `to_entries<K,V>()` (per the comment at example.cpp:1099-1110) and `set()` size-validation (0.4.4); `method_proxy::value_t` has only the templated implicit conversion. No `as_string()`, no `as_optional<T>()`, no `is_null()`. Users reaching for parity with field-side ergonomics find none.
- `field_proxy` for `Ljava/lang/String;` fields works on every JDK (it uses the heap-OOP read path, not call_stub). `method_proxy::call()` for `Ljava/lang/String;` returns only works when `_call_stub_entry` is *missing* (forcing the JNI fallback). So the same wrapper-class line works for a String field but silently fails for a String method on the same JDK — confusing inconsistency for users wrapping POJOs with both String getters AND String fields.
- `call_jni` correctly logs every failure mode with `VMHOOK_LOG(error_tag, ...)`; the call_stub path in `call()` has the `default:` arm that silently truncates the handle with no log. Failure visibility is asymmetric between the two paths.
- The String-return fast-path in `call_jni` uses `jni_get_string_utf` + `jni_delete_local_ref` (correct: jstring local-ref). If the call_stub-path fix is implemented naively reusing this code, it must NOT call `jni_delete_local_ref` because the OOP returned by the stub is not a JNI local ref — it is a raw OOP in the result slot. Easy footgun if the fix is copy-pasted.
