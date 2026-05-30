# hook_arg_decoding_wrappers

## Summary
The wrapper-decoding path in `detail::extract_frame_arg` (vmhook.hpp:7184-7259) reads the interpreter local slot, decodes it as a compressed OOP, looks up the C++ wrapper's type_index in `type_to_class_map`, then runs the registered factory to construct a `std::unique_ptr<wrapper_type>`. Several silent-failure modes degrade the API: no klass cross-check between the C++-declared `unique_ptr<T>` and the actual Java object at the slot, no diagnostic when the wrapper is unregistered, lock-free reads of `type_to_class_map` despite documented contract requiring the lock, and a `decode_oop` heuristic that mis-decodes raw heap pointers on JVMs that disable compressed OOPs.

## Bugs

### [high] No klass cross-check between declared wrapper_type and actual OOP klass
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:7216-7237
- **description:** The unique_ptr branch decodes the OOP, looks up the factory for `element_t`, and constructs `new wrapper_type{ oop }` without verifying that the OOP's narrow klass actually matches the wrapper's registered klass. If the hooked method's declared parameter is `java/lang/Object` (or any base/interface type) and the user writes `const std::unique_ptr<MyConcreteClass>& arg`, vmhook silently wraps an OOP of an unrelated klass as `MyConcreteClass`. Any subsequent `arg->get_field("...")` reads at offsets that are invalid for the real klass, producing garbage data or a SEH crash. `for_each_instance` (line 6756) does this exact klass match; `extract_frame_arg` does not.
- **repro:** Hook `Collection.add(Object)` with a callback typed `(return_value&, unique_ptr<Self> self, unique_ptr<MyFoo> item)`. Call `coll.add(new SomeOtherClass())`. The detour fires; `item` wraps the `SomeOtherClass` OOP as if it were `MyFoo`; reading `item->get_field("fooField")` reads bytes from offsets defined for `MyFoo` against `SomeOtherClass`'s layout.
- **suggested_fix:** Before invoking the factory, decode the narrow klass from `oop + 8` via `vmhook::hotspot::decode_klass_pointer` and compare against `vmhook::find_class(type_it->second)`. If mismatch, log a `error_tag` diagnostic naming the expected klass, the actual klass name (via `klass::get_name()`), the wrapper type, and return `nullptr`. Same pattern as `for_each_instance` at line 6755-6759.
- **confidence:** certain

### [high] decode_oop heuristic mis-decodes uncompressed OOPs
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:7198-7210
- **description:** `decode_oop` decides compressed-vs-raw purely on `bits <= 0xFFFFFFFFull`. With `-XX:-UseCompressedOops` (mandatory on JVMs with >32 GB heap and selectable below that), heap addresses are full 64-bit pointers. Object refs in the lower 4 GB of the address space then trip the "compressed" branch and get re-decoded against `Universe::_narrow_oop_base/_shift`, yielding a garbage pointer that the factory blindly dereferences in `new wrapper_type{ oop }`. The compressed-vs-raw decision needs to be a one-time check of the VM flag `UseCompressedOops`, cached at startup, not a per-slot value heuristic.
- **repro:** Launch the target JVM with `-XX:-UseCompressedOops`. Install any hook whose detour takes `const std::unique_ptr<Wrapper>& self`. The first low-address `self` OOP gets fed to `decode_oop_pointer` as if it were compressed; the result lands somewhere unrelated. Either nullptr (if `_narrow_oop_base != 0`) or a wild pointer.
- **suggested_fix:** Read `UseCompressedOops` once via `iterate_struct_entries("Universe", "_narrow_oop._base") != nullptr` (or the canonical Universe flag) and cache the result in a static. Branch on the cached flag, not on the slot value's magnitude. Failing that, at least guard the post-decode pointer with `vmhook::hotspot::is_valid_pointer` before handing it to the factory.
- **confidence:** likely

### [medium] Factory invocation can throw std::bad_alloc out of the detour
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:7236, 6825-6830
- **description:** The factory lambda registered at line 6825 is `+[](void* instance) -> object_base* { return new wrapper_type{ instance }; }`. `extract_frame_arg` calls it unguarded at line 7236. If `operator new` throws `std::bad_alloc`, the exception propagates up through the pack-expansion in `wrapper_detour` (line 7819-7821) and out of the JVM detour. There is no catch in `wrapper_detour`, no catch in `common_detour` at this layer. A bad_alloc in a JVM-attached native thread mid-interpreter-frame is an SEH/SIGSEGV waiting to happen on the JVM side. The same risk applies to a wrapper type whose constructor itself throws.
- **repro:** Memory-pressure scenario, or any wrapper whose ctor throws (the for_each_instance path at line 6763-6777 wraps the equivalent construction in `try/catch` precisely for this reason).
- **suggested_fix:** Wrap `factory_it->second(oop)` in `try { ... } catch (const std::exception& ex) { VMHOOK_LOG(...); return nullptr; } catch (...) { return nullptr; }`. Mirror the pattern at line 6763-6777.
- **confidence:** certain

### [medium] type_to_class_map / g_type_factory_map read lock-free from hot detour
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:1431-1438, 7224-7232
- **description:** The block comment at line 1431-1438 documents that "every read / mutation of this map AND of g_type_factory_map below must hold registration_mutex" — and then says readers in detour-hot paths (`extract_frame_arg / jni_signature_for_arg / on_class_loaded`) intentionally do NOT take the lock under a "register_class is called from single-threaded setup BEFORE hooks fire" contract. This contract is not enforceable: `register_class<T>()` is a public templated API a user may call at any time, including from a hook callback (for lazy registration of nested wrappers). If a hook detour reads `type_to_class_map.find(...)` while another thread is in `insert_or_assign` and triggers a rehash, the reader walks corrupted bucket pointers and crashes. Same hazard for `g_type_factory_map`.
- **repro:** Two threads. Thread A: in a loop, call `register_class<W1>("a"); register_class<W2>("b"); ...` with enough distinct types to force rehashes. Thread B: in a loop, fire a hook whose detour takes `const std::unique_ptr<W>& self`. Race window is tiny but exists on any rehash boundary.
- **suggested_fix:** Either (a) take a shared_lock by switching `registration_mutex` to `std::shared_mutex` and adding `std::shared_lock` at the detour read sites, (b) move both maps to `tbb::concurrent_unordered_map`, or (c) document at register_class<T>() that the user must not call it from any thread once any hook is armed, and add a `debug-build` assert that catches it. Pick one; the current "don't take the lock and hope" path is a latent crash.
- **confidence:** likely

### [medium] Silent nullptr return on missing registration with no diagnostic
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:7224-7227, 7229-7232
- **description:** If the wrapper isn't registered (`type_to_class_map.find == end`) or the factory is missing (`g_type_factory_map.find == end`), extract_frame_arg returns `nullptr` with no `VMHOOK_LOG`. The user's detour fires with `self == nullptr`, with zero clue why. Every other branch of extract_frame_arg either succeeds or static_asserts. This is the single most common user error (forgot to call `register_class<Wrapper>("...")` before `hook<Wrapper>`) and the diagnostic exists nowhere on this path. Compare line 9494-9499 in `jni_signature_for_arg` which DOES log the same condition.
- **repro:** Write a hook callback `(return_value&, const std::unique_ptr<MyClass>& self, const std::unique_ptr<UnregisteredArg>& other)`. Hook fires; `other` is nullptr; nothing in the log says "UnregisteredArg was never registered".
- **suggested_fix:** Add a `VMHOOK_LOG(error_tag, ...)` at both 7224-7227 and 7229-7232 naming the wrapper type via `typeid(element_t).name()` and identifying the index. One line each. This is pure user-facing improvement at zero hot-path cost (already on the failure branch).
- **confidence:** certain

### [low] No is_valid_pointer guard on decoded OOP before factory call
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:7219-7236
- **description:** `field_proxy::cast_for_variant` at line 10978 guards `if (!decoded || !vmhook::hotspot::is_valid_pointer(decoded)) return nullptr;`. `extract_frame_arg` only checks `!oop`. If `decode_oop_pointer` returned a non-null but bogus pointer (mis-decoded as per the previous bug, or a torn read from a moving GC), the factory constructs `new wrapper_type{ oop }` and the wrapper's first field read crashes. The cheaper guard would happen here, once, before the factory invocation.
- **repro:** Combine with the decode_oop bug above (uncompressed JVM mode), or a Shenandoah/ZGC concurrent move while the detour reads the slot.
- **suggested_fix:** Add `if (!vmhook::hotspot::is_valid_pointer(oop)) return nullptr;` between line 7223 and 7224, mirroring the field_proxy pattern.
- **confidence:** likely

## Improvements

### [S] [USER_FACING] Surface the actual Java klass name in the type-mismatch log
- **rationale:** If the klass cross-check from the high-severity bug above is added, the diagnostic should name both expected and actual klass names so the user can immediately see whether their declared `unique_ptr<MyClass>` was wrong or whether the Java side passed a subclass / null.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:7216-7237
- **suggested_change:** When mismatch is detected, decode the actual narrow klass via `*reinterpret_cast<std::uint32_t*>(static_cast<std::uint8_t*>(oop) + 8)`, resolve via `decode_klass_pointer` and `klass::get_name()`, and emit `VMHOOK_LOG(error_tag, "extract_frame_arg: arg {} declared as unique_ptr<{}> (registered as '{}') but actual OOP klass is '{}'. Returning nullptr.", index, typeid(element_t).name(), type_it->second, actual_klass_name)`. Halves debugging time on type errors.

### [S] [USER_FACING] Reuse the `for_each_instance` construction try/catch wrapper
- **rationale:** Lifting the construction site to a `try { return base_t{ static_cast<element_t*>(factory_it->second(oop)) }; } catch (const std::exception& ex) { VMHOOK_LOG(error_tag, "extract_frame_arg<{}>: wrapper ctor threw: {}", typeid(element_t).name(), ex.what()); return nullptr; }` parallels the safety boundary already established at line 6763. The wrapper_detour lambda is currently exception-naive, and a ctor exception escapes into the JVM interpreter.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:7236
- **suggested_change:** Wrap the single-line return in a try/catch as above.

### [S] [INTERNAL] Hoist the wrapper-decoding logic into a named helper
- **rationale:** The same six-line block (decode, type_to_class_map lookup, factory lookup, factory call, downcast, wrap) is repeated almost verbatim at line 7216-7237 (extract_frame_arg), line 10972-10987 (field_proxy::cast_for_variant), line 13573-13577 (collection::to_vector), line 13970-13973 (set::to_vector), line 14060-14063 (map iteration). A single `detail::wrap_oop_as<wrapper_type>(void* oop)` helper would centralise the klass-mismatch check, the is_valid_pointer guard, the try/catch, and the consistent diagnostics — fixing all five sites at once and preventing future divergence.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:7216-7237, 10972-10987, 13573-13577, 13970-13973, 14060-14063
- **suggested_change:** Add a template helper:
  ```cpp
  template<typename wrapper_type>
  inline auto wrap_oop_as(void* oop) noexcept -> std::unique_ptr<wrapper_type>;
  ```
  Implement once with all the safety checks; refactor the five sites to call it. Single source of truth.

### [XS] [USER_FACING] Document the contract in extract_frame_arg's doxygen
- **rationale:** The doxygen for `extract_frame_arg` at line 7094-7113 doesn't mention that unregistered wrappers silently return nullptr, doesn't mention the klass match assumption, doesn't link to `register_class`. Surfacing those in the docblock costs nothing and helps the user.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:7094-7113
- **suggested_change:** Add a `@pre wrapper_type T must be registered via vmhook::register_class<T>("internal/java/Name") before the enclosing hook fires.` line and a `@note The OOP at the slot is wrapped as `T` without verifying that the actual Java klass matches T's registered klass; pass the exact declared parameter type from the Java signature.` warning.

### [XS] [INTERNAL] Static_assert that element_t derives from object_base
- **rationale:** Line 7218 takes `using element_t = typename base_t::element_type;` and immediately passes to the factory; a `unique_ptr<int>` user-arg would static-cast the factory result to `int*`, which is UB. The `is_unique_object_ptr` trait already exists at line 7270-7280 for this exact purpose. Use it.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:7216-7218
- **suggested_change:** Insert `static_assert(std::is_base_of_v<vmhook::object_base, element_t>, "extract_frame_arg: unique_ptr<T> hook arg must have T derive from vmhook::object_base; raw unique_ptr<int> etc. are not Java wrappers.");` between the `using element_t` and the `decode_oop` call. Mirrors line 9486 and 9584.

## Tests

### [standalone_unit] [new] decode_oop_threshold_at_4gb_boundary
- **description:** Verifies that `extract_frame_arg`'s magic `bits <= 0xFFFFFFFFull` threshold matches the design intent. Should be a static or trait-level check on a stub `decode_oop` rebuilt from the source, or — once the heuristic is replaced with a cached `UseCompressedOops` flag — verifying the flag plumbing.
- **asserts:** decode_oop(nullptr) == nullptr; decode_oop(0x12345678) routes through decode_oop_pointer; decode_oop(0x12345678'00000001) returns 0x12345678'00000001 unchanged. After the suggested fix, verify the flag-based branch.
- **existing_file:** tests/test_traits.cpp (extend)

### [standalone_unit] [new] is_unique_object_ptr_value_type_t_resolves_correctly
- **description:** Mirror of the `is_unique_ptr_v` regression tests at test_traits.cpp:38-86 for the sister trait `is_unique_object_ptr` at vmhook.hpp:7270-7280. Same value_type-shadowing risk; same prophylactic value of a static_assert test.
- **asserts:** `is_unique_object_ptr<unique_ptr<object_base>>::value == true`; `is_unique_object_ptr<unique_ptr<int>>::value == false`; `is_unique_object_ptr<int*>::value == false`.
- **existing_file:** tests/test_traits.cpp (extend)

### [jvm_integration] [new] hook_arg_klass_mismatch_logs_and_returns_null
- **description:** Hook a method `Foo.bar(Ljava/lang/Object;)V` typed as `(return_value&, unique_ptr<Foo>, unique_ptr<UnrelatedWrapper>)`. From Java, call `foo.bar(new SomeOtherType())`. With the suggested klass check in place, the second arg should be nullptr and the log should name the actual klass.
- **asserts:** second arg is nullptr; log contains "actual OOP klass is 'pkg/SomeOtherType'"; the hooked call does not crash.

### [jvm_integration] [new] hook_arg_unregistered_wrapper_logs_diagnostic
- **description:** Hook a method whose detour signature includes `unique_ptr<W>` where `W` was never passed to `register_class<W>`. Fire the hook. Assert that a log entry mentions `W` by name and that the arg arrives as nullptr.
- **asserts:** Log contains both `error_tag` and `typeid(W).name()`; arg is nullptr; no crash.

### [jvm_integration] [new] hook_arg_uncompressed_oops_decodes_correctly
- **description:** Launch the test JVM with `-XX:-UseCompressedOops`. Hook a method that receives a wrapper arg with an OOP whose low 32 bits look like a compressed OOP. Verify that the wrapper points at the right Java object rather than at a garbage decoded address. Today this fails; with the suggested flag-aware decode_oop it passes.
- **asserts:** `arg->get_instance() == expected_java_oop` (where expected_java_oop is the Java-side reference, recovered via JNI for cross-check).

### [standalone_unit] [extend_existing] type_factory_returns_correct_concrete_pointer
- **description:** Construct a fake wrapper type, register it, look up the factory in `g_type_factory_map`, invoke with a stub OOP, verify the returned pointer's dynamic type is the wrapper type (not just object_base*). Today the contract is implicit; a test pins it down.
- **asserts:** `dynamic_cast<wrapper*>(factory(oop)) != nullptr`; `static_cast<wrapper*>(...)->some_member` is reachable.
- **existing_file:** tests/test_helpers.cpp

### [jvm_integration] [new] hook_arg_factory_throw_does_not_escape_detour
- **description:** Register a wrapper whose constructor throws `std::runtime_error`. Hook a method whose detour takes that wrapper. Fire the hook. Confirm the JVM does not crash and the detour either skips that arg (nullptr) or logs and continues — but does not propagate the exception into the JVM interpreter.
- **asserts:** JVM continues running; log contains the exception's `what()`; subsequent calls into the hooked method still work.

## Parity Concerns
- `field_proxy::cast_for_variant` (line 10972-10987) guards with `is_valid_pointer(decoded)`; `extract_frame_arg` (line 7219-7236) does not — same conceptual operation, different safety surface.
- `for_each_instance` (line 6753-6759) cross-checks the decoded klass against the registered klass before constructing the wrapper; `extract_frame_arg` skips the same check.
- `for_each_instance` (line 6763-6777) wraps wrapper construction in `try/catch`; `extract_frame_arg` does not.
- `jni_signature_for_arg` (line 9496-9499) and `append_jni_arg` (line 9584-9588) `static_assert` that `wrapper_type` derives from `object_base`; `extract_frame_arg` accepts arbitrary `unique_ptr<T>` and `static_cast`s the factory return — UB if T is not object_base-derived.
- `jni_signature_for_arg` (line 9496-9499) emits a fallback log when the wrapper is unregistered; `extract_frame_arg` silently returns nullptr.
- `collection::to_vector` / `map::to_entries` reuse `std::make_unique<element_type>(static_cast<vmhook::oop_t>(...))` directly rather than going through the registered factory, meaning a user who registers a custom factory variant in g_type_factory_map sees inconsistent construction depending on which API decoded the OOP.
