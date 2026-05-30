# return_value_stack_trace_depth

## Summary
`return_value::stack_trace(max_depth=64)` walks HotSpot's interpreter saved-rbp chain, validating every pointer with `is_valid_pointer` and yielding a `std::vector<caller_info>` ordered immediate-caller first. The implementation is defensive against most hostile inputs but has one real safety gap (no monotonicity / cycle check on the rbp chain, so a corrupted saved-rbp slot can spin the walk for the full `max_depth` instead of bailing) plus several small UX / parity / efficiency rough edges around the `max_depth` contract, the hardcoded `reserve(8)`, the redundant `get_name() + get_signature()` work, and the platform-degradation surface on arm64.

## Bugs

### [medium] Saved-rbp chain has no monotonicity / cycle check — corrupted frame can spin the walk for `max_depth` iterations
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:7433-7504
- **description:** The loop reads `caller_rbp = *current_rbp_slot` then assigns `current_rbp_slot = caller_rbp` for the next iteration. The only gate on `caller_rbp` is `is_valid_pointer`, which is a pure range/alignment/sentinel check — it accepts any 8-byte-aligned user-space pointer that is not one of the debug-fill patterns. If the saved-rbp slot was corrupted such that `caller_rbp == current_rbp_slot` (self-cycle) or `caller_rbp < current_rbp_slot` (chain pointing *down* the stack, impossible for a legitimate interpreted frame because the stack grows down so the caller's frame must be at a *higher* address), the walk happily continues. In the self-cycle case the loop re-reads the same Method* up to `max_depth` times and `push_back`'s the same `caller_info` 64 times by default; in the worst case the walk strays into a mapped-but-unrelated region and the inner `get_const_method() + get_constants() + Klass*` chain (which only does range checks, not safe-reads — see `get_const_method` at vmhook/ext/vmhook/vmhook.hpp:2219-2238 which does an unguarded `*(this + entry->offset)`) can fault. The sibling walker `for_each_thread` learned this lesson and added explicit cycle detection (vmhook/ext/vmhook/vmhook.hpp:6552-6571) — `stack_trace` should mirror that pattern.
- **repro:** Hook any interpreted Java method, then before the hook fires, overwrite the saved-rbp slot of the intercepted frame with the address of that same slot (i.e. point it at itself). On the next invocation, `stack_trace()` returns a 64-element vector all containing identical method-name / class-name strings — observable from the example.cpp probe by asserting `trace.size() <= some_sane_bound` and that no two consecutive frames have identical `method` pointers.
- **suggested_fix:** Track the rbp values already visited (small `boost::container::small_vector<void*, 16>` or a fixed-size on-stack array up to `max_depth`), and break out when either (a) the new `caller_rbp` has been seen before, or (b) `caller_rbp <= current_rbp_slot` (must strictly grow on x86_64). Add a comment matching the for_each_thread justification.
- **confidence:** likely

### [low] Unbounded vector growth from a runaway walk — `frames.reserve(8)` is decoupled from `max_depth`
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:7428, 7502
- **description:** `frames.reserve(8)` is hard-coded and bears no relation to `max_depth`. When called with the default 64 (or a larger explicit value), the vector silently re-allocates / copies under the hood as it crosses the 8 / 16 / 32 / 64 capacity boundaries — costing 3 reallocations and 3 moves of `std::string`-bearing `caller_info` per call for a deep stack. When called with a small `max_depth` (e.g. 4), the over-reservation wastes ~6 `caller_info` slots that will never be filled. Either way the user pays for the mismatch on every detour invocation, which on a hot-path interpreted method is non-trivial. The documented complexity is `O(D)` — a deterministic `reserve(std::min(max_depth, 64u))` keeps that promise without ever over-allocating.
- **repro:** Call `stack_trace(128)` from a detour on a method invoked by a deeply-nested interpreted recursion; observe via a tracing allocator that the underlying buffer is reallocated multiple times before the final size is known.
- **suggested_fix:** Replace `frames.reserve(8);` with `frames.reserve(std::min<std::size_t>(max_depth, 64));` (mirror the documented default cap so we never over-reserve on tiny `max_depth` calls and never under-reserve on the default).
- **confidence:** certain

### [low] `get_name()` called twice per frame — redundant Symbol → std::string copy
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:7458-7467
- **description:** `method_name = caller_method->get_name()` is invoked once just to test the validity-via-non-empty contract, then `info.signature = caller_method->get_signature()` runs immediately after — both `get_name` and `get_signature` separately re-run `is_valid_pointer(this) + get_const_method() + is_valid_pointer(const_method) + symbol->to_string()`. On a deep trace this doubles the per-frame syscalls / VMStruct cache lookups. Not a correctness bug but worth folding into the get_const_method() result that we already need below.
- **repro:** N/A — performance smell, observable as duplicate work in a profiler.
- **suggested_fix:** Read the `const_method` pointer once at the top of each iteration, validate it, then derive `method_name` and `signature` from it directly (mirroring how `caller()` does the same back-pointer dance but in one pass).
- **confidence:** certain

## Improvements

### [S] [USER_FACING] Document and enforce the "max_depth=0 means default" contract more cleanly
- **rationale:** The current API has two ways to ask for the default: omit the argument, or pass `0`. The docstring at vmhook.hpp:1313-1314 notes the magic-zero behavior, but a user reading the call-site `stack_trace(0)` would reasonably expect zero frames back. Either drop the magic-zero treatment (an explicit `0` should return an empty vector — matches the principle of least surprise and matches how `set_arg(0, ...)` does NOT silently substitute a default) or rename the magic value to a named constant like `vmhook::stack_trace_default_depth`. Today the test suite even asserts the magic behavior (test_helpers.cpp:884-888), which makes it a published contract.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:7413-7416, 1313-1314
- **suggested_change:** Drop lines 7413-7416 entirely and let `max_depth == 0` fall through the `for` loop to return an empty vector; update the docstring to say "Pass 0 to request zero frames (returns an empty vector)." Or, if back-compat with the existing test is required, introduce a `static constexpr std::size_t default_stack_trace_depth = 64;` at namespace scope, default the parameter to that constant, and reference it in the docstring instead of the literal `64`.

### [S] [USER_FACING] Surface the `max_depth` reached vs the limit so callers can detect truncation
- **rationale:** When `frames.size() == max_depth`, the caller cannot tell whether the trace was truncated by the depth cap or naturally ended at exactly that depth. Both `caller()` and `stack_trace` already use the same `caller_info` shape, so adding a `bool truncated` to the return type would be a breaking change; an alternative is a sibling overload that returns `std::pair<std::vector<caller_info>, bool>` or a simple comparison-friendly result.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:7409-7507, 1316-1317
- **suggested_change:** Either (a) document explicitly that "frames.size() == max_depth indicates the trace may have been truncated; pass a larger max_depth to disambiguate" in the existing docstring, or (b) provide a second overload `stack_trace(std::vector<caller_info>& out, std::size_t max_depth)` that returns `bool truncated` so users can detect the boundary case without re-running the walk.

### [S] [USER_FACING] Provide a callback overload to avoid the `std::vector<caller_info>` allocation cost on hot paths
- **rationale:** Every detour invocation that calls `stack_trace()` heap-allocates a `std::vector<caller_info>` plus 3 `std::string`s per frame. On a hot-path hook (e.g. on a method invoked thousands of times per second) this is unacceptable overhead just to find one specific frame. The sibling APIs `for_each_thread`, `for_each_loaded_class`, and `for_each_instance` already use the callback pattern for exactly this reason.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:7316
- **suggested_change:** Add an overload `template<typename visitor_t> auto stack_trace(visitor_t&& visit, std::size_t max_depth = 64) const noexcept -> std::size_t;` where the visitor receives `(std::size_t depth_index, const caller_info&)` and may return `bool` (false to stop early); return total frames visited. Keep the existing vector-returning overload as a convenience wrapper.

### [XS] [INTERNAL] Hoist the `pool_holder_entry` lookup to file scope so `caller()` and `stack_trace()` share it
- **rationale:** Both `caller()` (line 7389-7390) and `stack_trace()` (line 7430-7431) declare a `static const auto* const pool_holder_entry{...}` local. They resolve the same VMStruct entry. Hoisting to a single `inline static const auto*` at namespace scope eliminates the duplicate first-call cost and ties both implementations to one source of truth.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:7389-7390, 7430-7431
- **suggested_change:** Move the `iterate_struct_entries("ConstantPool", "_pool_holder")` lookup into a file-scope `inline static const vmhook::hotspot::vm_struct_entry_t* const pool_holder_entry{ ... }` (or a small helper `static auto& get_pool_holder_entry()`) and reference it from both functions.

### [S] [INTERNAL] Extract the per-frame Method → caller_info population into a shared helper
- **rationale:** Lines 7464-7500 of `stack_trace` and lines 7377-7404 of `caller` are almost identical: build `caller_info`, populate `method`/`method_name`/`signature`, then do the best-effort class-name lookup via ConstMethod → ConstantPool → Klass → Symbol. Duplicated logic invites divergence (e.g. `stack_trace` already added `is_valid_pointer(slot)` and `is_valid_pointer(name_symbol)` guards that `caller()` does NOT have — see line 7485 vs no equivalent at line 7394, and line 7492 vs no equivalent at line 7397). Pull the shared logic into a `static caller_info populate_caller_info(method*, const vm_struct_entry_t*)` helper in the anonymous detail namespace.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:7329-7407, 7409-7507
- **suggested_change:** Add `namespace detail { static caller_info populate_caller_info(vmhook::hotspot::method* m, const vmhook::hotspot::vm_struct_entry_t* pool_holder_entry) noexcept; }` and rewrite both functions to delegate. Bonus: when fixing the parity gap below (see "Parity Concerns"), only one place changes.

### [XS] [USER_FACING] Note the platform contract on the `stack_trace` doc comment
- **rationale:** The doc at vmhook.hpp:1273-1317 talks about x64 interpreter layout in passing ("HotSpot x64 interpreter" appears in `caller()` doc at line 1262) but never tells the reader that the function compiles on arm64 yet always returns an empty vector there. Surface the same `VMHOOK_RUNTIME_HOOKING_AVAILABLE` caveat that `hook<T>()` uses, otherwise users porting to arm64 get silent empty results instead of a clear error.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:1273-1317
- **suggested_change:** Add a line to the doc: "Platform: x86_64 only — on arm64 builds the function always returns an empty vector because `vmhook::hook<T>` itself is unavailable (see `VMHOOK_RUNTIME_HOOKING_AVAILABLE`)."

## Tests

### [standalone_unit] [extend_existing] stack_trace_promotes_zero_depth_to_default
- **description:** Verify the documented "Pass 0 for the default" contract: calling `stack_trace(0)` on a return_value with a non-null frame must walk up to 64 frames, not zero. (Currently `test_return_value_no_frame_helpers` asserts the empty-vector outcome only for the null-frame path, so the magic-zero promotion is untested when a frame is present.)
- **asserts:** `rv.stack_trace(0).size() <= 64`; if implementation changes to drop the magic-zero behavior, this test catches it.
- **existing_file:** tests/test_helpers.cpp (extend `test_return_value_no_frame_helpers` or add a new fixture with a synthetic frame).

### [standalone_unit] [new] stack_trace_max_depth_caps_size
- **description:** Construct a synthetic interpreter-style stack as a static array of `void*` slots wired into a saved-rbp chain (each slot points to the next slot's address, with a fake Method* at offset -24 that returns "fake" from get_name() — easiest done via a stubbed klass/const_method). Call `stack_trace(max_depth=3)` and assert exactly 3 frames returned, then call with `max_depth=100` and assert it terminates at the chain's tail without faulting.
- **asserts:** `rv.stack_trace(3).size() == 3`; `rv.stack_trace(100).size() == chain_length`; no use-after-end, no crash.
- **existing_file:** tests/test_helpers.cpp (new fixture; the existing one only covers the null-frame path).

### [standalone_unit] [new] stack_trace_terminates_on_self_cycle
- **description:** Build a one-slot synthetic frame whose saved-rbp slot points to itself. Call `stack_trace(max_depth=64)` and assert that either (a) after the fix, exactly one frame is returned (cycle detected, walk stopped), or (b) the call returns without infinite-looping (today's behavior: 64 identical frames). This is the regression test for the cycle-detection bug above.
- **asserts:** With the fix: `frames.size() <= 1` and no two consecutive entries share the same `method` pointer. Without the fix (negative-control assertion for documenting current behavior): `frames.size() <= 64`.
- **existing_file:** tests/test_helpers.cpp (new fixture).

### [standalone_unit] [new] stack_trace_terminates_on_descending_rbp
- **description:** Synthetic chain where `*saved_rbp` deliberately points to a *lower* address (impossible on a real stack, but possible via corruption). Assert the walk terminates immediately (after the fix) rather than walking into garbage.
- **asserts:** `frames.empty()` or `frames.size() == 1` (depending on whether the descending edge is detected on the first hop or the second).
- **existing_file:** tests/test_helpers.cpp (new fixture).

### [standalone_unit] [extend_existing] stack_trace_reserves_only_what_it_needs
- **description:** After fixing the `reserve(8)` mismatch, verify `frames.capacity()` is at most `std::min(max_depth, 64)` on return, for `max_depth` values of 1, 4, 8, 64, and 128. Catches future regressions where someone re-introduces an over-reservation.
- **asserts:** `rv.stack_trace(N).capacity() <= std::min(N, 64)` for `N in {1, 4, 8, 64, 128}`.
- **existing_file:** tests/test_helpers.cpp.

### [jvm_integration] [extend_existing] stack_trace_returns_truncated_when_max_depth_below_real_depth
- **description:** Extend the existing `callerProbe` JVM test by adding a deeper outerStep → middleStep → innerStep call chain. Inside the innerStep detour, call `stack_trace(2)` and assert exactly 2 frames are returned (truncated) and that `trace[0]` is `middleStep`, `trace[1]` is `outerStep`. Then call `stack_trace(64)` and assert the trace contains both `middleStep` and `outerStep` somewhere.
- **asserts:** truncated trace size, ordering (middle before outer), presence of the deeper frame in the un-truncated trace.
- **existing_file:** vmhook/src/example.cpp (extend the `caller_probe_class` block around line 2629-2706).

### [jvm_integration] [new] stack_trace_walk_terminates_at_first_compiled_frame
- **description:** Use a callerProbe variant where outerStep is forced to JIT-compile (e.g. warm it up before installing the hook). Assert that `stack_trace()` returns 0 or 1 frame (the JIT'd caller frame breaks the interpreter saved-rbp layout and the walk should bail at it). Documents the "Stops at the first compiled / native frame" CHANGELOG promise (CHANGELOG.md:156-159).
- **asserts:** `trace.size() <= 1` when the immediate caller is JIT-compiled; the existing `stackTraceIncludesOuter` assertion would FLIP to false in that case, confirming the walk really terminated at the compiled boundary.
- **existing_file:** vmhook/src/example.cpp (new sub-block alongside the existing callerProbe).

### [jvm_integration] [new] stack_trace_returns_empty_on_arm64
- **description:** Guard with `#if !VMHOOK_RUNTIME_HOOKING_AVAILABLE`: on arm64 / iOS, asserting `stack_trace()` returns an empty vector even when called from a (failed) hook attempt would document the platform contract.
- **asserts:** `rv.stack_trace().empty()` on a build where `VMHOOK_RUNTIME_HOOKING_AVAILABLE == 0`.
- **existing_file:** tests/test_helpers.cpp (compile-time-gated block).

## Parity Concerns
- `stack_trace()` adds `is_valid_pointer(slot)` (line 7485) and `is_valid_pointer(name_symbol)` (line 7492) guards that the sibling `caller()` lacks at the matching lines 7394 and 7397. Either `caller()` is missing those guards (and can crash on the same garbage-Klass scenario that `stack_trace()` was hardened against, per the comment at line 7469-7474) or `stack_trace()` is over-cautious. Extract the shared helper (improvement above) so the two never diverge again.
- `for_each_thread` (vmhook.hpp:6547-6580) implements an explicit cycle-detection + monotonicity guard on its intrusive-list walk; `stack_trace` walks a structurally similar pointer chain (saved-rbp chain) and should adopt the same pattern.
- The CHANGELOG entry at CHANGELOG.md:156-159 promises "Stops at the first compiled / native frame so the result reflects only the interpreted portion of the call stack." The implementation gates that promise on `is_valid_pointer` succeeding on the next saved-rbp slot — which it might pass for a compiled frame whose rbp value happens to fall in a mapped region. There is no positive identification of "this is a compiled frame" (e.g. by consulting CodeCache bounds) — the walk just bails on the first read that looks wrong. Document this caveat or strengthen the check.
- `frame()` is exposed publicly (vmhook.hpp:1326-1329) so advanced callers can walk the stack themselves, but no helper is provided for doing so safely — every consumer must re-implement the rbp+0 / Method* at rbp-24 dance. Consider exposing a `vmhook::hotspot::walk_interpreter_frames(void* rbp, callback)` free function so `stack_trace()`, `caller()`, and external advanced callers all share one implementation.
