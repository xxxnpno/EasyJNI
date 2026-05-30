# field_proxy_object_ref_unique_ptr

## Summary
Audited `field_proxy::value_t::cast_for_variant<unique_ptr<T>>` (vmhook.hpp:10972-10988), the implicit conversion that lets `unique_ptr<wrapper_type> x = get_field("ref")->get();` work for reference-typed fields. The null-oop case is handled correctly, but **no wrapper-vs-runtime klass match check exists** and **no signature-shape check exists**, so a stale or wrong-typed field silently produces a wrapper around the wrong object — every subsequent `get_field` / `get_method` call through that wrapper reads/writes JVM memory using the wrong klass layout. Additionally, this path bypasses `g_type_factory_map` entirely (contradicting the documented design at vmhook.hpp:1447, 6821-6830), creating a serious parity gap with `frame::extract_frame_arg`.

## Bugs

### [high] No wrapper-klass match check; wrong-type object yields a silently-broken wrapper
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:10972-10988
- **description:** When the field's runtime class is not assignable to `wrapper_type` (e.g. field declared `Animal` but stores a `Cat`, while the user wrote `unique_ptr<dog_class>`), this code unconditionally calls `new wrapper_type{ decoded }`. `dog_class::get_field("breed")` then looks up "breed" on the **dog_class-registered klass** (resolved via `typeid(*this)` in `object_base::resolve_klass`), and reads at `cat_instance + dog_breed_offset` — undefined memory access, silent data corruption, no diagnostic. The task brief explicitly calls this out as "Wrapper klass match check".
- **repro:** Java: `Animal a = new Cat();` in a field declared `Animal`. C++: `register_class<dog_class>("Dog"); register_class<cat_class>("Cat");` then `unique_ptr<dog_class> d = obj.get_field("animal")->get();` — `d` is non-null, and `d->get_field("breed")` returns garbage / corrupts memory.
- **suggested_fix:** Before constructing the wrapper, resolve the OOP's narrow-klass header, look up the registered klass for `wrapper_type` via `type_to_class_map`, walk the runtime klass's super-chain (or compare equal), and return `nullptr` with a `VMHOOK_LOG(error_tag, ...)` if the runtime class is not the wrapper's class or a subclass. The header at vmhook.hpp:153 already says the framework "wraps every object whose narrow-klass header matches" — the field path should honor that contract too.
- **confidence:** certain

### [medium] cast_for_variant bypasses g_type_factory_map, contradicting documented design and creating parity gap
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:10972-10988
- **description:** Comment at vmhook.hpp:1447 and 6821-6824 explicitly states that `g_type_factory_map` is "Used by field_proxy::get_as<T>() and frame::get_arguments() to construct C++ wrapper objects from decoded Java object references." `frame::extract_frame_arg` (vmhook.hpp:7216-7237) does follow this path (looks up `type_to_class_map` then `g_type_factory_map`, calls the factory). But `field_proxy::cast_for_variant<unique_ptr<T>>` does NOT — it bypasses both maps and inlines `new wrapper_type{ decoded }`. Consequences: (1) `wrapper_type` must be complete at every field-access call site, whereas frame's path tolerates forward declarations; (2) any future smarts added to the registered factory (e.g. interning, klass check, debug pool) silently apply only to detour args, not field reads; (3) the comment "Used by field_proxy::get_as<T>()" references a method that does not exist anywhere in the codebase (grep confirms zero hits outside the two docs comments).
- **repro:** Forward-declare a wrapper class in a header (no body), then write `unique_ptr<my_wrapper> = get_field("ref")->get();` in that header — instantiation fails with incomplete-type error inside `new my_wrapper{...}`. The same expression compiles cleanly when used as a frame argument because the factory was registered with a complete type.
- **suggested_fix:** Replace the inline `new wrapper_type{decoded}` with a `type_to_class_map` + `g_type_factory_map` lookup mirroring vmhook.hpp:7224-7236, then `static_cast<wrapper_type*>(factory(decoded))`. If `wrapper_type` is unregistered, `VMHOOK_LOG(error_tag, ...)` and return `nullptr`. Also delete the misleading `field_proxy::get_as<T>()` reference at lines 1447 and 6821, or actually implement `get_as<T>()` as a thin public wrapper for `static_cast<T>(get())` (see Improvements).
- **confidence:** certain

### [medium] No signature-shape check: array OOPs are silently wrapped as objects
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:10972-10988
- **description:** `cast_for_variant<unique_ptr<T>>` does not inspect `this->signature`. If the user writes `unique_ptr<my_wrapper> = get_field("ints")->get()` where the field signature is `[I` (int array), the code happily decodes the compressed OOP as an object pointer and constructs `my_wrapper{ int_array_oop }`. Now every `wrapper.get_field("foo")` reads at `array_header + foo_offset_in_unrelated_class` — same memory-corruption vector as the klass-mismatch bug above, but from a static type bug instead of a dynamic type bug. The sibling vector path at vmhook.hpp:10961-10970 already routes through `this->signature` via `read_array_value`; this branch should do likewise.
- **repro:** `unique_ptr<my_wrapper> w = obj.get_field("intArray")->get();` where `intArray` is `int[]` — `w` is non-null and any further use is UB.
- **suggested_fix:** At the top of the unique_ptr branch, check `this->signature.starts_with('L')` (and not `'['`). On mismatch, `VMHOOK_LOG(error_tag, "field_proxy: cannot decode array signature '{}' as unique_ptr<{}> - target must be a vector or wrapper for the element type", signature, typeid(wrapper_type).name())` and return `nullptr`. This is a one-line guard and converts a silent UB into a loud, actionable warning.
- **confidence:** certain

## Improvements

### [S] [USER_FACING] Add explicit `field_proxy::get_as<T>()` method to fulfill the documented API and reach parity with `method_proxy::call<T>()`
- **rationale:** Comments at vmhook.hpp:1447 and vmhook.hpp:6821 reference `field_proxy::get_as<T>()` as the explicit-typed getter; it does not exist. Users currently must spell the type at the assignment site (`unique_ptr<T> x = field->get();`) which forces them to declare a local just to trigger the implicit conversion. `method_proxy::call<T>(...)` already takes an explicit return-type template parameter — the parity gap was explicitly called out in the audit brief.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:11086-11148 (add after `get()`)
- **suggested_change:** Add `template<typename target_type> auto get_as() const noexcept -> target_type { return static_cast<target_type>(this->get()); }`. One-line wrapper, lets users write `auto ptr = field->get_as<unique_ptr<dog_class>>();` or `int hp = field->get_as<int>();`. Update the comments at 1447 and 6821 to point at the real method.

### [S] [USER_FACING] Log diagnostic when unique_ptr conversion bails out silently
- **rationale:** Today, `cast_for_variant<unique_ptr<T>>` returns `nullptr` silently on three distinct failures: (1) source variant isn't `uint32_t` (wrong source type), (2) `decode_oop_pointer` returned null (legitimate null OOP — fine, no log needed), (3) `is_valid_pointer` rejected the decoded pointer (corrupt narrow-klass header — should log). The sibling paths (`to_vector` at 14393, `to_entries` at 14418, `read_java_string` at 14442) all log on the invalid-pointer path with `warning_tag` / `error_tag` and the compressed value in hex. Field's unique_ptr path is the only one that goes quiet.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:10978-10981
- **suggested_change:** Distinguish the two null paths: leave `!decoded` silent (legitimate null reference, very common), but log the `!is_valid_pointer(decoded)` case with `VMHOOK_LOG("{} field_proxy::cast_for_variant<unique_ptr<{}>>: decoded OOP 0x{:016X} is not a valid pointer (compressed=0x{:08X}, sig='{}')", vmhook::warning_tag, typeid(wrapper_type).name(), reinterpret_cast<std::uintptr_t>(decoded), value, this->signature);`. Matches the diagnostic shape used by to_vector / to_entries.

### [XS] [INTERNAL] Use `std::make_unique<wrapper_type>` instead of raw `new` in the wrapper construction
- **rationale:** `new wrapper_type{ decoded }` is exception-unsafe if `wrapper_type`'s constructor ever throws (today none do, but the lib is header-only and used in detours where users might add throwing wrappers). Sibling code at vmhook.hpp:13577 / 13646 uses `std::make_unique`. Comment chain at vmhook.hpp:1452-1460 explaining why `g_type_factory_map` uses raw pointers does NOT apply here — cast_for_variant has the complete `wrapper_type` at hand.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:10982
- **suggested_change:** Replace `return clean_target_type{ new wrapper_type{ decoded } };` with `return std::make_unique<wrapper_type>(decoded);`. Identical behavior, exception-safe, matches collection::to_vector style.

### [S] [USER_FACING] Add `static_assert` requiring `wrapper_type` to derive from `object_base`
- **rationale:** `cast_for_variant<unique_ptr<T>>` happily compiles for any T that takes a `void*` constructor (or anything constructible from a `void*`). A user who writes `unique_ptr<int> x = field->get();` triggers `new int{decoded}` — undefined behavior because `int` is not a JVM wrapper and never had any meaningful "construct from oop" semantic. `frame::extract_frame_arg` at vmhook.hpp:7216-7237 has the same gap; both should fail at compile-time with a clear message. `is_unique_object_ptr` (vmhook.hpp:7270-7280) already exists for exactly this check.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:10976
- **suggested_change:** Inside the unique_ptr branch, add `static_assert(std::is_base_of_v<vmhook::object_base, wrapper_type>, "field_proxy cast to unique_ptr<T>: T must derive from vmhook::object_base. Register T via vmhook::register_class<T>(\"java/internal/Name\") and ensure it inherits from vmhook::object<T>.");` right after the `using wrapper_type = ...;` line. Identical guard belongs in `extract_frame_arg`.

## Tests

### [standalone_unit] [new] test_field_proxy_unique_ptr_null_oop_returns_nullptr
- **description:** Construct a `field_proxy` over a stack buffer holding `uint32_t{0}` for signature `Ljava/lang/Object;`. Implicit-convert `proxy.get()` to `std::unique_ptr<vmhook::object<>>`. Result must be `nullptr` and no crash, no log spam.
- **asserts:** `unique_ptr` is empty; storage bytes are unchanged after the read.
- **existing_file:** tests/test_helpers.cpp (sits naturally after the size_guard test at line 1108)

### [standalone_unit] [new] test_field_proxy_unique_ptr_uninitialized_field_pointer_returns_nullptr
- **description:** Construct `field_proxy{ nullptr, "Ljava/lang/Object;", false }` (null field_pointer — the early-return path at vmhook.hpp:11091-11093). Convert to `unique_ptr<wrapper>`. The early-return injects `int32_t{0}` into the variant, so `cast_for_variant` must take the `else { return nullptr; }` branch at vmhook.hpp:10984-10987.
- **asserts:** unique_ptr is empty; no crash.
- **existing_file:** tests/test_helpers.cpp

### [standalone_unit] [new] test_field_proxy_unique_ptr_wrong_variant_type_returns_nullptr
- **description:** Construct `field_proxy` over signature "I" (so `get()` populates the `int32_t` variant alternative), then explicitly construct a `value_t` holding an int32_t and convert it to `unique_ptr<vmhook::object<>>`. The cross-type branch (source = int32_t, target = unique_ptr) at vmhook.hpp:10984-10987 must return nullptr.
- **asserts:** unique_ptr empty; no compile error.
- **existing_file:** tests/test_helpers.cpp

### [standalone_unit] [new] test_field_proxy_unique_ptr_static_assert_for_non_object_base_T (compile-fail / SFINAE check)
- **description:** Negative compile test verifying that `unique_ptr<int>` as a target for `cast_for_variant` would either fail to compile or be filtered out by SFINAE. Requires the static_assert improvement above; until then, document expected behavior as a TODO and skip-with-message.
- **asserts:** Static assertion message contains "must derive from vmhook::object_base".
- **existing_file:** tests/test_traits.cpp (already has trait coverage; this is a small compile-time check)

### [jvm_integration] [new] test_field_proxy_unique_ptr_klass_mismatch_rejected
- **description:** In example/vmhook (add Cat extending Animal, plus a field `Animal a = new Cat();` on `Main`), register both `dog_class` and `cat_class`. Read field through `unique_ptr<dog_class>` — after the klass-match fix, result must be `nullptr` (or a `cat_class` if the test reads through `unique_ptr<animal_class>`). Today this silently constructs a dog_class over a Cat OOP.
- **asserts:** `unique_ptr<dog_class>` is null when field stores a Cat; `unique_ptr<cat_class>` is non-null; `unique_ptr<animal_class>` is non-null (assuming superclass walk).
- **existing_file:** vmhook/src/example.cpp (extends the existing Animal/Dog harness at line 224-242)

### [jvm_integration] [new] test_field_proxy_unique_ptr_array_signature_rejected
- **description:** Field with signature `[I` (int array). Convert via `unique_ptr<dog_class> w = obj.get_field("intArray")->get();`. After the signature-shape fix, must be `nullptr` with a logged warning. Today it silently constructs a wrapper over the array OOP and any further `w->get_field(...)` is UB.
- **asserts:** unique_ptr is null; warning_tag entry appears in log capture.
- **existing_file:** new entry alongside example/vmhook/Main.java's `intArray` field declarations

### [jvm_integration] [extend_existing] test_field_proxy_unique_ptr_uses_g_type_factory_map
- **description:** Register `cat_class` with a custom factory that increments a counter. Read a Cat field through `unique_ptr<cat_class>`. After routing field reads through `g_type_factory_map`, the counter must be 1. Today the counter stays 0 because cast_for_variant inlines `new` and bypasses the registered factory.
- **asserts:** factory call counter == 1 per field read; wrapper has the expected `get_instance()` OOP.
- **existing_file:** vmhook/src/example.cpp (animal_class / dog_class section near line 224)

## Parity Concerns
- `method_proxy::call<T>(...)` accepts an explicit return-type template parameter; `field_proxy::get()` has no `get_as<T>()` despite docs at vmhook.hpp:1447 / 6821 referencing one. Audit brief explicitly cites this gap.
- `frame::extract_frame_arg` (vmhook.hpp:7216-7237) uses the `type_to_class_map` + `g_type_factory_map` factory path; `field_proxy::cast_for_variant<unique_ptr<T>>` inlines `new wrapper_type{decoded}` instead. Identical input (decoded oop + target unique_ptr type) should produce identical output via identical machinery.
- `frame::extract_frame_arg` returns `nullptr` silently on every failure; `field_proxy::cast_for_variant` does likewise. Sibling helpers `value_t::to_vector` / `to_entries` log on invalid pointers. Unique_ptr path should match the to_vector / to_entries diagnostic style for consistency.
- `field_proxy::value_t::to_vector<T>()` (vmhook.hpp:14386-14400) constrains T only via duck-typing (`make_unique<T>(oop)` must compile); same lack of `is_base_of_v<object_base, T>` static_assert. A library-wide static_assert in one place (e.g. in `cast_for_variant`'s unique_ptr branch, in `to_vector`, in `to_entries`, in `extract_frame_arg`) would prevent every variant of the same misuse.
- `set()` for `unique_ptr<wrapper>` at vmhook.hpp:11187-11206 ALSO has no klass match check — it happily writes a wrapper's OOP into an arbitrarily-typed reference field. Symmetric bug to the get path; fix should land in both directions.
