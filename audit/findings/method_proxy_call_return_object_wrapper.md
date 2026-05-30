# method_proxy_call_return_object_wrapper

## Summary
Audited the `method_proxy::value_t` conversion operator (vmhook.hpp:11470-11494), the call-stub return decode (vmhook.hpp:12269-12292), and the JNI Object-return path (vmhook.hpp:12024-12067). The single biggest issue is the **complete absence of `std::unique_ptr<wrapper>` support** in `method_proxy::value_t::operator target_type()` — the sibling `field_proxy::value_t::cast_for_variant` (lines 10972-10988) handles it correctly, but `method_proxy` silently returns a null `unique_ptr` for every reference-returning Java method, with no log line. Secondary issues: the JNI-Object path stores a *truncated jobject indirect handle* as `uint32_t` and then the conversion operator mis-decodes it as a compressed OOP via `decode_oop_pointer`, returning garbage as the "raw heap pointer"; `std::string` cannot be obtained from the call-stub path; and the value_t has no `to_vector`/`to_entries` collection helpers despite `field_proxy::value_t` having them.

## Bugs

### [high] Title: `method_proxy::value_t` cannot convert to `std::unique_ptr<wrapper>` — silent null on every Object-returning Java method
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:11470-11494
- **description:** The conversion operator only special-cases `void*` with a stored `uint32_t`; for `std::unique_ptr<my_wrapper>` the `requires { static_cast<target_type>(v); }` clause fails (you cannot `static_cast` an integer/string/monostate to a `unique_ptr`), so the catch-all returns a default-constructed `target_type{}` — i.e. an empty `unique_ptr`. The sibling `field_proxy::value_t::cast_for_variant` (vmhook.hpp:10972-10988) detects `is_unique_ptr_v<clean_target_type>`, decodes the compressed OOP, and constructs `new wrapper_type{ decoded }`. Without this, any user who writes `std::unique_ptr<my_class> obj = player_proxy->get_method("getInventory")->call();` silently gets `nullptr` with NO `VMHOOK_LOG` and assumes the Java method returned null. This is the exact parity gap the audit brief calls out.
- **repro:** Register a wrapper, call a Java method that returns an object: `auto inv = player->get_method("getInventory")->call(); std::unique_ptr<inventory_wrapper> w = inv;` — `w` is always null, even when the JVM dispatch succeeded and produced a real heap pointer.
- **suggested_fix:** Mirror `field_proxy::value_t::cast_for_variant`. Inside the existing `std::visit` lambda add an `if constexpr` branch:
  ```cpp
  if constexpr (vmhook::detail::is_unique_ptr_v<target_type>) {
      using wrapper_type = typename vmhook::detail::is_unique_ptr<target_type>::value_type_t;
      static_assert(std::is_base_of_v<vmhook::object_base, wrapper_type>,
                    "method_proxy::value_t -> unique_ptr<T>: T must derive from vmhook::object_base");
      if constexpr (std::is_same_v<stored_type, std::uint32_t>) {
          void* const decoded{ vmhook::hotspot::decode_oop_pointer(v) };
          if (!decoded || !vmhook::hotspot::is_valid_pointer(decoded)) return nullptr;
          return target_type{ new wrapper_type{ decoded } };
      } else {
          return nullptr;
      }
  }
  ```
  Place it BEFORE the `void*` / `static_cast` arms.
- **confidence:** certain

### [high] Title: JNI-Object return path stores a TRUNCATED jobject indirect handle as `uint32_t`; conversion operator then mis-decodes it as a compressed OOP
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:12037-12067 (store), vmhook.hpp:11478-11483 (decode)
- **description:** On the JNI fallback path (call_jni), for `L`/`[` return types, `result_handle` is a JNI **indirect** local-ref (pointer-to-OOP-slot), not a compressed OOP. Code at line 12063-12064 truncates it via `static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(result_handle))` and stores it in the variant's `uint32_t` slot. Then when a caller does `void* p = result;`, the conversion operator at lines 11479-11483 routes the same `uint32_t` through `decode_oop_pointer` — which interprets it as a CompressedOops value and returns `narrow_oop_base + (handle_low32 << shift)`, which is a garbage pointer that's neither the JNI handle nor the underlying heap OOP. The 12057-12062 comment claims this loss is acceptable "so callers know it was non-null", but downstream callers who actually USE the pointer (cast to `void*` or pass to `unique_ptr<wrapper>`) get unusable bits. By contrast, the call_stub path at line 12291 also stores `result_holder` as `uint32_t` — and there `result_holder` *is* the decoded 64-bit heap pointer, so truncating loses the upper 32 bits of a valid pointer; decoding that truncated value again through `decode_oop_pointer` is doubly wrong.
- **repro:** Hook any method that returns an Object on a JDK 21+ build (where call_stub is missing so call_jni runs). Cast the result to `void*` — it never equals the actual object address; on a JVM with `narrow_oop_base != 0` it points outside the heap.
- **suggested_fix:** Either: (a) add a fresh variant alternative `std::uint64_t` to hold full pointers and have the `L/[` JNI path decode the local ref into the underlying OOP via `vmhook::detail::jni_oop_handle`-style dereference *then* store the full 64-bit value; or (b) widen the variant's reference alternative to `std::uintptr_t` and have `operator target_type()` skip the `decode_oop_pointer` step. Both call sites (12063, 12291) and the comment at 11461-11468 must move together so the variant's `uint32_t` invariant ("always a compressed OOP") is restored — or replaced by a clearly named alternative like `struct raw_pointer { void* p; };`. Without this, every reference-returning call is broken on at least one path.
- **confidence:** likely

### [medium] Title: `std::string` cannot be obtained from the call-stub path; only the JNI fallback can produce it
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:11470-11494 (operator), vmhook.hpp:12291 (store)
- **description:** On the JNI path at line 12054 a `Ljava/lang/String;` return value is eagerly decoded to `std::string` and stored as `std::string` in the variant — `static_cast<std::string>(std::string)` works at the conversion operator. But on the call-stub path (line 12291), every reference return is stored as `static_cast<std::uint32_t>(result_holder)`, and the conversion operator has no `std::string` arm at all: `requires { static_cast<std::string>(uint32_t); }` fails, the catch-all returns `std::string{}` (empty). So `std::string s = my_method.call();` reliably returns "" whenever call_stub is available (most JDK 8-20 builds), regardless of what the Java method actually returned. The sibling `field_proxy::value_t::cast_for_variant` at lines 10950-10960 handles this correctly by calling `read_java_string(decode_oop_pointer(value))`.
- **repro:** On JDK 17 (call_stub is present), `std::string s = some_proxy->get_method("toString")->call();` always returns "" even when the Java method returned a non-empty string.
- **suggested_fix:** Add an `if constexpr (std::is_same_v<target_type, std::string>)` arm in the conversion operator that, for `stored_type == std::uint32_t`, calls `vmhook::read_java_string(vmhook::hotspot::decode_oop_pointer(v))`; for `stored_type == std::string`, returns `v`. Make `v` capturable by reference (drop `auto v` for `auto& v`) so the `std::string` alternative isn't copied unnecessarily.
- **confidence:** certain

### [medium] Title: Conversion operator silently produces a default-constructed value on every "stored variant can't be cast to target" pair, with no log
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:11484-11491
- **description:** When the `requires` clause fails (e.g. user asks for `int` but variant holds `std::string`, or asks for `unique_ptr<T>` and we fall into the catch-all), the lambda returns `target_type{}`. There is no `VMHOOK_LOG` and no diagnostic. Compared to `method_proxy::call_jni` which logs richly with method name + signature on every failure (e.g. 11532, 11571, 11598, 11627), the conversion operator is silent. Users who hit this just see "the method returned 0 / empty / null" with no hint that the actual variant alternative was incompatible.
- **repro:** Call a method returning Object and try to convert to `int`. You get 0 with no log.
- **suggested_fix:** Add a one-line `VMHOOK_LOG` in the catch-all arm naming the target type (`typeid(target_type).name()`) and the held alternative index (`data.index()`), at WARN level. Gate behind a debug flag if the noise is a concern, but defaulting to silent is the wrong tradeoff given the rest of the file logs aggressively.
- **confidence:** certain

### [low] Title: Comment at lines 11461-11468 claims `uint32_t -> void*` returns the "FULL 64-bit decoded heap pointer", but on the JNI path it returns garbage (see bug #2)
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:11455-11468
- **description:** The doc-comment guarantees that `void*` conversion of a `uint32_t` stored value uses `decode_oop_pointer` to recover the heap pointer. That guarantee only holds on the call_stub path (which itself is buggy — see bug #2). On the JNI fallback path the variant holds a truncated `jobject` indirect handle, so the documented behaviour is silently violated.
- **repro:** Read the doc-comment; observe that it doesn't distinguish call_stub vs call_jni storage semantics.
- **suggested_fix:** Either fix the underlying storage (bug #2) so the comment is correct, or update the comment to say "for compressed-OOP returns only; reference-type JNI fallback returns are not recoverable as pointers".
- **confidence:** certain

## Improvements

### [S] [USER_FACING] Add `std::vector<unique_ptr<T>>` / array-returning conversion to `method_proxy::value_t`
- **rationale:** `field_proxy::value_t` has `to_vector<T>()` and `cast_for_variant<vector<T>>` (vmhook.hpp:11044-11046, 10961-10970) so users can read a Java array field as a typed vector. `method_proxy::value_t` has neither, so a Java method that returns `T[]` or `List<T>` cannot be unpacked at the call site without manually decoding the OOP and walking the array. This forces every caller to write the same boilerplate.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:11439-11495
- **suggested_change:** Mirror `field_proxy::value_t::to_vector<element_type>()` and add the matching `cast_for_variant` arm for `is_vector_v<target_type>`. Pull `read_array_value` into a free helper or duplicate the small implementation. Also add `to_entries<K, V>()` for `Map`-returning methods.

### [XS] [USER_FACING] Add explicit `method_proxy::call_as<T>(args...)` for typed returns (parity with `field_proxy::get_as<T>` that several audits have asked for)
- **rationale:** Today `auto r = proxy->call(args...); my_type t = r;` requires the user to declare a named local before the implicit conversion fires. With `call_as<my_type>(args...)` users can write `auto t = proxy->call_as<my_type>(args...);` and get type deduction in one line. The current signature `template<typename... args_t> auto call(args_t&&...)` already abuses the explicit-template-argument list (see line 13642 `call<std::uint32_t>(index)` which the reader is forced to mentally parse as "treat index as uint32_t" — not "return uint32_t"), creating confusion that an explicit `call_as<T>` would resolve cleanly.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:12106-12108
- **suggested_change:** Add `template<typename return_type, typename... args_t> auto call_as(args_t&&... args) const noexcept -> return_type { return static_cast<return_type>(call(std::forward<args_t>(args)...)); }`. Document this as the preferred typed-call entry point.

### [XS] [USER_FACING] Add `std::holds_alternative`-style helpers / `is_void()` / `is_null()` to `method_proxy::value_t`
- **rationale:** The variant holds `std::monostate` for void returns and for failure cases (lines 11442, 12113, 12130, etc.). There is no clean way to tell "method returned void" from "call failed" from a primitive zero. `field_proxy::value_t` has no monostate so the question doesn't arise there, but `method_proxy::value_t` does. Add `auto is_void() const noexcept { return std::holds_alternative<std::monostate>(data); }` and similar `is_string()` / `is_reference()` predicates.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:11439-11495
- **suggested_change:** A handful of one-liner predicates next to the conversion operator.

### [S] [INTERNAL] Capture variant alternative by `const auto&` instead of `auto` in the visit lambda to avoid copying `std::string`
- **rationale:** The visit lambda at line 11473 takes `auto v` by value, which copies `std::string` (the only non-trivially-copyable alternative). Cheap to fix.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:11473
- **suggested_change:** Change `[](auto v)` to `[](const auto& v)` (or `[]<typename stored_type>(const stored_type& v)`).

### [XS] [INTERNAL] Hoist the `data.index() == 0` (monostate) early-exit out of every visit branch
- **rationale:** Every call to `operator target_type()` on a failed call (monostate) walks the full `if constexpr` chain only to land in the catch-all. A single `if (data.valueless_by_exception() || data.index() == 0) return target_type{};` at the top of the operator removes a branch from the hot path and clarifies intent.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:11470-11494
- **suggested_change:** Early-return on monostate before `std::visit`.

### [XS] [USER_FACING] Document that `unique_ptr<wrapper>` return implies the target wrapper class was `register_class`-ed
- **rationale:** Once the bug #1 fix lands, the new path constructs `wrapper_type{ decoded }`. If `wrapper_type` requires class registration upstream (and many wrappers do, to drive type_index lookup) the failure mode is confusing. A `static_assert(std::is_base_of_v<vmhook::object_base, wrapper_type>, ...)` plus a doc note in the operator's @details block prevents the next user from blaming the conversion when the actual problem is missing `register_class`.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:11455-11494
- **suggested_change:** Static_assert + comment.

## Tests

### [standalone_unit] [new] test_method_proxy_value_t_unique_ptr_conversion
- **description:** Compile-time + runtime smoke test that `std::unique_ptr<my_wrapper> u = value_t{stored};` does the right thing for every variant alternative. Construct a `method_proxy::value_t` directly with a non-zero `uint32_t`, with a zero `uint32_t`, with `std::monostate`, and with `std::string`; convert each to `std::unique_ptr<my_wrapper>`.
- **asserts:**
  - `value_t{ std::uint32_t{0} }` -> null `unique_ptr`
  - `value_t{ std::monostate{} }` -> null `unique_ptr`
  - `value_t{ std::string{"junk"} }` -> null `unique_ptr`
  - `value_t{ std::uint32_t{0xdeadbeef} }` -> non-null `unique_ptr` when `decode_oop_pointer` produces a non-null pointer (mock CompressedOops base/shift via VMStruct test seam, or accept that without a JVM this case is just "compiles and runs")
  - `static_assert(std::is_constructible_v<std::unique_ptr<my_wrapper>, method_proxy::value_t>)` — purely compile-time guard against the silent default-construction bug regressing.
- **existing_file:** none — add `tests/test_method_proxy_value_t.cpp` and a CMake entry in `tests/CMakeLists.txt`.

### [standalone_unit] [extend_existing] test_traits — add `static_assert`s for `value_t -> unique_ptr<T>` conversion contract
- **description:** Extend `tests/test_traits.cpp` (which already pins down `is_unique_ptr<>::value_type_t` against the value_type-shadowing bug) with `static_assert`s on the API surface: `std::is_constructible_v<std::unique_ptr<test_wrapper>, vmhook::method_proxy::value_t>` and `std::is_convertible_v<vmhook::method_proxy::value_t, std::unique_ptr<test_wrapper>>`.
- **asserts:** The two `static_assert`s above must both pass after the bug-#1 fix. Before the fix, the `is_convertible` would still pass (because operator is templated) but the resulting `unique_ptr` is silently null — that's why the *runtime* test in the previous bullet is also necessary.
- **existing_file:** tests/test_traits.cpp

### [standalone_unit] [new] test_method_proxy_value_t_string_from_uint32
- **description:** Verifies that `std::string s = value_t{compressed_oop_uint32};` returns the underlying Java String contents (not an empty string). Without a JVM, asserts the more limited contract that conversion compiles and that `value_t{ std::string{"hi"} }` -> `"hi"` (i.e. eagerly-stored strings convert correctly).
- **asserts:**
  - `static_assert(std::is_convertible_v<vmhook::method_proxy::value_t, std::string>)`
  - `std::string{ vmhook::method_proxy::value_t{ std::string{"hi"} } } == "hi"`
  - After bug-#3 fix: with a mocked or live JVM, `std::string{ value_t{ encode_oop_pointer(java_string_oop) } } == "hi"`.
- **existing_file:** none — add a new standalone test.

### [standalone_unit] [new] test_method_proxy_value_t_silent_default_log
- **description:** Verify that converting to an unrepresentable type emits at least one `VMHOOK_LOG` warning so users have a breadcrumb. (Requires the bug-#4 fix.)
- **asserts:** Set up a capturing log sink, do `int x = value_t{ std::string{"oops"} };`, expect exactly one warning containing "value_t" and either the target type name or the held alternative index.
- **existing_file:** none — add `tests/test_method_proxy_value_t_log.cpp`.

### [jvm_integration] [extend_existing] example.cpp — exercise an Object-returning Java method and unwrap to `unique_ptr<wrapper>`
- **description:** Add a fixture Java method `public static Inventory getInventory(Player p)` and a wrapper, then in the integration driver call `player->get_method("getInventory")->call(p_proxy)` and assert the resulting `unique_ptr<inventory_wrapper>` is non-null. This exercises bug #1 (call_stub path on JDK 8/17) AND bug #2 (call_jni path on JDK 21+) end-to-end.
- **asserts:** After fix, `inv && inv->get_instance() == real_jvm_oop` on both call paths. Before fix, the call_stub path is silently null (bug #1) AND the call_jni path returns garbage when cast to `void*` (bug #2).
- **existing_file:** vmhook/src/example.cpp + example/vmhook/*.java fixtures.

### [jvm_integration] [new] test_method_proxy_call_array_return
- **description:** Java fixture method that returns `String[]` or `int[]`; the C++ side decodes it via the (proposed) `to_vector<T>()` helper. Verifies improvement #1 once added.
- **asserts:** Vector length matches, first/last elements match, empty array returns empty vector.
- **existing_file:** vmhook/src/example.cpp + new fixture method.

## Parity Concerns
- `field_proxy::value_t::cast_for_variant` (vmhook.hpp:10943-11008) handles `std::string`, `std::vector<T>`, `std::unique_ptr<T>`, and `void*` semantically; `method_proxy::value_t::operator target_type()` (vmhook.hpp:11470-11494) handles only `void*` and a static_cast fallback. Cited explicitly in the audit brief.
- `field_proxy::value_t` exposes `to_vector<T>()` and `to_entries<K,V>()` (vmhook.hpp:11044-11060) for collection-typed fields; `method_proxy::value_t` exposes neither, despite methods commonly returning `List`/`Map`/`T[]`.
- `field_proxy::get_as<T>()` is documented but missing (per `field_proxy_object_ref_unique_ptr.md` audit); the symmetric `method_proxy::call_as<T>()` is also missing here. Both should land together.
- `method_proxy::call_jni` logs richly on every failure path; `value_t::operator target_type` is silent on every conversion failure — inconsistent with the rest of the file.
- `field_proxy::value_t` has no `std::monostate` alternative, so its `cast_for_variant` never needs a "no value" early-out. `method_proxy::value_t` has `std::monostate` and needs predicates (`is_void()`, `is_null()`) so callers can distinguish "method returned void" from "call failed" from "primitive zero".
- The variant's `std::uint32_t` alternative is described as "reference / array (compressed OOP)" but on the JNI path it stores a truncated `jobject` indirect handle (vmhook.hpp:12063). Either the storage or the comment is wrong; pick one invariant and enforce it.
