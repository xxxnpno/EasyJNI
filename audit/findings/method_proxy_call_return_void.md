# method_proxy_call_return_void

## Summary
Audited the V (void) return path of `method_proxy::call()` (interpreter call_stub fast path at vmhook.hpp:12106-12293) and its JNI fallback (vmhook.hpp:11896-11943). The discard semantics are correct — `case 'V'` returns `value_t{ std::monostate{} }` without reading `result_holder`, and the call_stub still receives a valid result pointer plus T_VOID=14 so HotSpot's call_stub knows not to write anything. Top concern: the `<clinit>` re-route guard in `call_jni` requires `!is_static_call`, which means a `<clinit>` invocation (which is by definition static) silently falls through to `CallStaticVoidMethodA`, exactly the path the comment block calls out as forbidden. Second concern: the thread state is not RAII-protected around the call_stub call — any SEH/asynchronous exit leaves the JavaThread stuck in `_thread_in_Java`.

## Bugs

### [medium] `<clinit>` re-route guard is unreachable for static initialisers
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:11907-11928
- **description:** The comment at lines 11898-11906 explicitly states "Dispatch <init> / <clinit> through CallNonvirtualVoidMethodA … the documented way to invoke a constructor / static initialiser". However, the guard `is_init_call` requires `!is_static_call` (line 11909). A `<clinit>` is the class-static initialiser and `is_static_call` is `(this->object == nullptr)`, so a `<clinit>` proxy obtained via `static_method("<clinit>")` will have `is_static_call == true`, fail the guard, and fall through to `Call(Static)VoidMethodA` slot 143 — exactly the path the comment forbids. The `<init>` case in `is_init_call` is fine (constructors always have a receiver, so `is_static_call` is false there), but `<clinit>` is dead code.
- **repro:** Build a method_proxy for a class's `<clinit>` via the static_method path, then `proxy->call()` through the JNI fallback. The dispatch lands on slot 143 (CallStaticVoidMethodA), which per JNI spec is undefined behaviour for `<clinit>`; some JVMs silently no-op, others abort.
- **suggested_fix:** Either treat `<clinit>` separately (route it through CallStaticVoidMethodA explicitly with logging that the JVM has already run it during class init, since user-initiated `<clinit>` re-invocation is meaningless anyway), or strengthen the guard so `<clinit>` always goes through nonvirtual: `const bool is_init_call{ this->cached_class_handle && (method_name == "<init>" || method_name == "<clinit>") };` and pass `cached_class_handle` as the receiver when `is_static_call` is true. At minimum, the comment block lies relative to the code — pick one or the other.
- **confidence:** likely

### [medium] Thread state restoration is not RAII-protected around call_stub
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:12251-12267
- **description:** `set_thread_state(_thread_in_Java)` runs before the call_stub (line 12253) and `set_thread_state(previous_state)` only runs after a normal return (line 12267). The call_stub is a hand-written assembly trampoline that jumps to the interpreter entry — if the interpreter trips a SEH (uncaught Java exception promoted to a structured exception via vectored handler, stack overflow, divide-by-zero in JIT'd code, OS access violation in a faulting native method called from Java), control unwinds out of `call()` and the thread remains stuck in `_thread_in_Java`. The very next safepoint poll on that thread will assume Java semantics, walk the stack as Java frames, and either crash or deadlock the VM. This bug affects all return types, but the void path is the most likely user of "call a Java method for its side effect" in a detour — making it the most exposed to this hazard.
- **repro:** Hook a method, in the detour call `proxy->call(...)` where the callee triggers an OS exception (e.g. arithmetic in JIT code, or a null deref in a JNI downcall). The detour returns; the next GC pause walks the still-marked-as-`_thread_in_Java` thread and either reads bogus frame metadata or skips the safepoint entirely.
- **suggested_fix:** Wrap the transition in an RAII guard so the restore runs on any unwind path:
  ```cpp
  struct thread_state_guard {
      vmhook::hotspot::java_thread* t;
      vmhook::hotspot::java_thread_state prev;
      ~thread_state_guard() noexcept { t->set_thread_state(prev); }
  };
  const thread_state_guard guard{ thread, thread->get_thread_state() };
  thread->set_thread_state(vmhook::hotspot::java_thread_state::_thread_in_Java);
  ```
  Note this also fixes the subtle issue that if `selected_method`/`entry` validation between thread transitions ever evolves to early-return (it doesn't today, but it's a footgun for future edits).
- **confidence:** likely

### [low] Void path skips `check_callee_exception` parity with JNI void path
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:12255-12290
- **description:** When `call_stub` dispatches a void Java method that throws, the exception is left pending on the JavaThread. The JNI fallback's V branch (lines 11920, 11935) calls `check_callee_exception()` after each dispatch, logging the Throwable's toString and clearing the exception so subsequent JNI calls on the same detour thread don't get poisoned. The interpreter fast path does no equivalent — a thrown exception silently propagates as VM state. Users see "the call returned but nothing happened", same failure mode the JNI-side fix was written to defeat (see lines 11796-11800 comment). This affects all return types but is most visible for V where the caller has no return value to cross-check.
- **repro:** Inside a hook, `proxy->call(...)` on a void Java method that throws RuntimeException. The detour returns monostate; the next vmhook JNI helper (e.g. `make_java_string` on another arg) sees the pending exception and either silently fails or aborts.
- **suggested_fix:** After line 12264 (the call_stub call), check `current_jni_env`'s ExceptionCheck/ExceptionOccurred and surface via VMHOOK_LOG, then `ExceptionClear`. Reuse `check_callee_exception` from call_jni by hoisting it into a private member, or extract into a `detail::log_and_clear_pending_exception()` helper that both paths share.
- **confidence:** likely

## Improvements

### [S] [USER_FACING] Log when caller discards a non-void return as a void call
- **rationale:** Today `proxy->call(...)` with a non-V signature silently discards the return value if the caller writes `proxy->call(...);` instead of binding the value_t. Conversely, if the user binds `std::int32_t x = proxy->call();` on a V-signature method, value_t::operator returns a default-constructed 0 with no log line (lines 11470-11494) — the caller has no way to know the call had no return value vs returned zero. A one-line `[DEBUG]` log inside the variant visitor when the variant alternative is `monostate` and the target_type is non-void would catch a whole class of "I'm reading garbage" bugs.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:11470-11494, 12290
- **suggested_change:** In `value_t::operator target_type`, when `stored_type` is `std::monostate` and `target_type` is not `std::monostate`/`void`, emit a `VMHOOK_LOG` at debug level explaining the call returned void (or failed) and the caller is reading a default-constructed value. Cheap, no perf hit on success path.

### [XS] [USER_FACING] Document the void contract on `call()`
- **rationale:** The doc comment for `call()` at lines 12077-12105 mentions "value_t holding the Java return value, or monostate if the call gate is unavailable" but doesn't explicitly say monostate is *also* the success indicator for V-signature methods. A user reading the doc could reasonably write `if (!proxy->call(...).has_value()) { /* error */ }` (if such a method existed) and be wrong for void methods.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:12077-12105
- **suggested_change:** Add a sentence: "For methods whose JVM signature ends in 'V', the returned value_t always holds `std::monostate{}` on both success and failure — use the side effect or a try-catch in the Java callee to distinguish."

### [S] [INTERNAL] Drop the dead `result_holder` for V-only dispatch
- **rationale:** Lines 12251-12264 always allocate `result_holder` on the stack and always pass `&result_holder` to the call_stub. For V signatures the call_stub never writes to it. This is harmless (one stack slot) but it does mean someone reading the call site sees the address-of and may assume the V branch later reads it. Splitting the V dispatch to pass `nullptr` for the result pointer would let the switch on line 12269 collapse from 11 cases to 10 (V handled before the call) and make the contract explicit. (HotSpot's call_stub does accept a null result pointer when type is T_VOID — `stubGenerator_x86_64.cpp` only writes when `is_returning_basic_type`.)
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:12251-12290
- **suggested_change:** Early-return the V case before the call_stub block, OR pass `result_type == T_VOID ? nullptr : &result_holder`. Optional; current behaviour is correct.

### [S] [INTERNAL] Share the post-call exception-drain helper between fast path and JNI path
- **rationale:** See the "Void path skips check_callee_exception" bug above. Today `check_callee_exception` is a lambda local to `call_jni` (lines 11812-11892). Hoisting it to a private member function (or a free function in detail::) lets `call()` reuse it. Also helps the integer/string return paths in the call_stub fast path that have the same gap.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:11812-11892, 12264-12290
- **suggested_change:** Extract `check_callee_exception` to `detail::drain_and_log_pending_jni_exception(void* env, std::string_view ctx, std::string_view name, std::string_view sig)` and call it from both `call()` (after call_stub returns) and `call_jni` (every dispatch).

### [XS] [INTERNAL] Add `[[maybe_unused]]` to `result_holder` for clarity in V case
- **rationale:** Static analyzers will warn that `result_holder` is read for non-V cases only. Marking it `[[maybe_unused]]` documents the V path's discard.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:12251
- **suggested_change:** `[[maybe_unused]] std::intptr_t result_holder{ 0 };` (or just drop, see "Drop the dead `result_holder`" above).

## Tests

### [standalone_unit] [new] test_value_t_monostate_to_target_type_returns_default
- **description:** Construct `method_proxy::value_t{ std::monostate{} }`, implicitly convert to int / float / void*, verify defaults (0, 0.0f, nullptr) and that no UB occurs. Covers the discard contract end-to-end without needing a JVM.
- **asserts:** `static_cast<int>(value_t{ monostate{} }) == 0`, `static_cast<void*>(value_t{ monostate{} }) == nullptr`, `static_cast<float>(value_t{ monostate{} }) == 0.0f`, `static_cast<std::int64_t>(value_t{ monostate{} }) == 0`.

### [standalone_unit] [new] test_sig_char_to_basic_type_V_is_T_VOID_14
- **description:** Already covered at tests/test_helpers.cpp:243 (`sig_char_V_T_VOID`). Document the dependency; no new test needed.
- **asserts:** `sig_char_to_basic_type('V') == 14`.
- **existing_file:** tests/test_helpers.cpp

### [jvm_integration] [new] test_method_call_void_side_effect
- **description:** New JVM integration test mirroring `test_method_call_return_value` but for a void-returning Java method. Hook `nonStaticCallMe(int)` (already void at example/vmhook/Example.java:318), inside the detour invoke `nonStaticCallMe(99)` via `method_proxy::call()`, verify (a) the call_stub path returns `value_t{ monostate{} }` (when call_stub_entry is present) or falls back to JNI CallVoidMethodA, (b) the Java side observes the side effect (e.g. nonStaticCalled incremented), (c) no stale exception left on the JNI env, (d) the JavaThread state is back to `previous_state` after the call.
- **asserts:** `value_t.data.index() == 0` (monostate), Java-side `nonStaticCalled == expected_count + 1`, post-call `current_jni_env->ExceptionCheck() == JNI_FALSE`, `thread->get_thread_state() == previous_state`.
- **existing_file:** vmhook/src/example.cpp

### [jvm_integration] [extend_existing] test_method_call_void_static_path
- **description:** Extend the void integration test with a static-V variant. `staticCallMe(int)` is at example/vmhook/Example.java:317. Verifies the `is_static_call ? 143u : 63u` slot selection inside the JNI fallback (lines 11931) and that the call_stub fast path also handles `static_field=true` correctly (the receiver is not passed in `params[0]`, see lines 12172-12175).
- **asserts:** `staticCallMe` invocation observed at Java side; receiver slot not consumed; returns monostate.
- **existing_file:** vmhook/src/example.cpp

### [jvm_integration] [new] test_method_call_void_callee_throws_does_not_poison_thread
- **description:** Targets the "Void path skips check_callee_exception" bug. Hook a setup point, in the detour `proxy->call()` a void Java method that throws RuntimeException, then immediately attempt another vmhook JNI operation (e.g. `make_java_string("ok")` or get a field). Verify the second op either succeeds (because the exception was drained) or fails with a clean diagnostic — not a silent corruption.
- **asserts:** After the throwing void call, `make_java_string` returns non-null, AND `current_jni_env->ExceptionCheck() == JNI_FALSE` before the second op.

### [jvm_integration] [new] test_method_call_void_clinit_routes_through_nonvirtual
- **description:** Targets the `<clinit>` guard bug. Resolve a `method_proxy` for a class's `<clinit>` via the static_method path, call `proxy->call()`, verify the dispatch goes through `CallNonvirtualVoidMethodA` (slot 93) NOT `CallStaticVoidMethodA` (slot 143). May require a custom trace hook on the env's function table to observe which slot fires.
- **asserts:** `clinit_dispatch_slot_observed == 93`.

## Parity Concerns
- `method_proxy::call()` has no equivalent of `field_proxy::set`'s size-mismatch logging (vmhook.hpp:11241) — when the user binds a void return to a typed variable (e.g. `int x = proxy->call();` on V signature), the silent default-construction provides no feedback. field_proxy::set was specifically hardened to log this class of mistake; method_proxy::call has the same exposure with no parallel guard.
- The JNI fallback's V branch logs missing slots (lines 11924, 11939) and Java exceptions (line 11879), but the call_stub fast-path V branch has zero post-call diagnostics — neither "exception pending" nor "thread state recovered" telemetry. Sibling field_proxy::set logs all anomaly paths; method_proxy::call's fast path is conspicuously silent compared to its own JNI fallback.
- value_t.data carries 11 variant alternatives (line 11441) but the V path is the only one that produces `monostate`. Other failure paths (null method pointer, missing call stub, JVM not attached) ALSO produce `monostate`. Sibling pattern in field_proxy::get distinguishes "field not present" from "field present but zero" by returning `std::optional<T>`; method_proxy::call collapses both into the same monostate alternative, making error handling impossible without out-of-band signals.
- The void path's `is_init_call` guard at lines 11907-11912 is the only place in method_proxy that treats `<clinit>` specifically. `field_proxy::set` has no similar special-case for static fields written during class init, which is correct for fields (they're plain memory writes); the asymmetry is fine but worth a comment so future readers don't try to generalise.
