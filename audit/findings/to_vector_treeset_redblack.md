# to_vector_treeset_redblack

## Summary
Audited `vmhook::collection::to_vector<T>()`'s TreeSet fast path (vmhook.hpp:13648-13657)
and its delegate `vmhook::tree_map_walk_keys<E, out_t>` (vmhook.hpp:14304-14401), the
iterative in-order red-black walk over `TreeMap.root`. The empty-tree case is handled
correctly (root compressed-OOP `0` decodes to `nullptr`, the early-return path at
14325 exits with an empty vector). The two real concerns are (a) **silent
misclassification of `Collections.newSetFromMap(new HashMap<>())` as a TreeSet** —
both have a field named `m`, the walker silently returns empty when `root` is missing,
and the caller never knows; and (b) **unbounded inner left-spine loop with no
cycle/depth guard** — the outer `visited` counter is only incremented on pop, so a
corrupt or attacker-controlled left-cycle balloons the std::vector stack until OOM
before the (1<<24) cap can fire. Every diagnostic exit is also silent (no
`VMHOOK_LOG`) which diverges from sibling helpers like `read_java_string` and
`field_proxy::value_t::to_vector`.

## Bugs

### [medium] TreeSet fast path silently returns empty for `Collections.newSetFromMap(...)` (and any non-TreeMap "m" field)
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:13646-13657
- **description:** The cascade in `collection::to_vector<T>()` uses field-presence
  probing to decide which fast path to take. Step 4 keys off a field literally named
  `"m"` and unconditionally hands the OOP to `tree_map_walk_keys`. However, the JDK
  ships another Set wrapper with a field named exactly `m`:
  `java.util.Collections$SetFromMap`, returned by `Collections.newSetFromMap(map)`,
  declares `private final Map<E, Boolean> m;`. When the user passes
  `Collections.newSetFromMap(new HashMap<>())` (or `IdentityHashMap`,
  `ConcurrentHashMap`, ...), `get_field_by_oop_klass("m")` succeeds at line 13648,
  the walker is called, `find_field(map_klass, "root")` at vmhook.hpp:14316 returns
  `std::nullopt` (HashMap has no `root` field), and `tree_map_walk_keys` quietly
  returns on line 14319 without pushing anything. The caller then receives an empty
  vector at line 13656 — a silently wrong result for a non-empty Set. The other
  three branches in the cascade (`elementData`, `first`+`size`, `map`) all walked
  past it because TreeSet has none of those fields, but `m` collides with
  `SetFromMap`. Note that `Collections.newSetFromMap` is the documented way to make
  a Set out of a ConcurrentHashMap (Java 8 ConcurrentHashMap.newKeySet uses the
  same pattern internally), so this is a real fixture in production code.
- **repro:** In a Java fixture add `Set<A> setFromHash =
  Collections.newSetFromMap(new HashMap<>()); setFromHash.add(new A());` then call
  `get_field("setFromHash")->get().to_vector<a_class>()` from C++ — the returned
  vector is `.size() == 0` even though `size_field->get() == 1`.
- **suggested_fix:** Before committing to the TreeSet path, verify the inner map's
  klass actually carries the TreeMap layout. Either: (a) in `collection::to_vector`
  resolve the `m` field, decode the OOP, look up its klass, and check for `root`
  before calling `tree_map_walk_keys`, falling through to the generic `iterator()`
  path otherwise; or (b) make `tree_map_walk_keys` return a `bool` indicating
  whether it recognised the layout, and on `false` let the caller fall through to
  the generic Set iterator path. Option (a) is simpler and matches the
  field-presence probing already used elsewhere in the cascade.
- **confidence:** certain

### [medium] Inner left-spine descent has no depth/cycle guard — corrupt tree can OOM the process
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:14340-14359
- **description:** The iterative walk has two nested loops. The **inner** loop
  pushes `node_oop` onto `stack` and recurses into `entry.left` without any depth
  counter. The only safety cap is `visited > (1 << 24)` at line 14440, but
  `visited` is incremented in the **outer** loop body, **after** a pop. If the
  left-pointer of any reachable node points back to itself (or to an ancestor),
  the inner loop never terminates and never reaches the cap — it just keeps
  pushing entries onto `std::vector<void*> stack` until allocator failure. Since
  `is_valid_pointer` only checks the OS region and not graph identity, both
  conditions in the inner-loop `while` pass forever. This is the same class of
  bug that the team already fixed for `for_each_thread` in v0.4.4 (commit
  a337217: "for_each_thread cycle detection"). The sibling `hash_map_walk_keys`
  at line 14108 guards its inner per-bucket walk with `guard < (1 << 20)`; the
  TreeMap variants have no equivalent. With a forged TreeSet from an attacker-
  controlled deserialiser (e.g. a hostile heap dump replayed into a target
  process), this is exploitable as a DoS / OOM primitive on the injector.
- **repro:** Hard to repro from pure Java because `TreeMap` rebalances. Repro by
  hand-writing a fake `TreeMap.Entry` whose `left` field's compressed OOP encodes
  the entry itself, then place it in a field probed by `to_vector` — or run the
  walker over a heap that has had a `TreeMap.Entry.left` field overwritten by an
  unrelated hook. Either way `stack.size()` grows unbounded until `bad_alloc`.
- **suggested_fix:** Add a per-iteration guard to the inner loop mirroring the
  HashMap walker: introduce a local `std::int32_t left_guard{ 0 };` inside the
  inner `while` and break out (and log) when `++left_guard > (1 << 20)`. Replicate
  the same change in `tree_map_walk_entries` (lines 14195-14213). Optionally also
  promote `visited` to be incremented on every push (not just on each pop) so a
  pure right-spine cycle is also caught.
- **confidence:** likely

### [low] Walker is silent on every non-empty failure path — divergence from sibling helpers
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:14307-14328, 14340-14400
- **description:** `tree_map_walk_keys` has nine distinct early-return / break
  paths: invalid `map_oop` (14307), null `map_klass` (14311), missing `root`
  field (14316), null/invalid root (14325), null entry klass during descent
  (14342, 14366), missing `left` field (14348), missing `key`/`right` fields
  (14372), and the (1<<24) cap (14440). **None** of them emit a `VMHOOK_LOG`.
  Compare this to `field_proxy::value_t::to_vector` at line 14393 which logs
  with `warning_tag` when the collection OOP is null/invalid, or
  `read_java_string` (vmhook.hpp:14442) which logs every failure with class and
  pointer context. When a user file a "my TreeSet probe returns empty" bug, the
  current code provides zero observability — the user has no signal whether
  (i) the TreeSet really is empty, (ii) the klass lookup failed, (iii) a field
  the walker depends on was renamed in their JDK, or (iv) the (1<<24) cap fired
  on a malformed tree. The missing-`root` path is especially silent and matters
  because it is the same path that triggers the "SetFromMap" bug above.
- **repro:** Run the TreeSet probe against a malformed object whose klass has no
  `left` field; observe that the returned vector is empty and no message is
  printed. Compare with calling `read_java_string` on the same OOP — it logs
  with `error_tag`/`warning_tag` and includes the OOP address.
- **suggested_fix:** Add `VMHOOK_LOG` calls on each branch with the same
  `warning_tag` style used by `field_proxy::value_t::to_vector` — include
  `typeid(element_type).name()`, the map OOP address, and (where relevant)
  the missing field name. Pay particular attention to logging when the
  `(1 << 24)` cap fires so users can distinguish a genuine giant TreeSet from
  a cycle / corruption case.
- **confidence:** certain

## Improvements

### [S] [USER_FACING] Document TreeSet fast-path coverage in the `vmhook::set` doc comment
- **rationale:** The class doc at vmhook.hpp:13683-13699 lists "HashSet /
  LinkedHashSet / TreeSet" but does not mention that the fast path is keyed on
  field shape rather than on klass name, nor does it warn about
  `Collections.newSetFromMap`. Today the docs imply parity with the HashSet path
  when there is none.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:13682-13699
- **suggested_change:** After fixing the SetFromMap bug above, update this doc
  comment to enumerate exactly which `Set` impls are covered by which probe
  (`HashSet`/`LinkedHashSet`/`ConcurrentHashMap.KeySetView` → `map` field;
  `TreeSet` → `m` field; `Collections.newSetFromMap(...)` → not yet, falls
  through to generic iterator path).

### [XS] [INTERNAL] Hoist the `(1 << 24)` walker cap into a named constant
- **rationale:** The literal `(1 << 24)` is duplicated at vmhook.hpp:14339 and
  14440, and `(1 << 20)` is duplicated at 14108 and 14192. A user tuning the
  cap (or the team raising it in response to a real large heap) has to update
  multiple sites. Drift across the four walkers is also easy to introduce.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:14108, 14192, 14339, 14440
- **suggested_change:** Add `inline constexpr std::int32_t
  collection_walk_visited_cap{ 1 << 24 };` and `inline constexpr std::int32_t
  collection_walk_inner_guard{ 1 << 20 };` near `warning_tag` (line 399), and
  reference them by name in all four walkers.

### [S] [USER_FACING] Reuse one templated walker for both `to_vector` and `to_entries`
- **rationale:** `tree_map_walk_entries` (14186-14302) and `tree_map_walk_keys`
  (14304-14401) duplicate ~90% of their logic: same `root` lookup, same iterative
  stack walk, same field lookups, same caps. The only difference is what gets
  pushed onto `out`. Every concern raised in this audit (the cycle bug, the
  missing logs, the cap constant) has to be fixed twice and is already drifting
  (the `keys` version doesn't capture `value_entry`, but otherwise the prologue
  and epilogue are byte-for-byte identical). Consolidate them.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:14186-14401
- **suggested_change:** Introduce an internal helper that takes a visitor
  callable `void(void* entry_oop, vmhook::hotspot::klass* entry_klass)`. Have
  `tree_map_walk_keys` instantiate it with a visitor that pushes
  `make_unique<element_type>(key_oop)` and `tree_map_walk_entries` instantiate
  with one that emplaces a pair. Cuts the bug surface in half.

### [XS] [USER_FACING] Use `warning_tag` when the `(1 << 24)` cap fires
- **rationale:** When a real user has a 32-million-element TreeSet (huge but
  possible for a long-running cache), the walker silently truncates and
  returns. Without a log they can't tell why their vector is smaller than
  `size()`. Even a single `VMHOOK_LOG("{} tree_map_walk_keys: visited cap...")`
  would save hours of debugging.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:14439-14443, 14338-14342
- **suggested_change:** Replace `if (++visited > (1 << 24)) { break; }` with a
  log-then-break block that prints the cap and the visited count.

## Tests

### [standalone_unit] [extend_existing] tree_map_walk_keys_handles_null_inputs
- **description:** Call `vmhook::tree_map_walk_keys<element_w>(nullptr, out)`
  and verify (i) it returns without crashing, (ii) `out` is empty. Same for
  an `is_valid_pointer == false` poisoned pointer.
- **asserts:** `out.size() == 0` after the call; no exception escapes.
- **existing_file:** tests/test_api_surface.cpp

### [standalone_unit] [new] tree_map_walk_keys_empty_tree_returns_empty
- **description:** Wire a fake klass with a `root` field-entry but read `root`
  back as compressed zero; ensure the walker returns immediately with an empty
  `out`. Verifies the empty-TreeSet path that this audit focused on.
- **asserts:** `out.empty()`; walker did not call `find_field` for `left` or
  `key` (use a spy/mock if practical, else just verify empty + non-crashing).

### [standalone_unit] [new] collection_to_vector_treeset_misclassifies_setfrommap
- **description:** Regression test for the `Collections.newSetFromMap` bug.
  Construct a fake collection whose oop's klass has field `m` but whose `m`
  payload is a HashMap-shaped klass (has `table`, no `root`). Call
  `to_vector<element_w>()`. Today this returns empty silently — the test should
  pin that behaviour as a known-broken-case and flip its expectation when the
  cascade is taught to detect the wrong layout.
- **asserts:** Currently `result.empty()`; after fix, the generic
  `size()`+iterator fallback should be reached and `result.size() == n`.

### [jvm_integration] [new] test_tree_set_probe
- **description:** Mirror `test_set_probe` (currently HashSet-only) for an
  actual `java.util.TreeSet<A>`. Add `Set<A> treeSetOfAs = new TreeSet<>();`
  to `example/vmhook/Example.java` populated with three `A` instances using a
  Comparator that is stable across runs (use the `name`/identity field).
  Wire `tree_set_probe_*` static fields and a `get_tree_set_of_as_vector()`
  helper on `example_class`. Calls
  `get_field("treeSetOfAs")->get().to_vector<a_class>()` and checks size and
  per-element identity. This is the only place where the live red-black walk
  is actually exercised; today there is zero JVM coverage for
  `tree_map_walk_keys`.
- **asserts:** `entries.size() == 3`; each returned `a_class*` matches one of
  the three known elements (any order is fine since TreeSet ordering depends
  on the comparator — assert as a set-equality check, not a sequence check).
- **existing_file:** vmhook/src/example.cpp + example/vmhook/Example.java +
  example/vmhook/Main.java

### [jvm_integration] [new] test_tree_set_empty_probe
- **description:** Companion to the previous test: declare an empty
  `Set<A> emptyTreeSetOfAs = new TreeSet<>();` and call `to_vector<a_class>()`
  on it. Pins the empty-tree code path this audit task is focused on.
- **asserts:** Returned `vector.empty()`; no log noise; the call completes
  without taking the generic iterator fallback.
- **existing_file:** same triple as above

### [standalone_unit] [new] tree_map_walk_keys_visited_cap_logs_when_capped
- **description:** Once the suggested log on cap-fire lands, capture `stderr`
  during a fabricated 16M+ entry walk (or lower the cap behind a test hook)
  and assert the warning string is emitted. Today no test could even fail
  because the cap exit is silent.
- **asserts:** Captured log contains `"visited cap"` or equivalent identifier;
  vector size equals the cap.

### [standalone_unit] [new] tree_map_walk_keys_left_cycle_does_not_oom
- **description:** Regression test for the cycle bug. Build a fake TreeMap
  klass and a fake entry whose `left` compressed-OOP decodes back to itself,
  then call `tree_map_walk_keys`. With the bug present the inner loop pushes
  onto `stack` until allocator failure; with the suggested per-iteration
  guard it should break out within `(1 << 20)` iterations.
- **asserts:** Walker returns within reasonable wall-clock time and
  `stack.capacity() < (1 << 21)`; suggested log was emitted.

## Parity Concerns
- `tree_map_walk_keys` and `tree_map_walk_entries` carry the same bugs (no
  inner guard, no logs, hard-coded caps) — any fix must land in both. See
  improvement "Reuse one templated walker for both `to_vector` and
  `to_entries`" above.
- `hash_map_walk_keys` (vmhook.hpp:14082) **does** have an inner guard
  (`guard < (1 << 20)` at line 14116). The TreeMap walkers should match that
  pattern. Today only the HashMap family is guarded against per-bucket cycles.
- `field_proxy::value_t::to_vector` (vmhook.hpp:14393) logs with `warning_tag`
  on null/invalid OOP. The tree walker peers downstream emit no log at all,
  so the field-proxy log is the only diagnostic a user sees — and it doesn't
  fire for the "field `m` exists but isn't TreeMap" misclassification or for
  the silent missing-`root` / missing-`left` exits inside the walker.
- The fast-path cascade dispatches by field name (`elementData`, `first`,
  `map`, `m`). The `m` collision documented in Bug #1 is the only fast-path
  discriminator that isn't unique to one container family — every other
  branch picks fields that exist on exactly one JDK Set/List impl. Worth
  raising as a class-of-bug review for any future fast paths added to this
  cascade.
