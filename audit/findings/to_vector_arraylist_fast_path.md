# to_vector_arraylist_fast_path

## Summary
Audited the ArrayList fast path inside `vmhook::collection::to_vector<T>()` (vmhook.hpp:13554-13586), which reads `ArrayList.size` and `ArrayList.elementData` directly and walks the backing `Object[]` from `0..size`. The size-vs-capacity logic is correct (loop bound is `size`, per-element bounds check inside `get_array_element` is against `elementData.length`), and null elements are properly tolerated. Top concerns are (1) the path also silently matches non-ArrayList containers that happen to share the same field names (`java.util.Vector`, `java.util.Stack`, plus any user-written subclass that defines both `size` and `elementData`), and (2) several silent-failure branches where the user gets an empty/partial vector without any log, contrary to the explicit "silent failure where a log was intended" anti-pattern called out in the audit brief.

## Bugs

### [medium] Silent fall-through to generic path when elementData is unreadable
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:13558-13586
- **description:** When `size_opt && data_opt` are both present but `decode_array_oop(compressed_array)` returns null (or `is_valid_pointer` rejects it), control silently drops out of the outer `if` at line 13586. Execution then falls through to the LinkedList probe (which fails for ArrayList because `first_opt` is null), HashSet/TreeSet probes (also null), and finally lands in the generic `get(int)` fallback at line 13627. The user gets a vector that is silently O(N) virtual-dispatched instead of the advertised O(N) direct read, with no log line explaining that the fast path was bypassed despite the class shape matching. For a hot detour this is a serious performance regression that is invisible to the caller.
- **repro:** Hook a method that receives an ArrayList whose `elementData` field has just been GC'd or moved (i.e. the snapshotted compressed OOP is stale across a GC). The first `to_vector()` call will hit `array_oop` invalid, silently degrade to per-element JNI-style `get()` calls, and emit no warning.
- **suggested_fix:** Add a `VMHOOK_LOG` with `vmhook::warning_tag` inside the `if (size_opt && data_opt)` block when `array_oop` fails validation, mirroring the log already present in `field_proxy::value_t::to_vector()` at line 14393. Either log-and-fall-through or log-and-return-empty — current behaviour does neither.
- **confidence:** likely

### [low] `n <= 0` on ArrayList short-circuits before even probing other paths
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:13558-13564
- **description:** If the live OOP has both `size` and `elementData` fields (i.e. the class is ArrayList/Vector/Stack/subclass), and `size <= 0`, the function returns immediately with an empty vector. This is the correct answer for ArrayList — but the early-return also short-circuits the case where `size_opt->get()` returns `0` because the field-by-name lookup matched a wrong field with a non-`"I"` signature whose value_t→int32 conversion happens to coerce to zero. In that scenario the caller gets `{}` instead of taking the generic `size()`/`get(int)` fallback, which uses virtual dispatch and would have returned the real elements. The mismatch is unlikely on standard ArrayList but is observable on any user class that shadows `size` with an unrelated field name.
- **repro:** Define a Java class that has both an `int size` field unrelated to count and an `Object[] elementData` unrelated to storage (e.g. a custom serialization buffer). Construct it with non-empty backing collection data accessible only via overridden `get(int)`. `to_vector<T>()` returns empty.
- **suggested_fix:** Either (a) document that fields named `size`+`elementData` are treated as ArrayList layout (caller responsibility), or (b) gate the fast path on `oop_klass()->get_name()` matching one of {ArrayList, Vector, Stack, SubList} and skip otherwise. (b) is the safer route and matches how the LinkedList path is implicitly bounded by Node walking.
- **confidence:** speculative

### [low] No verification that `n` ≤ `elementData.length` before the loop
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:13566-13584
- **description:** `n` (ArrayList.size) is read at line 13560 and the loop runs `for (index = 0; index < n; ++index)`. If a concurrent mutator increments `size` before the corresponding `grow()` swaps in a larger `elementData`, the snapshotted `n` will exceed `array_length(array_oop)`. The per-element bounds check inside `get_array_element` (line 10724) silently returns `0` for the OOB indices, which decodes to `nullptr`. The user sees a vector of mostly real entries followed by phantom `nullptr` slots for the indices `[capacity .. n)`, with no log indicating the truncation happened. Not a memory bug (the bounds check is honored), but the silent corruption-shaped output is confusing.
- **repro:** Construct an ArrayList from another thread while `to_vector` is mid-call. With sufficient timing jitter, you can land in the state where `size > elementData.length` for the duration of `grow()`.
- **suggested_fix:** Read `array_length(array_oop)` once before the loop, take `n = std::min(n, array_length)`, and emit a `VMHOOK_LOG` warning if they differed. This gives the caller actionable diagnostics instead of silently-padded output.
- **confidence:** likely

## Improvements

### [XS] [USER_FACING] Log when fast path matches but `array_oop` is invalid
- **rationale:** The sibling `field_proxy::value_t::to_vector` (line 14393) already logs when the collection OOP is invalid. Parity gap — the inner ArrayList path is the *common* fast path and emits no log on similar failure.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:13568
- **suggested_change:** Add `else { VMHOOK_LOG("{} ArrayList fast path: elementData decode failed (compressed=0x{:08X}, size={}) - falling back to generic path.", vmhook::warning_tag, compressed_array, n); }` to the failed branch of the `is_valid_pointer` check.

### [S] [USER_FACING] Document that Vector and Stack also take this path
- **rationale:** The fast-path-selection comment (lines 13522-13534) lists ArrayList → direct array walk but omits that any class with both `size` (int) and `elementData` (Object[]) fields — i.e. `java.util.Vector` and `java.util.Stack` — will match this branch. Users debugging "why is my Vector silently working" need this in writing.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:13522-13534
- **suggested_change:** Extend the docstring to read `"1. ArrayList / Vector / Stack  ("elementData" + "size")  → direct array walk"` and add a sentence noting that the path matches any class with that field shape.

### [S] [INTERNAL] Hoist `array_length` read out of the loop and use it as the inner bound
- **rationale:** `get_array_element<std::uint32_t>` re-reads `array_length(array_oop)` on every call inside the loop (line 10723). That's `n` redundant `is_valid_pointer` + length-load operations. Reading length once and using `std::min(n, length)` as the loop bound also gives the diagnostics hook for bug #3 above.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:13566-13584
- **suggested_change:** Compute `const std::int32_t cap{ vmhook::array_length(array_oop) }; const std::int32_t bound{ (std::min)(n, cap) };` then loop `for (i = 0; i < bound; ++i)` and read the element via raw pointer arithmetic without re-bounds-checking. Optionally log when `n > cap`.

### [S] [USER_FACING] Surface `n <= 0` as a debug log
- **rationale:** Returning an empty vector for an empty ArrayList is the right answer, but for users debugging "my collection is unexpectedly empty" there's currently no way to confirm whether the fast path was taken and saw `size=0` versus whether the function bailed earlier on null `instance`. A `VMHOOK_LOG_DEBUG`-level message would help.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:13561-13564
- **suggested_change:** Add a debug-level log before `return result;` at line 13563 noting `size=0` from the ArrayList path.

### [M] [INTERNAL] Cache the `size`/`elementData` field entries per-klass
- **rationale:** `get_field_by_oop_klass("size")` and `get_field_by_oop_klass("elementData")` are called on every `to_vector()` invocation. Both invoke `find_field`, which scans the klass field table. For a hot detour iterating ArrayLists every frame, this is wasted work. A small `std::unordered_map<klass*, std::pair<int, int>>` cache of `(size_offset, elementData_offset)` keyed by klass pointer would let the fast path skip name lookup entirely on warm calls.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:13554-13556
- **suggested_change:** Add a static thread-local cache keyed by `oop_klass()` to memoize the two field offsets; invalidate on klass mismatch.

## Tests

### [standalone_unit] [extend_existing] test_to_vector_arraylist_size_lt_capacity_returns_size_elements
- **description:** Construct a fake ArrayList-shaped object whose `elementData` array length is 16 but whose `size` field is 3. Verify `to_vector()` returns exactly 3 elements (not 16, and no phantom nullptrs from indices 3..15).
- **asserts:** `result.size() == 3` and `result[0..2]` are the constructed wrappers, with no read past index 2.
- **existing_file:** tests/test_api_surface.cpp

### [standalone_unit] [new] test_to_vector_arraylist_size_zero_returns_empty
- **description:** ArrayList shape with `size=0` and a non-empty `elementData` (capacity 10). Should return empty vector without reading any element slots.
- **asserts:** `result.empty()` and no element-read attempt observable via a sentinel-poisoned array.

### [standalone_unit] [new] test_to_vector_arraylist_null_elements_become_nullptr_slots
- **description:** Build an ArrayList shape with `size=5` where slots 0, 2, 4 hold compressed-OOP 0 (null reference). Verify `result[0]`, `result[2]`, `result[4]` are `nullptr` while `result[1]`, `result[3]` are live wrappers.
- **asserts:** Per-index null vs non-null pattern matches the source array exactly.

### [standalone_unit] [new] test_to_vector_arraylist_invalid_array_oop_emits_log
- **description:** Simulate the case where `decode_array_oop` returns an address that fails `is_valid_pointer`. After the proposed log addition, verify the warning is emitted and the call returns either empty or falls through cleanly.
- **asserts:** `VMHOOK_LOG` capture contains the warning string; return value is the documented empty vector.

### [jvm_integration] [extend_existing] test_to_vector_arraylist_after_trim_to_size
- **description:** In Example.java, create an ArrayList with capacity 100, add 3 elements, call `trimToSize()` so `capacity == size == 3`, then hit a hook that calls `to_vector<a_class>()`. Verify 3 elements come back. Then create a second ArrayList with `ensureCapacity(100)` and only 3 elements (capacity != size) — verify exactly 3 elements come back, not 100.
- **asserts:** Both cases yield `result.size() == 3` with correct element identity.
- **existing_file:** example/vmhook/Example.java + vmhook/src/example.cpp

### [jvm_integration] [new] test_to_vector_vector_class_takes_arraylist_path
- **description:** Add a `java.util.Vector<A> vectorOfAs` field to Example.java, populate it with 3 elements, and verify `to_vector<a_class>()` works. This is the silent-Vector-support case mentioned in the comment-improvement above. If the team decides to *restrict* the path to ArrayList only, this test becomes a negative test (expect generic fallback).
- **asserts:** Either (a) `result.size() == 3` if Vector support is intentional, or (b) expected fallback if scoping is tightened.

### [jvm_integration] [new] test_to_vector_arraylist_subclass_with_overridden_storage
- **description:** Define a Java class that extends ArrayList but stores actual data in a different field, e.g. `class WeirdList extends ArrayList<A> { private List<A> real = ...; }`. Verify `to_vector<T>()` does the documented thing — either follows `elementData` (which may be empty) or recognizes the override. Documents the current behaviour for the audit trail.
- **asserts:** Result matches whatever the user-visible contract is; primarily an exploratory test.

## Parity Concerns
- `field_proxy::value_t::to_vector` (line 14393) logs an explicit warning when the collection OOP is invalid; the inner ArrayList fast path at 13568 does not log on equivalent failures (invalid `array_oop`). Same audit pattern, inconsistent diagnostic output.
- `linked_list_walk_items` (line 13930) accepts `size` as an explicit bound argument; `to_vector`'s ArrayList path computes the bound inline. The two sibling fast paths share no helper for "read size + reserve + iterate", so refactoring one does not benefit the other.
- The docstring at lines 13518-13542 lists 5 fast paths, but in practice path #1 (ArrayList) matches Vector and Stack too. Other paths' docstrings are explicit about which classes match — parity gap.
- `field_proxy` family has explicit per-type setters (`set_bool_array`, `set_str_array`, `set_str_field`); there is no equivalent `to_vector` for `Vector`/`Stack` that emits a deprecation/info log, even though the codebase is otherwise rich in "this method also serves X" documentation.
