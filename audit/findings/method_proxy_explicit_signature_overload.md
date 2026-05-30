# method_proxy_explicit_signature_overload

## Summary
Audited the two `object::get_method(name, signature)` overloads (instance at
vmhook.hpp:12977-13019 and static-by-`type_index` at vmhook.hpp:13087-13129),
plus the deducing-this / static-fallback forwarders at lines 13267-13310 and the
portable alias `static_method(name, signature)` at lines 13334-13338. The match
logic is genuinely exact — the comparison `current_signature == method_signature`
on lines 13008 and 13118 has zero looseness (no empty-string short-circuit, no
prefix match, no case folding), so `defineClass(String,[B,I,I,ProtectionDomain)`
cannot be confused with any sibling overload. The library is solid here; the
concerns are mostly around silent-failure modes (empty signature returns
`nullopt` with a misleading log) and parity gaps with sibling APIs
(`get_method_by_oop_klass` has no signature-aware overload, the
`hook<T>(name, signature)` path still allows the loose-match
`signature.empty()` short-circuit that this code path deliberately rejects).

## Bugs

### [low] Doc claims "Exception safety: does not throw" but functions are not noexcept and allocate std::string per method
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:12971, 12977, 13007, 13080, 13087, 13117
- **description:** The doc comments for both `get_method(name, signature)` overloads state "Exception safety: does not throw; returns nullopt on failure." But neither overload is marked `noexcept`, and inside each loop `const std::string current_signature{ current_method->get_signature() }` (line 13007 / 13117) heap-allocates one std::string per candidate method. On a class with thousands of methods (e.g. java/lang/Class subclasses on a fat JDK) or under memory pressure, `std::bad_alloc` will escape the function — contradicting the documented contract. The instance log-message-building format also performs allocations that can throw. Sibling `get_method(name)` at line 12925 has the same defect.
- **repro:** Inject under a Linux container with a tight cgroup memory limit; call `get_method("foo", "()V")` on `java/lang/Class`. The std::string allocations inside the per-method loop can throw and unwind out of the function, but callers (e.g. `vmhook::hook<>`) catch with the assumption the function returns rather than throws — the assumption matches the doc but not the code.
- **suggested_fix:** Either (a) wrap the body in a `try { ... } catch (...) { return std::nullopt; }` to honor the doc, or (b) mark the function `noexcept` and let std::bad_alloc terminate — at least the contract is honest. Option (a) is the more user-friendly fix; field_proxy::get already uses (a).
- **confidence:** likely

### [low] Empty-signature input silently returns nullopt with a log that doesn't explain the empty-string trap
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:13008, 13015-13017, 13118, 13125-13127
- **description:** If a caller passes `method_signature = ""` (empty string_view), the strict `current_signature == method_signature` comparison can never match (every real JVM descriptor starts with `(` and is at least 3 chars long). The function logs `"no method with this exact name+signature found in class hierarchy"` but the user probably wanted "you passed an empty signature; use the no-signature overload if you want match-by-name." This mirrors the divergence with `vmhook::hook<T>(name, signature)` (line 7745), which deliberately treats empty signature as "match any" and is documented to do so. Two siblings, opposite empty-signature semantics, no comment cross-referencing them.
- **repro:** `obj.get_method("toString", std::string_view{})` returns nullopt with a generic "not found" log — there is no clue that the user actually wanted `get_method("toString")` (no second argument).
- **suggested_fix:** Add an early check: `if (method_signature.empty()) { VMHOOK_LOG("{} object::get_method('{}'): empty signature - use the single-arg overload for match-by-name.", vmhook::error_tag, method_name); return std::nullopt; }` near the top of both signature-aware overloads. Cheap, prevents user confusion, and documents the contract.
- **confidence:** certain

### [low] Allocates std::string for every candidate even when name doesn't match
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:13007-13008, 13117-13118
- **description:** The loop computes `const std::string current_signature{ current_method->get_signature() }` BEFORE checking `current_method->get_name() == method_name`. Because `&&` short-circuits, the name comparison happens after the signature allocation in source order, but the compiler still has to materialize the std::string first. The allocation is wasted for every name mismatch. Walking the entire java/lang/Object hierarchy (~30 methods) to find one is fine; walking a fat JVM class (Class, ClassLoader on a JDK with many synthetic accessor methods) does dozens of unnecessary heap allocations per call. Compare with `resolve_compatible_method()` at line 12630 which orders the cheap check first: `current_method->get_name() != method_name) continue; ... current_method->get_signature()`.
- **repro:** Profile `get_method("defineClass", "(Ljava/lang/String;[BII)Ljava/lang/Class;")` against java.lang.ClassLoader on JDK 21; observe N-1 wasted allocations per call (N = number of methods on ClassLoader).
- **suggested_fix:** Reorder so the name check runs first:
  ```cpp
  if (current_method->get_name() != method_name) { continue; }
  const std::string current_signature{ current_method->get_signature() };
  if (current_signature == method_signature) { return vmhook::method_proxy{ ..., current_signature }; }
  ```
  This matches the pattern used by `resolve_compatible_method`. Improves both throughput and allocator pressure under heavy lookup workloads (e.g. during agent startup when wrappers register).
- **confidence:** certain

## Improvements

### [S] [USER_FACING] Add signature-aware sibling to `get_method_by_oop_klass`
- **rationale:** `object::get_method(name)` has a signature-aware sibling (12977) but `get_method_by_oop_klass(name)` (13495 / 13799) does NOT. Collection wrappers (`list`, `map`, `set`) use the by-oop variant precisely because they need to dispatch into a dynamic, possibly-anonymous subclass — exactly where overloaded methods are most likely to bite (e.g. `java.util.HashMap$Values::contains(Object)` vs `containsAll(Collection)`). Users hooking custom container subclasses today have to drop down to `vmhook::hook<>` for overload disambiguation.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:13495-13510, 13799-13820
- **suggested_change:** Add two-arg overload mirroring 12977 but walking `oop_klass()` instead of `resolve_klass()`. Eight lines each, near-zero risk.

### [XS] [USER_FACING] Add a noexcept early-out for null method_name AND log a hint when a sibling overload exists with the same name
- **rationale:** When `get_method("doFoo", "(I)V")` returns nullopt the log just says "no method with this exact name+signature found." If the class HAS methods named "doFoo" with other signatures, the user has no idea what they are — they end up grepping bytecode or running `javap -s`. A friendlier log lists the discovered signatures.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:13015-13017, 13125-13127
- **suggested_change:** While walking the hierarchy, accumulate `std::vector<std::string>` of all signatures seen for matching names. On miss, log them: `VMHOOK_LOG("{} get_method('{}{}'): no exact match; methods named '{}' have signatures: [{}].", ...);` Bounded list (cap at 16 entries to prevent log spam). Cuts user debugging from minutes to seconds.

### [XS] [INTERNAL] Cross-reference the empty-signature semantics divergence in doc comments
- **rationale:** `vmhook::hook<T>(name, signature)` treats empty signature as "match any" (loose). `get_method(name, signature)` treats empty signature as "must literally equal empty" (returns nullopt). The contracts diverge silently. Future maintainers (and the parent agent's "user-friendliness is the top improvement goal" goal) benefit from a doc cross-reference.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:12963-12976, 13072-13086
- **suggested_change:** Append to the @details block: "Note: unlike `vmhook::hook<T>(name, signature)`, this overload does NOT treat an empty `method_signature` as a wildcard — pass an empty signature only if you literally want to match the empty descriptor, which never occurs in practice. Use the single-arg `get_method(name)` overload for match-by-name." Same for the static overload at 13072.

### [S] [USER_FACING] Decouple `instance` from result of `get_method(name, signature)` when method is static
- **rationale:** The instance overload at 13010 returns `method_proxy{ this->instance, ... }` regardless of whether the matched method is static. The static overload at 13120 correctly passes `nullptr`. So calling `obj.get_method("foo", "()V")` and getting back a proxy that thinks `obj` is the receiver — even though the method is static — may produce surprising interpreter behavior (frame setup with a phantom this slot). Field_proxy has parity here (see `is_static` flag handling on lines 12836 / 12894). Methods do not.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:13007-13011
- **suggested_change:** After matching, peek at `current_method->get_access_flags()` for `JVM_ACC_STATIC` (the helper exists elsewhere in the header). If static, pass `nullptr` as the owning object: `return vmhook::method_proxy{ is_static ? nullptr : this->instance, current_method, current_signature };` Eliminates the silent corner case.

### [S] [USER_FACING] Promote `static_method(name, signature)` to a true alias name like `get_method_static(name, signature)` OR add `method(name, signature)` for symmetry with `field(name)`
- **rationale:** Parity with `static_field` is good, but the broader user-facing API of vmhook leans on `get_field` / `get_method` symmetry. Users not on MSVC/Clang ≥18 hit the deducing-this gap and need a *separate* portable name. Today: `static_field` and `static_method` both exist. The two-arg `static_method(name, sig)` does too. But `static_method` reads as "this method is static" not "this is a static accessor" — confusing for users who actually have overloaded static methods.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:13322-13338
- **suggested_change:** Either rename to `get_static_method` (clearer) or add it as an alias and deprecate `static_method`. At minimum, expand the doc comment at 13332-13333 to clarify "static_method" means "static-context accessor", not "static Java method."

## Tests

### [standalone_unit] [new] test_get_method_explicit_signature_compile
- **description:** Compile-only check that all four invocation patterns of the explicit-signature overload type-check on every supported toolchain. Mirrors `test_unified_call_syntax.cpp` (which already covers some of this) but adds: (a) the `std::string_view` non-literal path, (b) the `static_method("m", "()I")` portable form on GCC/Android NDK Clang where deducing-this isn't available, (c) the `this->object_base::get_method(name, sig)` explicit-qualified form.
- **asserts:** Translation unit compiles successfully; main returns 0; printed banner on success.
- **existing_file:** could be folded into tests/test_unified_call_syntax.cpp

### [standalone_unit] [extend_existing] test_unified_call_syntax_empty_signature_returns_nullopt
- **description:** Without a JVM (no real klass), construct a wrapper with `oop = nullptr` and call `get_method("anything", "")`. The function should return `nullopt` (since `resolve_klass` fails on null oop) and not crash. This codifies the documented "returns nullopt on failure" contract.
- **asserts:** `proxy.has_value() == false`. No segfault. No unhandled exception (catches `bad_alloc` etc.).
- **existing_file:** tests/test_unified_call_syntax.cpp

### [jvm_integration] [extend_existing] test_get_method_disambiguates_overloads_exactly
- **description:** Today's `test_overloaded_methods` (vmhook/src/example.cpp:2594) only verifies the Java-side overload resolution from inside a probe. Extend it to call from C++ side: `wrapper.get_method("overloadProbeProcess", "(I)I")->call(5)` and `wrapper.get_method("overloadProbeProcess", "(Ljava/lang/String;)Ljava/lang/String;")->call(std::string{"foo"})` against the existing overloaded fixture, then assert each returned the correct overload's result. Also pass a NON-matching signature like `"(JJ)V"` and assert `has_value() == false`.
- **asserts:** Each explicit-signature call returns the correct value (130 / "[foo]"); the non-matching signature returns nullopt and emits an "[ERROR]" log line; the log line does NOT contain "loose match" or any indication that a fallback was used.
- **existing_file:** vmhook/src/example.cpp (test_overloaded_methods)

### [jvm_integration] [new] test_get_method_signature_does_not_loose_match_define_class
- **description:** Direct regression test for the ClassLoader.defineClass case. After `vmhook::on_class_loaded(...)` installs its hook, query via `class_loader_wrapper::get_method("defineClass", "(Ljava/lang/String;[BIILjava/security/ProtectionDomain;)Ljava/lang/Class;")` and a non-existent variant `"(Ljava/lang/String;[BII)Ljava/lang/Class;"` (without ProtectionDomain). The first must succeed; the second must return nullopt — not silently land on one of the four other defineClass overloads.
- **asserts:** Exact-match returns a proxy whose `signature_text` equals the input. Bogus signature returns nullopt; no proxy returned; log emitted.

### [standalone_unit] [new] test_get_method_static_inherited_signature
- **description:** Place a Java static method `bar(I)V` on a superclass and try to look it up via `get_method(type_index{typeid(child)}, "bar", "(I)V")`. Verifies the superclass walk in the static overload (line 13099) handles inherited static methods, and that the returned proxy's `object` field is `nullptr` (since this is the static overload — see [bug] above for the instance-overload counterpart).
- **asserts:** `proxy.has_value()`; `proxy->signature_text == "(I)V"`; calling `proxy->call()` against the Java side records the static method having run.

## Parity Concerns
- `get_method_by_oop_klass(name)` at vmhook.hpp:13495 and 13799 has no signature-aware sibling. Collection wrappers using the by-oop path can't disambiguate overloaded methods on dynamic subclasses without dropping to `vmhook::hook<>`.
- `vmhook::hook<T>(name, signature)` at vmhook.hpp:7745 treats `signature.empty()` as a wildcard (loose match), but `object::get_method(name, signature)` at vmhook.hpp:13008/13118 treats empty as strict (no match). The contracts diverge silently — neither doc comment cross-references the other.
- Instance overload `object::get_method(name, signature)` at 13010 always sets `owning_object = this->instance` even when the matched method is static. The static overload at 13120 correctly passes `nullptr`. Field_proxy carries an `is_static` flag and gets this right; method_proxy does not.
- `field_proxy` has only a name-based accessor (no signature variant) — that's appropriate because fields cannot be overloaded by descriptor in Java. The method API's signature overload is novel and lacks unit-test coverage of the "no false-positive loose match" property the changelog promised in v0.4.0.
- Doc inconsistency: instance overload (12970-12971) and static overload (13079-13080) both claim "Exception safety: does not throw" but neither is `noexcept` and both heap-allocate per-iteration. Sibling `get_method(name)` at 12925 has the same defect.
