# static_field_portability

## Summary
Audited the deducing-this overload set on `vmhook::object<derived>` and the always-on `static_field`/`static_method` portable aliases (vmhook.hpp:13228-13339), the macro that selects between them (vmhook.hpp:246-252), the underlying `object_base::get_field/get_method` (vmhook.hpp:12816-13129), and the only compile-time test that exercises this matrix (`tests/test_unified_call_syntax.cpp`). The mechanism is mostly sound, but there is a real parity gap (no `static_field(name, signature)` overload despite the matching `static_method` overload existing), the deducing-this overloads silently drop `const` correctness on the proxy due to `char const*`-only signatures, and there is no integration test that ever calls `static_field` against a live JVM — every `static_field` call site in `example.cpp` exists, but no test actually verifies the GCC-portable path produces a usable proxy.

## Bugs

### [medium] Deducing-this overloads only match `char const*` — string literal vs `std::string` mismatch breaks instance-context `get_field`
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:13254-13271
- **description:** The deducing-this overloads of `get_field` and `get_method` declare their name parameter as `char const* name`, while the inherited `object_base::get_field`/`get_method` use `std::string_view`. When deducing-this is enabled (MSVC, non-NDK Clang), calling `get_field(std::string{"x"})` or `get_field(some_string_view)` from an instance method dispatches to the static `std::string_view` overload (the type_index-based static-field lookup) instead of the inherited `object_base` instance overload, because the deducing-this overload only matches a `char const*`. The static fallback then either logs "klass not resolved for wrapper type" (if not registered) or returns a static-field proxy that ignores `this->instance` entirely. This is silent and surprising: identical-looking code does different things depending on the argument's static type.
- **repro:** In a wrapper `class my : public vmhook::object<my>` on MSVC, write `auto get_x(std::string n) { return get_field(n)->get(); }`. The call resolves to `static get_field(std::string_view)` and walks the type registry instead of the live OOP, even though the method is non-static and `this` is available.
- **suggested_fix:** Change the deducing-this overloads to take `std::string_view` (matching the inherited signature) so that overload resolution picks the deducing-this overload for *any* string-like argument in an instance context, and the static fallback wins only when there is no implicit object. Alternatively, add a second deducing-this overload for `std::string_view` alongside the `char const*` one.
- **confidence:** likely

### [low] `static_field` has no `(name, signature)` overload despite `static_method` having one — silent disparity with the deducing-this overload set
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:13313-13338
- **description:** `static_method` exposes both `(name)` and `(name, signature)` overloads (13325, 13334) mirroring the underlying `object_base::get_method(type_index, name)` and `object_base::get_method(type_index, name, signature)` (13034, 13087). `static_field` only exposes `(name)` (13316). There is no underlying `object_base::get_field` overload taking a signature either — field lookup in `vmhook::find_field` only matches by name (see callers at 12828, 12885). This is consistent inside the file but breaks the user-facing symmetry promised by the doc comment "Same pattern as fields" (README.md:247). If two Java classes in a class hierarchy declare a field with the same name but different signatures (legal in JVM bytecode via `Synthetic`/access widening or via field shadowing across a class hierarchy), the user has no way to disambiguate from C++.
- **repro:** Define a Java subclass that shadows a parent field of the same name but different type. `static_field("foo")` returns whichever `find_field` walks first, with no way to disambiguate. `static_method` users can disambiguate via the signature overload; `static_field` users cannot.
- **suggested_fix:** Either (a) add a `static auto static_field(std::string_view name, std::string_view signature)` that filters by signature inside `find_field`, or (b) document explicitly in the `static_field` doc comment (13313-13315) that field shadowing is not disambiguable, mirroring the existing `static_method` API surface intent.
- **confidence:** speculative (depends on whether vmhook's `find_field` callers ever encounter shadowed fields in production)

### [low] Macro `VMHOOK_HAS_DEDUCING_THIS` excludes Android NDK Clang but does not exclude MinGW GCC explicitly
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:246-252
- **description:** The feature-test check is `__cpp_explicit_this_parameter >= 202110L && (clang || msvc) && !__ANDROID__`. This correctly excludes GCC (which never sets `__clang__`) and Android NDK Clang. But the doc comment (232-245) and README (200-205) describe the gate as "GCC's overload-resolution behavior". MinGW Clang on Windows would be enabled; that is probably fine. The actual concern: there is no static_assert anywhere that fires when a user manually `#define VMHOOK_HAS_DEDUCING_THIS 1` on GCC. If a user defines the macro themselves (e.g. via `-D`), the static-context `get_field`/`get_method` overloads get emitted on GCC and produce the documented "cannot call a function with deducing this without an object" error — confusing, because the README told them GCC needs `static_field`.
- **repro:** Build with `g++ -std=c++23 -DVMHOOK_HAS_DEDUCING_THIS=1` and a wrapper class containing `static auto x() { return get_field("y")->get(); }`. Compile error originates inside vmhook.hpp with no indication that the user overrode a portability guard.
- **suggested_fix:** Add `#if VMHOOK_HAS_DEDUCING_THIS && defined(__GNUC__) && !defined(__clang__)\n  #error "VMHOOK_HAS_DEDUCING_THIS=1 is not supported on GCC; use static_field/static_method"\n#endif` immediately after the macro definition (after 13252), or change the macro to `#if !defined(VMHOOK_HAS_DEDUCING_THIS)` so that user-supplied definitions win cleanly but still warn.
- **confidence:** speculative

## Improvements

### [S] [USER_FACING] Surface `static_field` / `static_method` in the `object<derived>` doxygen example
- **rationale:** The class-level doc block at 13186-13226 talks at length about the three overload sets but the worked example only shows `get_count() -> int { return static_field("entityCount")->get(); }` once (13222). Users skimming the header will not realize `static_method("name", "(I)V")->call()` is the parity overload for signature-disambiguated calls. Adding 2-3 lines of usage for `static_method` (with and without signature) directly in the Usage block raises discoverability without growing the API.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:13213-13226
- **suggested_change:** Extend the example block with `static auto reset() -> void { static_method("reset")->call(); }` and `static auto add(int x, int y) -> int { return static_method("add", "(II)I")->call(x, y); }` so the entire portable static API is visible in a single eye-span.

### [S] [USER_FACING] Add a static_assert that fires when `static_field<derived>` is called before `register_class<derived>`
- **rationale:** `static_field("name")` (13316) silently returns nullopt and logs at runtime when `register_class<derived>` was never called. Because this is a static method, the registration state cannot be checked at compile time, but the *type* `derived` is. A `static_assert` inside `register_class<T>` that records a tag-trait on `T` would not work without macros, but the error message inside `resolve_klass(type_index)` (13172) could be made significantly more actionable by including the suggestion text "call `vmhook::register_class<your_wrapper>(\"java/internal/Name\")` during JVM init" and including `typeid(derived).name()` in the log when called from `static_field`/`static_method`. Currently it logs only the typeid name, which on MSVC is "class my_namespace::my_class" — fine, but the actionable hint is buried in `object_base`, not the wrapper-facing API.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:13166-13183 (resolve_klass) plus the static_field/static_method bodies at 13316-13338
- **suggested_change:** Inside `static_field` / `static_method` wrap the inner call with a check on the optional and, on nullopt, emit a wrapper-specific log line that includes `typeid(derived).name()` and the hint "ensure `vmhook::register_class<...>()` was called and the class is loaded by the JVM."

### [XS] [INTERNAL] Doc comment says "Portable across compilers" — clarify which API is portable vs which is GCC-only
- **rationale:** The one-line doc on `static_field` (13313-13315) says "Explicit static field accessor.  Portable across compilers." That is true but does not contrast with `get_field`. Adding "(equivalent to the same-name static `get_field(std::string_view)` overload that exists on MSVC and Clang only — use this name to remain GCC-compatible)" makes the choice obvious without forcing the reader to scroll back to the class-level doc 100 lines up.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:13313-13315
- **suggested_change:** Replace the one-liner with a 3-line @brief that mentions the deducing-this equivalent and explicitly calls out GCC.

### [S] [USER_FACING] Mirror `field_proxy` vs `method_proxy` parity: ensure `static_method(name, signature)` and `static_field` both compile when called on a wrapper that has *not* been registered yet — currently both return nullopt silently
- **rationale:** User explicitly called out method_proxy vs field_proxy parity gaps. A common foot-gun is calling `static_field("x")` before `register_class<derived>` has run (e.g. inside a constructor of another wrapper). The current behavior is "return nullopt + log a warning". A library-level helper `vmhook::require_registered<derived>()` that aborts loudly during development would catch this at the call site instead of crashing later via `->get()` on a nullopt.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:13316-13320 and 13325-13338
- **suggested_change:** Add `vmhook::detail::require_registered<derived>()` (no-op in release, asserts in debug) and call it at the top of `static_field` / `static_method`. Strictly internal; no API change.

## Tests

### [standalone_unit] [exists] test_unified_call_syntax
- **description:** Verifies that the static call sites `static_field`, `static_method(name)`, `static_method(name, signature)` all compile, plus the deducing-this `get_field`/`get_method` variants when the macro is set.
- **asserts:** Compile-only — printf "OK" on success.
- **existing_file:** tests/test_unified_call_syntax.cpp

### [standalone_unit] [extend_existing] test_static_field_does_not_collide_with_string_view_arg
- **description:** Compile-time check that calling `get_field` with a `std::string` or `std::string_view` argument from an *instance* method on MSVC/Clang still dispatches to the inherited instance overload, not the static fallback. Could be done with a `concepts`-style probe that checks the return type binds to a non-static path (e.g. by registering a wrapper, building a fake oop, and asserting `instance` is read).
- **asserts:** static_assert on overload resolution — e.g. `static_assert(std::is_same_v<decltype(my{nullptr}.get_field(std::string_view{"x"})), std::optional<vmhook::field_proxy>>)` plus a runtime probe that verifies the instance path was taken (not the static path) by registering a wrapper whose type_index is *unknown* — the static fallback would log "type not registered", the instance path would log "instance pointer is null".
- **existing_file:** tests/test_unified_call_syntax.cpp

### [standalone_unit] [new] test_static_field_macro_override_warns_on_gcc
- **description:** Negative-compile test that confirms a user-supplied `-DVMHOOK_HAS_DEDUCING_THIS=1` on GCC produces a clear `#error` rather than a deep template error inside `object<derived>`. Skip on non-GCC builds.
- **asserts:** Build expected to fail with a known message. Driven from CMakeLists with `try_compile` and `EXPECT_FAIL`.

### [jvm_integration] [new] test_static_field_against_live_jvm
- **description:** No existing test ever calls `static_field` against a real running JVM. `vmhook/src/example.cpp` uses it heavily, but the example is a manual driver, not an automated test. A focused integration test should: register a wrapper, force-load the Java class, call `static_field("intMaxValue")->get()` and assert the result matches Java's `Integer.MAX_VALUE`. This also exercises the static-mirror path in `object_base::get_field(type_index, name)` (12873-12909) which currently has no automated coverage.
- **asserts:** `static_field("intMaxValue")->get() == 2147483647`, `static_field("unknownField").has_value() == false`, `static_field("notStaticField").has_value() == false` (instance field — must log "needs an object instance").
- **existing_file:** vmhook/src/example.cpp (could be added as a new probe class alongside the existing static_probe_class around line 311+).

### [standalone_unit] [new] test_static_field_typeid_uses_derived_not_dynamic
- **description:** When `static_field` is called from a derived-of-derived class (e.g. `class b : public a, public vmhook::object<a>`), `typeid(derived)` resolves to `a`. Verify behaviorally that `static_field` always resolves through the registered CRTP type, never through any other dynamic type — this is a foot-gun for users who diamond-inherit two wrappers.
- **asserts:** Construct two wrappers `a : object<a>` and `b : object<b>`; register both with distinct class names; call `b::static_field("x")` and assert the lookup uses `b`'s registered name, not `a`'s.

## Parity Concerns
- `static_method` has a `(name, signature)` overload (13334); `static_field` does not (13316). README says "Same pattern as fields" (README.md:247) — the asymmetry is undocumented.
- The deducing-this overloads take `char const* name` (13255, 13261, 13267); the inherited `object_base` overloads take `std::string_view` (12816, 12925, 12977). This means `get_field(std::string{"x"})` from an instance context silently falls through to the *static* fallback overload on MSVC/Clang, ignoring `this`.
- `static_field` / `static_method` log via `vmhook::error_tag` but do not include `typeid(derived).name()` in the log (12879, 12888, 12896); the user has to deduce from the call site which wrapper failed. Contrast with `watch_static_field` (15005, 15012) which does include the typeid in every log line.
- No `field_proxy::raw_address()` helper appears in the static-context API surface; `watch_static_field` (15009) calls it, but user code that wants to install a custom watcher on a `static_field("x")` proxy would have to discover this manually. Mirrors a documentation gap, not a behavioral one.
