# method_proxy_call_jni_local_ref_leaks

## Summary
Audited the `string_handle_cleanup` RAII (vmhook.hpp:11710-11727) plus the
companion result-handle release in the `'L'/'['` arm (12053, 12065) that
together implement the v0.4.3 fix. The RAII pattern is sound for string and
object args, runs even on JNI-side exceptions because exception_check is
noexcept and cleanup is scope-bound, and correctly distinguishes synthetic
stack handles from real local refs. The top concern is a **critical
union-aliasing bug**: `jni_value` is a `union`, so any non-zero primitive
argument (long, double, int with high bit, etc.) writes bits that the cleanup
loop then re-reads as `value.l` and hands to `DeleteLocalRef` on a garbage
pointer.

## Bugs

### [critical] Primitive args trigger DeleteLocalRef on garbage pointer (union aliasing)
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:11710-11727 (cleanup) + 8663-8674 (`union jni_value`) + 9669-9732 (`write_jni_arg_to_slot`)
- **description:**
  `jni_value` is a **union** (vmhook.hpp:8663) whose `.l`, `.j`, `.i`, `.d`,
  `.f`, ... all share storage. For a primitive argument,
  `write_jni_arg_to_slot` zero-initializes the union (line 9674) then writes
  exactly one member, e.g.
  `value.j = static_cast<std::int64_t>(arg)` (line 9714) for a 64-bit int.
  The cleanup loop unconditionally reads `this->values_ptr[i].l` (line 11720)
  — but that is the same storage as `.j`. So for a long arg holding
  `0x1234'5678'1234'5678`, `value.l` reads back as
  `0x1234'5678'1234'5678` (non-null, almost never equal to
  `&storage_ptr[i]`), and the destructor invokes
  `jni_delete_local_ref(0x1234'5678'1234'5678)`, which becomes a
  `DeleteLocalRef` call with a bogus jobject. Affected primitives include
  every non-zero `bool` (`value.z = true` → `.l == 0x01`), `int8_t`, `int16_t`,
  `uint16_t`, `int32_t` (any value with at least one bit set), `int64_t`,
  `float` (e.g. `1.0f` → `.l == 0x3F800000`), and `double`.

  Behavior on HotSpot: `DeleteLocalRef` with a non-table pointer either
  silently no-ops on debug builds (you lose your real local-ref slot but
  nothing visibly crashes) or hits an internal assertion / SEH access
  violation depending on JDK + jcheck flags. The previous reviewer's mental
  model — "primitive args leave `.l = nullptr` so the null guard handles it"
  — does NOT hold for a union: the null-init at line 9674 is overwritten by
  every subsequent primitive store.
- **repro:**
  Any method call where one of the args is a non-zero primitive:
  ```cpp
  auto m = vm::get_method_proxy(some_obj, "Foo", "doIt", "(J)V");
  m.call_jni(int64_t{ 0x4242'4242'4242'4242 });
  // cleanup destructor reads values[0].l == 0x4242'4242'4242'4242,
  // sees candidate != &storage_ptr[0], calls
  // DeleteLocalRef(0x4242'4242'4242'4242).
  ```
  Run under `-Xcheck:jni` to surface "Invalid local ref" warnings, or compile
  with a fastdebug HotSpot to see the assertion.
- **suggested_fix:**
  Track which slots hold a true reference type instead of reading the union
  back. Two clean options:
  1. **Tag array.** Add `bool is_ref[arg_cap]{}` alongside `handle_storage[]`.
     Have `write_jni_arg_to_slot` accept a `bool& is_ref` out-parameter (or
     return a tag) and set it to `true` only on the string / object / unique_ptr
     branches. The cleanup loop then iterates `if (is_ref[i] && ...)` instead
     of reading `value.l`.
  2. **Track synthetic-vs-jni-vs-primitive ternary.** Replace
     `handle_storage[]` with `slot_kind kind[arg_cap]{}` where
     `kind ∈ { primitive, synthetic_oop, jni_local_ref }`, set by
     `write_jni_arg_to_slot`, consumed by cleanup.

  Either fix is single-digit lines and removes the union-aliasing footgun
  entirely. Apply the same fix to `jni_make_unique`'s `jni_arg_cleanup`
  (vmhook.hpp:9789-9813) which has the identical hazard via the same
  `make_jni_args` builder. The `make_jni_args` variant escapes by chance
  only because object handles live inside the contiguous `object_handles`
  vector and the begin/end range-check (line 9805) happens to exclude most
  primitive bit patterns — but the same union-as-pointer read is UB and will
  bite the first time a primitive bit pattern lands inside the vector's
  address range (e.g. ASLR'd stack addresses look exactly like jint
  arguments).
- **confidence:** certain

### [low] Cached class_handle is a JNI local ref but cached across calls
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:11589-11653, 12657
- **description:**
  `cached_class_handle` is populated from `jni_find_class()` (line 11619) on
  the static path or `jni_get_object_class()` (line 11642) on the instance
  path. Both return JNI **local** references whose lifetime is tied to the
  current JNI frame. The proxy stores this local ref in
  `cached_class_handle` (declared at 12657) and reuses it on subsequent
  calls. This is correct only when (a) the proxy is reused exclusively
  inside the same JNI frame that created the handle, or (b) the calling
  thread is one of the long-lived attached detour threads that never sees a
  JNI frame pop — which the rest of `call_jni`'s commentary (line 11700)
  acknowledges is exactly the assumed environment.

  However: if a caller ever uses the same `method_proxy` from a transient
  thread that pushes/pops JNI frames (e.g. user code that attaches/detaches
  per request, or HotSpot internally pops the frame around a hook callback
  on some JDKs), the cached handle becomes a dangling local ref and the next
  `call_jni` will SEH on `Call*MethodA(receiver=garbage, ...)`. There is no
  defensive validation of `cached_class_handle` before reuse on line 11737
  or 11919. This is not a leak per se but an adjacent UAF hazard introduced
  by the same caching that the leak fix preserves.
- **repro:**
  Hard to repro in the headless test suite — needs a Java-side scenario that
  legitimately pops the JNI frame between two `call_jni` invocations on the
  same proxy. On vanilla HotSpot inside vmhook detours it does not trigger
  because detour threads stay attached, hence the bug-class label is "low"
  even though the failure mode is UAF.
- **suggested_fix:**
  Promote `cached_class_handle` to a **JNI global ref** the first time it is
  computed: call `NewGlobalRef(class_handle)` (table slot 21) and store the
  global. Use `DeleteGlobalRef` (slot 22) in the `method_proxy` destructor.
  Document at the field declaration (12657) that this is a global to remove
  the "is this safe across frames?" reader question. Same applies to
  `cached_method_id` strictly speaking — but jmethodIDs are not local refs
  (they live in the JVM-internal method table), so they are unaffected.
- **confidence:** likely

## Improvements

### [S] [USER_FACING] Surface NewStringUTF failure to the caller
- **rationale:**
  `write_jni_arg_to_slot` calls `jni_new_string_utf()` (line 9679) and
  silently leaves `value.l = nullptr` if the JNI table is unavailable or
  NewStringUTF returns null (OOM, allocation failure, pending exception
  poisoning the env). The current code then passes a null jstring to the
  Java callee which usually NPEs deep inside the method. `field_proxy` /
  `return_value` log a clear "both JNI NewStringUTF and ... failed" line for
  this exact case (see vmhook.hpp:7594, 7614) — `call_jni` should match.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:11688-11692
- **suggested_change:**
  After the fold-expression that fills `values[]`, walk the result and log
  any slot whose source was a string arg but whose `.l` came back null. The
  cleanest hook is to thread an `bool string_failed` out-parameter through
  `write_jni_arg_to_slot` (same plumbing as the bug fix above) so cleanup
  and diagnostic logging share one tag array.

### [XS] [INTERNAL] Document cleanup discriminator using clearer pointer arithmetic
- **rationale:**
  The discriminator `candidate != &this->storage_ptr[i]` (line 11721) works
  because `storage_ptr` is `handle_storage` decayed to `void**`, so
  `&storage_ptr[i] == handle_storage + i == &handle_storage[i]`. This is
  correct but takes the reader a beat to verify. The block comment at lines
  11704-11709 references `&handle_storage[i]` (the original array name)
  rather than `&storage_ptr[i]` (the actual code), which makes them re-read
  the field to be sure they match.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:11704-11727
- **suggested_change:**
  Either store the raw `void**` array address as `void** storage_array` and
  compare `candidate != (storage_array + i)`, or expand the comment to
  explicitly note that `storage_ptr` is the same array, just renamed.

### [S] [USER_FACING] Non-String L/[ return loses the object via uint32 truncation
- **rationale:**
  Lines 12056-12066 release the result handle (correct, no leak) but the
  variant returned to the caller is
  `static_cast<std::uint32_t>(reinterpret_cast<uintptr_t>(result_handle))`
  — a truncated handle value that the caller cannot meaningfully use. The
  comment claims this is "best-effort", but after the DeleteLocalRef the
  truncated handle refers to nothing the caller can dereference. Either we
  should promote the result to a global ref before deletion and return an
  opaque wrapper, or we should change the public contract to admit that
  arbitrary `L/[` return types are simply not supported through `call_jni`
  (only `Ljava/lang/String;` is). The current half-measure looks like a
  feature but yields garbage.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:12056-12066
- **suggested_change:**
  Two options:
  1. Add an `oop_type_t` arm to `value_t`: decode `result_handle` to its
     uncompressed OOP via `jni_decode_object` (which already exists in the
     codebase, see line 9039+) BEFORE releasing the local ref, then return
     `value_t{ oop }`. Callers wrap the OOP in their own `vmhook::object_base`
     subclass.
  2. Emit a `VMHOOK_LOG(error_tag, ...)` and return `monostate` when the
     return type is `L/[` but not `Ljava/lang/String;`, making it loud that
     this path is unsupported. Matches the "fail loudly" stance applied to
     `GetMethodID == null` at 11667.

### [M] [USER_FACING] method_proxy lacks a `clear_caches()` parity helper
- **rationale:**
  `field_proxy` has explicit field-pointer caching invalidation paths (see
  `field_proxy::reset_cache` patterns elsewhere). `method_proxy` accumulates
  `cached_method_id`, `cached_class_handle`, `cached_ret_char`, and
  `cached_effective_signature` (12656-12667) but exposes no way to drop
  them. A user who replaces the underlying `method*` (e.g. after class
  retransform) or rebinds the receiver `object` has no way to invalidate
  the caches and silently dispatches via the stale jmethodID. With the
  cached_class_handle UAF risk (bug above), this is the user-facing footgun
  that turns the latent issue into a crash.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:12646-12667
- **suggested_change:**
  Add `void clear_caches() noexcept` that nulls `cached_method_id` and
  releases `cached_class_handle` (delete-local-ref OR delete-global-ref
  depending on the fix chosen for the bug above), resets `cached_ret_char`,
  and clears `cached_effective_signature`. Call it automatically from any
  setter that mutates `object` or `method`.

## Tests

### [standalone_unit] [new] test_call_jni_primitive_arg_does_not_call_delete_local_ref
- **description:**
  Build a `method_proxy` with a stub JNIEnv whose function table records
  every `DeleteLocalRef` invocation. Drive `call_jni` with a long-typed
  argument set to a recognizable sentinel value (e.g.
  `0xCAFE'BABE'DEAD'BEEF`). Assert that the recorded delete list does NOT
  contain `0xCAFE'BABE'DEAD'BEEF` (or any of the primitive bit patterns).
  Repeat for `bool true`, `int32_t{ -1 }`, `float{ 1.0f }`, `double{ 1.0 }`,
  `int16_t{ 0x1234 }`. This directly catches the union-aliasing bug.
- **asserts:** recorded `DeleteLocalRef` arguments contain only the indirect
  string-handle pointers, not any primitive bit patterns; recorded list is
  empty when all args are primitive.

### [standalone_unit] [new] test_call_jni_string_arg_releases_jstring
- **description:**
  Stub JNIEnv where `NewStringUTF` returns a known fake jstring pointer.
  Invoke `call_jni` with a `std::string("hi")` arg. Assert the
  same fake jstring lands in the recorded `DeleteLocalRef` list exactly
  once after the call returns (and is NOT released before the Call*MethodA
  dispatch). This verifies the v0.4.3 fix still works.
- **asserts:** `DeleteLocalRef(fake_jstring)` happens once after
  `Call*MethodA` and exactly once.

### [standalone_unit] [new] test_call_jni_object_arg_skips_delete_local_ref
- **description:**
  Pass a `vmhook::object_base`-derived arg through `call_jni`. Confirm the
  cleanup discriminator detects the synthetic stack handle and does NOT
  call `DeleteLocalRef` on `&handle_storage[i]`.
- **asserts:** stub `DeleteLocalRef` list does not contain the address of
  any `handle_storage[i]` slot.

### [standalone_unit] [new] test_call_jni_result_handle_released_for_L_return
- **description:**
  Method signature `()Lcom/example/Foo;` (non-String reference return).
  Stub `CallObjectMethodA` to return fake handle `0xC0FFEE00`. Confirm
  `DeleteLocalRef(0xC0FFEE00)` happens exactly once and the returned
  `value_t` is not `monostate`.
- **asserts:** result handle deleted once; returned `value_t` indices match
  the expected truncated-uint32 alternative (and once Improvement #3 lands,
  the test should be updated to check the oop alternative).

### [standalone_unit] [new] test_call_jni_no_leak_under_jni_exception
- **description:**
  Stub `Call*MethodA` so that after it returns, `ExceptionCheck` reports
  `true`. Pass a string arg. Confirm:
  (a) the string-arg jstring is still released exactly once via
      `DeleteLocalRef` (cleanup destructor runs on scope exit even though
      `check_callee_exception` ran first), and
  (b) for the L-return arm, the result handle is released even though the
      exception path executed.
- **asserts:** every `NewStringUTF` produces exactly one `DeleteLocalRef`;
  result handle released; `ExceptionDescribe` + `ExceptionClear` called.

### [jvm_integration] [new] test_call_jni_tight_loop_no_local_ref_table_overflow
- **description:**
  In a real JVM (vmhook/src/example.cpp driver), call a String-arg method
  in a tight loop of N = 4096 iterations (well past HotSpot's default
  16-slot local-ref table) and assert no
  `JNI local reference table overflow` warnings are emitted on stderr.
  Use `-Xcheck:jni` to amplify any missing release. Without the v0.4.3
  fix this test fails within ~16 iterations.
- **asserts:** stderr (captured) contains zero occurrences of the
  substring "local reference table overflow"; loop completes; receiver
  identity is preserved (read receiver hash before and after loop).

### [jvm_integration] [extend_existing] test_call_jni_primitive_long_arg_does_not_crash
- **description:**
  Inside the example driver, call a `(J)V` method with
  `int64_t{ 0x7FFF'FFFF'FFFF'FFFE }`. Run with `-Xcheck:jni`. Without the
  proposed primitive-arg bug fix this fires
  `WARNING in native method: JNI call made with exception pending` or
  `Invalid local reference` and may take the JVM down on
  fastdebug builds.
- **asserts:** call returns cleanly; no jcheck warnings; receiver still
  reachable afterward.
- **existing_file:** vmhook/src/example.cpp

## Parity Concerns
- `method_proxy::clear_caches()` does not exist whereas `field_proxy` has
  explicit cache-invalidation entry points (see improvement #4). Adding it
  is a prerequisite for safely fixing the `cached_class_handle` UAF.
- `jni_make_unique`'s `jni_arg_cleanup` (vmhook.hpp:9789-9813) has the
  identical union-aliasing hazard — the begin/end range check on the
  `object_handles` vector "saves" it by accident for low-magnitude
  primitives, but high-magnitude primitives (typical jlong / jdouble bit
  patterns) can fall outside the vector range and still trigger the bogus
  delete. Fix in lockstep with the `call_jni` fix.
- `field_proxy::set`'s string path (vmhook.hpp:7594, 7614) emits a clean
  failure log when `NewStringUTF` returns null; `method_proxy::call_jni`
  silently passes the null jstring to the callee. Add the parity log.
