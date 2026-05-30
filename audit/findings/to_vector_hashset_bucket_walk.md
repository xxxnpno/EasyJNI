# to_vector_hashset_bucket_walk

## Summary
Audited the HashSet / LinkedHashSet fast path in `collection::to_vector` (vmhook.hpp:13600-13611) and its underlying bucket walker `hash_map_walk_keys` (vmhook.hpp:14078-14149). The walker is correct for both regular Node and TreeNode bins because `find_field` walks the superclass chain and HotSpot keeps the linked `Node.next` chain populated even after treeification. Top concerns are silent failure modes (no log on missing `table` / unresolvable `node_klass`), LinkedHashSet insertion-order is **not** preserved (undocumented gotcha for users), and the test surface is compile-only — there is no runtime test covering treeified bins, LinkedHashSet specifically, or buckets containing the legal null element.

## Bugs

### [low] LinkedHashSet insertion order is silently lost (documentation/user-expectation defect)
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:13600-13611, vmhook/ext/vmhook/vmhook.hpp:14078-14149, vmhook/ext/vmhook/vmhook.hpp:13683-13701
- **description:** `LinkedHashSet` extends `HashSet` and exposes the documented contract that iteration order matches insertion order, achieved via the `LinkedHashMap.Entry.before/after` doubly-linked list that overlays the bucket table. `hash_map_walk_keys` walks the bucket array left-to-right and the `Node.next` chain inside each bucket, which produces hash-order, not insertion order. Java callers who switch from `LinkedHashSet` to `HashSet` (or vice versa) and rely on `to_vector` ordering will get silently different results from C++ vs. Java iteration. Neither the wrapper comments (`set` at :13683, `to_vector` at :13520) nor the walker doc-comment at :14078 mention this constraint, and the cascade gives no way to opt in to the `before/after` walk.
- **repro:** Java side: `LinkedHashSet<A> s = new LinkedHashSet<>(); s.add(a0); s.add(a1); s.add(a2);` then read with `get_field("s")->get().to_vector<a_class>()`. The returned vector's element OOP order is determined by `hash(a_i) & (table.length-1)`, not insertion. For three `new A()` objects the resulting order is unpredictable and effectively random across runs because `Object.hashCode` is identity-based.
- **suggested_fix:** Either (a) detect `LinkedHashMap` via presence of `head`/`tail` fields on the backing map klass and walk `LinkedHashMap.Entry.after` from `head` instead of the bucket table when present, or (b) update the doc-comments on `vmhook::set` and `collection::to_vector` to explicitly call out "iteration order for HashSet/LinkedHashSet matches HotSpot bucket order, NOT insertion order — wrap in `std::sort` if you need determinism". Option (a) is the user-friendly fix; option (b) at minimum stops surprising users.
- **confidence:** certain

### [low] Silent empty-vector return when backing HashMap layout is unrecognised
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:14083-14107, vmhook/ext/vmhook/vmhook.hpp:14108-14129
- **description:** `hash_map_walk_keys` returns silently from five different failure points (`!map_oop` / `!map_klass` / `!table_entry` / `!table_oop` / bucket walk `break` on missing `key`/`next`). None of them log anything, so a HashSet field that "works" but returns an empty vector is indistinguishable from a HashSet that truly is empty or whose backing map shape is unexpected (e.g., a future JDK that renames `table`, a stripped/obfuscated build, a custom Set whose backing map field happens to be named `map` but isn't a HashMap). The sibling `find_field` *does* `VMHOOK_LOG` on miss (vmhook.hpp:10306) — but that only logs ONCE per (klass, name) miss due to caching, and the per-bucket `break` at :14128 fires silently for every node thereafter.
- **repro:** Inject into a process running a HotSpot build that exposes a HashSet-shaped class whose backing map klass has no `table` field. Reading the set returns an empty vector; nothing appears in the log even at debug verbosity. The user assumes the set is empty.
- **suggested_fix:** Add `VMHOOK_LOG("{} hash_map_walk_keys: backing HashMap missing 'table' field — returning empty vector.", vmhook::error_tag);` at vmhook.hpp:14098, and a similar one when the per-node `key`/`next` resolution breaks the loop at :14128. The `find_field` log is too far upstream for the user to correlate.
- **confidence:** likely

### [low] Spec-correct null-element handling differs from what HashSet actually stores
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:14133-14141
- **description:** Comment at :13536 promises "Null Java elements become nullptr entries in the returned vector." For `HashMap` this is straightforward — `key` may be `null`. For `HashSet`, however, the backing `HashMap` value slot holds the `HashSet.PRESENT` sentinel for every entry, and the `key` is what the user passed to `add()`. So `nullptr` in the vector can come from two distinct sources: (1) the user really did `set.add(null)` (legal — HashSet supports a single null), or (2) a torn read where `key` decoded to an invalid pointer (`!is_valid_pointer`). The current code silently merges these and gives the caller no way to distinguish. This isn't strictly wrong, but the doc-comment claims symmetry with the Java semantics that doesn't fully exist.
- **repro:** Java: `Set<A> s = new HashSet<>(); s.add(null); s.add(new A());`. C++ reads `vec.size() == 2` with one entry `nullptr` — but if `is_valid_pointer` had rejected the second entry's key, the caller would see the same shape and assume Java held a null.
- **suggested_fix:** Either (a) tighten the comment at :13536 to say "Null entries in the returned vector mean either Java null or an unreadable key — log at error_tag when an OOP fails `is_valid_pointer` so the two cases are distinguishable in the log", or (b) add a `VMHOOK_LOG` at :14138-14140 when `key_oop` is non-null but fails `is_valid_pointer`, so the torn-read case is auditable.
- **confidence:** speculative

## Improvements

### [S] [USER_FACING] Document the "HashSet vs LinkedHashSet" ordering caveat on the wrapper itself
- **rationale:** The `vmhook::set` wrapper class doc-comment at :13683-13693 reads as a complete API and doesn't warn the user that LinkedHashSet iteration order is lost. Users coming from Java naturally expect `LinkedHashSet → to_vector` to give insertion order. A one-paragraph note here would catch them at the documentation surface they're most likely to read.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:13683-13693
- **suggested_change:** Add to the docstring: "Iteration order: HashSet returns elements in hash-bucket order; LinkedHashSet's insertion-order contract is **not** preserved — the walker reads the backing HashMap.table directly and ignores the LinkedHashMap.Entry.before/after overlay. If you need deterministic order, sort the result vector by some stable element key."

### [S] [USER_FACING] Cascade comment lists three "map"-bearing sentinel fields without saying what wins when they all match
- **rationale:** Read the cascade at :13554-13624 cold: it's not obvious that a user-defined Java class with a `size` int *and* a `map` reference *and* a `m` reference would silently route to ArrayList path (no `elementData`), then HashSet path. The doc at :13520 lists "in order of specificity" but doesn't show which probe wins. A short "first-match wins; ArrayList requires both size+elementData; HashSet only requires map" line in the doc would help reverse engineers reading their first probe failure.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:13520-13539
- **suggested_change:** Add to the docstring: "Disambiguation: only ArrayList requires two co-occurring fields (size + elementData); LinkedList/HashSet/TreeSet are each identified by a single field (first / map / m). If your Java class happens to have a field named `map` that isn't a HashMap, the HashSet path will be selected and return an empty vector — register a wrapper for your class and call `to_vector` through that instead."

### [XS] [INTERNAL] Hoist per-bucket field-offset lookups out of the inner loop
- **rationale:** `hash_map_walk_keys` re-calls `vmhook::find_field(node_klass, "key")` and `(node_klass, "next")` for every single node visited (:14124-14125). The cache makes each call O(1) but it's still a mutex acquire + hash lookup per node. Since HotSpot guarantees Node vs. TreeNode klass is fixed within a bucket, the lookups could be hoisted to once per bucket (with a fallback re-resolve when the klass changes). At 50k+ entries this is measurable.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:14115-14129
- **suggested_change:** Cache `last_node_klass`, `last_key_entry`, `last_next_entry` across iterations; only call `find_field` when `node_klass != last_node_klass`. Same applies to `hash_map_walk_entries` at :14034-14046 for consistency.

### [S] [USER_FACING] No `reserve()` before bucket walk — many small reallocations on large sets
- **rationale:** `to_vector` reserves the result vector in the ArrayList path (:13570) and the generic fallback path (:13639) using the known `size()`. The HashSet path at :13602-13610 calls `hash_map_walk_keys` with no reservation. The walker has no idea how many keys it will visit. A 100k-entry HashSet will produce ~17 vector reallocations (2× growth starting from 0). The map's `size()` is cheap and known — read it once before the walk and reserve.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:13602-13611, vmhook/ext/vmhook/vmhook.hpp:14083-14149
- **suggested_change:** Either (a) read the map's `size` field at :13605 (same field name HashMap uses for its int count) and `result.reserve(static_cast<std::size_t>(map_size))` before delegating, or (b) add an optional `expected_count` parameter to `hash_map_walk_keys` and call `out.reserve(expected_count)` at the top.

### [XS] [INTERNAL] Sentinel `1 << 20` bucket-walk cap is silently swallowed
- **rationale:** The per-bucket loop guard at :14115-14117 caps at 1,048,576 nodes per bucket. If hit, the loop exits silently with no log — and a real bucket that large would mean either a treeified bin with a malformed `next` chain (HotSpot bug) or a corrupted heap. Either case deserves a log so the user can spot the truncation.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:14115-14147
- **suggested_change:** Track whether the guard fired (e.g., `if (guard == (1 << 20)) { VMHOOK_LOG("{} hash_map_walk_keys: bucket {} hit walk-cap of {}, chain may be corrupt.", vmhook::error_tag, bucket, (1 << 20)); }`) after the inner loop exits.

## Tests

### [jvm_integration] [extend_existing] test_linked_hash_set_probe_order
- **description:** Add a `LinkedHashSet<String>` field to `Example.java` with at least 8 deterministic string entries ("k0".."k7"). On the C++ side, assert (1) the vector size matches, (2) every element is non-null, and (3) document in the assertion message whether insertion order is preserved (it will not be — the test should currently *expect* hash-order, to lock the behavior in until/unless the LinkedHashMap-aware walker lands).
- **asserts:** `vec.size() == 8`; every `vec[i]` non-null; comment alongside the test calls out the order semantics so a future LinkedHashMap fast-path landing flips this assertion intentionally.
- **existing_file:** example/vmhook/Example.java (add field + populate in ctor), vmhook/src/example.cpp (add `test_linked_hash_set_probe` mirroring `test_set_probe` at :1958-1995)

### [jvm_integration] [new] test_hash_set_treeified_bin
- **description:** Construct a HashSet whose backing map has at least one treeified bin (≥8 keys hashing to the same bucket — easiest is `Integer` keys hand-picked so `(h ^ (h >>> 16)) & 15 == 0` for 12 distinct keys at the default capacity of 16). On the C++ side, read the set via `to_vector` and confirm all 12 keys are returned. This exercises the TreeNode-via-Node-super `find_field` path that the audit confirmed should work but has no runtime coverage.
- **asserts:** `vec.size() == 12`; the union of key identity hashes matches Java side; no truncation, no infinite loop.
- **existing_file:** none — add to `example/vmhook/Example.java` as `hashSetTreeified` field + new probe in `vmhook/src/example.cpp` mirroring the `setOfAs` probe pattern at :1958-1995.

### [jvm_integration] [new] test_hash_set_with_null_element
- **description:** `HashSet<A> s = new HashSet<>(); s.add(null); s.add(new A()); s.add(new A());`. Confirm `to_vector` returns 3 entries with exactly one `nullptr`. Locks in the documented "Null Java elements become nullptr" promise at :13536 for HashSet, which is currently asserted only for List paths in `test_set_probe` (:1985-1991 explicitly *rejects* null entries via `elements_ok = false` — that test would fail for a HashSet containing null).
- **asserts:** `vec.size() == 3`; `std::count(vec.begin(), vec.end(), nullptr) == 1`; the two non-null entries dereference cleanly.
- **existing_file:** none — new fixture field in `example/vmhook/Example.java` and new probe in `vmhook/src/example.cpp`.

### [standalone_unit] [extend_existing] test_api_surface_set_typed
- **description:** Today `test_api_surface.cpp:108` covers `vmhook::set::to_vector<element_w>()` against a null OOP and just checks it compiles. Extend it to also build a `vmhook::set` from a fake OOP whose backing map klass lookup fails (e.g., a heap-allocated stub) and assert the returned vector is empty rather than crashing — locking in the silent-empty-return contract called out in the bug above.
- **asserts:** vector is empty; no crash; no exception thrown.
- **existing_file:** tests/test_api_surface.cpp (extend `exercise_collection_wrappers` at :83-114)

### [standalone_unit] [new] test_hash_map_walk_keys_guard_cap
- **description:** Construct a synthetic "bucket array" in test memory where node N points back to node 0 (cycle). Confirm `hash_map_walk_keys` exits in bounded time via the `1 << 20` guard at :14115. This needs the standalone harness already used in `test_helpers.cpp` to fabricate fake oops + fake klass entries — non-trivial but high value, because the cycle case is impossible to test against a real JVM.
- **asserts:** Walk terminates within a wall-clock budget (e.g., 5 seconds); resulting vector size is exactly `1 << 20`.
- **existing_file:** tests/test_helpers.cpp has the fake-oop scaffold to model this on.

## Parity Concerns
- `tree_map_walk_keys` (:14274-14336) and `hash_map_walk_keys` (:14083-14149) are near-identical in shape but only the tree variant has a meaningful comment explaining the safety cap (vs. HashMap's silent break); both walkers should share a single doc-comment style + log style so behavior diffs are visible at audit time.
- The `map` cascade at :13602 has no sibling probe for `LinkedHashMap`-backed sets (would enable insertion-ordered LinkedHashSet) — `vmhook::map` at :13876 already covers LinkedHashMap-as-Map via the same `hash_map_walk_entries`, but the `set` path never reaches `head/tail/before/after` even when present.
- ArrayList and LinkedList paths log nothing on dispatch but DO have wrapper-level fallbacks (size() == 0 etc.); the HashSet path likewise logs nothing, but a *user-visible empty result* is the only failure signal — `field_proxy::value_t::to_vector` at :14392-14399 logs only when the entire collection OOP is null, not when the bucket walk silently fails inside. Parity with `field_proxy::set` (which carries explicit size guards and `VMHOOK_LOG` on mismatch per commit 18d5428) is the precedent the user already values.
- `hash_map_walk_keys` accepts `out_t& out` by reference; the entries walker (`hash_map_walk_entries`) also uses `out_t&`, but the doc-comments don't show the reservation/ownership contract. A `// out is appended to, never cleared; reserve before calling if you know the count` line on both keeps callers from re-discovering the convention.
