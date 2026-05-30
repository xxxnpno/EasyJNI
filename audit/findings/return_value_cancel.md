# return_value_cancel

## Summary
`return_value::cancel()` is a 3-line setter that flips `return_slot::cancel = true`
without touching `return_slot::retval`.  When the trampoline takes the cancel
path it unconditionally loads `[rsp+8]` into rax (and movq's it into xmm0)
regardless of method return type, so calling `cancel()` on a non-void method
silently forces the Java caller to receive zero/null/+0.0 — a behaviour that is
neither documented nor warned about anywhere in the public API.  The
implementation itself is correct and trivially thread-confined; the gaps are
all in user-facing safety nets and documentation.

## Bugs

### [low] cancel() lacks its own Doxygen docstring
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:1205-1209
- **description:** Unlike `set()` (which has a 26-line `@brief` + `@details`
  block at 1178-1203 for the typed-null overload alone) and `set_arg()`
  (which has a 19-line block at 1211-1228), `cancel()` is declared bare
  with no comment.  The only contract a user can find is the class-level
  blurb at 1124-1131 which says "Call cancel() to suppress without a
  return value (for void methods)".  Crucially, this does not document
  what actually happens when `cancel()` is called on a *non-void* method:
  the trampoline at vmhook.hpp:5314-5315 loads `[rsp+8]` (still zero from
  the `push 0x0` at 5295) into rax and xmm0, so the Java caller receives
  `0` / `null` / `+0.0` — but the user has no way to learn that from the
  header.  Discoverability bug: IDE hover on `cancel()` shows nothing.
- **repro:** Hover over `cancel()` in an IDE; observe no popup
  documentation.  Or hook a non-void method (e.g. `String getName()`),
  call `ret.cancel()`, and discover via printf that the caller saw `null`
  with no warning issued.
- **suggested_fix:** Add a `@brief` block matching `set_arg`'s style that
  spells out the void-vs-non-void semantics, recommends `set()` for
  non-void cancellation, and explicitly notes the zero/null/+0.0 fallback
  for users who do call it on a non-void method.
- **confidence:** certain

### [medium] cancel() is a silent footgun on non-void methods
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:1205-1209
- **description:** When a user hooks a method like `int getScore()` and
  calls `ret.cancel()` instead of `ret.set(0)`, the trampoline cancel
  path at vmhook.hpp:5310-5316 (Win64) and 5407-5413 (SysV) takes effect:
  `cmp byte ptr [rsp], 0` sees `cancel == true`, falls through to
  `mov rax, [rsp+8]` which reads the zero-initialised retval slot.  Java
  receives `0` (or `null`, or `+0.0`).  There is no log, no assertion,
  no diagnostic — yet every other "you probably didn't mean this"
  scenario in the file goes through `VMHOOK_LOG` with `error_tag`
  (compare set_arg at vmhook.hpp:7524-7527).  Two missing safety nets:
  (1) the runtime has no way to know the hooked method's return type
  at the point cancel is called, but it could log a one-time warning
  the first time `cancel()` is invoked from a hook whose method
  signature does not start with `()V`; (2) at compile time, the hook
  template could carry the JVM return-type descriptor and `static_assert`
  on it for the typed `cancel()` variant.
- **repro:** Hook `nonStaticForceReturnMe(int)` (returns int) with a
  detour that calls `retval.cancel()` instead of
  `retval.set(int32_t{42})`.  The Java probe will read `0`, but no log
  appears and the test would never know without an explicit
  `check_equal` on the returned value.
- **suggested_fix:** Either (a) capture the hooked method's signature
  in `wrapper_detour` (vmhook.hpp:7805-7824) and have `cancel()` query
  it before mutating the slot, logging a warning when the descriptor
  does not end in `V`; or (b) add a compile-time mechanism via a new
  template parameter on `hook<T>` so the trampoline-wrapper can
  `static_assert` on the signature at template-instantiation time.
  Option (a) is cheaper and doesn't break the API.
- **confidence:** likely

### [low] cancel() has no null-slot guard despite identical risk to set()
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:1205-1209
- **description:** `cancel()` dereferences `this->return_slot` without
  checking for null.  In normal use the trampoline always passes a
  valid slot (constructed on the native stack at vmhook.hpp:5295-5296),
  so this is not exploitable.  But `set()` and the wrapper `set(nullptr)`
  share the exact same un-guarded dereference (1157, 1201), and any
  user constructing a `return_value` directly with a null `slot`
  (which the constructor at 1141-1145 happily accepts) will get an
  immediate AV on the next call to any of the three.  Not strictly a
  cancel-only bug, but worth flagging since cancel() is the cheapest
  one to harden: a single `if (!this->return_slot) return;` is enough.
- **repro:** `vmhook::return_value rv{nullptr, nullptr}; rv.cancel();`
  → access violation on the `this->return_slot->cancel = true` write.
- **suggested_fix:** Either (1) tighten the constructor to assert/early-
  return on null slot, or (2) add the same null guard to all three
  mutators (`set`, `set(nullptr)`, `cancel`).  Option (2) keeps the
  noexcept contract intact in test code that intentionally probes
  edge cases.
- **confidence:** speculative

## Improvements

### [XS] [USER_FACING] Add a discharged() / is_cancelled() inspector
- **rationale:** Once `cancel()` is called the user has no way to
  ask whether they did so earlier in the detour — useful when
  composing multiple early-return paths ("if I already cancelled,
  don't try to set() over the top").  Today the only path is to
  hold a local bool, which is boilerplate.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:1205-1209
- **suggested_change:** Add
  ```cpp
  [[nodiscard]] auto is_cancelled() const noexcept -> bool
  {
      return this->return_slot && this->return_slot->cancel;
  }
  ```
  next to `cancel()`.  Documents the slot relationship and makes
  the "already short-circuited?" query trivial.

### [XS] [USER_FACING] Mark cancel() and set() with [[nodiscard]] semantics for the slot
- **rationale:** Not the return type (both are void) — but a
  consistency review: every other mutator on `return_value` either
  returns `void` (set, cancel) or `bool` (set_arg).  Users who
  think "did the cancel actually take effect" have to look at the
  source.  A short comment block on `cancel()` saying "always
  succeeds; cannot fail" matches the style at set_arg:7510-7530.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:1205-1209
- **suggested_change:** Add a one-line comment
  `// always succeeds; no failure path is possible because the
  return_slot is owned by the calling trampoline.` immediately
  above the `auto cancel()` line.

### [S] [USER_FACING] Document the zero-fill semantics for non-void cancellation
- **rationale:** Right now a user who reads `cancel() suppress
  without a return value (for void methods)` and applies it to a
  non-void method will silently get `0` / `null` / `+0.0`.  The
  surprise is unavoidable given the trampoline design, but a
  `@warning` block on `cancel()` and a sibling note on the class
  docstring would prevent the footgun from biting in production.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:1124-1131, 1205-1209
- **suggested_change:** Extend the class-level docblock with
  "If `cancel()` is called on a non-void method the Java caller
  receives 0 / null / +0.0 depending on its return type — prefer
  `set(0)` / `set<T>(nullptr)` for clarity." and mirror the warning
  on `cancel()` itself.

### [S] [USER_FACING] Add an explicit `set_void()` alias for symmetry
- **rationale:** `set()` reads as "I am supplying a value", `cancel()`
  reads as "I am giving up".  A void-method hook that wants to skip
  the original body is not "giving up" — it has done its work.  A
  trivial alias
  ```cpp
  auto set_void() noexcept -> void { this->cancel(); }
  ```
  reads cleanly at the call site (`ret.set_void();`) and lets
  `cancel()` recover a sharper "I want the default zero return"
  meaning later if that ever becomes useful.  Low-effort, no-API-break.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:1205-1209
- **suggested_change:** Add a one-line inline alias next to `cancel()`
  with a `@brief` that says "Synonym for cancel(); reads better in
  void-method hooks."

### [M] [INTERNAL] Capture hook return-type at template instantiation for diagnostic
- **rationale:** The wrapper_detour at vmhook.hpp:7805-7824 already
  has full type info about the user callable.  Threading the hooked
  method's JVM return descriptor (`'V'` vs anything else) into the
  `return_value` constructor and storing it as a bool/char would
  enable a one-time `VMHOOK_LOG(error_tag, ...)` warning the first
  time `cancel()` is called on a non-void method.  Catches the
  silent-zero footgun without forcing the user to remember to add
  `check_equal` to every test.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:1138-1145, 1205-1209,
  7805-7824
- **suggested_change:** Add an optional `char return_descriptor = '?';`
  field to `return_value`, set it in the wrapper_detour from
  `found_method->get_signature()` (parsing the trailing `)X`), and
  log once-per-method from inside `cancel()` when the descriptor is
  neither `?` nor `'V'`.

## Tests

### [standalone_unit] [exists] return_value_cancel_sets_flag
- **description:** Already covered at tests/test_helpers.cpp:867-869;
  constructs `return_value{ &slot, nullptr }`, calls `cancel()`, asserts
  `slot.cancel == true`.
- **asserts:** `slot.cancel == true` after `rv.cancel()`.
- **existing_file:** tests/test_helpers.cpp

### [standalone_unit] [new] return_value_cancel_preserves_retval
- **description:** Verify that `cancel()` does **not** overwrite the
  retval cell.  Pre-set `slot.retval = 0xDEADBEEF`, call `cancel()`,
  assert retval is unchanged.  Documents the behaviour the trampoline
  relies on (`set()` zeroes; `cancel()` does not).
- **asserts:** `slot.retval == 0xDEADBEEF` after `rv.cancel()` when
  retval was non-zero before the call.

### [standalone_unit] [new] return_value_cancel_idempotent
- **description:** Two back-to-back `cancel()` calls must leave the
  slot in the same state as one call.  Asserts there are no
  side effects from repeated invocation (relevant when refactoring
  toward `is_cancelled()` / `set_void()` aliases).
- **asserts:** After two `rv.cancel()` calls in sequence, both
  `slot.cancel == true` and `slot.retval == 0` (unchanged).

### [standalone_unit] [new] return_value_cancel_then_set_overrides
- **description:** Call `cancel()` then immediately `set<int32_t>(42)`.
  Asserts that `set()` overwrites the retval slot.  Establishes the
  documented "last write wins" contract for the user.
- **asserts:** `slot.cancel == true` AND `slot.retval == 42` after
  the call sequence.

### [standalone_unit] [new] return_value_set_then_cancel_preserves_value
- **description:** Inverse of the previous case: call `set<int32_t>(42)`
  then `cancel()`.  `cancel()` must not zero the retval that `set()`
  just stored.  This is the subtle case — a user who does `set()`
  early then conditionally `cancel()`s expects the value to survive.
- **asserts:** `slot.cancel == true` AND `slot.retval == 42` after
  the call sequence.

### [jvm_integration] [exists] test_method_cancel (cancelSkippedOriginalMethod)
- **description:** Already covered at vmhook/src/example.cpp:1716-1755
  for a void method (`nonStaticCancelMe`).  Asserts the original
  body was skipped (`get_cancel_called() == 0`) and the detour
  observed the arguments.
- **asserts:** `cancelHookCallCount == 1`, `cancelSawInstance`,
  `cancelSawExpectedArgument`, `cancelSkippedOriginalMethod`.
- **existing_file:** vmhook/src/example.cpp

### [jvm_integration] [new] test_cancel_on_non_void_returns_zero
- **description:** Hook a non-void method (e.g. an `int` returner like
  `nonStaticForceReturnMe`), have the detour call `retval.cancel()`
  WITHOUT calling `set()`, run the Java probe, and assert the Java
  caller observed `0` (the documented-after-fix fallback).  Locks
  in the trampoline's zero-fill cancel behaviour so a future
  rewrite of the cancel path cannot silently change it.
- **asserts:** Detour fires, original body did not run, Java probe
  records returned value == 0.

### [jvm_integration] [new] test_cancel_on_reference_returns_null
- **description:** Hook a method returning a Java reference (e.g.
  `String` or a wrapper class), call `retval.cancel()` only, run
  the probe.  Asserts the Java caller sees `null`.  Distinct from
  `set<wrapper>(nullptr)` test which goes through the typed null
  overload at vmhook.hpp:1196-1203.
- **asserts:** Detour fires, Java probe records `result == null`.

### [jvm_integration] [new] test_cancel_on_double_returns_zero_double
- **description:** Hook a method returning `double`, call `cancel()`,
  assert the Java caller sees `+0.0`.  Exercises the xmm0 movq path
  at vmhook.hpp:5315 (Win64) and 5412 (SysV) which is otherwise
  untested by the existing void-only cancel coverage.
- **asserts:** Detour fires, returned double == 0.0 (and not NaN).

### [jvm_integration] [new] test_cancel_then_original_runs_on_next_call
- **description:** Install a hook that cancels every other call.
  Confirms that the cancel state is per-invocation (lives only in
  the trampoline-allocated `return_slot` on the native stack) and
  does not stick across calls.  Useful regression coverage if
  someone ever switches to a heap-allocated or thread-local slot.
- **asserts:** First call: original body skipped (probe field == 0);
  second call: original body ran (probe field updated).

## Parity Concerns
- `set()` and `set(nullptr)` both have well-formed docblocks and
  rationale.  `cancel()` has none.  Bring it up to the same level.
- `set_arg()` returns `bool` with full diagnostic logging on failure
  (vmhook.hpp:7524-7537); `cancel()` returns void and logs nothing.
  Failure is impossible in the trampoline-driven path, but a one-time
  warning for "called on a non-void method" would match the
  user-friendliness already established by `set_arg`.
- There is no programmatic counter-operation to `cancel()` — no
  `uncancel()` / `restore()`.  Some hooking frameworks ship one for
  "I changed my mind".  Worth considering as an XS improvement if
  user feedback ever asks for it; otherwise the current write-once
  contract is fine.
- The class-level docblock at vmhook.hpp:1124-1131 covers `set()`
  and `cancel()` together, but only shows a `set()` example.  Adding
  a tiny `ret.cancel();` example for void methods would mirror the
  README at README.md:343-353 inside the header so IDE-only readers
  see it too.
