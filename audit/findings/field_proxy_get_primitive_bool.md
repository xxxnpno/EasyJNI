# field_proxy_get_primitive_bool

## Summary
Audited the `"Z"` branch of `field_proxy::get()` in `vmhook.hpp` (lines 11087-11148), the null-pointer guard at lines 11090-11093, and the static/instance dispatch in `object::get_field()` (lines 12816-12856 and 12873-12909). Top concern: the bool branch reads the byte directly into a C++ `bool` via `memcpy`, which produces a non-canonical bool whenever the source byte is anything other than 0x00 or 0x01 (UB to subsequently observe). This diverges from the safe pattern used everywhere else in the file - `method_proxy::call()` (line 12271), `call_jni` (line 11952), and the bool-array element reader (line 10833) all normalize through `(raw & 1) != 0` or `raw != 0`. Secondary concern: the null-pointer fallback returns a `std::int32_t{}` alternative for *every* signature including `"Z"`, breaking the documented invariant that `"Z"` populates the `bool` variant alternative.

## Bugs

### [medium] Raw memcpy into `bool` can synthesize a non-canonical bool value
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:11095-11100
- **description:** The `"Z"` branch does `bool value{}; std::memcpy(&value, this->field_pointer, sizeof(value));`. The C++ object representation of `bool` only permits the values 0 and 1; copying any other byte pattern produces an indeterminate / non-canonical bool. While HotSpot's interpreter and C2 normally store 0/1 for boolean fields, several real-world conditions can leave a non-canonical byte in that slot: (a) reading the field mid-write from another thread (HotSpot can briefly write a wider value before narrowing - this is the documented reason `Unsafe.getBoolean` masks the byte), (b) a third-party agent or sun.misc.Unsafe caller stuffing arbitrary bytes via `putByte` on the same offset, (c) HotSpot debug builds intentionally writing 0xAA into uninitialized fields. Any subsequent operation on the non-canonical bool (the implicit conversion to `int`, the variant storage, the `static_cast<bool>` in `cast_for_variant`) is UB; in practice with `/O2` MSVC and `-O2` GCC we have seen non-canonical bools convert to `2`, `200`, etc. when fed into `static_cast<int>`. Every sibling code path in this same file already protects against this: `method_proxy::call()` line 12271 does `(result_holder & 1) != 0`, `method_proxy::call_jni` line 11952 does `r != 0`, `append_array_value<vector<bool>>` line 10833 does `get_array_element<uint8_t>(...) != 0`. Only `field_proxy::get()` for `"Z"` reads raw.
- **repro:** `std::uint8_t storage{0x02}; vmhook::field_proxy fp{&storage, "Z", false}; bool b = fp.get(); int i = b; // i may be 2, not 1.` Then for a real-world repro, hook a HotSpot debug-build's freshly-allocated object whose boolean field has not yet been overwritten by the constructor and read it via `field_proxy::get()`.
- **suggested_fix:** Mirror the array-element pattern at line 10833. Replace lines 11095-11100 with:
  ```cpp
  if (this->signature_text == "Z")
  {
      std::uint8_t raw{};
      std::memcpy(&raw, this->field_pointer, sizeof(raw));
      return value_t{ raw != 0, this->signature_text };
  }
  ```
  Reads through `uint8_t` (legal for any byte), then normalizes to canonical `true`/`false`.
- **confidence:** likely

### [low] Null-pointer fallback emits wrong variant alternative for `"Z"` (and every other signature)
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:11090-11093
- **description:** When `field_pointer` is null the function returns `value_t{ std::int32_t{}, this->signature_text }` regardless of the field's signature. The class-level documentation (lines 11077-11086 and 10805-10808) and the dispatch table promise that signature `"Z"` populates the `bool` alternative, `"L"` populates `uint32_t`, etc. After this fallback the variant holds `int32_t{0}` but `signature` says `"Z"`. The user-facing implicit-conversion path still works for `bool`/numeric targets (static_cast(0) yields false/0), but any caller that does `std::visit` on `value_t::data` or uses `std::get<bool>()` will observe the broken invariant - `std::get<bool>` will throw `std::bad_variant_access`. For reference signatures (`"L..."`, `"["`) the fallback is even worse: caller doing `static_cast<std::string>(proxy->get())` or `proxy->get().to_vector<T>()` on a null-pointer proxy will silently get empty results because `cast_for_variant<string>` short-circuits when the source isn't `uint32_t` (line 10952-10959), instead of attempting a string decode.
- **repro:** `vmhook::field_proxy fp{nullptr, "Z", false}; auto v = fp.get(); std::visit([](auto x){ /* assumes bool when sig=="Z" */ }, v.data); // gets int32_t, not bool` Also: `vmhook::field_proxy fp{nullptr, "Ljava/lang/String;", false}; std::string s = fp.get(); // silently returns "" because variant is int32_t, not uint32_t`
- **suggested_fix:** Either (a) dispatch on the signature character before constructing the value_t (use the same switch as the main body, just default-construct each type instead of memcpy'ing), or (b) reuse `vmhook::detail::jvm_primitive_byte_width()` plus an empty staging buffer so the same code path runs. Minimal fix specific to `"Z"`:
  ```cpp
  if (!this->field_pointer)
  {
      if (this->signature_text == "Z") return value_t{ false, this->signature_text };
      if (this->signature_text == "B") return value_t{ std::int8_t{}, this->signature_text };
      // ... and so on; or factor into a helper.
      return value_t{ std::int32_t{}, this->signature_text }; // fallback
  }
  ```
  Also add a `VMHOOK_LOG` warning - silently returning a synthesized value when `field_pointer` is null hides bugs (`get_field` already logs when it returns nullopt, but a constructor caller that supplies a null pointer directly gets no signal).
- **confidence:** certain

## Improvements

### [S] [INTERNAL] Eliminate the if/else-chain repetition in `get()` with a helper
- **rationale:** Lines 11095-11148 are nine near-identical 5-line blocks that differ only in the C++ type and the signature character. The repetition makes it easy to miss a sibling change (the bool-from-raw-byte bug exists precisely because no one noticed all the other branches read pure-size-matched types). A single helper template like `template<typename T> value_t read_as() const { T v{}; memcpy(&v, field_pointer, sizeof(v)); return value_t{v, signature_text}; }` invoked from a flat `if/else if` chain would shrink the body to ~12 lines and centralize any future logging or bounds checks.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:11095-11148
- **suggested_change:**
  ```cpp
  auto read_as = [this]<typename T>(std::in_place_type_t<T>) noexcept -> value_t
  {
      T v{};
      std::memcpy(&v, this->field_pointer, sizeof(v));
      return value_t{ v, this->signature_text };
  };
  if (this->signature_text == "Z")
  {
      std::uint8_t raw{};
      std::memcpy(&raw, this->field_pointer, sizeof(raw));
      return value_t{ raw != 0, this->signature_text };
  }
  if (this->signature_text == "B") return read_as(std::in_place_type<std::int8_t>);
  if (this->signature_text == "S") return read_as(std::in_place_type<std::int16_t>);
  // ...
  ```

### [XS] [USER_FACING] Add a `field_proxy::name()` accessor for parity with `method_proxy::name()`
- **rationale:** `method_proxy::name()` (line 12298) lets users log "which method failed". `field_proxy` exposes `signature()` and `raw_address()` and `is_static()` but nothing identifying the field by Java name. Users hitting a bug in `get()` have to plumb the name through their own code. Storing the name in the proxy adds 16-32 bytes per proxy (cheap; these are short-lived). The constructor already runs after `find_field()` returns an `entry`, so the name is trivially available at construction time.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:11070-11075 (constructor), 11315-11318 (members), 12845 + 12855 + 12907 (callers in `get_field`)
- **suggested_change:** Add a `std::string field_name` member, an extra ctor parameter, and a `name()` accessor; pass `entry->name` from each `get_field` overload. Then `VMHOOK_LOG` lines inside `get()` can identify which field hit the issue.

### [S] [USER_FACING] Add a `VMHOOK_LOG` warning when `get()` is called with a null field pointer
- **rationale:** Today the null-pointer branch (line 11090-11093) silently returns a defaulted value. Combined with the wrong variant alternative (see bug above), the user sees "the field reads as false / 0 / empty" with zero diagnostic output. Callers who construct `field_proxy` directly (a documented use-case for `watch_static_field`) get no signal that they passed a bad pointer. `get_field` already logs failures, but the proxy is the last line of defense.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:11090-11093
- **suggested_change:**
  ```cpp
  if (!this->field_pointer)
  {
      VMHOOK_LOG("{} field_proxy::get(sig='{}'): field_pointer is null - returning default. "
                 "Did get_field() succeed? Did the proxy outlive its source object?",
                 vmhook::error_tag, this->signature_text);
      // ... return canonical default for this signature ...
  }
  ```

### [XS] [INTERNAL] Fix stale doc reference to non-existent `field_proxy::get_as<T>()`
- **rationale:** Lines 1447 and 6821 reference `field_proxy::get_as<T>()` as the user-facing API, but no such method exists - the closest is `value_t::operator T()` (implicit conversion). The docs mislead readers searching for this entry point. Either rename comments to "value_t::operator T()" or actually add a `template<typename T> T get_as() const { return get().operator T(); }` sugar method on `field_proxy` (the latter is more discoverable - users grep for `get_as` after seeing it in `frame::get_arguments`).
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:1447, 6821
- **suggested_change:** Add to `field_proxy` (after line 11148):
  ```cpp
  template<typename T>
  auto get_as() const noexcept -> T { return static_cast<T>(this->get()); }
  ```
  Then the docs at lines 1447 and 6821 are accurate, users get IDE autocompletion for `proxy->get_as<bool>()`, and the type is no longer hidden behind a conversion-operator visit.

## Tests

### [standalone_unit] [new] test_field_proxy_get_primitive_bool_canonical_zero
- **description:** Reads a `"Z"` field whose backing byte is 0x00.
- **asserts:** `get().operator bool() == false`; `std::get<bool>(get().data) == false`; the variant alternative index is the `bool` index, not `int32_t`.

### [standalone_unit] [new] test_field_proxy_get_primitive_bool_canonical_one
- **description:** Reads a `"Z"` field whose backing byte is 0x01.
- **asserts:** `get().operator bool() == true`; `static_cast<int>(get()) == 1` (not 1 mangled into something else); variant alternative is `bool`.

### [standalone_unit] [new] test_field_proxy_get_primitive_bool_non_canonical_byte
- **description:** Sets the backing byte to 0xFF (or 0x02, 0xAA), reads via `field_proxy::get()`. Documents the bug; after the fix passes by normalising any non-zero to `true` and producing exactly `1` when cast to int.
- **asserts:** `static_cast<int>(proxy.get()) == 1` (post-fix). Pre-fix: this test fails non-deterministically across compilers / optimization levels, which is exactly what proves the bug.
- **existing_file:** tests/test_helpers.cpp (extend - sibling `test_field_proxy_set_size_guard` already lives there)

### [standalone_unit] [new] test_field_proxy_get_primitive_bool_null_pointer
- **description:** Construct `field_proxy{nullptr, "Z", false}` and call `get()`.
- **asserts:** No crash; `static_cast<bool>(get()) == false`; variant alternative is `bool` (not `int32_t`); the signature on the returned value_t is still `"Z"`.
- **existing_file:** tests/test_helpers.cpp (extend)

### [standalone_unit] [new] test_field_proxy_get_primitive_bool_static_dispatch
- **description:** Build a `field_proxy{ptr, "Z", true}` (static) and a `field_proxy{ptr, "Z", false}` (instance) on the same backing byte; `get()` must produce identical values for both. Confirms `static_field` flag is not consulted by `get()` (it shouldn't be - both flavours have already been pointer-resolved by `get_field`).
- **asserts:** Both proxies return the same `bool` value; `is_static()` returns the constructed flag verbatim.

### [jvm_integration] [exists] examples covering static + instance bool round-trips
- **description:** `example/vmhook/Example.java` defines `staticBool` (line 19) and `notStaticBool` (line 29), and `vmhook/src/example.cpp` already round-trips them via `get_static_bool()` / `get_not_static_bool()` (lines 359 and 467 in example.cpp) and checks the round-trip at lines 1575 and 1585. This covers the *happy* path. NOT covered: the non-canonical byte case. To exercise that under a real JVM, add a Java fixture method that uses `sun.misc.Unsafe.putByte(o, offset, (byte)0xAA)` on a boolean field, then read it back via `field_proxy::get()` and assert `b == true && (int)b == 1`.
- **asserts:** Same as the unit-test cases above but inside the JVM driver.
- **existing_file:** vmhook/src/example.cpp + example/vmhook/Example.java

## Parity Concerns
- **`field_proxy` has no `name()` accessor; `method_proxy::name()` (line 12298) does.** Users debugging field issues cannot include the Java field name in their own logs without manual plumbing.
- **`field_proxy::get()` does not log on errors / null-pointer; `method_proxy::call_jni` and `method_proxy::call` log richly with the method name and signature.** A `get()` that silently returns a defaulted value is a much harder bug to track down than one that emits a `VMHOOK_LOG` warning.
- **`field_proxy::get()` for `"Z"` does a raw memcpy into `bool`; every other bool-producing site in the file (method_proxy::call line 12271, method_proxy::call_jni line 11952, append_array_value<vector<bool>> line 10833) normalises via `(raw & 1) != 0` or `!= 0`.** Treat as a code-style parity issue *as well as* a correctness bug.
- **Docs reference `field_proxy::get_as<T>()` (lines 1447, 6821) but no such method exists.** `method_proxy` has no equivalent symbol either, but the docs were written as though `field_proxy` did - the parity gap is between the docs and the code, not between the two proxies.
- **The null-pointer branch returns the wrong variant alternative** (`int32_t` for every signature including `"Z"`). The non-null path correctly populates the `bool` alternative for `"Z"`. Two callers reading the same field through two `field_proxy` instances - one with `field_pointer == nullptr` (e.g., a dropped lookup) and one valid - get inconsistent variant types.
