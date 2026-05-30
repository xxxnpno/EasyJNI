# return_value_set_wrapper_and_null

## Summary
`return_value::set` has two relevant overloads at vmhook.hpp:1147-1203: a generic templated `set(const value_type)` for trivially-copyable values and a typed-null `set<wrapper_type>(std::nullptr_t)`. The implementations are correct for what they do (sign-extension, cancel flag, zeroing of upper bytes for nullptr), but there is **no overload that accepts a live `std::unique_ptr<wrapper>` and encodes the OOP into the retval slot** — users returning an existing Java object from a hooked reference-returning method must call `vmhook::hotspot::encode_oop_pointer(self->get_instance())` themselves, while `field_proxy::set` provides exactly that convenience for fields. The wrapper/null branches also lack defensive null-checks on `return_slot` and dereference unconditionally, so accidentally constructing a `return_value` with a null slot crashes silently inside the inlined `set`.

## Bugs

### [low] return_value::set dereferences return_slot without a null-guard
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:1147-1203
- **description:** Both `set(value)` (1157-1175) and `set<wrapper_type>(nullptr)` (1201-1202) write to `this->return_slot->cancel` / `->retval` unconditionally. The constructor at 1141-1145 accepts any pointer (including `nullptr`) and never asserts. `cancel()` (1208) and `set_arg` (7522-7528, 7534-7537) both guard against missing state and log via `VMHOOK_LOG`, but the two `set` overloads do not. A null slot is constructible via the public constructor and the sibling unit test `test_return_value_no_frame_helpers` (test_helpers.cpp:862-897) already proves the contract that "no frame" must not crash — the same defensive posture is missing for "no slot".
- **repro:** `vmhook::hotspot::return_slot* s = nullptr; vmhook::return_value rv{ s }; rv.set(std::int32_t{ 0 });` — null deref. Real-world trigger: a future refactor that allocates the slot conditionally, or any user who manually constructs a `return_value` in their own test scaffolding.
- **suggested_fix:** Early-return with a `VMHOOK_LOG` if `this->return_slot == nullptr` in both overloads, mirroring the pattern already used by `set_arg` and the `cancel()` overload's contract.
- **confidence:** likely

### [low] set<wrapper>(nullptr) silently succeeds even with no narrow-OOP base configured
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:1196-1203
- **description:** The doc-comment at 1191-1193 states the wrapper overload "writes a null OOP". On HotSpot, a *compressed* null OOP is indeed 0 regardless of `narrow_oop_base`, so zero-bit-pattern is technically correct. But on JVMs configured without compressed OOPs (`-XX:-UseCompressedOops`, common with `-Xmx > 32G`) the i2i interpreter expects a full 64-bit pointer in `rax` on the cancel path; the value 0 is still semantically a null reference and works. The implementation is therefore correct, **but the doc comment makes no mention of either branch** — users with a heap > 32G have no way to learn from the header that "null is 0 either way" and may write `set<vmhook::oop_t>(encode_oop_pointer(nullptr))` defensively, which is a no-op but obscures intent. This is a documentation bug, not a runtime bug.
- **repro:** Read 1178-1194 with no prior HotSpot OOP knowledge — there is no mention of compressed vs. uncompressed semantics for the zero retval.
- **suggested_fix:** One sentence in the comment: "Writes a literal 0 bit pattern, which the interpreter treats as `null` regardless of whether `UseCompressedOops` is on."
- **confidence:** likely

### [low] set<wrapper>(nullptr) does not zero retval when called twice with different garbage
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:1198-1203
- **description:** The implementation writes `retval = 0` directly, which is correct in isolation. However, the trampoline pre-zeroes the slot via `push 0; push 0` at trampoline 5295-5296 / 5393-5394 before the callback fires, so the only way `retval` becomes non-zero in practice is if the callback itself stored a value and then later called `set<wrapper>(nullptr)`. In that case the current code does zero the slot correctly. No bug — included as a non-bug confirmation; the existing test (`test_helpers.cpp:619-626`) already pre-fills with `0xDEADBEEFCAFEBABE` and asserts the zero. Skip.
- **repro:** n/a (confirmed correct by inspection)
- **suggested_fix:** n/a
- **confidence:** certain

## Improvements

### [S] [USER_FACING] Add `set(std::unique_ptr<wrapper>&)` overload that auto-encodes the OOP
- **rationale:** `field_proxy::set` (vmhook.hpp:11187-11206) already accepts `std::unique_ptr<wrapper>` and calls `vmhook::hotspot::encode_oop_pointer(value->get_instance())` for the user. `return_value::set` lacks the symmetric overload, so users who want to return an existing wrapper from a hooked `L`-returning method must hand-roll `ret.set<vmhook::oop_t>(reinterpret_cast<void*>(static_cast<std::uintptr_t>(vmhook::hotspot::encode_oop_pointer(wrapper->get_instance()))))` — three reinterprets and an awkward OOP-vs-pointer cast. Adding the overload restores API parity with `field_proxy::set` and makes returning an object as readable as returning an int.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:1196-1203 (add a new overload immediately after the `nullptr_t` one)
- **suggested_change:**
  ```cpp
  template<typename wrapper_type, typename deleter_type>
      requires std::is_base_of_v<vmhook::object_base, wrapper_type>
  auto set(const std::unique_ptr<wrapper_type, deleter_type>& value) noexcept -> void
  {
      this->return_slot->cancel = true;
      void* const oop_pointer{ value ? value->vmhook::object_base::get_instance() : nullptr };
      const std::uint32_t compressed{ oop_pointer
          ? vmhook::hotspot::encode_oop_pointer(oop_pointer)
          : std::uint32_t{ 0 } };
      this->return_slot->retval = 0;
      std::memcpy(&this->return_slot->retval, &compressed, sizeof(compressed));
  }
  ```
  Mirror the `field_proxy::set` qualified-call (`value->vmhook::object_base::get_instance()`) for the same shadowing reason described at 11191-11195. Document that a moved-from / empty `unique_ptr` writes a null OOP, identical to `set<wrapper>(nullptr)`.

### [S] [USER_FACING] Replace the static_assert message with a fix-it hint for `unique_ptr<wrapper>` callers
- **rationale:** Today, a user who writes `ret.set(std::make_unique<my_class>(some_oop))` hits the `static_assert(std::is_trivially_copyable_v<value_type>, ...)` at 1153-1156 with the message "Pass a primitive, an oop pointer (void*), or a small POD by value." That message tells the user what is allowed but not what to do with the wrapper they already have. Once the overload above lands, the static_assert in the primitive path should special-case `is_unique_ptr_v<value_type>` and `std::is_base_of_v<object_base, value_type>` and emit a tailored diagnostic that names the right overload ("for a managed Java reference, pass the unique_ptr directly; for an explicit null, write `ret.set<MyClass>(nullptr)`").
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:1153-1156
- **suggested_change:** Add a third static_assert chained to the existing two:
  ```cpp
  static_assert(!vmhook::detail::is_unique_ptr_v<value_type>,
                "return_value::set: pass a unique_ptr<wrapper> directly (the dedicated overload "
                "encodes the OOP for you).  This catches accidental moves of a unique_ptr through "
                "the trivially-copyable path, which would copy the raw pointer bits instead of "
                "encoding the compressed OOP.");
  static_assert(!std::is_base_of_v<vmhook::object_base, value_type>,
                "return_value::set: a wrapper object is not trivially copyable.  Pass either "
                "the unique_ptr<wrapper> (encodes), nullptr with explicit <wrapper_type> "
                "(returns null), or wrapper.get_instance() cast to vmhook::oop_t.");
  ```

### [XS] [USER_FACING] Document the compressed-OOP contract on the primary `set` template
- **rationale:** The primary `set(value)` template's doc (1132-1137, 1147-1156) does not mention that for OOP returns the user must encode the pointer themselves. The example at 1133-1136 shows an `int32_t` return and the only mention of "oop pointer (void*)" is buried in the static_assert message. Users hooking `L`-returning methods today get correct code by accident only if they happen to know `encode_oop_pointer`. A two-line `@note` saying "for a Java reference return, pass `vmhook::hotspot::encode_oop_pointer(decoded_oop)` cast to `vmhook::oop_t`, or prefer the `unique_ptr<wrapper>` overload" would unblock the common case.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:1124-1137 (extend class doc) and 1147-1156 (extend method doc)
- **suggested_change:** Add a worked OOP-return example to the class-level doc.

### [XS] [INTERNAL] Factor the cancel-and-zero-retval prologue into a private helper
- **rationale:** Three overloads (`set(value)`, `set<wrapper>(nullptr)`, the proposed `set(unique_ptr)`) repeat the same pattern: `cancel = true`, optionally clear `retval`, write the encoded value. A private helper keeps the null-slot guard fix (Bug 1) in one place rather than three.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:1138-1203
- **suggested_change:**
  ```cpp
  private:
      auto write_retval_bits(std::int64_t bits) noexcept -> void
      {
          if (!this->return_slot) { VMHOOK_LOG(...); return; }
          this->return_slot->cancel = true;
          this->return_slot->retval = bits;
      }
  ```
  All three overloads then call `write_retval_bits(...)` with the precomputed 64-bit cell.

## Tests

### [standalone_unit] [extend_existing] test_return_value_set_unique_ptr_wrapper_encodes_oop
- **description:** Once the `unique_ptr<wrapper>` overload lands, verify that passing a wrapper whose `get_instance()` returns a known fake OOP writes the compressed-OOP bit pattern into the slot and sets `cancel`. Requires either a live JVM (true encode) or a test double that overrides `encode_oop_pointer` — in a standalone unit, drive `encode_oop_pointer` with a known narrow-OOP base/shift = 0 so encode is the identity (low 32 bits of the pointer).
- **asserts:** `slot.cancel == true`; `slot.retval == expected_compressed_oop`; calling `set(std::unique_ptr<wrapper>{})` (empty) writes `slot.retval == 0` (matches `set<wrapper>(nullptr)` semantics).
- **existing_file:** tests/test_helpers.cpp (extend `test_return_value_set_nullptr_for_wrapper` at line 612-633, or add a new sibling immediately after)

### [standalone_unit] [new] test_return_value_set_with_null_slot_does_not_crash
- **description:** Construct `vmhook::return_value rv{ /*slot=*/nullptr };` then call every public mutator (`set(int32_t)`, `set<wrapper>(nullptr)`, `cancel()`). After the bug fix above, none should AV; before the fix this would crash on `slot->cancel = true`.
- **asserts:** No segfault. After the fix, `VMHOOK_LOG` may be exercised — at minimum the call returns. (Optional: capture the log output and assert a diagnostic was emitted.)
- **existing_file:** tests/test_helpers.cpp (sibling of `test_return_value_no_frame_helpers` at 862)

### [standalone_unit] [extend_existing] test_return_value_set_nullptr_via_oop_t_alias
- **description:** Cover the alternative null syntax `ret.set<vmhook::oop_t>(nullptr)` mentioned in the doc-comment at 1186. Today the `nullptr_t` overload only matches types derived from `object_base`; `oop_t == void*` does NOT derive, so this call falls into the primary `set(value_type)` path with `value_type == void*` (trivially copyable, size 8). The static_assert passes, and a 0 is written to `retval`. Lock this contract in: if someone narrows the templated overload to reject non-arithmetic types later, this regression would slip silently.
- **asserts:** `slot.cancel == true`; `slot.retval == 0`; compile-time path: `static_assert(std::is_invocable_v<decltype(&vmhook::return_value::set<void*>), vmhook::return_value*, void*>);`
- **existing_file:** tests/test_helpers.cpp (extend `test_return_value_set_nullptr_for_wrapper` at line 612)

### [standalone_unit] [new] test_return_value_set_double_call_overwrites
- **description:** Call `ret.set(std::int32_t{ 42 })`, then `ret.set<fake_wrapper>(nullptr)`, then `ret.set(double{ 3.14 })` on the same `return_value`. The final state should reflect only the last call (retval == bit-pattern of 3.14, cancel still true). This catches a future "cancel-already-set, fast-path-out" optimisation that would skip the retval write on the second/third call.
- **asserts:** After third call, `slot.retval` bits decode to 3.14 via memcpy round-trip; `slot.cancel == true`.
- **existing_file:** new test in tests/test_helpers.cpp (sibling of `test_return_value_set_non_integer_types` at 789)

### [jvm_integration] [new] test_hook_returning_wrapper_via_unique_ptr
- **description:** Hook a JVM method whose Java signature returns a reference type (e.g. `()Ljava/lang/String;`). In the detour, build a `unique_ptr<string_wrapper>` from a known live OOP (e.g. the receiver itself or a newly-constructed string), pass it to the proposed `ret.set(unique_ptr)` overload, and from a Java caller verify the returned reference is identical (`==`) to the wrapper's OOP. Validates that `encode_oop_pointer` on the C++ side and the trampoline's `mov rax, [rsp+8]` on the cancel path produce a value the i2i interpreter unboxes correctly.
- **asserts:** Java-side reference equality between the wrapper's underlying object and the value returned by the hooked method; no SIGSEGV in the JVM after the call.
- **existing_file:** new integration test — there is no existing JVM-integration test file in `tests/`, so this requires the same infrastructure as the planned i2i-roundtrip tests (none exist today).

### [standalone_unit] [new] test_return_value_set_nullptr_writes_full_64_bits
- **description:** Pre-fill `slot.retval = 0xDEADBEEFCAFEBABE`, then call `set<fake_wrapper>(nullptr)`. Assert *all 64 bits* are zero, not just the low 32. Today the implementation does `retval = 0` (full 64-bit assignment) which is correct, but if the future `unique_ptr` overload uses the `memcpy(&retval, &compressed, sizeof(compressed))` pattern from `field_proxy::set`, only the low 32 bits would be cleared — leaving stale high bits that the i2i `areturn` could interpret as a valid (garbage) pointer on uncompressed-OOP JVMs.
- **asserts:** `slot.retval == 0` (compared as int64_t, not just low half).
- **existing_file:** tests/test_helpers.cpp (already covered partially at 625; extend to also assert the high half is zero with `slot.retval == std::int64_t{0}` rather than relying on default int promotion).

## Parity Concerns
- `field_proxy::set` accepts `std::unique_ptr<wrapper>` and encodes the OOP (vmhook.hpp:11187-11206). `return_value::set` does not — this is the largest API-asymmetry in the file's setter family. Both sides write a compressed OOP into a memory cell, both use `encode_oop_pointer`, both need the same null-handling. A user who learns "set a unique_ptr to write an object reference" on `field_proxy` is surprised when the same syntax fails on `return_value`.
- `return_value::set_arg` (vmhook.hpp:7510 onwards) similarly accepts wrappers/strings via its own type-dispatch chain. After this audit's improvement lands, the three setters (`field_proxy::set`, `return_value::set`, `return_value::set_arg`) all support the same input vocabulary and read symmetrically.
- The `set<wrapper>(nullptr)` overload provides a typed null sentinel; the future `set(unique_ptr<wrapper>{})` would do the same thing (empty unique_ptr → null OOP). Document that both are equivalent and choose one as canonical to avoid the doubled API surface.
