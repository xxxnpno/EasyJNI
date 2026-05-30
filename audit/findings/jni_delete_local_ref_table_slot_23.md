# jni_delete_local_ref_table_slot_23

## Summary
`vmhook::detail::jni_delete_local_ref` is a thin wrapper around `JNIEnv->DeleteLocalRef`
(JNI table slot 23). The declaration lives at vmhook/ext/vmhook/vmhook.hpp:7325 and
the implementation at vmhook/ext/vmhook/vmhook.hpp:8742-8756. It correctly guards
against null handles and a null `current_jni_env`, but it never calls
`ensure_current_java_thread()`, so on detour threads where no prior JNI helper has
been invoked the helper silently no-ops, re-introducing the very leak the v0.4.1
changelog entry says it fixed. The slot-23 magic number is also duplicated by
`method_proxy::call_jni`'s `check_callee_exception` (line 11919), which weakens the
"slot 23 is in one place" promise the doc comment makes.

## Bugs

### [HIGH] jni_delete_local_ref silently no-ops on detour threads that never attached to the JVM
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:8742-8756
- **description:**
  `jni_delete_local_ref` only looks up the function pointer via
  `jni_function<23, ...>(current_jni_env)`. If `current_jni_env` is null
  (`jni_function` early-returns at line 8696), the helper exits silently
  without releasing the handle. Every other JNI helper in this section
  (`jni_find_class` at line 8797, `jni_find_class_with_context_loader` at
  line 9034, `jni_call_object_method`, `jni_make_unique` at line 10099, etc.)
  first calls `vmhook::hotspot::ensure_current_java_thread()` which attaches
  the thread and populates `current_jni_env`. `jni_delete_local_ref` skips
  that step. That is fine when the *same* helper that produced the handle
  already attached the thread - but the symmetry breaks the moment the
  caller is on a detour thread that received a handle from a different
  helper (or a cached handle) and `current_jni_env` has been cleared
  (TLS reset across thread reuse, attach race) or was never set on that
  particular OS thread. The handle leaks and no diagnostic fires. This
  defeats the entire purpose of the v0.4.1 "Fixed" entry in CHANGELOG.md
  (lines 89-96), because the leak it claims to have fixed re-appears on
  any thread where set_arg is the *first* JNI helper run.
- **repro:**
  Construct an OS thread that has never run another JNI helper, manually
  call `jni_new_string_utf` (which DOES attach), squirrel the handle
  away, then on a *different* fresh OS thread that has not yet attached,
  call `jni_delete_local_ref(handle)`. The function returns without
  releasing the ref. Even without cross-thread setups: the very first
  detour invocation on a freshly created Forge worker thread that runs
  `set_arg(0, std::string)` will allocate a NewStringUTF handle (which
  attaches the thread inside `jni_new_string_utf`), then call
  `jni_delete_local_ref` - that path works. But if the same hook
  dispatcher arranges `set_arg` after some other early-return path that
  resets `current_jni_env = nullptr` (e.g. via `attach_current_native_thread`
  failure clearing it), the release silently leaks.
- **suggested_fix:**
  Add the same attach guard the sibling helpers use, so the contract is
  consistent and the release actually happens on cold detour threads:
  ```cpp
  inline auto jni_delete_local_ref(void* const object_handle) noexcept -> void
  {
      if (!object_handle) { return; }
      if (!vmhook::hotspot::ensure_current_java_thread()) { return; }  // NEW
      using delete_local_ref_t = void(*)(void*, void*);
      delete_local_ref_t const delete_local_ref{
          vmhook::detail::jni_function<23, delete_local_ref_t>(vmhook::hotspot::current_jni_env) };
      if (delete_local_ref)
      {
          delete_local_ref(vmhook::hotspot::current_jni_env, object_handle);
      }
  }
  ```
- **confidence:** likely

### [MEDIUM] Silent failure when JNI table slot 23 resolves to null is invisible to callers
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:8749-8755
- **description:**
  If `jni_function<23, ...>` returns nullptr (corrupted JNIEnv vtable, a
  custom JVMTI agent that has nulled the slot, or the rare scenario of a
  vmhook upgrade running against an exotic non-HotSpot JVM whose function
  table happens to have entry 23 zeroed for an unsupported op), the helper
  exits silently. Every neighbouring helper in this file (jni_find_class,
  jni_exception_clear, jni_get_object_class, etc.) similarly returns
  nullptr or no-ops, but they all return a non-void value the caller can
  test. This one returns `void`, so the caller has no way to learn the
  release didn't happen and can't fall back. A single VMHOOK_LOG line
  would convert "mysterious local-ref-table-overflow weeks later" into a
  pinpointable diagnostic.
- **repro:**
  Run any host where `current_jni_env` is set but `table[23]` is nullptr
  (e.g. a stripped/instrumented JNI vtable). The helper returns without
  any log entry, the handle leaks, and JNI eventually overflows with
  no breadcrumbs back to this site.
- **suggested_fix:**
  Add a one-line VMHOOK_LOG on the failure path:
  ```cpp
  if (delete_local_ref)
  {
      delete_local_ref(vmhook::hotspot::current_jni_env, object_handle);
  }
  else
  {
      VMHOOK_LOG("{} jni_delete_local_ref(0x{:016X}): JNIEnv table slot 23 "
                 "(DeleteLocalRef) is null - handle leaked.",
                 vmhook::error_tag,
                 reinterpret_cast<std::uintptr_t>(object_handle));
  }
  ```
- **confidence:** certain

### [LOW] Duplicate hard-coded slot 23 in call_jni::check_callee_exception bypasses the helper
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:11843, 11919-11924
- **description:**
  `method_proxy::call_jni::check_callee_exception` reads `table[23]` and
  calls `delete_local_ref(env_void, ...)` directly instead of going
  through the wrapped `vmhook::detail::jni_delete_local_ref`. The two
  paths cannot diverge today, but if anyone fixes either of the two
  preceding bugs (attach guard, error log), the call_jni path will silently
  retain the old behaviour. The doc comment at line 8738 ("JNI table
  slot 23 is DeleteLocalRef") implies a single source of truth that the
  code doesn't honour. This is technically a maintainability bug rather
  than a runtime correctness bug, but in a single-header library where
  "edit one place" is the design promise, this duplication is the kind
  that grows fixes-by-one-site and leaks of-by-others. Note the local
  vtable read already exists at this site (line 11762: `void** const table`),
  so the inlining was a micro-perf choice that is unmeasurably cheap
  compared to the JNI call itself.
- **repro:**
  Grep the file for `table[23]`. Two distinct call sites. Fix one for a
  user-reported leak (e.g. the silent-no-op bug above) and the bug
  re-appears in call_jni without anyone noticing.
- **suggested_fix:**
  Replace the inline triple-call at 11920-11923 with three calls to
  `vmhook::detail::jni_delete_local_ref(...)`. The `env_void` arg
  becomes implicit (the helper reads `current_jni_env`), and the
  `delete_local_ref_t` typedef at line 11843 can be removed too.
- **confidence:** certain

## Improvements

### [LOW] [USER_FACING] Document the "must follow ensure_current_java_thread" precondition
- **rationale:**
  The current doc comment at vmhook/ext/vmhook/vmhook.hpp:7311-7324
  explains *why* DeleteLocalRef matters but never tells a user-of-the-helper
  that the helper requires `current_jni_env` to already be populated.
  Users who reach for this from their own detour code (the public-ish
  `vmhook::detail::` surface is the only release path) will assume the
  helper attaches when needed (like every sibling helper does). One
  sentence prevents a class of "but I called it!" support tickets.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:7311-7324
- **suggested_change:**
  Append to the @details block:
  > Precondition: `current_jni_env` must already be populated (i.e. a
  > prior JNI helper such as `jni_new_string_utf` ran on this thread,
  > or the caller invoked `vmhook::hotspot::ensure_current_java_thread()`
  > directly). Without it the release silently no-ops and the local ref
  > leaks - the helper does not auto-attach because the typical use
  > pattern (release a handle that the same caller just allocated) makes
  > attachment redundant on the hot path.
  > Once the auto-attach fix lands, replace with: "Auto-attaches the
  > current thread to the JVM if needed."

### [LOW] [USER_FACING] Add an overload that takes a span/range of handles
- **rationale:**
  Three of the four in-tree call sites release multiple handles in a
  loop:
  - `jni_find_class_with_context_loader`'s `local_ref_bag` (line 9050)
  - `call_jni`'s `string_handle_cleanup` (line 11743)
  - `jni_make_unique`'s `jni_arg_cleanup` (line 9789)
  Each builds its own RAII struct. A
  `jni_delete_local_refs(std::span<void* const>)` overload would
  collapse the boilerplate, attach once for the whole batch (cheaper
  than attaching N times after the bug above is fixed), and skip null
  slots internally. Users in their own detours would benefit too.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:8742-8756
- **suggested_change:**
  Add:
  ```cpp
  inline auto jni_delete_local_refs(std::span<void* const> handles) noexcept -> void
  {
      bool any_non_null{ false };
      for (void* const h : handles) { if (h) { any_non_null = true; break; } }
      if (!any_non_null) { return; }
      if (!vmhook::hotspot::ensure_current_java_thread()) { return; }
      using fn_t = void(*)(void*, void*);
      fn_t const fn{ vmhook::detail::jni_function<23, fn_t>(vmhook::hotspot::current_jni_env) };
      if (!fn) { return; }
      for (void* const h : handles) { if (h) { fn(vmhook::hotspot::current_jni_env, h); } }
  }
  ```
  Then rewrite the three RAII bags to call this in their destructor with
  a `std::span` view of their underlying vector/array.

### [LOW] [INTERNAL] Cache the resolved slot-23 function pointer per JNIEnv
- **rationale:**
  Every DeleteLocalRef call performs two indirections (`*env` -> table,
  `table[23]` -> fn) before the actual JNI dispatch. JNIEnv*'s vtable
  is process-stable for the lifetime of the JVM, so the slot-23 pointer
  for the cached `current_jni_env` could be memoized in a
  `thread_local void* cached_delete_local_ref_fn`. Probably unobservable
  in profiles (the JNI call itself dominates), but for hot-path
  string-injection loops every helper call goes through this and the
  cached path is one branch instead of two indirections. Worth doing
  *if* a benchmark shows it; not worth doing speculatively.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:8742-8756
- **suggested_change:**
  Speculative — gather a `call_jni` microbenchmark first, then consider
  a thread-local cache keyed on `(env_pointer, last_seen_table_pointer)`.

### [LOW] [INTERNAL] Use the typedef where the type is already named
- **rationale:**
  Line 8749 declares `using delete_local_ref_t = void(*)(void*, void*);`
  and the identical typedef appears at line 11843 inside `call_jni`.
  Pulling the typedef out to namespace scope (next to the `exception_check_t`
  family used elsewhere) eliminates duplication and gives the slot a
  named C++ type that's grep-able.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:8749, 11843
- **suggested_change:**
  Promote `delete_local_ref_t` to `vmhook::detail::`, next to the other
  `*_t` aliases.

## Tests

### [unit] [extend_existing] test_jni_delete_local_ref_attach_no_jvm
- **description:**
  Add a third assertion to the existing `test_jni_delete_local_ref_no_jvm`
  that exercises the "ensure_current_java_thread returns false" path
  once the suggested attach guard lands. In a no-JVM unit-test process
  `ensure_current_java_thread()` will fail; the helper must still not
  crash and must not call through to a null function pointer.
- **asserts:**
  - `jni_delete_local_ref(non_null_handle)` returns without crash when
    `current_jni_env == nullptr`.
  - No assertion on side-effects (we have no JVM to observe).
- **existing_file:** tests/test_helpers.cpp:1200-1213

### [unit] [extend_existing] test_jni_delete_local_ref_slot_23_resolution
- **description:**
  Construct a synthetic JNIEnv whose function table is a stack array
  of 232 nullptrs with index 23 set to a recording stub, then point
  `current_jni_env` at that handle, invoke `jni_delete_local_ref(0xDEAD)`,
  and assert the stub recorded one call with `env` and `0xDEAD` as
  arguments. This proves "slot 23" actually matches DeleteLocalRef
  without needing a real JVM. The same harness can be parameterized
  to assert that slot indices for the other helpers in this file
  (6/15/16/17/31/33/36 etc.) are stable.
- **asserts:**
  - Stub at table[23] receives `(env, 0xDEAD)`.
  - Stubs at table[0..22] and table[24..N] receive zero calls.
- **existing_file:** tests/test_helpers.cpp (new test, near line 1200)

### [unit] [new] test_jni_delete_local_ref_null_table_logs_diagnostic
- **description:**
  Once the suggested "log when slot 23 is null" improvement lands,
  install a synthetic JNIEnv whose `table[23] == nullptr`, redirect
  VMHOOK_LOG to a capture buffer, call `jni_delete_local_ref(fake)`,
  and assert the buffer contains the diagnostic substring. Catches
  regressions where future refactors of the helper silently restore
  the old "leak with no log" behaviour.
- **asserts:**
  - Capture buffer contains `"slot 23"` and `"DeleteLocalRef"`.
- **existing_file:** tests/test_helpers.cpp

### [unit] [new] test_jni_delete_local_ref_batch_overload
- **description:**
  Covers the proposed `jni_delete_local_refs(std::span<...>)` overload:
  pass a span of three handles (null, valid stub-handle, null), assert
  the stub receives exactly one call. Verifies the early-out for
  all-null spans does not attach the thread.
- **asserts:**
  - Stub receives one call with the non-null handle.
  - For an all-null span: `ensure_current_java_thread` mock is *not*
    invoked.
- **existing_file:** tests/test_helpers.cpp

### [unit] [new] test_jni_delete_local_ref_handle_alignment_robust
- **description:**
  Pass deliberately unaligned and high-bit-set handle pointers
  (`0x1`, `0xFFFFFFFFFFFFFFFE`, `reinterpret_cast<void*>(SIZE_MAX)`)
  to `jni_delete_local_ref` with `current_jni_env == nullptr`.
  Should be a pure no-op (no fault, no UB). Guards against a
  hypothetical regression where someone adds an
  `is_valid_pointer(object_handle)` check that misclassifies
  legitimate JNI handles (which on some JVMs really do live in
  unusual ranges).
- **asserts:**
  - No crash for any of the three handle values.
- **existing_file:** tests/test_helpers.cpp

## Parity Concerns
- `jni_delete_local_ref` is the only helper in the
  `vmhook/ext/vmhook/vmhook.hpp:8780-9020` JNI helper section that does
  *not* call `ensure_current_java_thread()` before resolving the function
  pointer. Every sibling (jni_find_class, jni_exception_clear,
  jni_get_object_class, jni_get_method_id, jni_call_object_method,
  jni_new_string_utf, jni_get_string_utf) attaches the thread. The
  asymmetry is invisible to readers and will burn the next contributor
  who copy-pastes the pattern.
- `call_jni::check_callee_exception` (line 11919) re-implements slot 23
  inline instead of using the wrapped helper. After the suggested
  consolidation, the only direct `table[23]` reference in the codebase
  should be inside `jni_delete_local_ref`.
- Doc comment at line 7321 promises `Exception safety: noexcept — null
  handle is a safe no-op` but says nothing about a missing JNIEnv being
  *also* a safe no-op-but-leak. The contract is silently weaker than
  callers would assume from the comment.
- The slot-23 invariant assumed by the doc comment is correct across
  JDK 8/11/17/21/25 (DeleteLocalRef has been entry 23 in `JNINativeInterface_`
  since JDK 1.2 — verified against `jni.h` in every shipping HotSpot
  release). No JDK-version drift risk here, unlike the `safefetch_stub` /
  thread-list-offset bits elsewhere in vmhook. So the bug surface is
  caller-side (attach state), not table-layout-side.
