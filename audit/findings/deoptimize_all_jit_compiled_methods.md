# deoptimize_all_jit_compiled_methods

## Summary
`deoptimize_all_jit_compiled_methods()` is a one-line wrapper around `deoptimize_methods_if(always_true)` that walks every loaded klass, every JIT-compiled method, and applies the same Method-side deopt dance (`set_from_interpreted_entry`, `set_from_compiled_entry`, `set_code(nullptr)`) used by the per-hook install path. The implementation is small and correct in the common case but inherits several latent issues from sibling code (no InstanceKlass filter when walking the class loader graph, no compiler barrier between the three pointer stores, no API to surface the skipped-methods count) and silently mislabels the "skipped" reason in the only diagnostic it emits.

## Bugs

### [medium] Title: `methods` array walked with no InstanceKlass type check — risk of garbage walk on ArrayKlass / TypeArrayKlass
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:6397-6410
- **description:** `for_each_loaded_class` invokes the visitor for *every* klass it finds in the ClassLoaderDataGraph / SystemDictionary (instance klasses, ObjArrayKlass, TypeArrayKlass, MirrorKlass, ...). `deoptimize_methods_if` then unconditionally calls `k->get_methods_count()` / `k->get_methods_ptr()` on each. Those getters read at the `InstanceKlass::_methods` VMStruct offset (see `vmhook/ext/vmhook/vmhook.hpp:2589-2639` — note the explicit `@note This klass* must be an InstanceKlass*`). For non-InstanceKlass entries that offset aliases an unrelated field. Whether the loop bails out depends entirely on what happens to be at that offset:
  - If the value is null/invalid, `get_methods_count` returns 0 and we skip. (Best case.)
  - If the value is a readable pointer whose first 4 bytes happen to be a small positive integer, we accept it as `method_count` and dereference `methods[i]` for every slot. The per-slot `is_valid_pointer` filter catches most garbage, but with a large `method_count` (e.g. an aliased pointer field whose low 32 bits parse as 0x0EAFBEEF) we waste minutes scanning gigabytes of memory.
  - If by bad luck a "method-shaped" pointer survives the validity gate, we then mutate `_i2i_entry`-aliased / `_code`-aliased offsets on a non-Method object — corrupting random JVM heap state.
- **repro:** Call `vmhook::deoptimize_all_jit_compiled_methods()` on a process that has hot ObjArrayKlass / TypeArrayKlass entries. On JDKs where the aliased `InstanceKlass::_methods` slot lands on a populated unrelated field (e.g. `ArrayKlass::_component_mirror` OopHandle on JDK 21), the iteration walks a non-zero "count" and dereferences component-mirror oop bytes as Method**.
- **suggested_fix:** Add an InstanceKlass kind check before walking methods, e.g. either gate on `Klass::_layout_helper` (non-array instance class) or expose `klass::is_instance_klass()` and skip otherwise. Other sites that walk methods (`vmhook.hpp:7731`, `8074`, `8589`, `12620`, `12939`, `12991`, `13048`, `13101`, `13470`, `13804`) only ever come from `find_class(name)` so they implicitly know the klass is an InstanceKlass — the bulk-deopt path is the only walker that gets handed *every* klass kind without filtering.
- **confidence:** likely

### [low] Title: `skipped_no_c2i` counter and log message lump together two different failure causes
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:6427-6437, 6451-6452
- **description:** The validity gate at line 6431 trips on either `!i2i`, `!c2i`, or `!is_valid_pointer(c2i)`, but the counter and the only log line ("skipped (no recoverable c2i adapter)") only mention the c2i case. A method with a corrupt or missing `_i2i_entry` (which is far less expected and indicates real JVM trouble) gets silently lumped into the c2i bucket. Diagnosing a skipped method becomes harder because the message contradicts the actual cause.
- **repro:** Call `deoptimize_methods_if` on a JDK build where `get_i2i_entry()` returns null for some Method (e.g. `Method._i2i_entry` field not exported in this JDK's VMStructs) and observe the log claiming "no recoverable c2i adapter" for a c2i-recovery issue that isn't.
- **suggested_fix:** Split the diagnostic into two checks: count `i2i` failures separately and log both numbers, e.g. `"skipped {} (no i2i entry), {} (no c2i adapter)"`. Or at minimum rename the variable / log to `"skipped (could not recover entry points)"`.
- **confidence:** certain

### [low] Title: Three pointer stores are not separated by a compiler barrier — ordering comment is aspirational
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:6439-6447 (plus the setters at 2347-2432)
- **description:** The comment claims "Clear `_code` last so the entry-point writes are visible before HotSpot's safepoint check sees `_code == nullptr`." But the three writes are plain non-volatile, non-atomic stores through cached offsets — `set_from_interpreted_entry`, `set_from_compiled_entry`, `set_code` each contain just one such store. There is no `std::atomic_thread_fence`, no `volatile`, no `compiler_barrier()` between them. On x86 the architectural store order matches program order so this is fine *if* the compiler doesn't reorder; but the noexcept setters' bodies are trivial enough that LTO / IPO may inline all three into one block and freely reorder the unrelated stores. The same issue exists in the per-hook install path (vmhook.hpp:7940-7945) and the verify_hooks repair (vmhook.hpp:8265-8273), so this is a project-wide footgun, not a bulk-deopt-only one — but the comment makes a stronger claim than the code enforces.
- **repro:** Inspect MSVC / clang LTO output around `deoptimize_methods_if`. With aggressive inlining the three independent stores can be issued in any order. A racing JIT thread reading `_code == nullptr` could still see stale `_from_interpreted_entry` / `_from_compiled_entry`.
- **suggested_fix:** Make the setters `volatile` writes, or wrap each store in `std::atomic_ref<void*>{...}.store(value, std::memory_order_release)` (C++20, which the rest of the file already requires), or insert `std::atomic_signal_fence(std::memory_order_release)` between the calls. The release-store on `_code` is the one that really matters; the other two need only be ordered before it.
- **confidence:** likely

### [low] Title: `skipped_no_c2i` is not returned to the caller — no programmatic way to detect partial failure
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:6392-6453, 6473-6478
- **description:** The function returns only the count of successfully deoptimised methods. A caller that wants to know "did every JIT-compiled method actually get deopted, or did we skip a bunch?" has to scrape the log. For an API that's designed to be called once at startup right after installing all hooks, the caller's only sensible response to a non-zero skipped count is to *retry once HotSpot has settled* (or to log a warning) — neither of which is possible without the skipped count.
- **repro:** Call `deoptimize_all_jit_compiled_methods()` and try to assert "every hot method now routes through the interpreter". You can't, because there's no way to distinguish "0 skipped, sweep was complete" from "100 skipped, half the JIT'd JDK code is still bypassing your hooks".
- **suggested_fix:** Either change the return type to `struct { std::size_t deoptimized; std::size_t skipped; }` (breaking change — needs a major bump), or add a sibling overload `deoptimize_methods_if(predicate, std::size_t* out_skipped = nullptr)`, or add a global counter accessor (`get_last_deopt_skip_count()`). The struct return is the cleanest given this is still pre-1.0.
- **confidence:** certain

## Improvements

### [S] [USER_FACING] Title: Predicate signature gives the user no method name — every non-trivial predicate has to call `m->get_name()` itself
- **rationale:** The predicate is `bool(const std::string& class_name, vmhook::hotspot::method*)`. The class name is pre-stringified (the helper already paid the symbol-to-string cost). The method name is the second thing every interesting predicate wants, but the predicate has to do `m->get_name()` itself — which materialises a fresh `std::string` per method per call (allocating, copying the symbol bytes). A typical use case from the README ("Catch already-inlined Minecraft callers of any chat hook") doesn't need the method name, but a refined filter like "all `net/minecraft/client/Minecraft` methods *except* the renderer hot path" can't avoid the per-Method string allocation.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:6376-6391, 6422
- **suggested_change:** Add an overload (or a sibling helper) whose predicate takes `(const std::string& class_name, std::string_view method_name, vmhook::hotspot::method*)`. The helper pays the `get_name()` cost once per method and hands the predicate the view, sidestepping per-call allocations for the common filter case. Keep the old signature for backwards compatibility.

### [S] [USER_FACING] Title: Log line always prints, even when there are zero loaded classes / zero matches — and never tells the user how many classes / methods were scanned
- **rationale:** The single log line at line 6451 emits "0 deoptimised, 0 skipped" whenever the predicate doesn't match anything. With a tightly-scoped predicate the user can't tell whether the predicate is broken or whether nothing was JIT-compiled yet. Conversely, the unconditional info-level log on every call (typical startup-time deopt sweeps log thousands of methods every restart) clutters logs that downstream tools grep for genuine warnings.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:6451-6452
- **suggested_change:** Also count `examined` (methods passed to the predicate) and `classes_walked`; print all four. Promote the log to a single `VMHOOK_LOG_DEBUG` (or guard behind a verbose flag) so steady-state startup logs aren't dominated by it; bump to `vmhook::warning_tag` when `skipped_no_c2i > 0` so the warning actually surfaces in a default-config grep.

### [XS] [INTERNAL] Title: `predicate` is captured by reference inside the lambda then called without `std::forward` — works for the always-true lambda, awkward for stateful predicates
- **rationale:** `deoptimize_methods_if(predicate_type&&)` forwards the predicate into a lambda that captures it as a reference through `[&]`. Stateful predicates (e.g. a mutable-counter lambda the caller passes by rvalue) get called via lvalue access only, which is correct for the common case but means call operators marked `&&` cannot be used. The footgun is small but not zero — and the surrounding API uses perfect forwarding elsewhere (`for_each_loaded_class` does `std::forward<visitor_type>(visit)` at vmhook.hpp:6331).
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:6391-6453
- **suggested_change:** Capture `predicate` by `std::forward` into the inner lambda (or by perfect-forward via `[&pred = predicate]`), and invoke as `pred(class_name, m)`. Cheap.

### [S] [USER_FACING] Title: Convenience wrapper hides the predicate-elimination optimisation
- **rationale:** `deoptimize_all_jit_compiled_methods()` calls `deoptimize_methods_if([](auto&, auto*){return true;})`. Inside `deoptimize_methods_if` the predicate call is still made per-method (line 6422). The compiler may inline it away but only if `deoptimize_methods_if` is instantiated for the specific always-true lambda type. On MSVC release builds that inlining can fail when `-Ob1` is in effect, leaving a real indirect call per method. A dedicated implementation that skips the predicate entirely would shave a measurable amount off a sweep that touches 10k+ methods.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:6473-6478
- **suggested_change:** Hoist the body of `deoptimize_methods_if` into a private helper `deoptimize_methods_impl(const Predicate*)` that takes a nullable predicate pointer; have the wrapper call it with `nullptr` (predicate check elided), and `deoptimize_methods_if` call it with `&predicate`. Alternatively, just expand the loop into `deoptimize_all_jit_compiled_methods()` directly and stop pretending the wrapper is free.

### [M] [USER_FACING] Title: No safepoint coordination — concurrent JIT compilation can race the sweep
- **rationale:** The sweep walks every loaded klass with no JVM safepoint coordination. The HotSpot compiler thread can install a fresh nmethod into `Method::_code` between our `get_code()` read at line 6417 and our `set_code(nullptr)` write at line 6447 — we overwrite a fresh nmethod with null, then the JIT immediately re-compiles it. Same race with class redefinition (a `RedefineClasses` mid-sweep can free the InstanceKlass we're iterating). The README's "exercised continuously in production" note covers per-hook deopt at install time, but a startup-time bulk sweep happens while user code (and JDK classes!) are actively warming up the JIT, which is the worst-case race window.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:6392-6453
- **suggested_change:** Document the safepoint expectation (`@details Caller should pause user threads or call from within a safepoint`). Better, expose a `deoptimize_at_safepoint_if(predicate)` that uses JVMTI's `RawMonitorEnter` / SR1 lock around the sweep. Or at minimum, retry the per-Method dance one time if `get_code()` changes between the entry read and the cleared-state write, so we don't silently re-defeat a freshly-installed compilation.

### [XS] [INTERNAL] Title: The lambda calls `m->get_adapter()` for every JIT-compiled method even when the slow-path heuristic detection has not yet succeeded
- **rationale:** `get_adapter()` on JDK 9+ does the heuristic offset detection on the first call for every method until one validates (see vmhook.hpp:2481-2491). On a startup-time bulk deopt the *first* methods we see are the ones HotSpot warmed up earliest — they may all be in the same in-flight state that defeats validation, so we scan ~100 Method objects worth of slots per call before the offset is finally cached. That's not a correctness bug (the existing caching logic is intentional — see the comment about Forge / Lunar) but for a 10k-method sweep it can take seconds longer than necessary.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:6428, 2481-2498
- **suggested_change:** Before the iteration starts, call `detect_adapter_offset_from_method` on a probe method that's known-good (e.g. `find_class("java/lang/Object")->get_methods_ptr()[0]`), warming the cache. If that fails, fall back to today's per-call detection — but warming up the common case removes the worst-case slow start.

## Tests

### [standalone_unit] [new] test_deoptimize_methods_if_no_jvm_returns_zero
- **description:** With no JVM running (the standard test environment for vmhook), `deoptimize_all_jit_compiled_methods()` and `deoptimize_methods_if(any_predicate)` must both return 0 and not crash.
- **asserts:**
  - `vmhook::deoptimize_all_jit_compiled_methods() == 0`
  - `vmhook::deoptimize_methods_if([](auto&, auto*){return true;}) == 0`
  - `vmhook::deoptimize_methods_if([](auto&, auto*){return false;}) == 0`
- **existing_file:** tests/test_api_surface.cpp (extend the `exercise_hooks` block to also call both deopt entry points)

### [standalone_unit] [extend_existing] test_deoptimize_methods_if_predicate_type_compiles
- **description:** Compile-only sanity test that every predicate signature the public API documents (lambda by rvalue, std::function wrapper, function pointer, member-function-bound functor, generic-lambda with `auto&, auto*`) instantiates without warnings.
- **asserts:** all callers compile; no actual runtime assertion beyond `(void)result;`
- **existing_file:** tests/test_api_surface.cpp

### [jvm_integration] [new] test_deoptimize_clears_code_for_jit_method
- **description:** Bring up a JVM (HotSpot 8 + 17 + 21 in CI), warm a hot method by running it 10k+ times so HotSpot tiered-compiles it, snapshot `Method::_code`, call `deoptimize_all_jit_compiled_methods()`, assert `Method::_code` is now nullptr and `Method::_from_compiled_entry` points at the AdapterHandlerEntry's `_c2i_entry`.
- **asserts:**
  - `pre_deopt_code != nullptr`
  - `post_deopt_code == nullptr`
  - `post_deopt_from_compiled == get_c2i_entry_from_adapter(method.get_adapter())`
  - `post_deopt_from_interpreted == method.get_i2i_entry()`
  - return value >= 1 (at least the warmed-up method was counted)

### [jvm_integration] [new] test_deoptimize_skips_when_c2i_unrecoverable
- **description:** Synthetically corrupt `Method::_adapter` (or pick a JDK build where `get_adapter()` returns nullptr) and assert the helper does NOT mutate `_code` / `_from_compiled_entry` / `_from_interpreted_entry`. Today's bug (#2 above) means the log line will mislabel the cause if i2i is the culprit; the test should fail until both branches are split.
- **asserts:**
  - `pre_deopt_code == post_deopt_code` (unchanged)
  - return value does not include the corrupted method
  - log emits the new dedicated "no i2i" line (once the fix lands)

### [jvm_integration] [new] test_deoptimize_predicate_filters_classes
- **description:** Warm two classes' methods (`com.test.A.hot()`, `com.test.B.hot()`), call `deoptimize_methods_if(name == "com/test/A")`, assert A's method is deopted and B's is left alone.
- **asserts:**
  - `methodA._code == nullptr` after call
  - `methodB._code != nullptr` after call (still the original nmethod address)
  - return value == 1

### [jvm_integration] [new] test_deoptimize_idempotent
- **description:** Call `deoptimize_all_jit_compiled_methods()` twice back-to-back; the second call should return 0 (or close to it — race window with HotSpot re-JITting) and definitely should not crash, double-write entry points, or trigger the AdapterHandlerEntry slow-path detector to fail.
- **asserts:**
  - first call return value > 0
  - second call return value == 0 (allow small race delta with a tolerance comment)
  - no SEH / signal handler trip

### [jvm_integration] [new] test_deoptimize_on_array_klass_safe
- **description:** Reproduces bug #1: ensure the sweep doesn't walk methods on ObjArrayKlass / TypeArrayKlass / MirrorKlass entries returned by `for_each_loaded_class`. Inject a probe (or assert via debug counter) that `get_methods_count` is only invoked on `InstanceKlass*`.
- **asserts:** no `methods[i]` dereference happens for any klass whose `_layout_helper` indicates array-ness (post-fix); pre-fix, the test passes only by luck of memory layout.

### [standalone_unit] [extend_existing] test_deoptimize_predicate_noexcept_contract
- **description:** Compile-time check that a noexcept predicate keeps `deoptimize_methods_if` noexcept; a potentially-throwing predicate should still compile (the function is declared `noexcept` and will terminate on throw — verify behaviour matches the documentation).
- **asserts:**
  - `static_assert(noexcept(vmhook::deoptimize_methods_if(noexcept_lambda)))`
- **existing_file:** tests/test_api_surface.cpp

## Parity Concerns
- The per-hook install path (vmhook.hpp:7933-7968) has TWO branches for c2i recovery: the happy path AND a "forced deopt" fallback (`set_from_interpreted_entry + set_code(nullptr)`) that the bulk-deopt sweep deliberately omits. The README justifies the asymmetry, but a user familiar with the install path may expect the bulk sweep to be similarly aggressive and be surprised when methods stay JIT-compiled because of an unrecoverable adapter. Consider exposing a `deoptimize_methods_if(predicate, deopt_policy::strict | deopt_policy::force_when_no_c2i)` so the user picks.
- The per-hook install applies `set_dont_inline(found_method, true)` AND `*flags |= NO_COMPILE` to each hooked method (vmhook.hpp:7774-7781). The bulk sweep applies NEITHER to the methods it deopts. This is correct as documented — the bulk sweep targets *callers*, and only the hooked callees need the inline-inhibitor — but a user reading just `deoptimize_methods_if` won't realise their predicate-selected methods will be re-JIT'd within seconds. Worth a one-line note in the doc-comment explicitly calling that out.
- `verify_hooks` mode-3 repair (vmhook.hpp:8239-8276) uses the same three writes but tolerates a missing c2i adapter (skips just the `set_from_compiled_entry`, still clears `_code`). The bulk sweep treats the same condition as "skip the whole method". Three sites, three different behaviours for the same "c2i unrecoverable" condition — collapsing them into a single shared helper would prevent future drift.
- `for_each_loaded_class` is the only iteration helper that does NOT filter to InstanceKlass — every other caller of `get_methods_ptr` / `get_methods_count` goes through `find_class(name)` which returns InstanceKlass by construction. Either the helper should filter, or every consumer (this one is the most exposed) should be aware. Today only `deoptimize_methods_if` and the `for_each_loaded_class` documentation acknowledge the difference.
- The return type `std::size_t` matches sibling helpers (`for_each_instance` returns `std::size_t`), but the asymmetry between "deoptimized" and "skipped" is unique to this helper and should at minimum be documented in the `@return`.
