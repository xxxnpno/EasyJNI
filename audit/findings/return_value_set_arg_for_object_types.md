# return_value_set_arg_for_object_types

## Summary
`return_value::set_arg` is implemented at vmhook.hpp:7509-7638 with a `store_oop` lambda that decides between writing a raw 64-bit OOP or an encoded 32-bit narrow OOP based on the previous slot value's high bits. The implementation correctly handles `std::unique_ptr<wrapper>` and value-typed wrappers derived from `vmhook::object_base`, but it has two serious bugs around the compressed-OOP heuristic and the raw `vmhook::oop_t` (`void*`) path, plus several user-facing documentation gaps and a fallback regression in the JNI-string flow.

## Bugs

### [high] Raw oop_t / void* path silently bypasses compressed-OOP encoding
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:7624-7630
- **description:** When a user passes a `vmhook::oop_t` (typedef of `void*`, see line 12681/12690) or any pointer-sized trivially-copyable value, the implementation falls into the generic `is_trivially_copyable_v && sizeof <= sizeof(void*)` branch and writes the raw 64-bit value into `locals[-index]` verbatim — completely skipping `store_oop`. The README/changelog and the task description ("OOP store into local slot. Compressed-oop encoding for the slot.") both imply that handing a decoded OOP through `set_arg` should mirror the wrapper-pointer path. On any JVM where the slot expects a 32-bit narrow value (UseCompressedOops on, which is the default for heaps <= 32 GB), the interpreter then reads a high half of the raw pointer as a "compressed" reference, decodes it to an arbitrary address, and the next `aload`/GC walk produces garbage or a crash. The opposite mistake (passing a raw `void*` argument extracted in the detour and writing it back unchanged on a +UseCompressedOops VM) is the most common one and there is no warning.
- **repro:** Hook a method taking a reference parameter, capture the decoded `vmhook::oop_t` via `frame->get_arguments<vmhook::oop_t>()`, then `ret.set_arg(1, that_oop)`. The Java side observes a corrupted reference instead of the same object.
- **suggested_fix:** Add an explicit branch `else if constexpr (std::is_same_v<clean_value_type, void*> || std::is_same_v<clean_value_type, vmhook::oop_t>)` that routes through `store_oop(value)` before falling through to the generic trivially-copyable branch. Even better, treat *any* pointer-typed value (`std::is_pointer_v<clean_value_type>`) as a candidate OOP when the previous slot is compressed, since users routinely pass raw `void*` after manual decoding.
- **confidence:** certain

### [high] store_oop heuristic defaults to compressed encoding when the previous local was null
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:7542-7563
- **description:** The "is this slot compressed?" decision is `previous_bits > 0xFFFFFFFFull` (line 7554). When the original Java argument was `null`, `previous_value == nullptr` and `previous_bits == 0`, so the lambda *always* falls into the compressed-encoding branch and writes `encode_oop_pointer(oop)`. On a JVM with `-XX:-UseCompressedOops` (or a heap > 32 GB), the slot expects a full 64-bit pointer and the encoded 32-bit narrow value (or 0 if `narrow_oop_base/shift` VMStructs are unpublished, see `encode_oop_pointer` returning 0 at vmhook.hpp:4348-4351) is the wrong format. The interpreter then loads what it thinks is a heap reference and dereferences nonsense. The same trap fires whenever the previous slot held a valid heap address that happens to be below 4 GB on Linux (low-address mmap, no PIE) — `previous_bits <= 0xFFFFFFFFull` is true and the code force-encodes a raw uncompressed pointer.
- **repro:** On any JDK with compressed oops disabled (or heap > 32 GB), hook a method whose reference arg starts as `null`, call `set_arg(idx, std::make_unique<wrapper>(oop))`; the Java code sees a bogus reference. On Linux with `-no-pie` and a 4 GB heap, the same misclassification can occur when the original reference happened to sit below 4 GB.
- **suggested_fix:** Detect compressed-OOPs mode once at startup (`narrow_oop_base/shift` VMStructs are present and `narrow_oop_shift` is sane) and cache the answer in a static `bool` — use that instead of guessing from `previous_bits`. As a defensive fallback, when the previous slot is `nullptr`, walk the surrounding frame for any nonzero reference local and use *its* width as the hint.
- **confidence:** likely

### [medium] String / const char* path does not fall back to make_java_string when JNI succeeded but decode rejected
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:7587-7602
- **description:** The expression `string_handle ? jni_decode_object(string_handle) : make_java_string(value)` only falls back to `make_java_string` when `jni_new_string_utf` returned `nullptr` (i.e. JNI wasn't usable). If JNI returned a non-null handle but `jni_decode_object` validated the handle's underlying OOP and returned `nullptr` (e.g. the handle pointer didn't pass `is_valid_pointer`, vmhook.hpp:8731-8732), `string_oop` becomes `nullptr` and the function bails out with the "both ... failed" message even though `make_java_string` was never attempted. The error log is also wrong — it says "both ... failed" but only one path was tried.
- **repro:** Force a malformed JNIEnv (e.g. mid-detach race or non-canonical handle storage); the path emits a misleading "both failed" log and refuses to retry via the heap-allocation fallback.
- **suggested_fix:** Restructure as two sequential attempts: try `jni_new_string_utf`/`jni_decode_object`; if the resulting OOP is null, release the (possibly non-null) handle, then try `make_java_string`. Only log "both failed" after the second attempt.
- **confidence:** certain

### [medium] String paths skip max_locals guard for `nullptr` JNI env when index is valid but slot pointer math wraps
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:7577-7622
- **description:** Both string branches construct the Java String *before* the call site checks whether the slot can hold an OOP. If the value is huge (`std::string` of, say, 1 MB), `jni_new_string_utf` allocates a Java String first and only later (on the `!string_oop` path) gets to release it. When `set_arg` fails for an upstream reason (e.g. encoding fallback fails), the failure path runs `DeleteLocalRef`, but the heap allocation done by `make_java_string` is left as a temporary unreferenced Java object subject to GC. That's not a correctness bug, but the *cost* shape is surprising: an early max_locals failure performs no heap work; a late JNI failure performs full UTF-8 allocation. Document or short-circuit.
- **repro:** Pass a multi-megabyte std::string into a failing `set_arg` call (e.g. JNI env disappears mid-flight). The allocator is exercised before the failure path returns false.
- **suggested_fix:** Move the index/frame guards above the type-specific branches (they already are), and additionally early-out for unreasonably long strings (cap at e.g. 4 KB before paying for JNI/heap), matching `make_java_string`'s 4096-char cap at vmhook.hpp:10625. Currently `jni_new_string_utf` is unbounded.
- **confidence:** likely

### [low] Doc comment for set_arg never mentions object/string/wrapper support
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:1211-1231
- **description:** The doxygen on the declaration says only "must be trivially copyable and fit in a Java local-variable slot (up to 8 bytes)". That's wrong for the actually-supported cases (string, string_view, const char\*, std::unique_ptr<wrapper>, object_base-derived). A first-time user reading the header sees no reason to expect those overloads exist, and code search returns the wrong contract.
- **repro:** Read the public doc, attempt `set_arg(1, std::string{"x"})`; nothing in the doc indicates this is the right call.
- **suggested_fix:** Update the doc to enumerate supported `value_type` categories and link to the per-branch behaviour (parallels the doc on `field_proxy::set` at vmhook.hpp:11150-11160).
- **confidence:** certain

## Improvements

### [S] [USER_FACING] Static-assert with a friendly message in the catch-all branch
- **rationale:** The trailing `else` (vmhook.hpp:7631-7637) currently logs a runtime error via `VMHOOK_LOG` for an unsupported `value_type`. Most users will pass an unsupported type at compile time (e.g. a `std::vector`, a `std::pair`, a user struct); a compile-time `static_assert(dependent_false_v<clean_value_type>, ...)` catches the mistake at the call site instead of only after the hook fires. The sibling `field_proxy::set` (vmhook.hpp:11252-11258) and `detail::extract_frame_arg` (vmhook.hpp:7248-7257) already use this pattern.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:7631-7637
- **suggested_change:** Replace the runtime `VMHOOK_LOG`/`return false` with `static_assert(vmhook::detail::dependent_false_v<clean_value_type>, "return_value::set_arg: unsupported value type — pass a primitive, pointer, std::string / string_view / const char*, unique_ptr<wrapper>, or a wrapper derived from vmhook::object_base.")`.

### [S] [USER_FACING] Reject impossible-to-fit primitive sizes at compile time
- **rationale:** The trivially-copyable branch silently truncates anything > sizeof(void*). A user passing `__int128`, an aggregate with 12 bytes of payload, or any large POD currently fails the `sizeof <= sizeof(void*)` constexpr check and hits the else; raising the size check into a `static_assert` improves diagnostics.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:7624-7630
- **suggested_change:** Add `static_assert(sizeof(clean_value_type) <= sizeof(void*), "return_value::set_arg: value too wide for a JVM local slot (8 bytes).");` inside the branch when the constexpr guard is true.

### [M] [INTERNAL] Cache the "compressed oops enabled" answer once
- **rationale:** Looking up `CompressedOops::_narrow_oop._base` per `store_oop` call (indirectly via `encode_oop_pointer`) and per-arg-read in `extract_frame_arg` adds VMStructs walks on every hook fire. Caching a single `bool compressed_oops_enabled` plus the `narrow_oop_base/shift` raw pointers in static-local storage at module load removes the repeated iterate_struct_entries work and also fixes the "default to compressed when previous slot is null" bug (see the corresponding bug entry).
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:4226-4361, 7542-7563
- **suggested_change:** Add `inline bool compressed_oops_enabled() noexcept` in the `hotspot` namespace, populated lazily from the same VMStruct probes used by encode/decode. Use that flag instead of `previous_bits > 0xFFFFFFFFull` to pick the storage width.

### [S] [USER_FACING] Mirror string overloads for `std::wstring_view` / `std::u16string_view`
- **rationale:** Java Strings are natively UTF-16; passing a `std::u16string_view` is currently rejected (catch-all else branch). The wrapper would need a JNI `NewString` (slot 163) call instead of `NewStringUTF`. This is the only common Java String input that today silently fails.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:7577-7623
- **suggested_change:** Add another `else if constexpr (std::is_same_v<clean_value_type, std::u16string_view> || std::is_same_v<clean_value_type, std::u16string>)` branch that calls `JNIEnv::NewString` (slot 163) with the UTF-16 buffer.

### [S] [USER_FACING] Document the OOP-encoding decision so users can predict failures
- **rationale:** Even after the heuristic is fixed, users need to know what `set_arg` is doing under the hood. Currently the comment block (vmhook.hpp:7567-7571) only explains the static_cast quirk — nothing about why the value is stored as a 32-bit narrow OOP vs a 64-bit raw pointer.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:7542-7563
- **suggested_change:** Add a comment block above `store_oop` explaining the compressed/uncompressed branching and that nulls write a literal 0 (which decodes to nullptr in either mode).

### [S] [USER_FACING] Error log on success of long-string mutation should be observable
- **rationale:** On success, the user has no signal that the JNI handle was created/freed. Adding `VMHOOK_LOG_TRACE` (or whatever the verbose level is) would help diagnostics; currently the function returns `true` silently while having done two JNI calls.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:7600-7602, 7620-7622
- **suggested_change:** Wrap the success-path return in a verbose-level log emitting the index, the chosen path (jni vs make), and the resulting OOP for traceability.

## Tests

### [standalone_unit] [extend_existing] test_return_value_set_arg_oop_t_routing_compile_time
- **description:** Compile-time assertion that `set_arg(idx, oop_t{})` selects the OOP store path, not the trivially-copyable fall-through. Today no test enforces that the raw `void*` argument is treated as an OOP rather than a verbatim 8-byte word.
- **asserts:** SFINAE / static_assert probe that the branch taken for `void*` calls `store_oop` (e.g. instrument with a friend hook).
- **existing_file:** tests/test_helpers.cpp (extend `test_return_value_set_arg_guards`)

### [standalone_unit] [extend_existing] test_return_value_set_arg_string_overload_no_jvm
- **description:** Exercise the `std::string` / `std::string_view` / `const char*` overload paths with a null `vmhook::hotspot::current_jni_env`; verify it returns `false` cleanly and does not leak. Today only the integer-arg early-return is exercised at lines 907-931.
- **asserts:** `rv.set_arg(0, std::string{"x"}) == false`, no crash, log emitted; same for `std::string_view`, `const char*`, `nullptr` const char\*.
- **existing_file:** tests/test_helpers.cpp

### [standalone_unit] [new] test_return_value_set_arg_unique_ptr_null_no_jvm
- **description:** Pass `std::unique_ptr<wrapper>{}` (an empty unique_ptr) to `set_arg` with a null frame; verify the no-frame guard fires before the unique-ptr branch dereferences `value->vmhook::object_base::get_instance()` and returns `false`.
- **asserts:** No crash on null unique_ptr; return value is `false`; no log claiming a successful store.
- **existing_file:** tests/test_helpers.cpp (new function `test_return_value_set_arg_object_overloads_no_jvm`)

### [standalone_unit] [new] test_return_value_set_arg_object_base_value_no_jvm
- **description:** Construct a derived `object_base` value (with `instance == nullptr`) and pass by value to `set_arg`; verify that `get_instance()` is called and the no-frame guard still short-circuits to `false` without writing.
- **asserts:** Return is `false`; no segfault when the base subobject's instance is null; explicit qualification of `object_base::get_instance` resolves correctly.
- **existing_file:** tests/test_helpers.cpp

### [jvm_integration] [new] test_set_arg_object_argument_mutation_compressed_oops
- **description:** On a JVM with `-XX:+UseCompressedOops`, hook a method `void take(Foo f)`, replace `f` via `set_arg(1, std::make_unique<foo_wrapper>(other_oop))`, and verify the original Java code observes `other_oop`. Sibling `test_arg_mutation` (vmhook/src/example.cpp:2288) covers `int32_t`; this would mirror it for object references and would have caught the "raw oop_t bypass" bug if it exercised the `oop_t` overload too.
- **asserts:** Hook fires once; Java probe field updated to the new object's identity hash; no JVM crash on uninject.
- **existing_file:** vmhook/src/example.cpp (extend the arg-mutation test block at lines 2288-2362)

### [jvm_integration] [new] test_set_arg_object_argument_mutation_uncompressed_oops
- **description:** Same as above but launched with `-XX:-UseCompressedOops` so the slot expects a full 64-bit pointer; would directly catch the "default to compressed when previous arg was null" bug.
- **asserts:** Same as compressed case; specifically, the original Java code receives a *different* identity-hash object (proving the slot got rewritten, not corrupted).
- **existing_file:** none; needs a new JVM-launch variant or a parametrized harness

### [jvm_integration] [new] test_set_arg_raw_oop_overload_round_trip
- **description:** Hook a method, get a peer reference via `frame->get_arguments<vmhook::oop_t>()`, write it back into a different slot using `set_arg(2, that_oop)`, and verify the Java code sees the same object.
- **asserts:** Identity equality (Java `==`) of the slot-2 object against the originally captured `oop_t`. Would have caught the raw-pointer-path bypass.
- **existing_file:** vmhook/src/example.cpp

### [jvm_integration] [new] test_set_arg_string_handle_leak
- **description:** Run the string-set_arg path inside a tight loop (~10000 iterations) and verify the JNI local-ref table does not overflow (`JNI: local reference table overflow` line absent from logs).
- **asserts:** No JNI overflow warning; final RSS within bounds.
- **existing_file:** vmhook/src/example.cpp (extend `test_string_arg_mutation` at line 2328)

## Parity Concerns
- `field_proxy::set` (vmhook.hpp:11150-11260) and `return_value::set_arg` cover the same conceptual operation (write a typed value into a JVM slot), but with different surface area and quirks: field_proxy adds `std::vector<T>` (arrays), Java-char widening, and a size-mismatch guard with a friendly log; set_arg adds string/const-char*. Neither supports the other's bonus types. Consider extracting a shared `detail::store_typed_value(slot_pointer, value, signature_hint)` core so both stay in sync.
- The argument-read path (`extract_frame_arg` at vmhook.hpp:7184-7258, `frame::get_argument` at vmhook.hpp:5159-5189) uses the same `bits <= 0xFFFFFFFFull` heuristic to *decode*. If the read heuristic is wrong (e.g. on uncompressed-oop JVM, a small-but-real pointer below 4 GB), `set_arg`'s write heuristic will be wrong in the same way — both should share a single compressed-mode oracle.
- `vmhook::call` (vmhook.hpp:9486-9522, ~9580 onward) handles wrapper/unique_ptr/object_base for *arguments* of method calls too, with yet another copy of the same dispatch logic. A common `detail::oop_from_value(value)` helper would prevent the three implementations from drifting.
