# deoptimize_methods_if_predicate

## Summary
`vmhook::deoptimize_methods_if(predicate)` (vmhook.hpp:6391-6454) sweeps every loaded class, calls the user predicate for each JIT-compiled method and deoptimises the ones the predicate selects. The implementation correctly mirrors the per-hook install ordering (set_from_interpreted_entry → set_from_compiled_entry → set_code(nullptr)), but it is declared `noexcept` while invoking a user-supplied callable without any try/catch, so a throwing predicate calls `std::terminate`. It also diverges in policy from the per-hook install path and from `verify_hooks` repair when the c2i adapter cannot be recovered, and the deopt counter is incremented even when the underlying VMStruct-backed pointer writes silently no-op.

## Bugs

### [HIGH] Throwing predicate calls std::terminate via noexcept boundary
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:6392-6454
- **description:** `deoptimize_methods_if` is declared `noexcept` but invokes the user-supplied `predicate(class_name, m)` (line 6422) inside the inner `for_each_loaded_class` lambda with no try/catch around the call. Anything the predicate throws unwinds out of the inner lambda, through `for_each_klass`, into `for_each_loaded_class`'s `catch (const std::exception&)` block (line 6333-6336). That catches `std::exception` *but* the outer `deoptimize_methods_if` itself is `noexcept` — so the exception is technically handled by `for_each_loaded_class` and the noexcept boundary is not crossed for std::exception derivatives. HOWEVER, anything that is NOT a `std::exception` (an `int`, a `std::string`, an `enum class` error, a `const char*`, a user exception that does not inherit from `std::exception`) escapes the catch, then crosses the noexcept boundary of `deoptimize_methods_if` and calls `std::terminate`. The documentation block (line 6376-6388) gives no warning that the predicate must be no-throw or that only `std::exception`-derived exceptions are tolerated.
- **repro:**
  ```cpp
  vmhook::deoptimize_methods_if(
      [](const std::string& name, vmhook::hotspot::method*) -> bool
      {
          throw 42;  // not std::exception — terminates
          return false;
      });
  ```
- **suggested_fix:** Either (a) wrap the predicate call in `try { if (!predicate(class_name, m)) continue; } catch (const std::exception& ex) { VMHOOK_LOG("{} deoptimize_methods_if: predicate threw on '{}': {}", vmhook::error_tag, class_name, ex.what()); continue; } catch (...) { VMHOOK_LOG("{} deoptimize_methods_if: predicate threw non-std::exception on '{}'.", vmhook::error_tag, class_name); continue; }`, or (b) add a `static_assert(std::is_nothrow_invocable_r_v<bool, predicate_type, const std::string&, vmhook::hotspot::method*>, "predicate must be noexcept")` and document the requirement loudly.
- **confidence:** certain

### [MEDIUM] `deoptimized` counter is incremented even when the entry writes silently no-op
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:6445-6448
- **description:** `set_from_interpreted_entry`, `set_from_compiled_entry` and `set_code` (vmhook.hpp:2347-2432) are all defined as `noexcept` and silently return without writing anything when the cached VMStruct entry for `Method::_from_interpreted_entry`, `Method::_from_compiled_code_entry_point`/`_from_compiled_entry`, or `Method::_code` is missing for the current JDK build. On such a build, the loop unconditionally bumps `deoptimized` (line 6448) and returns it to the caller as the "number of methods successfully deoptimised", even though zero pointer writes actually landed. The summary log at line 6451-6452 then reports a deoptimisation count that did not happen, producing a silent failure mode that the caller cannot detect.
- **repro:** Run on a JDK build where `iterate_struct_entries("Method", "_code")` returns nullptr (a future HotSpot that renames the field, or a stripped/dev VM). `set_code(nullptr)` is a no-op, `deoptimized` still increments per matched method, and the function reports a fake non-zero deopt count while no methods are actually deoptimised. Hooks installed on already-inlined callers continue to silently miss.
- **suggested_fix:** Pre-resolve the three Method VMStruct entries once at the top of `deoptimize_methods_if` (or expose a `vmhook::hotspot::can_deoptimise_via_vmstructs()` predicate) and bail out early with a single warning if any are missing, instead of pretending to deopt. Alternatively, change `set_code` / `set_from_interpreted_entry` / `set_from_compiled_entry` to return `bool` and only count successful writes.
- **confidence:** likely

### [MEDIUM] Predicate is called against `klass*` that may not be an InstanceKlass
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:6397-6422
- **description:** `for_each_loaded_class` yields every `klass*` reachable through the ClassLoaderDataGraph — including `ArrayKlass`, `ObjArrayKlass` and `TypeArrayKlass`. `get_methods_count()` (vmhook.hpp:2589-2607) and `get_methods_ptr()` (vmhook.hpp:2617-2639) read `InstanceKlass::_methods` by offset, which is documented as requiring "this klass* must be an InstanceKlass*" (vmhook.hpp:2587, 2615). For a non-InstanceKlass, the bytes at the InstanceKlass `_methods` offset are unrelated memory. `is_valid_pointer(array)` filters obviously-bogus values but a junk pointer that happens to land in user-space and pass the sentinel check produces a garbage `method_count` (read at line 2606 as `*reinterpret_cast<std::int32_t*>(array)`) and a garbage `methods` base pointer (returned by `get_methods_ptr()` as `array + 8`). The inner loop then iterates up to `method_count` entries from that bogus base. Each `methods[i]` read is also bounds-unchecked against any source-of-truth Array length, only filtered by the per-element `is_valid_pointer(m)` test at line 6413.
- **repro:** Hard to trigger deterministically (depends on what happens to be at that offset for an ArrayKlass on the running JDK), but the risk is real: an int-array klass whose layout happens to put a valid-looking pointer at `_methods` offset would cause a multi-thousand-element scan over arbitrary memory before being filtered out element-by-element.
- **suggested_fix:** Add an `is_instance_klass()` predicate (read the VMStruct-exported `Klass::_kind`/`_layout_helper` or test `_methods` validity via a more robust check) and skip non-InstanceKlass entries before calling `get_methods_count()`. At minimum, cap the inner loop with `i < std::min(method_count, kMaxPlausibleMethodCount)` (e.g. 65535) to bound the damage if `_methods` happens to dereference junk.
- **confidence:** likely

### [LOW] Doc/impl mismatch on exception propagation
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:6305-6306 (for_each_loaded_class), 6391-6454 (deoptimize_methods_if)
- **description:** `for_each_loaded_class`'s doxygen claims "callback exceptions propagate; iteration stops at the throwing visit" (vmhook.hpp:6305-6306), but the implementation wraps the entire iteration in `try { ... } catch (const std::exception& ex) { VMHOOK_LOG(...) }` (vmhook.hpp:6328-6336) and swallows `std::exception` derivatives silently after logging. The inner `class_loader_data_graph::for_each_klass` doc says "noexcept boundary — callback exceptions are propagated as-is" (vmhook.hpp:3488-3489), but the public `for_each_loaded_class` wrapper hides that. `deoptimize_methods_if` inherits this confused contract: a throwing predicate produces either (a) silent-swallow-and-log if it inherits std::exception, or (b) std::terminate via the outer noexcept if it doesn't.
- **repro:** Read the docs, expect a throwing predicate to short-circuit the iteration with a propagated exception, observe instead that iteration silently stops with a log line.
- **suggested_fix:** Fix the `for_each_loaded_class` doc to state that std::exception derivatives are logged-and-swallowed and that non-std::exception throws cross the noexcept boundary. Mirror the same contract in `deoptimize_methods_if`'s doc (currently the doc mentions only "Safety: uses the same atomic single-pointer writes" without discussing predicate failure).
- **confidence:** certain

## Improvements

### [S] [USER_FACING] Document and enforce the predicate contract
- **rationale:** The current signature `template<typename predicate_type> auto deoptimize_methods_if(predicate_type&&)` produces a hostile error message when the user passes a non-callable, a wrong-arity lambda or a predicate that returns `void`. The documentation does not state that the predicate must be safe to call from inside the iteration and is called only on already-JIT-compiled methods.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:6376-6394
- **suggested_change:** Add a `static_assert(std::is_invocable_r_v<bool, predicate_type&, const std::string&, vmhook::hotspot::method*>, "deoptimize_methods_if predicate must be callable as bool(const std::string&, vmhook::hotspot::method*)")` at the top of the body. Extend the docblock with: "Called only for methods that are currently JIT-compiled (Method::_code != nullptr). Must not throw; non-`std::exception` throws cross the noexcept boundary and call std::terminate."

### [S] [USER_FACING] Rename / clarify the `skipped_no_c2i` counter and its log message
- **rationale:** The counter is bumped not only when the c2i adapter is unrecoverable but also when `i2i` itself is null (vmhook.hpp:6431-6437). On a JDK build where the VMStruct entry for `Method::_i2i_entry` is missing, every matched method gets accounted as "no recoverable c2i adapter" — actively wrong. The single user-facing log message at line 6451-6452 attributes the skip to the wrong cause.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:6395-6396, 6431-6436, 6451-6452
- **suggested_change:** Split into two counters (`skipped_no_i2i` and `skipped_no_c2i`) and log them separately. Alternatively rename to `skipped` and adjust the log message to "{deoptimized} deoptimised, {skipped} skipped (entry points unrecoverable on this JDK build)".

### [M] [USER_FACING] Match the per-hook install fallback policy for unrecoverable c2i adapter
- **rationale:** `vmhook::hook<T>(...)` (vmhook.hpp:7949-7968) and `verify_hooks()` repair (vmhook.hpp:8265-8274) both implement the same fallback: if the c2i adapter is unrecoverable they still write `set_from_interpreted_entry(i2i)` + `set_code(nullptr)` and accept that compiled inline caches repair themselves at the next safepoint. `deoptimize_methods_if` instead skips entirely (vmhook.hpp:6431-6437), leaving the method JIT-compiled and defeating the whole point of the sweep for any method on a JDK where c2i recovery fails. The doc justifies the conservative skip as "safer than crashing the JVM with a null-code/stale-entry combination" — but the very same combination is intentionally produced by `hook<T>` and verify_hooks, with the documented justification that the IC repair is harmless. The two policies should agree, with the most-careful one (probably the hook<T> fallback path) as the unified behaviour.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:6431-6437 (compare against 7949-7968 and 8265-8274)
- **suggested_change:** When `i2i` is non-null but c2i is unrecoverable, fall back to `set_from_interpreted_entry(i2i)` + `set_code(nullptr)` (no compiled-entry write) and bump a separate `forced_deopt_no_c2i` counter. Only the `!i2i` case should remain a hard skip.

### [S] [USER_FACING] Predicate could short-circuit at the class level
- **rationale:** A common use case (the README's own example, README.md:603-607, and the docblock example at vmhook.hpp:6383-6388) filters by class-name prefix. Today every class is fully scanned for methods even when the user filters by class, costing one `get_code()` read per method to evaluate the per-method predicate. Allowing a class-level predicate avoids touching unrelated InstanceKlasses entirely.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:6392-6454
- **suggested_change:** Add a sibling overload (or a `deoptimize_methods_if(class_predicate, method_predicate)` two-argument form) that consults the class predicate first and skips `get_methods_count`/`get_methods_ptr` entirely when it returns false. The existing single-arg overload can delegate to it with a `[](auto&) { return true; }` class predicate.

### [S] [USER_FACING] Provide `deoptimize_method(klass, name, signature)` for single-method sweeps
- **rationale:** The current sibling API is "all JIT-compiled methods" (`deoptimize_all_jit_compiled_methods`) or a predicate sweep. There is no targeted "deoptimise one specific method" entry point. Users who already hold a `klass*` + name + signature (e.g. from `find_class`) have to write a predicate that does a string comparison on every JIT-compiled method in the JVM, which is both verbose and orders-of-magnitude slower than a direct lookup.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:6391-6478
- **suggested_change:** Add `inline auto deoptimize_method(vmhook::hotspot::klass* k, std::string_view name, std::string_view signature = {}) noexcept -> bool` next to `deoptimize_all_jit_compiled_methods`, walking the klass's `get_methods_ptr()` directly and applying the same i2i / c2i / set_code dance.

### [XS] [INTERNAL] Forward the predicate inside the lambda capture
- **rationale:** The lambda captures `predicate` by reference (default `[&]` capture at line 6398) and calls it at line 6422. That works for both lvalue and rvalue predicates, but the perfect-forwarding intent of `predicate_type&&` is lost: if the user passes a non-copyable, owning predicate (e.g. a `std::move`-only functor whose captures are RAII handles to other state), the predicate's lifetime is tied to the caller's evaluation. For the current usage that's fine — but the function would be slightly more robust if it bound the predicate by reference explicitly: `[&predicate = predicate]`, or even forwarded into a local once at function entry (`auto local_pred{ std::forward<predicate_type>(predicate) };`) and captured the local.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:6398
- **suggested_change:** `auto local_pred{ std::forward<predicate_type>(predicate) };` at line 6395, then capture `local_pred` by reference in the lambda. Cosmetic but makes the noexcept reasoning easier.

### [XS] [USER_FACING] Document return value when iteration aborts mid-way
- **rationale:** If `for_each_loaded_class` catches a `std::exception` thrown by the predicate (or by anything inside the lambda), iteration stops mid-class and the function returns the partial `deoptimized` count. The doc says only "Number of methods successfully deoptimised" without explaining that this number may reflect a partial sweep.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:6379
- **suggested_change:** Append: "If the predicate throws a std::exception derivative, iteration is aborted at the throwing visit; the returned count covers only the methods deoptimised before the throw."

## Tests

### [standalone_unit] [new] deoptimize_methods_if_predicate_throws_std_exception_is_swallowed
- **description:** Pass a predicate that throws `std::runtime_error{"x"}` on a specific class name; verify that the call returns (does not terminate) and the count returned is the partial pre-throw deopt count. Documents the actual silent-swallow behaviour of the std::exception path.
- **asserts:** call returns; returned value is `<= total candidate count`; no abnormal exit.
- **existing_file:** none

### [standalone_unit] [new] deoptimize_methods_if_predicate_throws_int_terminates
- **description:** Death test (`EXPECT_DEATH` or equivalent) that confirms a predicate throwing a non-`std::exception` value (e.g. `throw 42;`) causes std::terminate due to the noexcept boundary. After the fix, replace with a test that the throw is logged and swallowed.
- **asserts:** process aborts with terminate signature (current behaviour) → after fix: process returns, log contains the error tag.
- **existing_file:** none

### [standalone_unit] [new] deoptimize_methods_if_predicate_signature_mismatch_compile_error
- **description:** A `static_assert` smoke test that pasting a predicate of the wrong arity / wrong arg types into `deoptimize_methods_if` produces a friendly compile-time message instead of a deep template error. Implement as a `.fail.cpp` file used by the CMake build-failure suite.
- **asserts:** compile fails; stderr contains the static_assert text.
- **existing_file:** none

### [standalone_unit] [new] deoptimize_methods_if_no_jvm_returns_zero
- **description:** Without a JVM present (the standard test-harness condition), `for_each_loaded_class` yields no classes so `deoptimize_methods_if` should return 0 regardless of predicate. Covers the "safe default" contract documented in test_api_surface.cpp:1-5.
- **asserts:** return value == 0; predicate is never invoked.
- **existing_file:** tests/test_api_surface.cpp (extend with a small `exercise_deoptimize_helpers()` section)

### [standalone_unit] [new] deoptimize_all_jit_compiled_methods_delegates_to_predicate_form
- **description:** Compile-only test verifying that `deoptimize_all_jit_compiled_methods()` is callable with the documented signature (`auto -> std::size_t`, `noexcept`), and that the predicate-true convenience wrapper at vmhook.hpp:6473-6478 type-checks identically to a hand-written `deoptimize_methods_if([](auto&, auto*) noexcept { return true; })`.
- **asserts:** both calls have the same return type; both have `noexcept(true)` per `noexcept(deoptimize_methods_if(...))`.
- **existing_file:** tests/test_api_surface.cpp

### [jvm_integration] [new] deoptimize_methods_if_clears_code_pointer
- **description:** Spin up a real JVM, force-JIT a synthetic Java method (`-XX:CompileCommand=compileonly,...` or `-XX:-TieredCompilation` + warm-up loop), invoke `deoptimize_methods_if` with a predicate matching that class, then read back `method->get_code()` and assert it became null. Repeats per JDK version (8/11/17/21/24/25 — gates align with the existing JDK matrix the README references).
- **asserts:** target method's `_code` is non-null before; `_code` is null after; subsequent invocation of the method runs through the interpreter (use a hook on the same method to observe the dispatch).
- **existing_file:** none (new integration-test bucket; align with hook_install_after_jit.md scaffold).

### [jvm_integration] [new] deoptimize_methods_if_predicate_false_leaves_code_intact
- **description:** With a JIT-compiled method present, call `deoptimize_methods_if([](auto&, auto*) { return false; })` and confirm no method is deoptimised: returns 0 and the method's `_code` pointer is unchanged.
- **asserts:** return value == 0; pre-call `_code` == post-call `_code` for every JIT-compiled method observed.
- **existing_file:** none

### [jvm_integration] [new] deoptimize_methods_if_skips_non_compiled_methods_silently
- **description:** Call against a JVM where some matched-by-predicate methods are interpreted and some are JIT-compiled. Verify that the predicate is only called on the JIT-compiled subset (documented behaviour: lines 6417-6420 short-circuit before the predicate when `_code == nullptr`).
- **asserts:** number of predicate invocations == number of JIT-compiled methods in matched classes (use an atomic counter inside the predicate).
- **existing_file:** none

### [jvm_integration] [new] deoptimize_methods_if_handles_array_klass_in_walk
- **description:** Real JVMs always have array klasses loaded (`[I`, `[Ljava/lang/Object;`, etc.). The sweep must not crash or report bogus deopts when it encounters them. Assert no crash and that the predicate is never called for an array-klass internal name.
- **asserts:** no abnormal exit; for every predicate invocation, `class_name` does not start with `[`.
- **existing_file:** none

## Parity Concerns
- Policy mismatch with `vmhook::hook<T>` install path (vmhook.hpp:7949-7968) and `verify_hooks` mode-3 repair (vmhook.hpp:8265-8274): both perform a forced-deopt fallback when the c2i adapter is unrecoverable (`set_from_interpreted_entry(i2i)` + `set_code(nullptr)`), while `deoptimize_methods_if` (vmhook.hpp:6431-6437) skips entirely. The three deopt sites should converge on one policy.
- `noexcept` posture differs between the helpers in this family: `deoptimize_methods_if` and `deoptimize_all_jit_compiled_methods` are `noexcept`, but `for_each_loaded_class` (vmhook.hpp:6326) — which they wrap — is not. The exception-handling contract is inconsistent and the user-facing docs disagree with the actual catch behaviour (vmhook.hpp:6305-6306 vs 6328-6336).
- No targeted `deoptimize_method(klass, name, signature)` exists, while `vmhook::hook<T>(name)` and `vmhook::hook<T>(name, signature)` do; the asymmetric API forces users to write predicate-based scans for single-method deopts.
- Counter naming inconsistency: `verify_hooks` reports `repaired` (vmhook.hpp:8276), `deoptimize_methods_if` reports `deoptimized` and `skipped_no_c2i` (vmhook.hpp:6395-6396, 6451-6452). Consider standardising on `(deoptimised, skipped, failed)` triplet across both APIs.
