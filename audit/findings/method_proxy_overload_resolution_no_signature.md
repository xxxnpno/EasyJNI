# method_proxy_overload_resolution_no_signature

## Summary
Audited `resolve_compatible_method` (vmhook.hpp:12602-12644) and its supporting helpers `signature_matches_arguments` (12557-12581), `argument_matches_descriptor` (12411-12494), and `next_argument_descriptor` (12513-12540). The scoring is binary first-match-wins with no real "best overload" notion — once a method whose JVM parameter list type-checks against `args_t...` is found, it is returned. Top concern: **for static call sites (`object == nullptr`) the function never walks the klass hierarchy at all, so overload resolution silently doesn't fire** — the static dispatch path will use whatever `get_method("name")` happened to latch onto first, exactly the Minecraft 1.8.9 `a` regression that motivated this feature in the first place.

## Bugs

### [high] Static-method overload resolution is dead code — never walks hierarchy when object is null
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:12611-12615
- **description:** `resolved_klass{ this->object ? klass_from_object_header(this->object) : nullptr }` short-circuits to `nullptr` for every static method_proxy (because `object_base::get_method(type_index, name)` at 13062 constructs the proxy with `nullptr` as the receiver). The follow-up `if (!resolved_klass) return this->method;` then bails before the for-loop runs. As a result, `static_method("a").call(IChatComponent{...})` cannot pick a different overload than the one `get_method` first hit when scanning by name — the exact OBF / Minecraft 1.8.9 scenario the comment at 11540-11550 says this code is meant to fix. The static path silently regresses to the bug.
- **repro:** Register a wrapper for a class that has two overloaded static methods sharing a name (e.g. obfuscated `static a(I)V` and `static a(F)V`). Call `wrapper::static_method("a")->call(3.14f)` — `get_method` returns the first match (the `(I)V` one), `resolve_compatible_method` sees `signature_text == "(I)V"`, finds it doesn't match `float`, sees `this->object == nullptr`, returns `this->method` unchanged, and the float gets bit-blasted into the int parameter slot. The interpreter then interprets the IEEE-754 bits of 3.14f as a signed int.
- **suggested_fix:** When `this->object == nullptr`, fall back to the declaring class via `this->method->get_const_method()->get_constants()->_pool_holder` (call_jni already does this dance at 11603-11623 to derive the static jclass — factor it into a shared helper). Then walk that klass + its supers exactly as the instance path does. Equivalent code path also makes the four `static_method(type_index, name)` (13034-13070) overloads usable for overload-by-arg-types resolution.
- **confidence:** certain

### [medium] First-match wins with no ambiguity detection — silent wrong-overload selection
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:12618-12640
- **description:** The for-loop returns `current_method` on the **first** signature that type-checks against `args_t...`. With the existing trait dispatch this is usually unique because each C++ type maps to exactly one descriptor letter, **but** the wrapper-object case at 12421-12444 falls back to "any L...; descriptor" when the registered `type_to_class_map` entry is missing (the `type_map_entry == end()` branch of the `&&` chain). For an unregistered wrapper type, EVERY object overload matches, so `add(Object)` and `add(String)` are indistinguishable and the loop picks whichever comes first in the methods array. There is no log, no `value_t{ std::monostate{} }`, no diagnostic — just a wrong jmethodID handed to the JVM. The same applies to two object-typed parameters where neither user-supplied wrapper has a registered Java name.
- **repro:** Define `struct foo : vmhook::object<foo> { ... };` but skip `register_class<foo>(...)`. Call `proxy.call(std::make_unique<foo>(oop))` against a class with `bar(Ljava/lang/String;)V` and `bar(Lfoo;)V` — the matcher accepts both, returns whichever methods_ptr[i] index is lower, and silently dispatches the wrong one.
- **suggested_fix:** Continue the loop after a first hit, count matches, and if `match_count > 1`, log a `VMHOOK_LOG(error_tag, ...)` listing every candidate signature + suggesting `get_method("name", "(...)V")` with an explicit descriptor, then return `this->method` (or, more permissively, the first match with a warning). Cheapest scoring: prefer matches where the registered-name path of `argument_matches_descriptor` participated (i.e. `type_map_entry != end()`) over the wildcard branch.
- **confidence:** likely

### [medium] Per-overload-candidate `get_signature()` allocates std::string then discards it
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:12635-12639
- **description:** `current_signature{ current_method->get_signature() }` constructs a fresh `std::string` (heap alloc) for every candidate method in the loop, even ones rejected by `signature_matches_arguments` on the first descriptor token (or on arity). With OBF-named classes where dozens of methods share a one-letter name, this can run 50+ allocs per `call()` on the hot path. `signature_matches_arguments` takes `string_view`; if `method::get_signature()` could be reshaped (or paired with a `get_signature_view()` reading the underlying Symbol* directly), the allocations vanish.
- **repro:** Hook a vanilla 1.8.9 obfuscated class with many name-collided methods (EntityPlayerSP has ~30 `a` methods), call any of them through `proxy.call(...)` in a tight loop, profile with PerfView — `std::string` ctor dominates the proxy path.
- **suggested_fix:** Add a `method::get_signature_view()` (or a small `signature_symbol()` returning a Symbol*) so resolve_compatible_method can match against the Symbol's UTF-8 bytes via string_view, only materializing a std::string when a match is found (since that string is stored in `cached_effective_signature` at 11555 anyway).
- **confidence:** likely

### [low] `next_argument_descriptor` leaves `position` past `close_paren` on malformed input
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:12513-12540
- **description:** Two failure paths leave `position` in inconsistent states. (1) When `signature[position] == 'L'` and no `;` is found before `close_paren`, the function returns `{}` but does NOT advance `position`. The fold-expression in `signature_matches_arguments` (12570-12578) sees `descriptor.empty()`, sets `matches = false`, and stops. So far ok — but the final check `position == close_paren` would now incorrectly succeed if the unterminated L came at the end of a non-matching path (matches is already false so the AND prevents bad returns — but only by luck). (2) The leading `[` loop at 12517-12520 advances `position` past arrays but the descriptor check below only handles single-letter primitives or `L...;`. For an unterminated array like `(I[)V` (close_paren reached mid-token after `[`), position lands exactly on close_paren after the `[` loop, returns `{}`, and the array bracket characters are silently consumed without ever being matched — the final `position == close_paren` evaluates true. Combined with `matches == true` (no args were checked since `args_t...` was empty), an empty-args call against `(I[)V` would falsely report "matches". Edge case, but a 1-byte cost to assert in.
- **repro:** Synthetic — feed `signature_matches_arguments<>("(I[)V")` directly. Returns true; should return false.
- **suggested_fix:** In `next_argument_descriptor`, after the `[` loop, require that `position < close_paren` AND a valid single-letter / `L...;` token follows; return `{}` AND restore `position = close_paren + 1` (or any sentinel > close_paren) on any malformed token so the outer `position == close_paren` check below fails.
- **confidence:** speculative

## Improvements

### [S] [USER_FACING] Emit a structured "no matching overload" diagnostic when fallback fires
- **rationale:** Today, when no candidate matches, the function returns `this->method` and the caller dispatches with the original (wrong) signature. If get_method returned an overload whose arg types don't match the call site, the user sees garbled return values or a Java exception with no hint that overload resolution was attempted. The most-asked support question after the 1.8.9 fix landed has been "why is this returning null?" — and the answer is invariably "your C++ arg types didn't match any of the JVM overloads". Add an explicit log.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:12618-12643
- **suggested_change:** Before `return this->method;` at 12643, count visited candidates; when `candidates_seen > 1` but no match, emit `VMHOOK_LOG(warning_tag, "method_proxy::resolve_compatible_method('{}'): no overload matches C++ arg types — falling back to '{}'. Considered: [list of (sig, mismatch reason)].", name, signature_text, ...)`. Mirrors the dispatch diagnostic at 11879.

### [S] [USER_FACING] Compile-time error for `void*` / raw-pointer args
- **rationale:** `argument_matches_descriptor<void*>` returns `false` (12490 catch-all), and the pack expansion in `signature_matches_arguments` makes the WHOLE signature fail to match. Users passing raw OOPs (the type they got back from `frame->get_arguments<vmhook::oop_t>()`!) into `proxy.call(oop)` get silent overload-resolution failure with no clue why. The library uses `vmhook::oop_t` (alias for `void*`) extensively as the receiver type — the same alias should be a valid Java `Object` argument descriptor match.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:12490-12493
- **suggested_change:** Either (a) add `else if constexpr (std::is_same_v<clean_type, void*>) return descriptor.size() >= 3 && descriptor.front() == 'L' && descriptor.back() == ';';` so a raw OOP matches any reference descriptor; or (b) replace the `return false` fall-through with `static_assert(dependent_false_v<argument_type>, "vmhook::method_proxy::call: argument type T is not supported as a Java parameter — register a wrapper deriving from vmhook::object<T>, pass std::string for java.lang.String, or use one of the primitive int8/16/32/64/float/double types.")`. The current silent-false is the worst of both worlds.

### [M] [USER_FACING] `get_method(name).call(...)` should be able to disambiguate by **arity**
- **rationale:** `get_method("name")` (12925-12961) latches onto the first method by name, ignoring arity. If the first hit is `m(I)V` but the user calls `proxy.call()` (zero args), `resolve_compatible_method` runs through the hierarchy looking for a 0-arity overload that matches, which is fine — but if the call site has `proxy.call(1, 2, 3)` and the class has both `m(III)V` and `m(I)V`, the matcher succeeds on arity but the loop still iterates every method on every super class. The cheapest filter — `current_method->get_arity() != sizeof...(args_t)` (or the equivalent parsed from signature length) — would skip 90%+ of candidates without parsing.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:12627-12640
- **suggested_change:** Inside the inner loop, after the name check at 12630, peek at the signature length (or parse arity once from the ConstMethod via `_size_of_parameters`) and `continue;` when it doesn't equal `sizeof...(args_t) + (object != nullptr ? 0 : 0)`. (`size_of_parameters` excludes the receiver, so direct comparison with `sizeof...(args_t)` works for both static and instance paths.)

### [S] [INTERNAL] Cache `resolve_compatible_method` result on the proxy
- **rationale:** Each `call()` re-walks the entire klass hierarchy from scratch. The proxy already caches `cached_method_id`, `cached_class_handle`, `cached_ret_char`, and `cached_effective_signature` (12656-12667), but does NOT cache the result of `resolve_compatible_method`. For a fixed call site (template `args_t...` is fixed at compile time), the answer is stable across calls. Add a `mutable vmhook::hotspot::method* cached_selected_method{}` keyed on the args_t hash, OR — simpler — store the result of the FIRST resolve_compatible_method call and reuse it (the args_t change between calls is detectable by template instantiation: each unique args_t pack generates a different function template specialization, so this->method getting re-resolved per arg-pack is already segregated).
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:12656-12667
- **suggested_change:** Add `mutable vmhook::hotspot::method* cached_selected_method{ nullptr };` and short-circuit the first line of resolve_compatible_method on a hit. Reset to nullptr alongside `cached_method_id`. NB: don't cache if `this->object` changes — but mutating object after construction isn't a supported pattern.

### [XS] [INTERNAL] `argument_matches_descriptor` should not silently accept `signed char` as Java `boolean`
- **rationale:** The current chain at 12464-12473 maps every 1-byte integral to "B" and every 2-byte integral to either "C" (unsigned) or "S" (signed). `char` (without explicit signed/unsigned) is platform-dependent and is_integral — it'll match "B". That's fine. But `bool` is special-cased FIRST at 12445-12448. Good. The comment at 12461-12463 calls out the intent explicitly. Just add a `static_assert(!std::is_same_v<clean_type, bool>)` in the 1-byte branch as a tripwire, in case someone reorders the chain later.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:12464-12467
- **suggested_change:** First line of the 1-byte branch: `static_assert(!std::is_same_v<clean_type, bool>, "bool must match 'Z' above, not 'B' here");`. Trivial drift defense.

### [S] [USER_FACING] `next_argument_descriptor` should handle invalid input by setting `position = close_paren` not silently swallowing tokens
- **rationale:** See bug #4 above. Trivial robustness change.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:12513-12540
- **suggested_change:** On any failure path (`[` followed by close_paren, unterminated `L`), set `position = close_paren + 1` (so `signature_matches_arguments`'s final equality check fails) before returning `{}`.

## Tests

### [standalone_unit] [new] test_resolve_compatible_method_static_path_overload_picks_correct_overload
- **description:** Regression test for the static-method bug above. Without a real JVM we can't drive call(), but we can verify resolve_compatible_method's behavior by stubbing out `klass::get_super` / `get_methods_ptr` / `get_methods_count` via friend-injection or by adding a test-only constructor that takes a methods array. Alternatively make the test JVM-driven (see jvm_integration entry below).
- **asserts:** For a static proxy with `object == nullptr`, a method named "m" with signature `(I)V` set as `this->method`, and a methods-array containing `(I)V` and `(F)V`, calling `resolve_compatible_method<float>()` should return the `(F)V` Method*, not the `(I)V` one.

### [jvm_integration] [new] test_method_proxy_static_overload_resolution
- **description:** Add a Java fixture `OverloadStatic` with two static methods `a(int)` and `a(float)` returning different sentinel ints. Register a wrapper, call `wrapper::static_method("a")->call(3.14f)` and `wrapper::static_method("a")->call(42)`. Both should hit their respective overloads.
- **asserts:** `call(3.14f)` returns the float-overload sentinel; `call(42)` returns the int-overload sentinel.

### [jvm_integration] [new] test_method_proxy_ambiguous_unregistered_wrapper_logs_warning
- **description:** Define two wrappers `wrapper_a` and `wrapper_b` neither registered via `register_class`. Java class has `m(Lwrapper_a;)V` and `m(Lwrapper_b;)V`. Call `proxy.call(std::make_unique<wrapper_a>(oop))`. Verify that exactly one call to either Java method fires (current behavior — first match wins) AND that a warning is logged listing both candidates.
- **asserts:** Hooks on both Java methods see the call; the warning string appears in the captured log.

### [standalone_unit] [extend_existing] test_traits_signature_matches_arguments_malformed_inputs
- **description:** Extend test_traits.cpp with constexpr/runtime checks for malformed signatures. signature_matches_arguments is private inside method_proxy, but argument_matches_descriptor + the trait invariants can already be verified — add a public helper `vmhook::detail::signature_matches_arguments_for_test` (or expose the function as a friend hook for tests) so the malformed-input cases are reachable.
- **asserts:** `(L)V` (unterminated L), `(I[)V` (trailing array bracket without type), `()V` (empty arg list with `args_t = {int}`), and `(II` (missing close_paren) all return false. `(I)V` with `args_t = {int}` returns true. `(Ljava/lang/Object;)V` with `args_t = {std::string}` returns false (String must match `Ljava/lang/String;` specifically).
- **existing_file:** tests/test_traits.cpp

### [standalone_unit] [extend_existing] test_traits_argument_matches_descriptor_void_pointer
- **description:** Document and lock in the current behavior of `void*` / raw OOP args. Either confirms the "improvement #2" change (void* matches any L descriptor) or asserts the current silent-false-then-fallback contract.
- **asserts:** After the improvement: `argument_matches_descriptor<void*>("Ljava/lang/Object;")` returns true; `argument_matches_descriptor<void*>("I")` returns false; `argument_matches_descriptor<vmhook::oop_t>("Ljava/lang/String;")` returns true (oop_t == void*).
- **existing_file:** tests/test_traits.cpp

### [jvm_integration] [new] test_method_proxy_instance_overload_picks_compatible_when_first_method_wrong
- **description:** The original 1.8.9 regression case in C++ form. Java class `Confused` with `void a(int)` and `void a(java.lang.String)`. Call `proxy.call("hello")` after `get_method("a")` returned the int-overload first. Expect the string-overload to fire.
- **asserts:** Java-side hook on the `String` overload sees "hello"; hook on the `int` overload never fires.

### [jvm_integration] [new] test_method_proxy_arity_disambiguation
- **description:** Java class with `void m()`, `void m(int)`, `void m(int, int)`. Call all three via `proxy.call()`, `proxy.call(1)`, `proxy.call(1, 2)`. Verifies arity-based resolution works even though all three share the name `m`.
- **asserts:** Each Java method fires exactly once with its expected arg pack.

### [standalone_unit] [extend_existing] test_traits_overload_resolution_array_descriptors
- **description:** The `[` array prefix is consumed but `argument_matches_descriptor` (12411-12494) has NO branch for array types — it treats `[I` as a primitive `[` followed by I. Verify what actually happens for `std::vector<int>` / Java `int[]` parameter resolution (test currently captures "today's behavior" which is "doesn't work").
- **asserts:** `argument_matches_descriptor<std::vector<int>>("[I")` returns false today. Document this gap explicitly so callers know array-typed params force the explicit-signature form of `get_method(name, sig)`.
- **existing_file:** tests/test_traits.cpp

## Parity Concerns
- **Static-context coverage gap vs. field_proxy:** `static_field` exists and works without an instance (vmhook.hpp:13316-13320 + 13775-13794 read through the java mirror). `static_method` exists symmetrically (13325-13338) but its result is unable to disambiguate by arg types because `resolve_compatible_method` short-circuits when `object == nullptr`. The static path needs the same hierarchy walk the instance path has, via the constant-pool `_pool_holder` already used by call_jni at 11603-11623.
- **Diagnostic depth vs. field_proxy:** `field_proxy::set` was recently hardened with a size guard + explicit log (per the existing audit/findings/field_proxy_set_size_guard.md). The equivalent "we silently picked the wrong overload" failure mode for method_proxy has no equivalent guard — a fallback to `this->method` is silent unless the user enables debug logging. Sibling parity says: log at warning_tag when resolve_compatible_method falls through without a match.
- **API surface vs. field_proxy:** `find_field` (10267) returns a single field entry by name — fields can't be overloaded in Java so there's no ambiguity. Methods CAN be, but the only "signature-aware" entry point is the two-string `get_method(name, signature)` overload (12977 / 13087). There's no `find_method` free function listing all overloads to help users debug "which overloads exist on this class". Adding `vmhook::find_methods(klass, name) -> std::vector<method_proxy>` would let users introspect ambiguity before calling.
- **Compile-time vs. runtime checks:** The argument-pack arity is checked at compile time via `static_assert(sizeof...(args_t) <= 8, ...)` (11683). There's no equivalent compile-time hint that `argument_matches_descriptor<T>` will return false for unsupported types (raw pointers, std::optional, etc.). The catch-all `return false` at 12490 should be a `static_assert(dependent_false_v<argument_type>)` like the sibling write_jni_arg_to_slot fall-through guards mentioned in tests/test_traits.cpp:103-109.
