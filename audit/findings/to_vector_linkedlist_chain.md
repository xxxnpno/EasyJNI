# to_vector_linkedlist_chain

## Summary
Audited `vmhook::linked_list_walk_items` (the LinkedList Node-chain walker
behind `collection::to_vector<T>()` for `java.util.LinkedList`), the
LinkedList branch in `collection::to_vector`, and the `field_proxy::value_t`
entry point. The top concern is a documentation-vs-code mismatch: the
header comment claims the Node klass and field offsets are read "once per
node", but the loop body re-runs `klass_from_oop` and two `find_field`
calls (each O(F)) on every iteration, so the walk is actually O(N*F),
not the flat O(N) the README and class comment advertise. A secondary
concern is the absence of cycle detection — the exact issue that drove
the `for_each_thread` fix in commit 18d5428 applies here too, but
the bound is `size`, not `1<<20`, so a corrupted/concurrently-mutated
chain that loops back will silently produce duplicate entries up to
`size` times.

## Bugs

### [medium] Klass and field lookup repeated every iteration — walk is O(N*F), not O(N)
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:13929-13985
- **description:**
  The function's own comment (lines 13921-13928) and the class-level
  `to_vector` doc (line 13538: "Complexity: O(N) where N = collection
  size") promise that the node klass and `item`/`next` field offsets are
  resolved once and reused. The body does the opposite: every iteration
  calls `vmhook::klass_from_oop(node_oop)` (line 13956) and then
  `vmhook::find_field(node_klass, "item")` and
  `vmhook::find_field(node_klass, "next")` (lines 13961-13962), and
  `find_field` is documented as O(F) on the class's field table (header
  intro, line 40). Net cost is O(N*F) on top of two redundant memory
  reads per node. HotSpot guarantees every Node in one LinkedList shares
  the same concrete `java.util.LinkedList$Node` klass (the doc even
  states this), so the lookup can — and per the doc, *should* — be
  hoisted out of the loop.
- **repro:**
  Walk a LinkedList of 100k elements. Compare wall-clock against the
  ArrayList fast path on the same data; the LinkedList version pays
  ~3x the per-element cost because of the repeated `find_field` scans.
  (No correctness failure — just a silent perf regression that violates
  the README/class-doc complexity claim that the audit task brief calls
  out by name.)
- **suggested_fix:**
  Hoist the lookups above the loop, exactly as the comment says is being
  done:
  ```cpp
  vmhook::hotspot::klass* const node_klass{ vmhook::klass_from_oop(node_oop) };
  if (!node_klass) return;
  const auto item_entry{ vmhook::find_field(node_klass, "item") };
  const auto next_entry{ vmhook::find_field(node_klass, "next") };
  if (!item_entry || !next_entry) return;
  const std::int32_t item_off{ item_entry->offset };
  const std::int32_t next_off{ next_entry->offset };
  for (std::int32_t i{ 0 }; i < size && node_oop && vmhook::hotspot::is_valid_pointer(node_oop); ++i) { ... }
  ```
  Optional belt-and-braces: re-check `klass_from_oop(node_oop) == node_klass`
  on each iteration with a single comparison and break out on mismatch
  (covers the pathological case where a custom LinkedList subclass mixes
  node types — never happens in JDK, but cheap).
- **confidence:** certain

### [low] No cycle detection — a corrupted/concurrent `next` chain produces duplicate entries
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:13952-13984
- **description:**
  The loop is bounded only by `size` (the Java `LinkedList.size` field
  the caller read). If the `next` pointer cycles back to an earlier node
  (corruption, mid-mutation race, or a hostile heap), the walk will emit
  the same `item` up to `size` times into `out`, with no warning, until
  the iteration counter runs out. The header comment on
  `linked_list_walk_items` (line 13926-13927) explicitly mentions "a
  corrupt or concurrently-mutated chain cannot loop forever" — which is
  true, but the chain *can* still cause `size` worth of duplicate
  reporting. Commit 18d5428 already addressed exactly this class of bug
  for `for_each_thread` (see vmhook.hpp:6553-6571), where a corrupted
  `_next` chain on JavaThread could otherwise invoke the visitor on
  duplicates up to the cap. The same precedent applies here.
- **repro:**
  Synthesise an in-memory layout where Node A's `next` points back at A,
  then call `to_vector<T>()` with `size = 5`. Result: vector of 5 copies
  of A's item. Real-world trigger requires either heap corruption or a
  concurrent mutation, but unlike `for_each_thread` the iteration bound
  is user-controlled (via the Java `size` field) so a malicious or
  unlucky `size` value amplifies the problem.
- **suggested_fix:**
  Match the `for_each_thread` pattern: keep a small
  `std::unordered_set<const void*> visited;` and break out of the loop
  the first time `visited.insert(node_oop).second` is false. Bounded by
  `size`, so memory is small. Optionally `VMHOOK_LOG` the cycle so the
  user knows the chain was malformed rather than guessing why the
  vector is short.
- **confidence:** likely

### [low] Hard-coded 32-bit OOP reads silently misread on `-XX:-UseCompressedOops` builds
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:13946-13948, 13967-13970, 13980-13983
- **description:**
  `first`, `item`, and `next` are read as `*reinterpret_cast<const std::uint32_t*>`
  and decoded via `decode_oop_pointer(uint32_t)`. On 64-bit HotSpot built
  or launched with `-XX:-UseCompressedOops` (or heaps > 32 GB without
  default compression), object references are 64-bit wide; the read will
  pick up only the low 32 bits and `decode_oop_pointer` will reconstruct
  nonsense. The result is either a `nullptr` slot for every element
  (best case — the high bits make `is_valid_pointer` fail) or a wild
  pointer dereference. This bug is **shared with every other walk
  helper** (`hash_map_walk_entries`, `hash_map_walk_keys`,
  `tree_map_walk_entries`, `tree_map_walk_keys`, the ArrayList branch
  in `collection::to_vector`, and the `field_proxy::value_t::to_vector`
  delegator), so fixing it here in isolation would create a parity
  inconsistency; either fix the whole family or document the
  precondition. The library README is HotSpot-only and most users do
  run with compressed OOPs by default, but it should be called out.
- **repro:**
  Launch the example JVM with `-XX:-UseCompressedOops` and run the
  LinkedList probe — `linkedListToVectorSize` will pass (size comes
  from a Java probe), but `linkedListToVectorElements` will fail
  because the items decode wrong.
- **suggested_fix:**
  Either (a) add a `decode_oop_pointer_64(uint64_t)` overload and a
  compile-time or runtime branch on `UseCompressedOops` (read once via
  VMStruct), or (b) explicitly document at the top of
  `linked_list_walk_items` (and siblings) that the helper assumes
  compressed OOPs, and fail loudly with one `VMHOOK_LOG(warning_tag, ...)`
  early-return guarded on `UseCompressedOops` being false. (b) is the
  XS-effort fix that preserves parity.
- **confidence:** likely

## Improvements

### [S] [USER_FACING] Log silent early returns so callers can diagnose empty vectors
- **rationale:**
  Every failure path in `linked_list_walk_items` returns silently:
  invalid `list_oop`, `klass_from_oop` returning null, `find_field`
  missing `first`/`item`/`next`. The user sees an empty vector with
  no hint of what went wrong, which is exactly the pattern that drove
  the diagnostic-log additions in `field_proxy::value_t::to_vector`
  (line 14393). The entry point already logs when the *collection* OOP
  is bad, but if the field lookup inside the walker fails (e.g. a
  custom List subclass that has `first` and `size` but not `item`),
  the user gets nothing.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:13932-13945, 13957-13966
- **suggested_change:**
  Add one `VMHOOK_LOG("{} linked_list_walk_items: ...", vmhook::warning_tag, ...)`
  on each early-return path that doesn't represent the "empty list"
  fast path. Match the format used by `field_proxy::value_t::to_vector`
  at line 14393.

### [S] [USER_FACING] Add `linked_list::to_vector()` thin wrapper for parity with method_proxy / field_proxy callsites
- **rationale:**
  The audit brief flags "method_proxy vs field_proxy parity gaps" as
  the top user-friendliness concern. Today, a caller who holds a
  `vmhook::linked_list ll{ oop }` has to write `ll.to_vector<T>()`
  which works because `linked_list` inherits from `list` → `collection`
  → templated `to_vector` is reached. But there is no LinkedList-specific
  fast path on the wrapper — the dispatch in `collection::to_vector`
  re-probes the OOP every time even though the caller already told us
  it's a LinkedList by typing the wrapper as `linked_list`. A
  `linked_list::to_vector<T>()` override that calls
  `linked_list_walk_items` directly (skipping the four-way cascade in
  `collection::to_vector`) would shave a couple of `find_field` calls
  off the dispatch and improve discoverability via IntelliSense.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:13715-13722
- **suggested_change:**
  ```cpp
  class linked_list : public vmhook::list {
  public:
      using vmhook::list::list;

      template<typename element_type>
      auto to_vector() const -> std::vector<std::unique_ptr<element_type>> {
          std::vector<std::unique_ptr<element_type>> result;
          const auto size_opt{ this->get_field_by_oop_klass("size") };
          if (!size_opt) return result;
          vmhook::linked_list_walk_items<element_type>(this->instance, size_opt->get(), result);
          return result;
      }
  };
  ```
  (Tiny addition; preserves the existing inheritance behavior for
  callers who use the generic `collection::to_vector` path.)

### [XS] [INTERNAL] Replace magic-string field lookups with a one-shot constant
- **rationale:**
  Strings `"first"`, `"item"`, `"next"` are written as bare string
  literals inside the templated body; they're stable JDK names but
  every translation unit that instantiates `to_vector<T>` will paste
  them in. A `static constexpr std::string_view` triplet at namespace
  scope (or inside the helper) is one line and removes the typo
  surface area if JDK 25/26 ever renames a field (the way it renamed
  the `CompressedOops` struct keys — see lines 4234-4237).
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:13941, 13961-13962
- **suggested_change:**
  ```cpp
  inline constexpr std::string_view k_linkedlist_first{ "first" };
  inline constexpr std::string_view k_linkedlist_item { "item"  };
  inline constexpr std::string_view k_linkedlist_next { "next"  };
  ```
  ...and use them. Zero runtime cost, small clarity win.

### [XS] [INTERNAL] Update doc comment to match actual behavior (or fix the code to match the doc)
- **rationale:**
  Lines 13923-13925 state "Reads the Node klass once per node ... then
  offsets into each node by the cached `item` and `next` field offsets."
  As shown in the medium-severity bug above, the code re-reads klass
  and re-runs `find_field` every iteration; nothing is cached. Either
  the doc has to be downgraded ("Reads the Node klass and field offsets
  on each iteration; HotSpot guarantees they are stable for one list")
  or the code has to be raised to match the promise. The latter is
  preferred, but at minimum the documentation should not lie about
  perf characteristics.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:13920-13928
- **suggested_change:**
  Pair this with the medium-bug fix; one Edit can correct both.

## Tests

### [standalone_unit] [extend_existing] linked_list_walk_items_compiles_for_typical_wrappers
- **description:**
  `test_api_surface.cpp` already instantiates
  `vmhook::linked_list{ nullptr }.to_vector<element_w>()` (line 109).
  Extend it to also explicitly instantiate
  `vmhook::linked_list_walk_items<element_w>(nullptr, 0, vec)` with a
  null OOP so the templated body type-checks against typical wrappers
  on the standalone build path.
- **asserts:**
  Compile succeeds; runtime call on null OOP returns immediately and
  leaves `vec` empty (use `EXPECT_EQ(vec.size(), 0)` or the harness's
  `check`).
- **existing_file:** tests/test_api_surface.cpp

### [standalone_unit] [new] linked_list_walk_items_null_and_negative_size_guards
- **description:**
  Standalone unit covering the early-return guards: `list_oop == nullptr`,
  `is_valid_pointer == false` (use a deliberate garbage pointer
  `reinterpret_cast<void*>(0x1)`), and `size <= 0`. Each call must
  return without crashing and without mutating `out`.
- **asserts:**
  `out.empty()` after each call; no segfault; if `VMHOOK_LOG_FILE` is
  set, expect a warning line for the invalid-pointer case.
- **existing_file:** *(new)*

### [standalone_unit] [new] linked_list_walk_items_oversized_size_terminates_on_null_next
- **description:**
  Mock-OOP test (use the `is_valid_pointer` test seam already exercised
  in `test_os_protect_interaction.cpp`) where `size = 999` but the
  chain is only 3 nodes long with `next == 0` on node 3. Walk must
  produce exactly 3 entries (the `node_oop` guard short-circuits the
  loop) and not crash trying to dereference past the chain's end.
- **asserts:**
  `out.size() == 3`; entries 0..2 are non-null wrappers.
- **existing_file:** *(new)*

### [standalone_unit] [new] linked_list_walk_items_cycle_detection_dedups_entries
- **description:**
  Construct a 2-node chain that cycles (node B's `next` points back at
  node A) and call the walker with `size = 100`. Without cycle
  detection (current behavior) the result is 100 entries. With the
  proposed cycle-detection fix, the result is exactly 2 entries.
  Test asserts the post-fix behavior; in the meantime it can be marked
  `// pending fix` so it documents the bug.
- **asserts:**
  `out.size() == 2` (post-fix); each unique `item` OOP appears at most
  once.
- **existing_file:** *(new)*

### [standalone_unit] [new] linked_list_walk_items_null_item_becomes_nullptr_slot
- **description:**
  Mock a 3-node chain where node 2 has `item == 0`. Verify that the
  vector contains `[wrapper, nullptr, wrapper]` rather than dropping
  the null entry (the documented behavior on line 13927 and 13536).
- **asserts:**
  `out.size() == 3`; `out[1] == nullptr`; `out[0]` and `out[2]` are
  non-null.
- **existing_file:** *(new)*

### [jvm_integration] [extend_existing] linkedListProbe_perf_no_quadratic_regression
- **description:**
  The existing `test_linked_list_probe` in `vmhook/src/example.cpp`
  (line 1920) verifies size/elements correctness on a 3-element list.
  Extend `Example.java` to build a 10_000-element `LinkedList<A>` and
  add a wall-clock assertion: the to_vector call must complete in
  under, say, 50 ms on the CI box. With the doc-vs-code O(N*F) bug
  unfixed, this will catch any future regression that re-introduces
  the per-node `find_field` cost; once the bug is fixed, it serves as
  a perf canary.
- **asserts:**
  `vec.size() == 10000`; elapsed time below threshold; element[k]'s
  identityHashCode matches `linkedListOfAs.get(k)` for a sampled few
  indices.
- **existing_file:** vmhook/src/example.cpp + example/vmhook/Example.java + example/vmhook/Main.java

### [jvm_integration] [extend_existing] linkedListProbe_handles_null_elements
- **description:**
  The current JVM probe only adds three `new A()` elements (no nulls).
  Extend it to also build a list with `add(null)` so the LinkedList
  fast path exercises the `out.push_back(nullptr)` branch end-to-end
  on real HotSpot — not just a mocked Node.
- **asserts:**
  `vec.size() == expected`; `vec[null_index] == nullptr`; subsequent
  non-null indices wrap correctly.
- **existing_file:** vmhook/src/example.cpp + example/vmhook/Example.java

## Parity Concerns
- `linked_list_walk_items` hoists nothing per iteration, while
  `hash_map_walk_entries` (line 13997) also re-runs `find_field` for
  `key`/`value`/`next` on every node — same O(N*F) anti-pattern. Either
  fix both consistently or document both consistently.
- `hash_map_walk_entries` and `tree_map_walk_entries` include an
  explicit per-bucket / per-traversal cap (`1<<20`, `1<<24`) to defend
  against corrupt chains; `linked_list_walk_items` relies solely on
  `size`. That is asymmetric defensive coding — the audit brief
  explicitly asks about cycle detection here.
- `tree_map_walk_*` carries an inline rationale comment for its
  iterative-with-stack approach; `linked_list_walk_items` has nothing
  comparable explaining why the walk is *not* defended against
  cycles, which makes the absence look unintentional.
- `field_proxy::value_t::to_vector` (line 14386) emits one
  `VMHOOK_LOG(warning_tag, ...)` on a bad collection OOP; the
  downstream walker silently swallows every other failure mode. The
  user only gets diagnostics for the "your field is null" case, not
  the "your custom List subclass doesn't expose `first`" case.
- The `linked_list` wrapper class (line 13715) is documented as a
  type-tag with "no LinkedList-specific behavior". `hash_map`
  (line 13904) and `set` / `list` are the same. That is fine for the
  user, but if `linked_list::to_vector` were to add the fast path
  suggested above, the parity would shift; do it for `hash_map` too
  (`hash_map::to_entries` direct-dispatch) so the wrappers all behave
  the same way.
