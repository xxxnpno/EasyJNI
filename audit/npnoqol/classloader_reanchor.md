# classloader_reanchor — Custom classloader visibility / class re-anchoring

## How NPNOQOL used it

NPNOQOL runs as an injected native thread inside a Minecraft JVM that may use a
custom classloader chain (Lunar Client's "Genesis" loader, Forge's
`LaunchClassLoader`). The fork added a 4-function re-anchoring stack on top of
upstream's auto-capture machinery, and NPNOQOL drives it from exactly one place.

- **Single consumer call site:** `npnoqol/src/feature/impl/module/hypixel_module.cpp:44-70`
  wraps `vmhook::reanchor_classes_via_oop(anchor_oop, {...})` in a once-only
  `try_bootstrap_lunar_classloader(anchor_oop)`. The anchor OOP is the local
  player instance (`local->get_instance()`), passed in from the worker tick at
  `hypixel_module.cpp:574-578`:
  ```cpp
  if (const std::unique_ptr<sdk::entity_player_sp> local{ feature::minecraft->get_the_player() };
      local && local->get_instance())
  {
      try_bootstrap_lunar_classloader(local->get_instance());
  }
  serializer = sdk::legacy_component_serializer::legacy_section();
  ```
- **What it pins** (`hypixel_module.cpp:58-66`): the Adventure text classes
  (`adventure_component`, `legacy_component_serializer`) **plus** the authlib
  classes (`game_profile`, `property`, `property_map`) — because Lunar ships its
  own duplicate copies of `net.kyori.adventure.*` and `com.mojang.authlib.*` in
  its Genesis loader, distinct from the JDK/system loader copies.
- **Why those specific classes:** the comment at `hypixel_module.cpp:40-43` and
  `61-62` spells it out — the SDK wrappers (`serializer->deserialize(...)` at
  `:659`, `vmhook::make_unique<sdk::game_profile>(...)` at `:122`,
  `make_unique<sdk::network_player_info>(profile)` at `:143`) construct and call
  into these Java types via JNI, and the *result objects* are handed straight
  back to host code (`client_player.set_player_info(info)` at `:641`). If the
  wrapper resolved the wrong loader's copy, the host's
  `NetworkPlayerInfo`/`GameProfile` field stores throw `ClassCastException`.
- **The fork stack it leans on** (fork header
  `npnoqol/ext/vmhook/vmhook.hpp`):
  `reanchor_classes_via_oop` (line 10387) composes
  `set_host_classloader_via_oop` (10333) +
  `find_class_via_oop` (10211) +
  `override_class_lookup` (10305). All four are **absent upstream** —
  upstream (`vmhook/ext/vmhook/vmhook.hpp`) only has the private auto-capture
  (`capture_host_classloader_klass` @ 6163,
  `inherit_host_context_classloader_for_current_thread` @ 9281) and a name-only
  `find_class` (@ 6202).
- **Defensive avoidance even after re-anchoring:** NPNOQOL still refuses to do
  class-dispatching JNI on the render thread — `hypixel_module.cpp:256-258`:
  > "the render-thread detours only do an atomic load + map lookup / pinned-oop
  > return — no class-dispatching JNI (which, like the nametag bridge, would risk
  > the Lunar multi-classloader ClassCastException)."
  Re-anchoring made worker-thread JNI *correct*, but the fork author did not
  trust it enough to let JNI dispatch happen on the render thread at all.

## Pain points encountered (the real-world lessons)

### Auto-captured host classloader is non-deterministic and lands on the WRONG loader
- **evidence:** fork header `npnoqol/ext/vmhook/vmhook.hpp:10312-10328`, the
  doc-comment on `set_host_classloader_via_oop`:
  > "vmhook auto-captures the first non-bootstrap classloader it sees during
  > start-up, but in Lunar Client / Forge setups that capture often lands on the
  > wrong one (system / app loader instead of the host's custom loader). When two
  > loaders host duplicate copies of a class ... every JNI dispatch from worker
  > threads ends up on the wrong copy and produces ClassCastException when the
  > result is handed back to host code."
- **underlying vmhook gap:** Upstream `capture_host_classloader_klass`
  (`vmhook/ext/vmhook/vmhook.hpp:6239-6265`) publishes the **first** non-bootstrap
  klass via a one-shot CAS and never revises it. "First non-bootstrap loader seen
  during startup" is whatever class `find_class` happened to resolve first — for
  an injected library that is almost never the host's custom loader. Once latched,
  the CAS guard (`if (host_classloader_klass.load(...)) return;` @ 6243-6247)
  makes it **permanent and uncorrectable** through any public API. There is no
  "re-point the captured loader" entry point upstream.
- **severity:** blocker

### `find_class` cannot pick a loader when duplicate copies of a class exist
- **evidence:** upstream `find_class` resolves via
  `graph.find_klass(class_name)` (`vmhook/ext/vmhook/vmhook.hpp:6254`), which
  returns the **first** `Klass*` matching by *name* across the entire
  `ClassLoaderDataGraph`. The fork needed an OOP-anchored alternative,
  `find_class_via_oop` (fork @ 10211), which walks
  `anchor_oop -> getClass() -> getClassLoader() -> loadClass(name)` to force the
  *specific* loader's copy.
- **underlying vmhook gap:** `class_loader_data_graph::find_klass` keys purely on
  the internal name. With two loaded copies of `net/kyori/adventure/text/Component`
  the winner is graph-iteration-order-dependent and unspecified — and there is no
  parameter, no predicate, no loader filter to disambiguate. `find_class("…/Component")`
  is simply the wrong abstraction when a class name is not unique in the process.
- **severity:** blocker

### The fallback loader chain is hardcoded to Forge/LaunchWrapper, not Lunar
- **evidence:** upstream `jni_find_class_with_context_loader`
  (`vmhook/ext/vmhook/vmhook.hpp:9139-9155`) tries thread-context loader, then
  system loader, then specifically:
  ```cpp
  void* const launch_class{ ... jni_find_class("net/minecraft/launchwrapper/Launch") };
  ... jni_get_static_field_id(launch_class, "classLoader",
        "Lnet/minecraft/launchwrapper/LaunchClassLoader;") ...
  ```
  Lunar's Genesis loader is not reachable by any of these three paths, so the
  fork bypassed the whole fallback with an explicit anchor OOP instead.
- **underlying vmhook gap:** upstream bakes in one launcher's class topology
  (`net/minecraft/launchwrapper/Launch.classLoader`). Anything else — Lunar,
  modern Fabric/Quilt, a bespoke loader — falls through to "class not loaded
  anywhere" (the warning at 9143-9147) even though the class *is* loaded, just
  under a loader upstream doesn't know how to reach. The "general" mechanism
  (resolve through a loader you can name with an object reference) was missing,
  so a Minecraft-launcher-specific shortcut had to stand in for it.
- **severity:** major

### No public way to override / correct a cached klass resolution
- **evidence:** the fork added `override_class_lookup` (fork @ 10305):
  ```cpp
  std::lock_guard<std::mutex> lock{ vmhook::klass_lookup_cache_mutex };
  vmhook::klass_lookup_cache[std::string{ class_name }] = k;
  ```
  This is the *only* way the fork can make every downstream SDK wrapper
  (which call plain `find_class`) pick up the correct loader's copy after
  `find_class` already cached the wrong one.
- **underlying vmhook gap:** upstream owns `klass_lookup_cache` /
  `klass_lookup_cache_mutex` (`vmhook/ext/vmhook/vmhook.hpp:6185-6186`) and
  populates it with `insert` ("so a racing thread … doesn't overwrite their
  entry", 6267-6269) — i.e. **first write wins, deliberately**. There is no
  supported overwrite/eviction/seed API. Once a wrong copy is cached, every SDK
  wrapper in the program is permanently poisoned with no escape hatch. The fork
  had to reach into upstream's private cache symbol directly.
- **severity:** major

### Re-anchoring is a fragile multi-step dance the caller must orchestrate and retry
- **evidence:** the fork's `reanchor_classes_via_oop` (fork @ 10387-10416)
  exists *only* to wrap the 3-step "set loader → resolve-per-class → override
  cache" sequence the comment at 10371-10378 documents, and it returns
  `bool all_resolved` so callers can retry. NPNOQOL then has to build *its own*
  once-latch around even that (`hypixel_module.cpp:47-69`):
  ```cpp
  static std::atomic<bool> done{ false };
  if (done.load(std::memory_order_acquire)) { return; }
  ...
  if (vmhook::reanchor_classes_via_oop(anchor_oop, { ... }))
      done.store(true, std::memory_order_release);
  ```
- **underlying vmhook gap:** the timing is implicit and unspecified — the anchor
  OOP (local player) does not exist until a world is loaded, so the caller must
  poll every tick until `reanchor_classes_via_oop` returns `true`, and must
  remember to do it exactly once. Upstream offers no "anchor your class
  resolution to this object, retrying until ready" lifecycle, so each consumer
  re-implements the same latch + retry + "is the anchor live yet" guard.
- **severity:** annoyance

### `find_class_via_oop` re-resolves the loader on every call (no per-loader caching)
- **evidence:** `find_class_via_oop` (fork @ 10211-10292) does a full JNI chain
  (`GetObjectClass` → `getClassLoader` → `loadClass`) **per class name, every
  call**; `reanchor_classes_via_oop` loops it over the whole class list at
  10399-10409. The fork mitigates by caching the *result* via
  `override_class_lookup`, but the loader-resolution itself is uncached.
- **underlying vmhook gap:** there is no notion of a reusable "resolved host
  loader handle" upstream. Each lookup re-walks `oop → Class → ClassLoader`. For
  the 5 classes NPNOQOL pins that is tolerable, but the general primitive should
  let you resolve a loader once and then `loadClass` many names through it.
- **severity:** annoyance

## Upstream improvements this implies

### [M] Port `find_class_via_oop` — resolve a class through a named anchor object's loader
- **what to add upstream:** public
  `vmhook::hotspot::klass* find_class_via_oop(void* anchor_oop, std::string_view internal_name) noexcept`
  that walks `anchor_oop -> getClass() -> getClassLoader() -> loadClass(name)`
  (exactly the fork body at fork @ 10211-10292, which is already 100% HotSpot/JNI
  generic — no Minecraft types). This is the general answer to "the class name is
  not unique; resolve the copy visible from *this* object."
- **why it removes the pain:** kills the "first-by-name wins" non-determinism of
  `graph.find_klass` for any process with duplicate-name classes under multiple
  loaders (OSGi, app servers, plugin hosts, modded games — not just Minecraft).
- **fork reference:** `vmhook::find_class_via_oop`.

### [S] Port `override_class_lookup` — supported way to seed/correct the klass cache
- **what to add upstream:**
  `void vmhook::override_class_lookup(std::string_view internal_name, vmhook::hotspot::klass* k) noexcept`
  that does the guarded `klass_lookup_cache[name] = k` (fork @ 10305-10310).
  Pair it with an `evict_class_lookup(name)` for completeness.
- **why it removes the pain:** turns the private, first-write-wins
  `klass_lookup_cache` into something a caller can correct after a misresolve,
  so every downstream `find_class`-based wrapper transparently follows the
  override instead of being permanently poisoned. Stops forks from reaching into
  upstream's internal cache symbol.
- **fork reference:** `vmhook::override_class_lookup`.

### [M] Port `set_host_classloader_via_oop` — re-point the captured host loader
- **what to add upstream:**
  `void vmhook::set_host_classloader_via_oop(void* anchor_oop) noexcept`
  that force-stores `host_classloader_klass` (bypassing the one-shot CAS) and
  re-runs `inherit_host_context_classloader_for_current_thread()` (fork @
  10333-10362). Also widen `capture_host_classloader_klass` with an optional
  "force" path so the correction is first-class, not a backdoor.
- **why it removes the pain:** the auto-capture is best-effort and *documented by
  the fork itself* as "often lands on the wrong one." Upstream needs an explicit
  "the host loader is *this* object's loader" override, because no injected
  library can reliably make its first observed loader be the host's custom loader.
- **fork reference:** `vmhook::set_host_classloader_via_oop`.

### [S] Port `reanchor_classes_via_oop` — one-shot convenience composing the three primitives
- **what to add upstream:**
  `bool vmhook::reanchor_classes_via_oop(void* anchor_oop, std::initializer_list<const char*> names) noexcept`
  exactly as fork @ 10387-10416: set host loader, then per name
  `find_class_via_oop` + `override_class_lookup`, returning `true` only if **all**
  names resolved (so callers can retry on a later tick). Keep the initializer-list
  shape — it reads cleanly at the call site (`hypixel_module.cpp:58-66`).
- **why it removes the pain:** the 90%-case ergonomic wrapper. Without it every
  consumer hand-rolls the set→resolve→override loop.
- **fork reference:** `vmhook::reanchor_classes_via_oop`.

### [M] Generalize the `find_class` fallback chain beyond LaunchWrapper
- **what to add upstream:** make `jni_find_class_with_context_loader`'s loader
  list extensible instead of hardcoding
  `net/minecraft/launchwrapper/Launch.classLoader`
  (`vmhook/ext/vmhook/vmhook.hpp:9139-9155`). E.g. a registration hook
  `vmhook::register_fallback_classloader_provider(std::function<void*()>)` and/or
  an optional "anchor OOP of last resort." The Minecraft `Launch` probe becomes
  one *registered* provider, not a baked-in assumption.
- **why it removes the pain:** upstream is meant to be HotSpot-generic, yet the
  fallback path hardwires one Minecraft launcher's static field. Lunar, Fabric,
  Quilt, and non-game hosts all fall through to "not loaded anywhere" today.
- **fork reference:** none directly — the fork sidestepped the fallback entirely
  with anchor OOPs; this generalizes the upstream code path the fork couldn't use.

### [S] Cache a reusable resolved-loader handle for repeated `*_via_oop` resolves
- **what to add upstream:** an internal `loader_oop -> jweak` (or
  loader-klass-keyed) handle so `find_class_via_oop` / `reanchor_classes_via_oop`
  resolve the loader once and `loadClass` many names through it, instead of
  re-walking `oop → Class → ClassLoader` per name (fork @ 10211-10292, looped at
  10399-10409).
- **why it removes the pain:** turns N-class re-anchoring from N full loader
  walks into one walk + N `loadClass` calls; also lets callers re-anchor late-loaded
  classes cheaply without re-deriving the loader.
- **fork reference:** none — this is an efficiency improvement over the fork.

## New test scenarios (mostly jvm_integration — exercise the real usage)

### [jvm_integration] duplicate_class_two_loaders_resolves_via_anchor
- **scenario:** Load class `Dup` (a trivial class with one static field) under
  TWO custom `URLClassLoader`s, A and B, in the test JVM. Create an instance
  `objB` under loader B. Assert `find_class("Dup")` (name-only) may return
  *either* copy, but `find_class_via_oop(decode(objB), "Dup")` returns the
  Klass* whose `ClassLoaderData._class_loader` == loader B.
- **asserts:** `find_class_via_oop` Klass != the loader-A Klass; equals the
  loader-B Klass; `klass_to_class_loader_oop(result)` identity-equals loader B.

### [jvm_integration] override_class_lookup_redirects_find_class
- **scenario:** After the above, call
  `override_class_lookup("Dup", klass_from_loader_B)`, then call plain
  `find_class("Dup")`.
- **asserts:** `find_class("Dup")` now returns the loader-B Klass deterministically
  (the override wins over the graph walk and over any previously cached value);
  a follow-up `evict_class_lookup("Dup")` restores the name-only behavior.

### [jvm_integration] reanchor_then_construct_no_classcast
- **scenario:** Reproduce the NPNOQOL pattern minimally: two loaders each holding
  a copy of a `Holder` class with a method `void take(Payload p)`. Build a
  `Payload` via the *wrong* loader and confirm `Holder.take` throws
  `ClassCastException`; then `reanchor_classes_via_oop(anchorFromHolderLoader,
  {"Payload"})`, rebuild `Payload`, and confirm `take` succeeds.
- **asserts:** pre-reanchor call raises a pending JNI exception (CCE); post-reanchor
  call returns cleanly; `reanchor_classes_via_oop` returned `true`.

### [jvm_integration] reanchor_returns_false_until_anchor_live
- **scenario:** Call `reanchor_classes_via_oop(nullptr, {...})` and with an
  anchor whose class loads only *some* of the requested names.
- **asserts:** returns `false` for the null anchor and for the partial case
  (mirrors NPNOQOL's retry-until-true latch at `hypixel_module.cpp:58-69`);
  returns `true` only when every name resolves.

### [unit] set_host_classloader_via_oop_overrides_auto_capture
- **scenario:** Force an initial wrong capture via
  `capture_host_classloader_klass(bootstrapAdjacentKlass)`, then call
  `set_host_classloader_via_oop(objFromCustomLoader)`.
- **asserts:** `host_classloader_klass` now equals the custom-loader klass (the
  CAS-once guard is bypassed); `klass_to_class_loader_oop(host_classloader_klass)`
  is the custom loader, not the first-captured one.

### [jvm_integration] attached_thread_inherits_corrected_loader
- **scenario:** Spin up a fresh native thread (so it gets the platform loader as
  context loader), run `set_host_classloader_via_oop(objFromCustomLoader)` on it,
  then read `Thread.currentThread().getContextClassLoader()` back via JNI.
- **asserts:** the thread's context classloader identity-equals the custom loader
  reachable from the anchor — i.e. the correction propagates to the thread, not
  just to the cache.

## Verdict: should this be ported upstream?

**Yes — port the general mechanism, drop the Minecraft specifics.** The four
fork functions are pure HotSpot + JNI (`oop → getClass → getClassLoader →
loadClass`, plus a guarded write into upstream's own `klass_lookup_cache` and a
force-store of upstream's own `host_classloader_klass`); they contain zero
Minecraft types and address a genuinely general HotSpot problem that upstream's
own code half-acknowledges. Upstream already concedes the limitation — its
auto-capture comment (`vmhook.hpp:6271-6276`) and the fork's
`set_host_classloader_via_oop` doc (fork @ 10312-10324) both describe the same
"first loader we see is the wrong loader" failure — but upstream ships only the
*automatic, uncorrectable, first-write-wins* half: `find_class` keyed on a
possibly-ambiguous name, a one-shot CAS capture, and an insert-only cache. The
moment a process has two copies of a class under different loaders (modded games,
OSGi, app servers, any plugin host), that automatic half silently resolves the
wrong copy with no escape hatch, and the only symptom is a `ClassCastException`
thrown deep in host code when the result is handed back. Porting
`find_class_via_oop` + `override_class_lookup` + `set_host_classloader_via_oop` +
the `reanchor_classes_via_oop` convenience gives users the explicit, *corrective*
half: "resolve / pin this class through the loader of an object I already hold."
What must NOT be ported is the Minecraft-flavoured shortcut already in upstream —
the hardcoded `net/minecraft/launchwrapper/Launch.classLoader` fallback
(`vmhook.hpp:9139-9151`) should be *generalized* into a registerable fallback
provider, with the anchor-OOP API as the real answer, so HotSpot-generic vmhook
stops baking one launcher's class topology into its core resolution path.
