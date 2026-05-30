# return_value_caller_walk

## Summary
`return_value::caller()` and `return_value::stack_trace()` walk the HotSpot x64 interpreter saved-rbp chain (`[rbp+0]` = caller rbp, `[caller_rbp-24]` = caller `Method*`), gating every pointer with `is_valid_pointer`. The implementation is largely correct and matches `frame::get_method`'s `-24` offset, but the safety story has two real holes: (1) the raw `*current_rbp_slot` dereference is NOT fault-safe — `is_valid_pointer` only checks address bounds/alignment/sentinels, never asks the OS whether the page is committed — so a stack walk that strays past the thread-stack boundary into an unmapped or guard page will SIGSEGV; (2) there is no cycle / monotonicity check, so a corrupted frame whose saved-rbp equals (or is below) the current rbp will spin for `max_depth` iterations and may emit spurious duplicate frames. Both the architecture assumption (x86-64 only) and the JDK 8-25 stability of the `-24` offset are baked in without any compile-time or runtime guard.

## Bugs

### [high] Raw saved-rbp dereference can fault when walking past thread stack boundary
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:7346, 7355-7356, 7439, 7445-7446
- **description:** Both `caller()` and `stack_trace()` use unguarded raw dereferences (`*reinterpret_cast<void* const*>(current_rbp_slot)` and `*reinterpret_cast<vmhook::hotspot::method* const*>(caller_method_slot)`) to read the saved-rbp slot and the caller's `Method*`. The accompanying doc comment claims "Every dereference is gated by `is_valid_pointer` so a non-interpreter frame produces an early return rather than a crash" (line 7424-7426 / 7263-7265), but `is_valid_pointer` (line 1768-1805) does only address-range, alignment, and sentinel-pattern checks — it does NOT call `os::query_region` and does NOT use `ReadProcessMemory`/`safe_read`. Any saved-rbp value that survives those filters but points into an unmapped page, a stack guard page, a freed VirtualAlloc region, or a deallocated thread stack will crash the host JVM the moment the raw deref happens. The sister helper `is_readable_pointer` (line 1739-1753) does the committed-readable check via `os::query_region`, and `safe_read_pointer` (line 1838-1862) does the fault-safe read; both are unused here.
- **repro:** Hook a Java method whose interpreter caller is the last interpreter frame before a native trampoline at the thread-stack base; the saved-rbp slot reads the native sender frame address which may sit just below the guard page. Or: deopt thrashes during the walk and a freed nmethod-anchored frame leaves a saved-rbp pointing to a recently-unmapped page. `is_valid_pointer` returns true, the raw deref SIGSEGVs, JVM crashes with no recoverable hs_err for the user.
- **suggested_fix:** Replace each raw deref with `vmhook::hotspot::safe_read_pointer(current_rbp_slot)` (returns nullptr on fault) and `vmhook::hotspot::safe_read_pointer(caller_method_slot)`. Alternatively guard each read with `vmhook::hotspot::is_readable_pointer(slot)` before the raw deref — `query_region` is cheap and bounded. Apply consistently to BOTH `caller()` (lines 7341-7362) and `stack_trace()` (lines 7427-7452). While in the area, fix the misleading doc comment at lines 7263-7265 and 7424-7426 once the implementation actually does what it claims.
- **confidence:** certain

### [medium] No cycle / monotonicity check during saved-rbp walk
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:7433-7504
- **description:** `stack_trace()` reassigns `current_rbp_slot = caller_rbp` at the end of every iteration (line 7503) without verifying that `caller_rbp > previous_rbp_slot` (stacks grow down on x86-64; the caller's rbp is always at a higher address). A corrupt frame whose saved-rbp slot accidentally points at itself, at the current rbp, or at a lower address creates a loop that will run for the full `max_depth` (default 64), passing every `is_valid_pointer` check and emitting up to 64 duplicate frames in the output vector — or, worse, oscillate between two frames forever within the bound. The sibling `for_each_thread` walk explicitly handles this case with an `unordered_set<...>` visited set and an explicit cycle break (line 6552-6571), citing exactly the same risk class ("a corrupted intrusive list … can form a cycle, and without cycle detection we would happily visit the same entry up to 4096 times").
- **repro:** Manually construct (e.g. in a synthetic test) a fake frame chain whose saved-rbp slot points back to itself, then call `stack_trace()`. The result vector contains `max_depth` copies of the same `caller_info` instead of stopping at one frame.
- **suggested_fix:** Track the previous rbp and require strict monotonic ascent (`caller_rbp > current_rbp_slot`) before the next iteration; break on violation. Optionally also reject jumps larger than the per-thread stack size (typically ~1 MB) as a sanity cap. A tiny inline branch — no allocation — is sufficient; the unordered_set used by `for_each_thread` is overkill here because the chain is naturally linear.
- **confidence:** likely

### [medium] No architecture guard — ARM64 build silently walks wrong layout
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:7329-7407, 7409-7507
- **description:** The `-24` offset and `[rbp+0]` slot semantics come from `frame_x86.hpp` (interpreter_frame_method_offset = -3 words on x64) and are explicitly documented as x86-64 in `frame::get_method` (lines 4934-4948). The header declares `VMHOOK_ARCH_X86_64` and `VMHOOK_ARCH_ARM64` macros (lines 173-183), and other arch-sensitive code guards itself with `#if VMHOOK_ARCH_X86_64` (e.g. line 192). `caller()` and `stack_trace()` use neither a `#if` guard nor a runtime check — on an aarch64 build the layout is different (interpreter frame on aarch64 uses different sender_sp / method slot offsets in `frame_aarch64.hpp`), so the walk will read garbage. `is_valid_pointer` will reject most of it, but the user-visible result is "stack_trace() always returns empty" with no diagnostic; in the worst case a layout coincidence yields a confidently-wrong `caller_info`.
- **repro:** Cross-compile vmhook for `_M_ARM64` / `__aarch64__` and invoke `caller()` from a hook. The slot at `caller_rbp - 24` does not hold a `Method*` on aarch64; the result is at best garbage that fails `get_name()`'s length check, at worst a wrong-but-plausible frame entry.
- **suggested_fix:** Either (a) wrap the bodies in `#if VMHOOK_ARCH_X86_64 ... #else return {}; #endif` with a `VMHOOK_LOG` warning at first use on aarch64, or (b) factor the offset/layout out into `vmhook::hotspot::interpreter_frame::caller_rbp_offset` / `method_offset_from_rbp` constants chosen per arch. At minimum the doc comments (lines 1257-1270, 1273-1317, 7337-7356, 7422-7426) should note the x86-64-only constraint so users on aarch64 know why they always see `valid() == false`.
- **confidence:** likely

### [low] `max_depth` default and `0` promotion are duplicated, not derived
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:1316, 7413-7416
- **description:** The default `max_depth = 64` is hard-coded in two places: the declaration (line 1316) and the "promote zero" branch in the definition (line 7415). If a future change bumps the default in the declaration but forgets the definition (or vice-versa), `stack_trace(0)` returns a different cap than `stack_trace()` with no argument. The promote-zero behaviour is documented at line 1314-1315 ("Pass 0 for the default") so it is intentional, but the two literals can drift silently.
- **repro:** Change line 1316 from `64` to `128` and forget line 7415; `stack_trace()` returns up to 128 frames, but `stack_trace(0)` returns up to 64. The discrepancy is invisible until a user notices the asymmetric caps.
- **suggested_fix:** Define a single `static constexpr std::size_t default_stack_trace_depth{ 64 };` inside `return_value` (or in a nearby namespace) and use it in both the declaration default and the promote-zero branch. As a bonus, the constant becomes visible to users via `vmhook::return_value::default_stack_trace_depth`.
- **confidence:** certain

### [low] First frame's class-name lookup in `caller()` lacks the safety belts `stack_trace()` adds
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:7385-7404, 7475-7500
- **description:** `caller()` does the ConstMethod / ConstantPool / Klass / Symbol walk with only the first-tier null check (`if (const auto* const const_method{ caller_method->get_const_method() })`) — it never calls `is_valid_pointer` on `const_method`, `cp`, `slot`, or `name_symbol`. `stack_trace()` performs the SAME walk one block later (line 7475-7500) and explicitly wraps every step in `is_valid_pointer`, complete with a comment ("Class-name lookup is best-effort: when the stack walk strays into invalid territory the chain of pointer reads … can land on garbage that happens to pass the not-null check") that explains exactly why the extra guards are needed. By construction `caller()` is the same walk for the same first frame, so the same justification applies, but the gates are missing. In practice `klass::get_name()` (line 2530) already validates `this`, so the most dangerous step is gated downstream — but the `pool_holder` slot deref (line 7393-7394) and the deref of `cp` itself are not.
- **repro:** A caller frame whose `Method*` passes `is_valid_pointer` but whose `_constMethod` field happens to be a stale or garbage non-null value. `get_const_method()` returns it; the null check passes; the next pointer hop reads garbage. `stack_trace()` survives this because of its second-tier `is_valid_pointer` check; `caller()` does not.
- **suggested_fix:** Refactor the class-name resolution into a shared helper (e.g. `static auto resolve_class_name(method*) noexcept -> std::string`) used by both `caller()` and `stack_trace()`. That removes the divergence and makes the cache lookup of `pool_holder_entry` a single `static const` instead of two.
- **confidence:** likely

## Improvements

### [S] [INTERNAL] Factor the per-frame caller_info build into a shared helper
- **rationale:** `caller()` (lines 7329-7407) and `stack_trace()` (lines 7409-7507) duplicate ~50 lines of identical logic: validate `caller_rbp`, validate `caller_method_slot`, read `Method*`, read `get_name`, build `caller_info`, then walk ConstMethod → ConstantPool → Klass → Symbol for the class name. Drift between the two is already visible (see [low] bug above on safety divergence in the class-name walk). A single helper `auto build_caller_info_from_rbp(void* current_rbp_slot, const vm_struct_entry_t* pool_holder_entry) noexcept -> std::optional<std::pair<caller_info, void*>>` returning the populated info plus the next rbp slot eliminates the divergence and shrinks the implementation.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:7329-7507
- **suggested_change:** Extract a `namespace detail` helper that does one hop, returning `{caller_info, next_rbp_slot}` on success and `std::nullopt` on failure. `caller()` calls it once and returns the info; `stack_trace()` calls it in a loop until it returns nullopt or `max_depth` is hit.

### [XS] [USER_FACING] `caller_info` lacks an `is_compiled` / `is_native` hint
- **rationale:** The doc string promises "valid()=false for compiled/native callers" (line 1238-1240), so users have no way to distinguish "no caller" (top of stack) from "caller is compiled" or "caller is native" — both report `valid()==false` with empty fields. A common diagnostic question — "is my caller the JIT?" — currently requires the user to call `caller_method->get_code()` themselves, which is impossible when `caller_info.method == nullptr`.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:1241-1255
- **suggested_change:** Add an `enum class caller_kind { unknown, interpreted, compiled, native, no_frame };` field to `caller_info`. Populate it during the walk: `interpreted` on success, `no_frame` when `stack_frame` is null, `unknown` for the rest (with a TODO for the compiled/native disambiguation that may need `nmethod::is_in` or a return-address range check). Users get a precise reason instead of "valid() is false, why?".

### [XS] [USER_FACING] Document that `caller_info::method` may become dangling after class redefinition
- **rationale:** `caller_info::method` (line 1243) is a raw `vmhook::hotspot::method*` lifted off the interpreter stack. If `RedefineClasses` runs after the hook callback returns but before the user dereferences the saved pointer (e.g. they stash the trace in a log queue), the pointer can become a stale `Method*` to a freed MethodData. The current doc string says nothing about lifetime.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:1241-1255, 1273-1317
- **suggested_change:** Add a sentence to the `caller_info` doc: "The `method` pointer is only safe to dereference for the duration of the hook callback that produced it — RedefineClasses or class unloading can free the underlying Method*. Copy `class_name`/`method_name`/`signature` (already owned `std::string`s) if you need to retain the data."

### [S] [USER_FACING] `stack_trace()` silently swallows the reason it stopped
- **rationale:** The doc lists three terminal conditions ("max_depth reached", "saved-rbp fails validation", "Method* fails validation") at lines 1280-1285, but the returned `vector<caller_info>` carries no indication of which one fired. Users debugging "why does my trace stop at frame 3?" must add prints to the header itself. A more useful API returns a small wrapper, or sets a sticky reason on a sentinel last entry.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:7409-7507
- **suggested_change:** Add an overload `auto stack_trace_with_reason(std::size_t max_depth = 64) const noexcept -> std::pair<std::vector<caller_info>, stop_reason>` where `enum class stop_reason { max_depth_reached, rbp_invalid, method_invalid, method_name_empty, no_frame };`. Keep the existing `stack_trace()` as a thin wrapper that drops the reason. Users who need to diagnose silent truncations get a one-line answer; the common path is unchanged.

### [XS] [INTERNAL] Replace `vector::reserve(8)` magic literal with named constant
- **rationale:** Line 7428 reserves 8 frames as a starting capacity. The number is plausible but arbitrary and undocumented — a reader can't tell whether 8 was measured or guessed.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:7428
- **suggested_change:** `static constexpr std::size_t typical_stack_depth_hint{ 8 };` with a one-line comment explaining the heuristic (Java method frames on a hook callstack typically average 4-12).

### [M] [INTERNAL] `pool_holder_entry` `static const` cache duplicated across call sites
- **rationale:** Three locations (lines 7389-7390, 7430-7431, 11609-11610) each declare their own `static const auto* const pool_holder_entry{ iterate_struct_entries("ConstantPool", "_pool_holder") };` cache. The redundancy means a typo or future rename to `_holder` would have to be chased through three places, and `iterate_struct_entries` is run three times at first-call (one per cache).
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:7389-7390, 7430-7431, 11609-11610
- **suggested_change:** Add an inline accessor `inline auto cached_pool_holder_entry() noexcept -> const vm_struct_entry_t*` in `namespace hotspot`, with the `static const` cache inside the function. Replace the three duplicates with the helper.

## Tests

### [standalone_unit] [extend_existing] caller_walk_cycle_detection_does_not_loop
- **description:** Construct a tiny stack buffer that simulates an interpreter frame chain whose saved-rbp slot at offset 0 points back to itself (or to a previous slot). Call `stack_trace()` against this synthetic frame.
- **asserts:** result.size() is at most 2 (one frame, then break on the cycle) — current implementation will emit `max_depth` duplicates of the same frame because there is no monotonicity check.
- **existing_file:** tests/test_helpers.cpp (see existing `test_return_value_no_frame_helpers` at line 862)

### [standalone_unit] [extend_existing] caller_walk_max_depth_default_consistency
- **description:** Verify that `stack_trace()` and `stack_trace(0)` produce identical caps. The two literals live in different places (declaration default at line 1316, promote-zero branch at line 7415) and can silently drift.
- **asserts:** Given a long enough synthetic interpreter chain, `rv.stack_trace().size() == rv.stack_trace(0).size()`.
- **existing_file:** tests/test_helpers.cpp

### [standalone_unit] [extend_existing] caller_walk_returns_empty_when_first_rbp_unaligned
- **description:** Pass a `frame*` whose address is odd (1-aligned). `is_valid_pointer` rejects odd addresses, so the walk should bail at the first guard with empty result.
- **asserts:** caller().valid() == false; stack_trace().empty().
- **existing_file:** tests/test_helpers.cpp

### [standalone_unit] [extend_existing] caller_walk_returns_empty_when_saved_rbp_is_sentinel
- **description:** Construct a tiny buffer whose first 8 bytes hold one of the sentinel patterns rejected by `is_valid_pointer` (e.g. `0xCCCCCCCCCCCCCCCC`, `0xCAFEBABECAFEBABE`). Pass its address as the frame. The first iteration reads the sentinel as `caller_rbp` and must reject it.
- **asserts:** caller().valid() == false; stack_trace().empty().
- **existing_file:** tests/test_helpers.cpp

### [standalone_unit] [new] caller_walk_returns_empty_when_method_slot_holds_null
- **description:** Construct a synthetic two-frame buffer: outer rbp slot is followed by a +24 byte gap leaving the `Method*` slot at zero. The walk should read the slot, see null, and bail out.
- **asserts:** stack_trace() returns an empty vector; no crash.
- **existing_file:** tests/test_helpers.cpp (new test added next to the existing return_value helpers)

### [jvm_integration] [exists] caller_info_class_name_method_name_signature_populated
- **description:** Existing example in `vmhook/src/example.cpp` lines 2629-2706 (`test_caller_info`) hooks `caller_probe_class::innerStep`, checks `caller().valid()`, `caller_info.class_name == "vmhook/CallerProbe"`, `method_name == "outerStep"`, and verifies `stack_trace()` agrees with `caller()` at index 0.
- **asserts:** Already verified: `callerProbeHookInstalled`, `callerInfoMethodPtrFound`, `callerInfoMethodName`, `callerInfoClassName`, `stackTraceNonEmpty`, `stackTraceFirstMatchesCaller`, `stackTraceIncludesOuter`.
- **existing_file:** vmhook/src/example.cpp:2629

### [jvm_integration] [new] caller_walk_compiled_caller_yields_invalid
- **description:** Force the caller method to be JIT-compiled (warm it up enough to trigger C1/C2), then hook the callee. The doc promises `valid()==false` for compiled callers. The walk should bail at the `Method*` slot read because the compiled frame layout differs.
- **asserts:** `caller().valid() == false`; `stack_trace().empty()` (or only the first interpreted frame if there's still one on the stack).
- **existing_file:** Could extend `test_caller_info` in vmhook/src/example.cpp.

### [jvm_integration] [new] caller_walk_native_caller_yields_invalid
- **description:** Invoke a hooked Java method via JNI `CallStaticVoidMethodA` from a native test stub. The caller of the hooked method is the JNI call stub (native), not an interpreter frame; the doc promises `valid()==false`.
- **asserts:** `caller().valid() == false`; `stack_trace().empty()`.
- **existing_file:** new test in vmhook/src/example.cpp.

### [jvm_integration] [new] caller_walk_deep_recursion_capped_at_max_depth
- **description:** Build a Java method that calls itself recursively N times (N > 64), then hook the leaf. Verify `stack_trace()` returns exactly `max_depth` frames (default 64) and stops cleanly without spilling into the native sender frames.
- **asserts:** `stack_trace().size() == 64`; all entries have the same class/method/signature.
- **existing_file:** new test in vmhook/src/example.cpp.

### [jvm_integration] [new] caller_walk_explicit_small_max_depth_truncates
- **description:** Same setup as the recursion test, but call `stack_trace(3)`. Result must have exactly 3 entries.
- **asserts:** `stack_trace(3).size() == 3`; `stack_trace(1).size() == 1`.
- **existing_file:** new test in vmhook/src/example.cpp.

### [jvm_integration] [new] caller_walk_max_depth_zero_promotes_to_default
- **description:** Verify the documented "Pass 0 for the default" behaviour: with a deep recursion of >64 frames, `stack_trace(0)` returns 64 entries (the default), not 0.
- **asserts:** `stack_trace(0).size() == 64`.
- **existing_file:** new test in vmhook/src/example.cpp.

### [jvm_integration] [new] caller_info_survives_long_signature
- **description:** Hook a method whose caller's signature is unusually long (e.g. 200+ chars of nested generics post-erasure: many `Ljava/util/HashMap;` slots). Verify `caller_info.signature` is not truncated and matches the JVM-reported signature.
- **asserts:** `caller_info.signature.size() > 200`; `signature == expected_signature`.
- **existing_file:** new test in vmhook/src/example.cpp.

## Parity Concerns
- `caller()` and `stack_trace()` duplicate the per-frame build logic without sharing a helper, which has already caused divergence in the safety belts around the class-name walk (`stack_trace()` gates each pointer hop with `is_valid_pointer`, `caller()` only checks for null). Any future fix to one will need to be mirrored to the other or the divergence will widen.
- The saved-rbp deref pattern here uses raw `*reinterpret_cast<...>(p)`, while the rest of the codebase (klass::get_name at 2547, class_loader_data accessors at 3126/3157, dictionary lookups at 3285+) uses `safe_read_pointer` for stack-walk-style chasing. The asymmetric safety story is the most visible API parity gap.
- `method::get_name` / `method::get_signature` validate `this` before deref (lines 2250, 2287); the `caller_method->get_name()` call at line 7371/7458 relies on that downstream validation. That works today but couples the two layers. If `get_name` ever drops the validation, the stack walk will crash silently.
- Three sites (`return_value::caller`, `return_value::stack_trace`, `method_proxy::call_jni` at 11609) each maintain their own `static const auto* pool_holder_entry` cache. A single `cached_pool_holder_entry()` accessor would remove the duplication.
- The implementation is x86-64 only by hardcoded offset (`-24`) and slot layout (`[rbp+0]` = caller rbp), but unlike other arch-sensitive blocks in the header (line 192) there is no `#if VMHOOK_ARCH_X86_64` guard, no fallback `return {}`, and no warning to the user that aarch64 builds will always see `valid()==false`.
