# return_value_frame_raw_access

## Summary
`return_value::frame()` is a one-line accessor that returns the raw `hotspot::frame*` the trampoline stashed in the `return_value` at construction time. It is an escape hatch for users who want to walk the interpreter stack themselves; the underlying pointer is the live `rbp` of the intercepted frame, so it is valid only while the hook callback is on the stack. The implementation is correct, but the public contract under-documents the lifetime and platform constraints, and the test surface only exercises the null case.

## Bugs

### [low] Doc comment omits lifetime / "valid only inside the hook" guarantee
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:1319-1329
- **description:** The Doxygen block on `frame()` says only "Returns the intercepted interpreter frame, or nullptr." and "Exposed for advanced use cases that need to walk the call stack themselves; callers should prefer `caller()` when only the immediate caller is required." It does not state that the returned pointer is the live `rbp` of an interpreter frame, that it must not be cached past the callback return, that it must not be used from another thread, that all reads off it must be safe-pointer gated, or that the frame layout it points at is x64 HotSpot only. Sibling helpers (`set_arg`, `caller`, `stack_trace`) all carry an explicit "Thread safety: must be called only from the hook callback" note (e.g. line 1221, 1269, 1309-1311); `frame()` does not, despite being a strictly more dangerous API because the user is now driving the dereference themselves.
- **repro:** A user reads the Doxygen and writes `auto* f = ret.frame(); g_saved_frame = f;` then dereferences `g_saved_frame->get_method()` from a worker thread after the hook returns. The pointer is to a stack slot that has since been overwritten by a different interpreted method (or by C++ frames after the trampoline returned), producing silent corruption or an AV.
- **suggested_fix:** Expand the `@details` to match the sibling notes: add "Lifetime: valid only while the hook callback is on the stack — the pointer is the live `rbp` of the intercepted interpreter frame and points into the trampoline's own activation. Do not store, copy across threads, or use after the callback returns.", "Platform: x64 HotSpot interpreter only — returns a pointer whose layout matches `frame_x86.hpp`; meaningless on compiled / native / non-x64 frames.", and the standard "Thread safety: must be called only from the hook callback" line that the sibling APIs use. Also document that `frame()->get_locals()` may return `nullptr` on JDKs where the locals_offset scan failed.
- **confidence:** certain

### [low] No `[[nodiscard]]` on `frame()` — silently dropping the only return value is harmless but pointless
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:1326-1329
- **description:** `frame()` is a pure accessor whose sole purpose is to hand the caller a pointer. Forgetting to use the return value is always a bug (the call has no side effects). The sibling `caller_info::valid()` accessor at line 1251 isn't `[[nodiscard]]` either, but `frame()` is the more dangerous one to drop because there is no compiler help if the user typed `ret.frame()->get_method();` versus `ret.frame();` by accident.
- **repro:** `ret.frame();` compiles cleanly with no diagnostic; the user expected to dereference it.
- **suggested_fix:** Mark the accessor `[[nodiscard]]`: `[[nodiscard]] auto frame() const noexcept -> vmhook::hotspot::frame*`.
- **confidence:** certain

## Improvements

### [XS] [USER_FACING] Mention `frame()` alongside `caller()` / `stack_trace()` in the class-level `return_value` Doxygen example
- **rationale:** The class-level docs at vmhook.hpp:1124-1137 show `set()` and `cancel()`. `caller()` and `stack_trace()` are advertised further down, but `frame()` is buried under a one-paragraph "advanced use cases" note that doesn't appear in any example. A single line like "// or walk the raw interpreter stack: ret.frame()->get_method()" in the example would surface its existence to users skimming the class header.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:1124-1137
- **suggested_change:** Add a third bullet to the "Call set / cancel" docstring with one inline snippet showing `ret.frame()->get_method()` (or a link reference back to the `frame()` accessor), and explicitly cross-reference `hotspot::frame::get_arguments` / `get_locals` since those are the most useful things to do with a raw frame pointer.

### [XS] [USER_FACING] Make the implicit recommendation explicit: "prefer `caller()` / `stack_trace()` / `get_arguments()` to walking the raw frame"
- **rationale:** The current "callers should prefer caller() when only the immediate caller is required" line at 1323-1324 reads as a half-hearted nudge. A user holding a raw `frame*` will reach for pointer arithmetic; the header already provides `frame::get_method`, `frame::get_locals`, `frame::get_arguments`, and `return_value::stack_trace` which cover almost every realistic use case. Listing them in the `@see` block (or a short bulleted alternative list) inside the `frame()` docs would steer most users away from the raw escape hatch.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:1319-1329
- **suggested_change:** Replace the single-sentence advisory with an explicit list: "Prefer one of these higher-level helpers: `caller()` for the immediate caller, `stack_trace()` for an interpreter call stack, `frame()->get_arguments()` for the typed argument tuple, `set_arg()` for in-place mutation. Reach for `frame()` only when none of those covers the layout you need."

### [S] [USER_FACING] Move `frame()` definition out-of-line for symmetry with `caller()` / `stack_trace()` and to unblock a future debug-mode assert
- **rationale:** Every other interpreter-walk helper on `return_value` is declared inline-in-class and defined out of line (see `caller` at 7329 and `stack_trace` at 7409). `frame()` is the odd one — the whole body is in the class. That asymmetry is minor today, but it also means there is nowhere to drop a `VMHOOK_LOG` "frame() called from non-hook context" warning or a debug assertion that the current thread is the one holding the trampoline activation. Pulling the body out of the class header (keeping it `inline`) costs nothing and lets future audits add diagnostics without forcing a recompile of every TU that already includes vmhook.hpp.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:1326-1329
- **suggested_change:** Forward-declare `auto frame() const noexcept -> vmhook::hotspot::frame*;` inside the class and move the one-line body next to `caller()`'s out-of-line definition at line 7329.

### [XS] [INTERNAL] Rename internal member to match the accessor (`stack_frame` -> `frame_pointer`)
- **rationale:** The constructor parameter is `frame`, the accessor is `frame()`, but the field is `stack_frame` (line 1333). Internal call sites read `this->stack_frame` (lines 7332, 7341, 7417, 7427, 7522, 7531). A reader scanning between the public API and the private state has to remember the rename. Aligning the field name with the accessor would also let a future `[[deprecated]] frame() -> ...` cleanup land without renaming the member.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:1141-1142,1333,7329-7507
- **suggested_change:** Rename the member from `stack_frame` to `frame_pointer` (matches the trampoline argument at line 7806) and update the six interior references. Pure refactor, no behaviour change.

## Tests

### [standalone_unit] [extend_existing] return_value_frame_accessor_round_trips_non_null
- **description:** The existing `test_return_value_no_frame_helpers` only verifies that `frame()` returns `nullptr` when constructed with a null frame (line 896). It never checks the symmetric path — that a non-null pointer handed to the constructor is returned identity-equal. A typo like `return this->return_slot;` in the body would slip past CI today.
- **asserts:** Construct `return_value rv{ &slot, reinterpret_cast<hotspot::frame*>(0xDEADBEEF) }`; assert `rv.frame() == reinterpret_cast<hotspot::frame*>(0xDEADBEEF)`. Also assert that the accessor is `const`-callable (compile-only check by binding to `const auto& cref = rv; cref.frame();`).
- **existing_file:** tests/test_helpers.cpp (extend `test_return_value_no_frame_helpers` or add a sibling test next to it).

### [standalone_unit] [new] return_value_frame_accessor_is_nodiscard
- **description:** If the suggested `[[nodiscard]]` improvement is adopted, lock it in with a `static_assert`-style test that the attribute is actually present. Easiest form is a SFINAE/`requires`-based compile-time probe via `__has_cpp_attribute` plus a `#pragma` warning check, or a runtime test that calls the accessor and asserts the warning is enabled in the build (CI-only). Without this guard, a future refactor can silently strip the attribute.
- **asserts:** Compile-time `static_assert` that the attribute is present (or, more pragmatically, a `// expected-warning` style annotation if the project ever adopts clang's verify mode).
- **existing_file:** tests/test_traits.cpp (already houses compile-time trait checks).

### [jvm_integration] [new] return_value_frame_method_matches_hooked_method
- **description:** Inside a real hook detour, call `ret.frame()->get_method()` and assert the returned `method*` is the same one the hook was installed on (i.e. `g_hooked_methods.back().method`). This is the only test that exercises the "raw access actually works on a live interpreter frame" guarantee that motivates the API.
- **asserts:** `ret.frame() != nullptr`; `ret.frame()->get_method() != nullptr`; `ret.frame()->get_method()->get_name() == "<expected name>"`; `ret.frame()->get_locals() != nullptr` for a method with at least one local.
- **existing_file:** none currently — fold into whichever JVM-integration suite covers `caller()` / `stack_trace()` so the three are tested together.

### [jvm_integration] [new] return_value_frame_locals_round_trip_with_set_arg
- **description:** Verify that the pointer returned by `frame()` aliases the same locals array that `set_arg()` mutates: write a value via `ret.set_arg(0, 0x1234)`, then read `ret.frame()->get_locals()[-0]` (the index sign-flip used by the JDK 21+ path at vmhook.hpp:5084) and assert they agree. Catches future drift between the two code paths.
- **asserts:** `ret.frame()->get_locals()[0]` reflects the value written by `set_arg(0, ...)` (handle the sign convention used by `get_arguments` / `set_arg`).
- **existing_file:** none currently — pair with the `set_arg` integration test.

### [jvm_integration] [new] return_value_frame_get_arguments_typed_matches_get_arguments_autodetect
- **description:** Cross-check that the templated `frame::get_arguments<int, jstring>()` (line 5016) and the descriptor-driven `frame::get_arguments()` (line 5038) both reach through `ret.frame()` and produce equivalent argument lists. This is the most realistic "advanced use case" that the docs hint at; without coverage, a regression in either path goes unnoticed when called via the raw frame accessor.
- **asserts:** For a hooked `static int foo(int x, Object o)`: `auto [xv, ov] = ret.frame()->get_arguments<int32_t, void*>();` matches the auto-detected `ret.frame()->get_arguments().as<int32_t>(0)` / `.as<Object>(1)` values.
- **existing_file:** none currently — new integration test file or fold into the existing arguments suite.

## Parity Concerns
- `caller()` (line 1271), `stack_trace()` (line 1316), and `set_arg()` (line 1230) all carry an explicit `Thread safety: must be called only from the hook callback` note; `frame()` does not (line 1319-1329). Same audience, same constraint — should be stated identically.
- Sibling raw-pointer accessors elsewhere in the header (e.g. `object_base::oop()`, field-proxy raw getters) tend to be marked `[[nodiscard]]`. `frame()` is the obvious candidate for the same treatment.
- The constructor parameter is `frame` and the accessor is `frame()`, but the private member is `stack_frame`; the trampoline calls its local `frame_pointer` (line 7806). Three different names for the same value across one feature — pick one and propagate.
- The README at line 419 already tells the user "walking deeper requires reading saved-rbp chains manually starting from `ret.frame()`" — that hint is more concrete than the header's own docstring. The header should mirror the README's framing rather than leave it stranded in prose.
