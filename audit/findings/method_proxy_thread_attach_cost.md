# method_proxy_thread_attach_cost

## Summary
Audited the thread attach probe in `method_proxy::call_jni` (vmhook.hpp:11525-12075) plus the supporting TLS state (`current_jni_env` at vmhook.hpp:3843), `attach_current_native_thread` (3970-4041) and `ensure_current_java_thread` (4058-4101). The hot-path attach test is correct and cheap — it is a single `thread_local void*` read at line 11529 with no per-call `AttachCurrentThread`. The README/CHANGELOG claim ("residual ~1.5x gap = type-safe variant + thread-local attach probe") is accurate, but there are two latent issues: `call_jni` itself never calls `ensure_current_java_thread`, so the FIRST call from an unattached thread silently fails with a log line that misleadingly suggests the user forgot to attach (when actually the only caller `call()` *does* attach, but a direct `call_jni()` invocation does not); and on Windows clang/MSVC, `thread_local` resolves through `__tls_get_addr` style helpers unless `inline` is paired with a `constinit`-style init, which we could reinforce. Other than that the feature is in good shape.

## Bugs

### [medium] call_jni does not attach the thread; first-call failure is silent on direct use
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:11525-11536
- **description:** `call_jni` reads `vmhook::hotspot::current_jni_env` and bails with a log + `monostate` if it is null. The TLS variable is only ever populated as a side effect of `ensure_current_java_thread()` / `attach_current_native_thread()`. The wrapping `call()` (vmhook.hpp:12125, 12138) does the attach itself before falling through to `call_jni`, but `call_jni` is a `public` member — any caller that invokes it directly (e.g. `proxy.call_jni(args...)` from an injected worker thread that has not yet been attached) hits the `if (!env_void)` early-out on the very first invocation. The diagnostic message ("have we attached the calling thread?") points the finger at the user when the real situation is that this entry point intentionally skips the attach for speed. Either `call_jni` should perform a one-shot `ensure_current_java_thread()` when `env_void` is null (matching the contract every other `vmhook::detail::jni_*` helper has via `ensure_current_java_thread`), or `call_jni` should be marked private / renamed `call_jni_attached_fast` to make the precondition explicit.
- **repro:** From a freshly-attached injector thread that has NOT yet called `vmhook::hotspot::ensure_current_java_thread()`, invoke `method_proxy::call_jni(args...)` directly. Returns `monostate` and logs "current_jni_env is null"; subsequent calls on the same thread that do attach will succeed.
- **suggested_fix:** Add a one-shot attach at the top of `call_jni`:
  ```cpp
  auto* env_void{ vmhook::hotspot::current_jni_env };
  if (!env_void)
  {
      if (vmhook::hotspot::ensure_current_java_thread())
      {
          env_void = vmhook::hotspot::current_jni_env;
      }
      if (!env_void)
      {
          VMHOOK_LOG(...);
          return value_t{ std::monostate{} };
      }
  }
  ```
  The probe cost is still a single TLS read on the steady-state hot path (the `if (!env_void)` predicts not-taken after the first iteration). This brings `call_jni` to parity with every other JNI helper and with `call()`'s own behaviour.
- **confidence:** likely

### [low] ensure_current_java_thread skips JNIEnv repair when JavaThread cache survives a detach
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:4058-4069
- **description:** The fast path
  ```cpp
  if (vmhook::hotspot::current_java_thread && is_valid_pointer(current_java_thread))
  {
      if (!vmhook::hotspot::current_jni_env)
      {
          attach_current_native_thread();
      }
      return true;
  }
  ```
  re-attaches when `current_jni_env` is null but the JavaThread* still looks valid. The reverse case — `current_jni_env` is stale (points at a freed JNIEnv after a manual `DetachCurrentThread`, or after a thread was re-used by another JVM via JavaVM teardown/restart) — is not checked. The fast-return then hands `call_jni` a stale JNIEnv* and the next JNI dispatch crashes inside the JVM. This is unlikely in normal use (vmhook never detaches), but a host application that calls `DetachCurrentThread` on its own threads (or unloads/reloads a JVM in the same process) will trip it.
- **repro:** Attach a thread, drive one `call()`. Then call the host JVM's `DetachCurrentThread` on that thread directly. Drive a second `call()` — the fast path returns true with a now-dangling `current_jni_env`, and the next JNI vtable dispatch faults.
- **suggested_fix:** When restoring `current_jni_env` is needed, also clear `current_jni_env` first so the GetEnv path inside `attach_current_native_thread` re-detects a stale JNIEnv. Better: in the fast path, validate the JNIEnv via a cheap `GetVersion`-style sanity probe before trusting it, OR document explicitly that vmhook owns the TLS and host code must never call `DetachCurrentThread` on a vmhook-attached thread.
- **confidence:** speculative

## Improvements

### [S] [INTERNAL] Hoist single TLS read so the per-call dispatch only touches it once
- **rationale:** On Windows/MSVC, `thread_local` on a non-`inline` variable goes through a tls-init helper (`_Init_thread_header`/`_tls_get_addr`) on first access in each function. The header marks `current_jni_env` as `inline thread_local void*` (vmhook.hpp:3843), which is the right choice, but the CHANGELOG explicitly identifies the TLS read as part of the residual ~1.5x gap. The variable is read once at the top of `call_jni` (line 11529), then bound to the local `env_void`, which is the optimal pattern — confirm with a comment so a future contributor does not "simplify" by repeating `vmhook::hotspot::current_jni_env` inline at the call sites.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:11529, 11729, 12044-12053
- **suggested_change:** Add a one-line comment at line 11529 explaining that `env_void` is the single-touch alias for the TLS slot, and audit `vmhook::detail::jni_*` helpers (vmhook.hpp:8751, 8830-8833, 8852-8853, 8876-8877, etc.) — they each re-read `vmhook::hotspot::current_jni_env` 2-4 times per call, which IS a multiplier on hot paths that use them. Either hoist into a local at the helper entry, or take `env` as an explicit parameter from `call_jni`.

### [S] [USER_FACING] Surface "thread not attached" with the same wording across call() and call_jni()
- **rationale:** `call()` logs "no current JavaThread" (line 12140) and "call_stub_entry missing AND ensure_current_java_thread() failed" (line 12127). `call_jni()` logs "current_jni_env is null - have we attached the calling thread?" (line 11532). The three messages describe overlapping failures with different vocabulary, which makes log triage harder. Unify to one phrase: `"thread is not attached to the JVM (call ensure_current_java_thread() first, or invoke via method_proxy::call() which attaches automatically)"`.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:11532-11534, 12127-12129, 12140
- **suggested_change:** Define a `constexpr` log helper or reuse a single literal so the diagnostic text matches across all three sites.

### [S] [INTERNAL] Document the public/private contract of call_jni vs call
- **rationale:** As noted in the Bug section, `call_jni` is public but assumes the thread is already attached, while `call()` does the attach itself. Without a doc-comment line explicitly saying "call_jni() does NOT attach; use call() instead unless you have already called ensure_current_java_thread()", every user of the public method has a footgun.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:11510-11525
- **suggested_change:** Add to the existing doc-comment:
  ```
  @pre  vmhook::hotspot::current_jni_env must be set on the calling
        thread (i.e. ensure_current_java_thread() must have succeeded
        previously on this thread).  call() satisfies this precondition
        automatically; direct callers must too.
  ```

### [XS] [INTERNAL] Mark hot fast-path early-out with [[likely]] / [[unlikely]]
- **rationale:** In `call_jni` the `if (!env_void)` is the cold path (almost always env_void is set on the hot loop). Same for `if (!this->cached_method_id)` — taken once, false thereafter. C++20 `[[unlikely]]` lets MSVC and clang lay out the warm path linearly. Negligible cost, free perf win on the residual gap the CHANGELOG calls out.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:11530, 11587, 11566
- **suggested_change:** `if (!env_void) [[unlikely]] { ... }` and `if (!this->cached_method_id) [[unlikely]] { ... }`.

### [M] [INTERNAL] Cache JNIEnv* on the proxy after first successful call_jni
- **rationale:** The `current_jni_env` TLS load is the residual gap. For a proxy that only ever gets dispatched from ONE thread (the common case for hook detours and for a single-threaded bench), we could cache the JNIEnv* on the proxy alongside `cached_method_id` and avoid the TLS read entirely on subsequent calls from the same thread. We would still need to validate-on-mismatch: store `std::pair<thread_id, JNIEnv*>` and only trust the cache when `os::current_thread_id() == cached_tid`. That check is a register read; the TLS slot access is at least a `mov fs:[off]` plus an init-once dance on some toolchains. Worth experimenting if the speedtest still shows >1.2x.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:11529, 12656-12667
- **suggested_change:** Add `mutable std::uint64_t cached_jni_tid{0}; mutable void* cached_jni_env{nullptr};` and short-circuit the TLS read when `cached_jni_tid == vmhook::os::current_thread_id()`.

## Tests

### [standalone_unit] [new] test_call_jni_attach_precondition_logs_clearly
- **description:** In a no-JVM unit test, construct a `method_proxy` with a stub method pointer and call `call_jni(42)` while `vmhook::hotspot::current_jni_env == nullptr`. Verify `value_t::data` is `monostate`. Capture the log buffer (or set `VMHOOK_LOG_FILE`) and assert the diagnostic mentions thread-not-attached.
- **asserts:** return is monostate; log line contains "attach"
- **existing_file:** tests/test_helpers.cpp (sibling tests already validate jni_function returns null when env is null at line 1195-1206; extend pattern here)

### [standalone_unit] [new] test_thread_local_current_jni_env_is_per_thread
- **description:** Spin two `std::thread`s, set `vmhook::hotspot::current_jni_env` to two different fake pointers in each (e.g. `0xdead0001` and `0xdead0002`), and verify each thread reads back its own value. Confirms the `thread_local` declaration is actually per-thread on the toolchain in use (would catch a regression where someone accidentally drops `thread_local`).
- **asserts:** thread A sees 0xdead0001; thread B sees 0xdead0002; both threads see nullptr after join + main-thread reset
- **existing_file:** none — new test file `tests/test_thread_local_attach_state.cpp`

### [jvm_integration] [extend_existing] test_speedtest_logs_attached_ratio_under_threshold
- **description:** `vmhook/src/speedtest.cpp` already prints the vmhook/JNI ratio. Add an assertion (or write the ratio to a known file) so CI flags a regression if the ratio creeps back above ~2.0x. This protects the "~1.5x residual" claim the CHANGELOG makes — if a future change re-introduces a per-call attach, the ratio jumps.
- **asserts:** ratio < 2.0 (with a generous margin to avoid CI flakiness)
- **existing_file:** vmhook/src/speedtest.cpp

### [jvm_integration] [new] test_call_jni_from_unattached_native_thread
- **description:** Spawn a fresh `std::thread` that has never been registered with the JVM, hand it a `method_proxy` for a static method, and call `call_jni()` (NOT `call()`). Today: silently fails with the misleading log. With the fix proposed in the Bug section: succeeds via auto-attach. This is the regression test for the proposed bug fix.
- **asserts:** call_jni returns the expected value (e.g. `42` for `staticReturnMe(40)`)
- **existing_file:** none — new fixture under `example/vmhook/`

## Parity Concerns
- `field_proxy::get` / `field_proxy::set` (audited in sibling reports) call `ensure_current_java_thread` themselves when they need JNI; `method_proxy::call_jni` does not. Direct callers of `call_jni()` have a different mental model than direct callers of `field_proxy::*`. Fixing the bug above brings them to parity.
- The `vmhook::detail::jni_*` family of helpers (vmhook.hpp:8783-9000) all re-read `vmhook::hotspot::current_jni_env` 2-4 times each, while `method_proxy::call_jni` hoists it into `env_void` once. The improvement above suggests unifying that pattern across the JNI helpers so the per-call TLS cost is a single touch globally, not a per-helper one.
- `make_java_object` (vmhook.hpp:10444) and `make_java_string` (called from `call()` at vmhook.hpp:12186) both invoke `ensure_current_java_thread` explicitly. `call_jni` should do the same — the inconsistency is the root cause of the bug above.
