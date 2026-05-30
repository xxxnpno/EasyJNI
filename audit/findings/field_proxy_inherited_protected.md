# field_proxy_inherited_protected

## Summary
Audited the inherited-protected-field path: `vmhook::find_field` (vmhook.hpp:10267-10308) walks the `klass::get_super()` chain (vmhook.hpp:2707-2719) and asks each ancestor's `klass::find_field()` (vmhook.hpp:2953-3059 + stream variant 2841-2933) for the named slot, returning the first hit. Shadowing semantics are correct (child wins, walk starts at `target_klass`). The top concerns are a documentation comment that contradicts the actual behavior, a missing negative-result cache that pathologically degrades repeated misses, and the absence of any cycle / depth guard on the super-chain walk while sibling `get_super`-based walks elsewhere have one.

## Bugs

### [medium] find_field doc comment contradicts actual superclass-walking behavior
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:10250-10258
- **description:** The Doxygen for `vmhook::find_field` states: *"@param target_klass  The klass that declares the field (obtain via find_class()). Only the declaring class is searched - not superclasses."* and *"On the first call for a given (target_klass, name) pair the full InstanceKlass._fields array is walked"*. The implementation 5 lines below (lines 10291-10304) does walk `get_super()` and the closing log at 10306 even says *"field not found in class hierarchy"*. Users reading the header (the single-header library's primary documentation surface) will write workarounds — manually walking `get_super()` themselves — for a problem that does not exist, or worse, conclude that inherited fields are unsupported and never reach for `b_ptr->get_field("protectedInt")` despite it working.
- **repro:** Read `vmhook.hpp:10250-10258`; the `@param` text contradicts both the loop at 10295 and the success log at 10306.
- **suggested_fix:** Replace the `@param` blurb with "The starting klass for the lookup; the InstanceKlass `_fields` array of `target_klass` is searched first, then each ancestor returned by `klass::get_super()`. Returns the first match so child-declared fields correctly shadow inherited fields with the same name." Also update the `@details` to say "the super-chain is walked outside the cache lock" so the existing comment at line 10263 is no longer the only place that mentions the walk.
- **confidence:** certain

### [low] find_field has no cycle / depth guard on the super-chain walk, while every sibling walker does
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:10295-10304
- **description:** `for (klass* k{ target_klass }; k != nullptr; k = k->get_super())` blindly trusts the JVM's `_super` chain. Every other `get_super()` walker in the file is bounded — e.g. `for_each_thread` carries a visited-set with a 4096 cap and a TODO comment that even quotes *"_next chain can form a cycle"* (lines 6555-6567), `find_class` has `visited < 65536` / `kl_visited < 1048576` caps (lines 3500-3516), even the dictionary chain has `chain_visited < 1048576` (lines 3338-3340). A `Klass._super` chain that loops (corrupt metadata, a hypothetical malicious RedefineClasses, or — practically — `is_valid_pointer` accidentally returning true on garbage after a JVM teardown) hangs the calling thread forever. Java's Object hierarchy is at most ~12 deep in practice, so a hard cap of 256 is harmless. The defensive style is consistent with the rest of the file; this loop is the outlier.
- **repro:** No real-world repro — depends on either VM metadata corruption or a klass whose `_super` slot circularly references an ancestor. Synthetic repro: hand-patch `_super` on a test klass to point back to itself, call `find_field` for a non-existent name. The thread spins.
- **suggested_fix:** Add a depth counter consistent with peers in the file:
  ```cpp
  std::int32_t depth{ 0 };
  for (vmhook::hotspot::klass* k{ target_klass }; k != nullptr && depth < 256; k = k->get_super(), ++depth)
  ```
  Optionally log a warning if `depth == 256` since the Java spec caps real hierarchies far below this.
- **confidence:** likely

### [low] Failed find_field lookups are not negatively cached, causing repeated full hierarchy walks
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:10295-10307
- **description:** When the field genuinely does not exist anywhere in the hierarchy, every call walks the entire chain (`target_klass` plus every ancestor up to `java.lang.Object`) and parses every ancestor's `_fields` / `_fieldinfo_stream`. The success path caches `(klass*, name) -> entry` at line 10301, but the not-found path falls through to the `VMHOOK_LOG` at 10306 and returns `nullopt` without inserting anything. A user wrapper that calls `get_field("optional_or_typoed_name")->get()` on every tick will incur O(super-depth * F) work and an error-tagged log line each frame. For comparison, `find_class` caches both hits and misses indirectly (the dictionary walk is short-circuited by `klass_lookup_cache`); here the cost is per `(B*, "foo")` and never amortizes.
- **repro:** In a tight hook detour call `obj->get_field("doesNotExist")`; the `_fields` arrays of `B`, `A`, and `java.lang.Object` (and any intermediate class) are parsed on every invocation.
- **suggested_fix:** Cache the negative result, e.g. by storing a sentinel `field_entry_t{ ~0u, false, "" }` and treating a hit with `offset == ~0u` as "known-miss, return nullopt". Alternatively change the value type to `std::optional<field_entry_t>`. Skip the `VMHOOK_LOG` on the cached negative path so the log isn't spammed once per frame; emit it only on the first failure.
- **confidence:** likely

### [low] Inherited-field shadowing is not reflected in the cache key, so the same offset is duplicated across every descendant
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:10247, 10300-10302
- **description:** For deep hierarchies (e.g. a Spring AOP-style multi-level inheritance with dozens of subclasses each looking up `java.lang.Object.hash`), the cache holds one `(subclass_klass*, "hash") -> entry` per descendant even though every entry has the same `offset`/`signature`/`is_static`. Not a correctness bug — just memory waste and a marginal lock-contention point because every miss for a different subclass still grabs `g_field_cache_mutex` and writes. A two-level cache (lookup at `target_klass`, fall back to the *declaring* klass and reuse its entry) would dedupe automatically.
- **repro:** Register 50 subclasses of an abstract base, call `get_field("inheritedField")` once on each — `g_field_cache.size()` is 50, with identical inner values.
- **suggested_fix:** When walking the super chain, return `k`'s entry but also stash it under `k` (the declaring klass), then when consulting the cache at the top of `find_field` consult `target_klass` *and* every ancestor. Or simpler: stash under both `(target_klass, name)` and `(k, name)` so the next descendant that walks past `k` finds it without re-parsing. This is an optimization, not a correctness change.
- **confidence:** speculative

## Improvements

### [S] [USER_FACING] Surface the declaring klass in field_entry_t so users can detect inherited vs own fields
- **rationale:** Currently `field_proxy` carries `signature`, `is_static`, and a raw byte pointer — there is no way for user code to ask "was this field actually declared on `B`, or inherited from `A`?". A user implementing a Java-style reflection wrapper (`isDeclaredHere()`) cannot do it without re-walking the chain themselves. Adding `klass* declaring_klass` to `field_entry_t` is one extra word and is already known by the loop (it's `k` at line 10297).
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:2510-2515, 10295-10304, plus the two callsites that build entries at 2928 and 3055
- **suggested_change:** Add `vmhook::hotspot::klass* declaring_klass{ nullptr };` to `field_entry_t`. Set it to `k` inside `find_field`'s loop (line 10301 path). Add a `field_proxy::declaring_klass()` accessor and an `is_inherited(target_klass)` helper that returns `declaring_klass != target_klass`. This matches the parity goal — `method_proxy` can already infer this from the `method*` it stores, but `field_proxy` cannot.

### [XS] [USER_FACING] field_proxy::is_protected / is_private / access_flags
- **rationale:** `field_entry_t` (vmhook.hpp:2510-2515) only stores the `is_static` bit but throws away the full 16-bit `access_flags` word both `find_field_in_stream` (read at line 2892 but only used for `0x0008`) and the JDK-≤17 path (read at line 3038 but only `& 0x0008` survives) already extract. Users debugging "is this field hidden behind a `private` shadowing problem?" or "why does this `final` field misbehave?" must hand-decode the constant-pool index themselves. Costing 2 extra bytes in `field_entry_t` and one variable rename gives `field_proxy::is_private()`, `is_protected()`, `is_public()`, `is_final()`, `is_volatile()` for free.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:2510-2515, 2918, 2928, 3038, 3050, 3055
- **suggested_change:** Add `std::uint16_t access_flags{ 0 };` to `field_entry_t`. Pass the full `access_flags` from both paths instead of just `is_static`. Add helpers on `field_proxy`:
  ```cpp
  auto is_static()    const noexcept -> bool { return (access_flags & 0x0008u) != 0u; }
  auto is_final()     const noexcept -> bool { return (access_flags & 0x0010u) != 0u; }
  auto is_protected() const noexcept -> bool { return (access_flags & 0x0004u) != 0u; }
  auto is_private()   const noexcept -> bool { return (access_flags & 0x0002u) != 0u; }
  auto is_public()    const noexcept -> bool { return (access_flags & 0x0001u) != 0u; }
  auto is_volatile()  const noexcept -> bool { return (access_flags & 0x0040u) != 0u; }
  ```

### [XS] [USER_FACING] Demote the "field not found in class hierarchy" log from error to warning or rate-limit it
- **rationale:** Many wrappers feature-detect optional fields by calling `string_klass->find_field("coder").has_value()` — see lines 10624, 14486. The current implementation logs an `error_tag` line *every* time the feature is absent, on a VM that legitimately doesn't declare it (JDK 8 lacks `coder`; JDK 7 lacks `hash`). The same applies to any user wrapper that probes for an optional field. Combined with the negative-cache bug above, a per-tick `obj->get_field("optional")` floods the log.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:10306
- **suggested_change:** Either downgrade `error_tag` to `warning_tag` (the call-sites that genuinely care wrap the result in `VMHOOK_LOG` themselves), or — preferred — emit the log only once per `(klass*, name)` by sharing the negative-cache bit suggested in the Bugs section.

### [S] [INTERNAL] Reduce string allocation on the cache-hit path
- **rationale:** `find_field` constructs `std::string name_str{ name }` at line 10276 *before* the cache check at 10278-10289, which allocates on every call regardless of hit or miss. For a small-string-optimization compiler this is free, but on libstdc++ <C++17 and on names longer than 15 chars (`protectedString`, `notStaticString`, JDK internal `enumConstantDirectory`, etc.) it always heap-allocates. Move the construction below the cache hit so the success path skips the allocation. Or use `std::string_view` as the inner map key with a `transparent` hasher; C++20 lets `unordered_map::find` take a `string_view` directly.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:10276, 10283
- **suggested_change:** Move `const std::string name_str{ name };` below the cache hit, or use heterogeneous lookup (`std::unordered_map<std::string, ..., string_hash, std::equal_to<>>`) so the `find()` at line 10283 takes `name` directly. The latter eliminates the allocation entirely on hot-path hits.

### [S] [INTERNAL] Pull the super-chain walk into a helper to share with method lookup
- **rationale:** Six near-identical `for (klass* k{ ... }; k != nullptr; k = k->get_super())` blocks live in this file at 10295, 12618, 12937, 12989, 13046, 13099, 13468, 13802. None has cycle protection. Hoisting the walk into a `vmhook::for_each_super(klass*, callback)` helper that *does* enforce the cap fixes all eight sites with one change and removes ~80 lines of duplication.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:10295, 12618, 12937, 12989, 13046, 13099, 13468, 13802
- **suggested_change:** Add a templated helper:
  ```cpp
  template<typename callback_t>
  inline auto for_each_super(vmhook::hotspot::klass* start, callback_t&& cb) -> void {
      std::int32_t depth{ 0 };
      for (auto* k{ start }; k != nullptr && depth < 256; k = k->get_super(), ++depth) {
          if (cb(k)) return;  // callback returns true to stop
      }
  }
  ```
  Then `find_field`'s loop becomes a four-line lambda body and every other walker collapses similarly.

## Tests

### [jvm_integration] [exists] poly_probe_inherited_field
- **description:** Pre-existing integration test that verifies B's `get_field("protectedInt")` returns 1337 from A.
- **asserts:** `get_protected_int() == 1337` (vmhook/src/example.cpp:2190-2192) and the cross-validated Java-side assertion via `polyJavaInheritedField` at 2216.
- **existing_file:** vmhook/src/example.cpp:2157-2220

### [jvm_integration] [new] poly_probe_field_shadowing
- **description:** Verifies that when a subclass declares a field with the same name as one already declared on a superclass, `find_field` returns the *child's* slot (shadowing). The current fixtures (A.java declares `field`; B.java does NOT declare `field`) cannot exercise this. Add a `shadowedInt` field to both A (value 1) and B (value 2) and assert B's reads see 2.
- **asserts:** `b_ptr->get_field("shadowedInt")->get() == 2` and `a_class{b_oop}.get_field("shadowedInt")->get() == 1` (separately, by constructing an a_class wrapper around the same B instance). Also assert that mutating B's slot via `b_ptr->get_field("shadowedInt")->set(99)` does NOT change A's slot read for an actual A instance.

### [jvm_integration] [new] poly_probe_inherited_field_offset_consistency
- **description:** Verifies that B and a sibling subclass C (both inheriting `protectedInt` from A) report the same field offset, i.e. the inherited field lives at the same byte position relative to the object header in both subclass instances. This guards against an inheritance regression where `find_field`'s cache or the super walk somehow returned a different offset for the same inherited slot.
- **asserts:** `b_ptr->get_field("protectedInt").value().raw_address() - reinterpret_cast<std::uint8_t*>(b_ptr->get_instance()) == c_ptr->get_field("protectedInt").value().raw_address() - reinterpret_cast<std::uint8_t*>(c_ptr->get_instance())`. Requires adding a `vmhook/C.java` extending A in the same way as B.

### [jvm_integration] [extend_existing] poly_probe_deep_hierarchy
- **description:** Currently the hierarchy under test is only two levels (A -> B). Add a third level (C extends B extends A) and assert that `c_ptr->get_field("protectedInt")` still resolves via the super walk after passing through B.
- **asserts:** `c_ptr->get_field("protectedInt")->get() == 1337` plus a check that `b_ptr->get_field("nonExistent").has_value() == false` to exercise the missing-field path through two ancestors.
- **existing_file:** vmhook/src/example.cpp:2157-2220

### [standalone_unit] [new] test_field_entry_caches_per_klass
- **description:** Sanity-check that `g_field_cache` is keyed by the requesting klass, not the declaring klass — a regression like "stash under declaring klass" without also adding a lookup pass would cause cache misses when querying via a subclass. Run two `find_field` calls (B then A) for `"protectedInt"` and assert both succeed.
- **asserts:** `vmhook::find_field(b_klass, "protectedInt").has_value()` and that a subsequent call from a second thread does not race-corrupt the cache (loop 1000 iterations across 4 threads, then check no entry has `offset == 0`).

### [standalone_unit] [new] test_find_field_negative_result_does_not_spin
- **description:** Calls `vmhook::find_field(b_klass, "doesNotExistOnBOrA")` ten thousand times and asserts the cumulative wall-clock is bounded (e.g. < 50ms). Today this scales linearly with iterations because the negative result isn't cached; the test will document the regression once a negative-cache fix lands.
- **asserts:** Total `chrono::steady_clock` duration of 10000 negative lookups < 50ms on the CI host.

### [standalone_unit] [new] test_find_field_super_chain_depth_capped
- **description:** Constructs a fake klass struct where `_super` points back to itself, calls `vmhook::find_field(fake, "anything")`, and asserts the call returns within a bounded number of iterations rather than hanging. Requires the bug fix above (depth cap) to pass.
- **asserts:** Call returns `nullopt` within 1 second; no infinite loop. Skips on builds where the test cannot fabricate a klass (e.g. unit tests that do not link against HotSpot internals).

## Parity Concerns
- `field_proxy` does not expose `is_static()` / `is_final()` / `is_protected()` / `is_private()` while `method_proxy` exposes its full signature via `name()`/`signature_text` — there is no analogous descriptor surface on the field side. The user-facing model is asymmetric: methods are introspectable, fields are opaque.
- `field_proxy` carries no `declaring_klass` accessor, but `method_proxy::resolve_compatible_method` (vmhook.hpp:12602-12644) implicitly knows the chain it walked. Users cannot ask "is this field inherited?" without writing their own super-walk, even though `find_field` already had `k` in scope.
- `vmhook::find_field` returns `std::optional<field_entry_t>` while the `object_base::get_field` wrappers return `std::optional<field_proxy>`. Documentation reads "@note Walk the superclass chain manually to locate inherited fields" on `klass::find_field` (line 2796-2797 and 2950-2951) which is correct for the per-klass helper but easily mis-read by users glancing at the header.
- The negative-result cache is absent for fields but `find_class` short-circuits via `klass_lookup_cache`. A user who probes for an optional method gets the same penalty (no method-name cache either), so the lack of negative caching is a library-wide pattern — fixing it for fields without methods would create the opposite asymmetry.
