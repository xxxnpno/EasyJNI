# return_value_set_arg_max_locals_bound

## Summary
`return_value::set_arg` (vmhook.hpp:7510-7638) gained a `max_jvm_locals = 0xFFFF` upper bound check
in the Unreleased section of CHANGELOG.md to stop arbitrary `index > 65535` values from being
turned into `locals[-index]` writes that walk off the HotSpot interpreter local-variable array
into adjacent thread state (operand stack, saved registers, frame header). The cap is implemented
as a single combined check `if (!this->stack_frame || index < 0 || index > max_jvm_locals)`. The
fix is correct in spirit but has a few latent issues: the bound is the JVM-spec ceiling rather
than the per-method `_max_locals` (which would require new ConstMethod field plumbing), the test
coverage cannot actually distinguish the new guard from the pre-existing null-frame guard, and
the same `index < 0` / out-of-range hazard still exists in the sibling `frame::get_argument`
(vmhook.hpp:5160) and `frame::get_arguments` (vmhook.hpp:5063, 5084).

## Bugs

### [low] Sibling `frame::get_argument` and `frame::get_arguments` still lack the same bounds check
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:5063-5108, 5160-5202
- **description:** The bug this fix prevents (`locals[-index]` walking off the local-variable
  array) is symmetric: reads are just as unsafe as writes once `index > max_locals`. The two
  read paths `frame::get_arguments` (line 5063 / `locals[-slot_index]` at line 5084) and
  `frame::get_argument<T>(index)` (line 5160 / `locals[-index]` at line 5170) have no upper
  bound on `index` / `slot_index`. `get_argument` is templated over any signed `std::int32_t`,
  so a caller supplying a negative or very large index produces an out-of-bounds dereference
  of a `void**` interpreter slot. `get_arguments` is driven by the parsed signature so it is
  much harder to abuse, but `get_argument` is a public method that takes a raw `int32_t`.
- **repro:** From inside a hook callback, `auto x = ret.frame()->get_argument<int>(0x100000);`
  reads `locals[-1048576]` and returns whatever happens to live there (or crashes if the
  address is unmapped).
- **suggested_fix:** Factor the cap into a single inline helper used by all three sites:

  ```cpp
  // Inside namespace vmhook::hotspot, near frame:
  inline constexpr std::int32_t max_jvm_locals{ 0xFFFF };  // JVM spec u2 ceiling

  inline auto is_valid_local_slot(std::int32_t index) noexcept -> bool
  {
      return index >= 0 && index <= max_jvm_locals;
  }
  ```

  Then guard the start of both `get_argument` and the parameter loop in `get_arguments` with
  `if (!is_valid_local_slot(index)) return ...;`. The constant is also re-exported as a public
  `vmhook::max_local_slot_index` so callers can size their own loops without re-hardcoding 65535.
- **confidence:** likely

### [low] `index == 65535` is allowed but is invalid for any real HotSpot frame
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:7521-7522
- **description:** `max_locals` in the JVM spec is the *count* of slots, so the maximum legal
  slot *index* is `max_locals - 1 == 65534`. The current check `index > max_jvm_locals`
  permits `index == 65535`, which on the maximum-sized frame writes one word past the local
  array. The off-by-one is benign in practice (almost no methods get within an order of
  magnitude of this ceiling) but the comment claims "rejected anything past 65535", which is
  what `index >= max_jvm_locals` would actually do.
- **repro:** Static reasoning from the JVM spec ClassFile / Code_attribute definition.
- **suggested_fix:** Change `index > max_jvm_locals` to `index >= max_jvm_locals`, and update
  the log message to `"index >= JVM max_locals limit (65535)"`. Or, more usefully, separate
  the two clauses so the user gets a precise diagnostic instead of one combined sentence:

  ```cpp
  if (!this->stack_frame) {
      VMHOOK_LOG("{} return_value::set_arg(index={}): no stack_frame attached - this hook "
                 "was not registered with frame support.", vmhook::error_tag, index);
      return false;
  }
  if (index < 0) {
      VMHOOK_LOG("{} return_value::set_arg(index={}): index must be non-negative.",
                 vmhook::error_tag, index);
      return false;
  }
  if (index >= max_jvm_locals) {
      VMHOOK_LOG("{} return_value::set_arg(index={}): index exceeds JVM max_locals ceiling "
                 "(must be < {}). HotSpot rejects classfiles past this u2 cap.",
                 vmhook::error_tag, index, max_jvm_locals);
      return false;
  }
  ```
- **confidence:** likely

## Improvements

### [S] [USER_FACING] Diagnose the actual failure mode in the log line
- **rationale:** The single log message `"missing stack_frame, negative index, or index above
  JVM max_locals limit (65535) (stack_frame=0x…)"` forces the user to mentally OR three
  conditions and inspect the printed `stack_frame` pointer to know which one fired. Splitting
  the check (see the bug above) gives a precise message per failure path with no extra cost.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:7522-7529
- **suggested_change:** Replace the combined `if` with three sequential `if` statements, each
  with its own targeted `VMHOOK_LOG` (see suggested fix in the off-by-one bug above).

### [S] [USER_FACING] Publish `max_local_slot_index` as a public constant
- **rationale:** The CHANGELOG documents `65535` as the ceiling, but neither the public
  `return_value` API nor the docs expose it. A user writing a generic argument-injection
  helper has to either hard-code `0xFFFF`, parse the docs, or read the implementation.
  Exposing it as `vmhook::max_local_slot_index` (or similar) makes their bound check trivial
  and stays in sync with the implementation.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:7521 (currently a local `constexpr` inside
  `set_arg`)
- **suggested_change:** Lift to a public `inline constexpr std::int32_t max_local_slot_index
  { 0xFFFF };` next to `return_value` in the `vmhook` namespace, and use it from `set_arg`,
  the proposed `get_argument` guard, and the proposed `get_arguments` guard.

### [M] [USER_FACING] Tighten the bound to the per-method `_max_locals` from `ConstMethod`
- **rationale:** The current cap is the JVM-spec ceiling, not the actual method's
  `_max_locals`. A real method with 4 locals still allows writes at index 65534, which is far
  off the end of *that* frame's local array. Reading `ConstMethod._max_locals` (a `u2` field
  already present in HotSpot since JDK 8) via `iterate_struct_entries("ConstMethod",
  "_max_locals")` would let `set_arg` and the read paths reject any out-of-bounds slot for
  the actual current method, not just the spec ceiling.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:7510-7529 (set_arg), 5063-5067, 5160-5167
  (frame readers); follow the same pattern already used at vmhook.hpp:1977 / 2001 / 2045 for
  other ConstMethod fields.
- **suggested_change:** Add a `vmhook::hotspot::const_method::get_max_locals() const noexcept
  -> std::uint16_t` accessor and a wrapping `method::get_max_locals()`, cached the same way
  `_name_index` / `_signature_index` are cached. Use that as the upper bound when the frame's
  current `method*` is available, falling back to the spec ceiling only if the per-method
  lookup fails.

### [XS] [INTERNAL] Use the same constant as the rest of the file rather than `0xFFFF`
- **rationale:** Several places in the header already mention "u2" / "JVM spec 65535" in
  comments without sharing a single constant. The `set_arg` site introduces a fresh local
  `constexpr` instead of a header-wide named constant, which makes the next reviewer wonder
  whether 0xFFFF here is coincidence with the other 0xFFFFs in the file.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:7521
- **suggested_change:** Defined once, used by every site (see `max_local_slot_index` proposal
  above).

## Tests

### [standalone_unit] [extend_existing] test_return_value_set_arg_guard_messages_are_distinct
- **description:** The existing `test_return_value_set_arg_guards` (test_helpers.cpp:907)
  calls `set_arg(0x10000, …)` with a null frame and asserts `false`. Because the null-frame
  check fires *before* the max-locals check in the combined `if` (vmhook.hpp:7522), the test
  cannot distinguish whether the new max-locals guard actually rejects 0x10000 — refactoring
  the check to `>=` instead of `>` (or removing it entirely) would still leave the test
  green. Either split the implementation guards (see bugs above) and assert each branch with
  its own `VMHOOK_LOG` capture, or construct a fake `vmhook::hotspot::frame` whose
  `get_locals()` returns a sentinel that would crash if reached, so the max-locals check
  must fire ahead of it to keep the test from segfaulting.
- **asserts:** `set_arg(0x10000, …)` with a stub non-null frame still returns `false`
  without invoking `get_locals()`; the captured log message contains
  `"max_locals"` rather than `"missing stack_frame"`.
- **existing_file:** tests/test_helpers.cpp (`test_return_value_set_arg_guards`,
  test_helpers.cpp:907-931)

### [standalone_unit] [new] test_return_value_set_arg_boundary_at_65534_and_65535
- **description:** Lock down the exact ceiling. Documents which side of the u2 ceiling is
  inclusive vs exclusive so future refactors do not silently shift the bound by one.
- **asserts:**
  - `set_arg(65534, …)` is *not* rejected by the bound check (only the null-frame check
    fires; once a real frame is supplied this would proceed to `get_locals`).
  - `set_arg(65535, …)` matches whichever side of the documented boundary the team picks
    (current code allows it; the off-by-one fix would reject it).
  - `set_arg(65536, …)` is unconditionally rejected.
- **existing_file:** tests/test_helpers.cpp (`test_return_value_set_arg_guards`)

### [jvm_integration] [new] test_set_arg_above_max_locals_does_not_corrupt_frame
- **description:** With a real JVM-attached interpreter frame, calling
  `retval.set_arg(0x10000, std::int32_t{ 1 })` from inside a hook callback must return
  `false` and leave the original argument values intact when the original method body runs.
  Today the example.cpp test suite (example.cpp:2290+) only exercises the success path with
  a valid in-range index. A failing-path integration check would verify that the new bound
  also fires under the real interpreter, not just in unit tests with a null frame.
- **asserts:** Hook installed on `nonStaticArgMutationMe`; `set_arg(0x10000, 42)` returns
  `false`; the original method observes the original argument value (i.e. nothing was written
  past the local array, and no JVM crash on uninject).
- **existing_file:** vmhook/src/example.cpp (extend the existing `argMutation*` block around
  example.cpp:2290-2325)

### [standalone_unit] [new] test_frame_get_argument_rejects_out_of_range_index
- **description:** Cover the symmetric read path. Today `frame::get_argument<int>(0x100000)`
  has no bound check and would dereference `locals[-1048576]`. If the proposed shared
  `is_valid_local_slot` helper lands, this test asserts that both negative and
  above-max-locals indices return a default-constructed value instead of reading off the end.
- **asserts:** `frame::get_argument<int>(-1)` returns `0`; `frame::get_argument<int>(0x10000)`
  returns `0`; no crash with a null frame (already covered indirectly elsewhere).
- **existing_file:** tests/test_helpers.cpp

## Parity Concerns
- `frame::get_argument<T>(index)` (vmhook.hpp:5160) and the manual `locals[-slot_index]`
  lookup in `frame::get_arguments` (vmhook.hpp:5084) do not enforce any upper bound on
  `index`, despite using the exact same `locals[-index]` arithmetic that motivated this fix.
  The fix should be applied symmetrically — either factor the cap into a `hotspot::frame`
  helper used by all three sites, or document that the bound is intentionally one-sided.
- The existing `index < 0` rejection in `set_arg` is shared with `get_argument` only by
  implicit assumption: `get_argument`'s `std::int32_t` parameter has no `< 0` check either,
  and a negative `index` would read at a *positive* byte offset into the interpreter's
  expression-stack area, which is also dangerous.
- The fix's bound is the JVM spec ceiling, not the per-method `_max_locals`. The rest of the
  library already plumbs ConstMethod fields (vmhook.hpp:1977, 2001, 2045), so reading
  `_max_locals` for a tighter per-method bound would fit the established pattern.
