# method_proxy_static_method_portability

## Summary
Audited the portable `static_method(name)` / `static_method(name, sig)` factories on `vmhook::object<T>` (lines 13325-13338), the underlying `object_base::get_method(type_index, ...)` static overloads they delegate to (lines 13034-13129), and the `method_proxy` constructor + `call`/`call_jni` paths that consume the resulting proxy (lines 11502-12330). The portability shim itself is byte-identical to the field side, but `method_proxy::is_static()` is broken (always returns false) and the static `get_method` overloads silently accept *instance* methods - both gaps where the parallel `static_field`/`get_field` side gets it right.

## Bugs

### [high] method_proxy::is_static() always returns false - constructor never sets the static_field flag
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:11502-11508, 12326-12330, 12649, 13062, 13120
- **description:** `method_proxy` has a member named `static_field` (line 12649 - confusingly reusing the field_proxy nomenclature) that backs the public `is_static()` accessor (lines 12326-12330). The only constructor (lines 11502-11508) hard-codes `, static_field{ false }` and there is no setter. Both `static_method("name")` factory paths construct via `vmhook::method_proxy{ nullptr, current_method, ... }` (lines 13062, 13120) and `is_static_call` inside `call_jni` is derived from `this->object == nullptr` (line 11581) rather than from this flag, so dispatch *happens* to work, but every public consumer of `is_static()` is silently lied to. This mirrors `field_proxy::is_static()` (lines 11293-11297) which actually returns the truth because field_proxy's constructor takes an `is_static_flag` parameter (line 11070).
- **repro:**
  ```cpp
  auto proxy = animal_class::static_method("kingdomCount");
  assert(proxy->is_static());  // FAILS - returns false
  ```
- **suggested_fix:** Either (a) overload the constructor with an explicit `bool is_static` parameter and have every call site that constructs from a `static_method`/`get_method(type_index,...)` path pass `true`, or (b) infer it inside `is_static()` itself by reading the method's JVM_ACC_STATIC bit: `return this->method && this->method->get_access_flags() && ((*this->method->get_access_flags()) & 0x0008u) != 0u;`. Option (b) is a one-line fix that also gives the right answer when an instance overload of `get_method()` happens to return a static method, and removes the dead `static_field` member entirely. Also rename the member - `static_field` on `method_proxy` is actively misleading.
- **confidence:** certain

### [medium] static get_method() returns instance methods silently - no JVM_ACC_STATIC check
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:13034-13070, 13087-13129, 13325-13338
- **description:** `object_base::get_method(type_index, name)` and the name+signature overload walk the superclass chain and return the FIRST method matching by name (lines 13056-13064) or name+signature (lines 13109-13122) with no `JVM_ACC_STATIC` access-flag check. The parity-equivalent `object_base::get_field(type_index, name)` at lines 12873-12909 explicitly does the check at line 12894 (`if (!entry->is_static)`) and bails out with a clear "needs an object instance" log message. So `static_method("instanceMethod")` will silently return a proxy with `object=nullptr` pointing at an instance method - and then `call_jni` will route through the static dispatch slots (143/116/etc, line 11931) with a jclass receiver and a non-static jmethodID. The JVM will respond with a JNI/JVM error or, worse on JDK 8 where jmethodIDs are raw Method*, just dispatch incorrectly. The user has no way to know they typo'd a static reference.
- **repro:**
  ```cpp
  // Dog has an instance speak() method, no static speak()
  auto proxy = dog_class::static_method("speak");      // returns a valid optional
  proxy->call();                                       // crashes / wrong dispatch / silent failure
  ```
- **suggested_fix:** Mirror the field path. After locating a candidate method on lines 13059-13060 / 13111-13118, check `((*current_method->get_access_flags()) & 0x0008u) != 0u` and skip non-static matches. After the loop, if any match was found but none were static, log `"object::static_method('{name}'): a method with this name exists but is not static. Use get_method() on an instance instead."` so the user knows what went wrong.
- **confidence:** certain

### [low] Static get_method() with name-only returns first-by-name even when class has both static and non-static overloads
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:13034-13070
- **description:** Even after the JVM_ACC_STATIC fix above, the name-only overload still picks whichever same-named method appears first in the InstanceKlass._methods array. If a Java class declares both `static void m()` and `void m(int)` (or any other instance overload), the iteration order is unspecified - the user can get either. The instance-side `get_method(name)` has the same first-by-name pitfall but resolves correctly later in `call_jni` because `resolve_compatible_method<...>()` (line 11551) re-walks the live OOP's klass hierarchy against the C++ arg types. For static calls `this->object == nullptr` so `resolve_compatible_method` (line 12611) takes the early `return this->method` branch on its second attempt, and the wrong overload sticks.
- **repro:** Author a Java class with both `static int m(int)` and `int m(long)`; `static_method("m")->call(int32_t{1})` may or may not pick the int overload depending on InstanceKlass layout.
- **suggested_fix:** Have `resolve_compatible_method` fall back to walking the *resolved class* (via `resolve_klass(typeid(derived))` or by stashing the wrapper type_index on the proxy at construction) instead of bailing when `this->object` is null. Alternatively, make the name-only `static_method` log a warning when multiple same-named methods exist and recommend the explicit-signature overload.
- **confidence:** likely

### [low] cached_class_handle on method_proxy stores a JNI local ref - dangling after the originating frame pops
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:11619, 11632, 11650, 11737, 12657
- **description:** In the static path, `class_handle` comes from `jni_find_class` (line 11619) which returns a JNI local reference. In the instance path it comes from `jni_get_object_class` (line 11642). Both are then cached on the proxy at line 11632/11650 and reused on every subsequent call as the JNI receiver (line 11737). JNI local refs are only valid for the duration of the JNI frame that created them - the moment the hook detour returns, that local ref slot can be reused for a different object. A `method_proxy` stored as a class member (or even a static-lifetime cache) will then have its `cached_class_handle` silently aliased to the wrong jclass. This is most likely to bite static-method users because they're more likely to cache the proxy on the C++ wrapper to avoid re-resolving every call.
- **repro:** Build any program that constructs a `static_method(...)` proxy in hook frame A, returns from A, then later in hook frame B calls `proxy->call()` after enough other JNI activity has churned the local ref table.
- **suggested_fix:** Promote `cached_class_handle` to a JNI global ref via `NewGlobalRef` immediately after acquisition, store with a destructor that calls `DeleteGlobalRef`. Or accept the risk and re-resolve every call with a comment explaining why caching the jclass is unsound.
- **confidence:** likely

## Improvements

### [S] [USER_FACING] Add a static_method(name, signature) doc-comment example mirroring static_field
- **rationale:** The existing static_field docs at lines 13313-13320 show no usage example. `static_method` has TWO overloads (name-only, name+sig) but the brief comments at 13322-13338 don't explain when to pick which. Users hit the overload-ambiguity bug above (#3) and don't know that `static_method("m", "(I)V")` is the safer call. Add a usage block to the doc comment.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:13322-13338
- **suggested_change:** Replace the two single-line doc blocks with:
  ```cpp
  /*
      @brief Explicit static method accessor.  Portable across MSVC, Clang, GCC.
      @details
      Use this from a static C++ method of the wrapper class.  When the Java class
      has overloaded static methods sharing this name, this overload returns the
      FIRST one found in the methods table - prefer the (name, signature) overload
      below for unambiguous selection.

      Usage:
          static auto count() -> int { return static_method("kingdomCount")->call(); }
  */
  ```
  And on the name+signature overload, document the JVM descriptor format briefly.

### [S] [USER_FACING] Static get_method log messages don't say "static" in the bare-name overload until after the failure
- **rationale:** Lines 13067 logs "method not found in class hierarchy" without telling the user that they called `static_method("foo")` specifically - if they're using deducing-this `get_method("foo")` from a static context (the MSVC/Clang case), the (static) tag is the only hint that they took the wrapper_type/typeid path rather than the instance path. The line is correct but could mention "(static, via wrapper_type)" to be more self-explanatory in logs.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:13067-13068, 13125-13127
- **suggested_change:** Log "object::static_method('{name}'): no method with this name found in class hierarchy of registered type." instead of "object::get_method('{name}') (static): method not found...". The "(static)" suffix today reads like the resolution context, not as a hint about which API to look at.

### [S] [INTERNAL] Add a constexpr capability flag for the portable static_method names
- **rationale:** The README/example.cpp comments at lines 47-52 describe the MSVC/Clang vs GCC split for `get_method` deducing-this. The compiler-feature macro `VMHOOK_HAS_DEDUCING_THIS` exists (line 13254) but is internal. Exposing a documented `constexpr bool vmhook::static_method_via_get_method_supported` (a one-liner alias) lets users write a `static_assert` to enforce one style or the other in their codebase.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:13254 (near `VMHOOK_HAS_DEDUCING_THIS`)
- **suggested_change:** Add at namespace scope: `inline constexpr bool static_method_via_get_method_supported = VMHOOK_HAS_DEDUCING_THIS != 0;`

### [XS] [INTERNAL] Member name static_field on method_proxy is actively misleading
- **rationale:** Reading lines 12647-12649, `static_field` on a class called `method_proxy` looks like a copy-paste leftover from `field_proxy`. Combined with the constructor always setting it to false (line 11506) and `is_static()` returning it verbatim (line 12329), this is a textbook trap for the next reader. Rename to `is_static_method` and (per Bug #1) actually plumb it through.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:11506, 12172, 12329, 12649
- **suggested_change:** Single-shot rename + correct initialisation. If Bug #1's fix (b) is taken, just delete the member entirely.

### [S] [USER_FACING] static_method returns std::optional but never explains failure modes in one place
- **rationale:** A user calling `static_method("foo")->call()` gets a hard crash when foo doesn't exist (UB on optional<>::operator->). The docs at 13322-13338 don't enumerate failure modes. Field side has the same issue but at least logs distinguish "field not found" from "needs an instance". Add a single sentence mentioning the failure modes and the log.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:13322-13338
- **suggested_change:** Add `@return nullopt if no static method with this name is declared on the registered wrapper type (logged via VMHOOK_LOG with the error_tag).`

## Tests

### [standalone_unit] [extend_existing] test_unified_call_syntax_static_method_is_static
- **description:** Compile-only test extending the existing unified-call-syntax fixture to assert that `static_method("m")->is_static()` returns true. Currently fails because of Bug #1.
- **asserts:** After resolving via static_method, the returned proxy reports is_static()==true. Mirror the proof that the field side already has.
- **existing_file:** tests/test_unified_call_syntax.cpp

### [jvm_integration] [new] test_static_method_rejects_instance_methods
- **description:** Verifies Bug #2's fix - calling `dog_class::static_method("speak")` (where speak is declared as an instance method on Dog/Animal) returns nullopt rather than a proxy that crashes at call time.
- **asserts:** `assert(!dog_class::static_method("speak").has_value());` and that the diagnostic line appears in the VMHOOK log buffer.

### [jvm_integration] [new] test_static_method_overload_signature_disambiguates
- **description:** Adds two static overloads of the same name to a fixture (e.g. `static int kingdomCount()` and `static int kingdomCount(int multiplier)`), then verifies `static_method("kingdomCount", "(I)I")->call(7)` dispatches the 1-arg overload and `static_method("kingdomCount", "()I")->call()` dispatches the no-arg overload. Catches the static-side `resolve_compatible_method` early-return bug (#3).
- **asserts:** Two distinct return values.

### [jvm_integration] [extend_existing] test_static_method_caching_survives_local_ref_invalidation
- **description:** Constructs a `static_method` proxy on Animal.kingdomCount in one hook frame, returns, allocates enough JNI local refs in unrelated hook frames to churn the local ref table, then calls the proxy. Catches Bug #4 if cached_class_handle becomes dangling.
- **asserts:** The second call still returns 6 (the literal in Animal.kingdomCount).
- **existing_file:** vmhook/src/example.cpp (extend the existing Animal probe path)

### [standalone_unit] [new] test_method_proxy_is_static_reports_access_flag_bit
- **description:** Pure compile/link-level check that, given a method_proxy whose Method* has JVM_ACC_STATIC set, `is_static()` returns true; given one without, returns false. Cannot actually be exercised without a real JVM, but the test_api_surface.cpp pattern already builds wrappers that exercise the signatures.
- **asserts:** Compile-time `static_assert` that `is_static()` is declared `const noexcept -> bool`; runtime path skipped if no JVM.

### [jvm_integration] [extend_existing] test_static_method_on_interface_default_method
- **description:** Animal.greet() is a Java 8 interface DEFAULT method (instance). Verify `static_method("greet")` rejects it (after Bug #2 fix), and `get_method("greet")->call()` on a Dog instance still works. Closes a gap where interface-declared methods could leak through the static path.
- **asserts:** `assert(!animal_class::static_method("greet").has_value());`
- **existing_file:** example/vmhook/Animal.java (no change needed) + vmhook/src/example.cpp (add assertion)

## Parity Concerns
- `method_proxy::is_static()` does not actually report whether the method is static; the parallel `field_proxy::is_static()` does (constructor takes `is_static_flag`, member is honestly named).
- Static `get_method(type_index, name)` does NOT check JVM_ACC_STATIC; static `get_field(type_index, name)` DOES (`if (!entry->is_static)` at line 12894). Calling `static_method` on an instance method silently succeeds; calling `static_field` on an instance field returns nullopt with a helpful log.
- `static_method` has no equivalent of `static_field`'s "needs an object instance" log. Add a parallel diagnostic.
- Static method overload resolution (when the same name appears twice) silently picks first; the instance side resolves via the live OOP's klass walk in `resolve_compatible_method`. The static side has no equivalent recovery path.
- `cached_class_handle` (jclass local ref) reuse across calls is an issue on both static and instance paths, but the static path makes the bug more reachable because users typically hold static_method proxies as class statics.
