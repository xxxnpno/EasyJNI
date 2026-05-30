# gametype_and_modules — Module lifecycle + gametype detection

> Files named in the focus exist under a deeper path than the prompt gave. Actual locations:
> `npnoqol/src/feature/impl/module/impl/gametype_manager.cpp`, `.../module/module.cpp`,
> `.../module/module_manager.cpp`, `.../module/impl/bedwars_module.cpp`,
> `.../module/impl/camera_no_clip.cpp`. The lifecycle glue lives one level up in
> `.../feature/feature_manager.cpp` and `.../module/hypixel_module.cpp`, which I also read.

## How NPNOQOL used it

NPNOQOL has a two-layer design that is the single most important fact for this angle:

- **vmhook detours are installed exactly once, globally, at startup — never per module.** Every
  hook is created in the `feature_manager` constructor:
  `npnoqol/src/feature/feature_manager.cpp:12-29` installs `sendChatMessage`, `printChatMessage`,
  `loadWorld`, `runTick`, `rayTraceBlocks`, `orientCamera`, and either `getDisplayName` (vanilla)
  or `bridge$getDisplayNameComponent` (Lunar) via `vmhook::hook<T>(name, &cb)`. Teardown is a
  single `vmhook::shutdown_hooks()` in `~feature_manager` (`feature_manager.cpp:34`).

- **Module enable/disable is NOT hook install/uninstall.** A module is a C++ object holding
  `std::atomic<bool> enabled{ true }` (`module.hpp:52`). Each global detour fans out to every
  registered module and each module self-gates via `should_fire()`
  (`module.cpp:55-59`: `return this->enabled.load()`). The `module_manager` loops the module
  vector and skips any module where `!mod->should_fire()`
  (`module_manager.cpp:55-63`, `:97-110`, `:122-130`, etc.). So "disable camera_no_clip" just
  flips an atomic; the `rayTraceBlocks` detour stays armed and keeps calling into the manager —
  `camera_no_clip::on_ray_trace_blocks` returns early on `!enabled` (`camera_no_clip.cpp:13-16`).

- **Gametype gating is layered on top of the same `should_fire()` mechanism.** `hypixel_module`
  overrides `should_fire()` to also require `is_active_gametype()`
  (`hypixel_module.cpp:512-516`), which compares `hypixel_gametype::current_gametype` /
  `hypixel_mode::current_mode` set by `gametype_manager::on_print_chat_message` parsing the
  `/locraw` JSON (`gametype_manager.cpp:99-115`). A `bedwars_module` therefore "comes online"
  the instant the gametype atomic changes — with no hook churn at all
  (`bedwars_module.cpp:9-12` registers gametype `BEDWARS`).

- **`return_value` is the per-detour control surface.** Modules call `return_value.cancel()`
  (via `feature_manager.cpp:45,54`), `return_value.set<T>(nullptr)`
  (`camera_no_clip.cpp:34`), `return_value.set(oop)` (`module_manager.cpp:193`), and read
  `return_value.caller()` → `caller_info` with `.valid()`, `.class_name`, `.method_name`
  (`camera_no_clip.cpp:18-32`). `camera_no_clip` actually relies on caller introspection to
  fire only when `rayTraceBlocks` was reached *from* `EntityRenderer.orientCamera`.

- **The only "dynamic" hook in the whole app is installed lazily but still permanently.** The
  tab-list hooks are deferred out of the constructor into the first live-world tick
  (`hypixel_module.cpp:551-561`) and resolved by JVM descriptor, then installed via
  `vmhook::hook<...>(resolved_name, signature, &detour)` inside `install_tab_hook()`
  (`hypixel_module.cpp:362-402`). They are guarded by a one-shot atomic
  (`tab_hook_attempted.exchange(true)`, `:556-560`) and never removed before
  `shutdown_hooks()`.

- **Fork-only API in active use for the lifecycle:** `vmhook::reanchor_classes_via_oop(anchor_oop, {...})`
  (`hypixel_module.cpp:58-66`) and `vmhook::get_class_methods<T>()` for descriptor-based method
  resolution (`hypixel_module.cpp:330-337`, `:347-348`). Both are present in the fork header and
  **absent upstream** (confirmed below).

Net: NPNOQOL deliberately avoided per-module hook install/uninstall. That choice is itself the
evidence — it routed around vmhook's hook-lifecycle weaknesses rather than using them.

## Pain points encountered (the real-world lessons)

### Per-module enable/disable cannot use real hook install/remove — so it doesn't
- **evidence:** `module.hpp:52` `std::atomic<bool> enabled{ true }` + `module.cpp:55-59`
  `should_fire()`; every detour is gated by an `enabled.load()` check
  (`camera_no_clip.cpp:13-16`, `module_manager.cpp:99-102`) while the underlying
  `vmhook::hook<>` from `feature_manager.cpp:12-29` stays installed for the whole process.
  Upstream *has* `scoped_hook`/`hook_handle` (`vmhook.hpp:8549`, class at `:6959`) — the obvious
  tool for "enable a feature = install its hook, disable = drop the handle" — and NPNOQOL pointedly
  does **not** use it for any module.
- **underlying vmhook gap:** `hook_handle::stop()` (upstream `vmhook.hpp:7012`) is documented as
  unsafe to call while the method might be executing: *"In-flight callbacks for the method being
  removed must have finished before stop() returns to avoid racing the detour's std::function
  destruction; in practice this means ensuring no Java thread is currently inside the hooked
  method"* (upstream `vmhook.hpp:6953-6957`). For a hook on a hot path like `rayTraceBlocks`,
  `runTick`, or `getDisplayName` — fired every frame from the render thread — the consumer can
  **never** prove that invariant from a UI toggle thread. So removal-based disable is a UAF
  waiting to happen, and NPNOQOL fell back to an `enabled` atomic that every detour must
  re-check. There is no safe per-hook *disarm-but-keep-installed* primitive.
- **severity:** major

### Re-enabling after disable would mean re-installing — and re-install is not idempotent
- **evidence:** the lazy tab hook is wrapped in a one-shot guard precisely so it is installed at
  most once: `hypixel_module.cpp:556-560`
  `static std::atomic<bool> tab_hook_attempted{ false }; if (!tab_hook_attempted.exchange(true)) { install_tab_hook(); }`.
- **underlying vmhook gap:** `vmhook::hook<T>()` has no "this method is already hooked with this
  detour — no-op" guard. Calling it twice on the same `Method*` pushes a second entry and re-runs
  the i2i patch path; the only collision handling that exists is for a *foreign* DLL's JMP
  (`vmhook.hpp:7879-7901` "already JMP'd by another hooker; chaining"), not for vmhook
  re-hooking itself. So any "disable then re-enable" cycle that re-installs would double-register
  the detour. The consumer cannot re-enable a removed hook safely, which is the other half of why
  modules never remove hooks — they only gate with an atomic. A first-class
  *disable/re-enable* on a handle would remove both the churn and the guard boilerplate.
- **severity:** major

### Install must be deferred to "a live world tick", with no library support for that
- **evidence:** `hypixel_module.cpp:551-561` — the tab hook is *not* installed in the
  constructor; the comment explains: *"Deferred to here (a tick with a live world) rather than the
  feature_manager ctor so GuiPlayerTabOverlay is loaded and the chat-based failure diagnostic has
  somewhere to print."* The descriptor-resolved name can only be found once the class is loaded
  (`resolve_method_by_signature<>` over `get_class_methods<>`, `hypixel_module.cpp:326-338`).
- **underlying vmhook gap:** vmhook hooks a method by name/descriptor *now*; if the class is not
  yet loaded, the hook silently does not take. There is no "install this hook the moment class C
  is first loaded" deferral. The fork has `on_class_loaded` / `watch_static_field` machinery
  hinted at in the header (upstream `vmhook.hpp:7018-7022`), but no turnkey
  *hook-on-load* that defers a `hook<T>()` until the target class appears. The consumer had to
  hand-roll the deferral by piggybacking on the per-frame `runTick` detour plus a one-shot guard.
- **severity:** major

### Installing a detour that may race a live class requires self-built classloader pinning
- **evidence:** `hypixel_module.cpp:44-70` `try_bootstrap_lunar_classloader()` calls
  `vmhook::reanchor_classes_via_oop(anchor_oop, { adventure_component, legacy_component_serializer,
  game_profile, property, property_map })` once, with the comment *"Lunar Client has its own copy
  of net.kyori.adventure.* loaded by its Genesis classloader, distinct from the JDK / system
  loader's copy. Pin the Adventure classes to the loader of a Lunar-side anchor OOP once, so the
  SDK wrappers transparently resolve through the correct loader."* This must run *before* the
  bridge detour's snapshot is built (`hypixel_module.cpp:572-580`).
- **underlying vmhook gap:** `reanchor_classes_via_oop` and its three primitives
  (`set_host_classloader_via_oop` `vmhook.hpp:10333`, `find_class_via_oop`,
  `override_class_lookup`) are **fork-only** — a grep of upstream
  `C:/repos/cpp/vmhook/vmhook/ext/vmhook/vmhook.hpp` finds none of them. Upstream's `find_class`
  resolves through whatever loader it first hits (ClassLoaderDataGraph walk), so on a multi-loader
  host (Lunar Genesis, Forge LaunchClassLoader) a hook/SDK wrapper silently binds to the **wrong
  copy** of a class and the detour either no-ops or throws `ClassCastException` when its return
  value is handed back to Java. Without the fork addition, the bridge-displayname module simply
  could not be installed correctly on Lunar.
- **severity:** blocker (on multi-classloader hosts)

### A live detour that hands an OOP back to Java needs that OOP pinned across GC — entirely consumer-side
- **evidence:** every "build once on the worker thread, return from the render-thread detour"
  path stores results in `vmhook::jni::global_ref` and returns `.oop()` from inside the detour:
  the nametag snapshot (`hypixel_module.cpp:653-675`, returned at `:742`
  `return it->second.oop()`), the tab-line list (`tab_snapshot::player_list`,
  `hypixel_module.cpp:265,482`, returned at `:319-321`), and the denick skin
  `NetworkPlayerInfo` (`skin_entry::network_player_info`, `:81-83`, returned at `:169,189`).
  The comment at `:88-90` spells out the constraint: *"Entries pin a NetworkPlayerInfo that live
  entities may still reference, so we never evict."*
- **underlying vmhook gap:** the consumer must understand and hand-manage GC-pinning lifetime for
  any object a detour returns. `return_value.set(oop)` takes a bare `oop_t`
  (`module_manager.cpp:193`, `camera_no_clip.cpp:34`) with no notion of *"keep this object alive
  until the Java caller has consumed it."* If the worker thread builds an object and the next GC
  relocates/frees it before the render detour returns it, the detour returns a dangling OOP.
  vmhook gives `global_ref` as a raw building block but no detour-scoped pinning, so every module
  re-implements the same pin-and-cache pattern. This is a hook-*lifecycle* gap (object lifetime
  vs. detour lifetime), distinct from the well-trodden field-read path.
- **severity:** major

### Detour callbacks may not be `noexcept`, and that constraint is undocumented at the call site
- **evidence:** `hypixel_module.cpp:270-274` carries an explicit warning:
  *"NB: detours are not noexcept — vmhook::detail::function_traits only specialises plain
  (non-noexcept) function pointers, matching every other hook callback."* The detour
  `apply_tab_line` is therefore declared without `noexcept` even though every other function in
  the file is `noexcept`.
- **underlying vmhook gap:** `detail::function_traits` (upstream `vmhook.hpp:7044-7049`) only has
  specialisations for non-`noexcept` function pointers, so passing a `noexcept` detour to
  `hook<T>()` / `scoped_hook<T>()` fails template substitution with an obscure error. This is a
  hook-*installation* footgun: the type the install API accepts is silently narrower than the
  user expects, and nothing in the signature or docs says so. The consumer learned it the hard
  way and left a tombstone comment.
- **severity:** annoyance

### Installing the hook can fail at runtime, but the only signal is a `bool` and an exception
- **evidence:** `install_tab_hook()` wraps each `vmhook::hook<>` in `try { ... } catch (...) {}`
  and inspects the returned `bool`: `hypixel_module.cpp:369-376` (`catch (...) {}` around the
  list hook) and `:388-397` (`installed = vmhook::hook<...>(...); catch(...) installed = false;`).
  On failure it falls back to a chat-printed diagnostic dumping every `String`-returning method
  of the class (`report_tab_name_failure`, `:342-359`).
- **underlying vmhook gap:** when a descriptor-resolved hook does not match (obfuscated/mixin
  builds), `hook<T>()` returns `false` or throws, with **no structured reason** (class not
  loaded? method not found? signature mismatch? already patched?). The consumer had to build its
  own "list all candidate methods and print them to chat" debugger because vmhook's own
  diagnostics are compiled out in release (`VMHOOK_DEBUG_LOGS=0`, noted at `:340-341`). Hook
  install needs a machine-readable failure reason, especially for the install-by-descriptor path
  that obfuscated targets force.
- **severity:** annoyance

## Upstream improvements this implies

### [M] Add `hook_handle::set_enabled(bool)` — disarm/re-arm without uninstalling
- **what to add upstream:** on `class hook_handle` (upstream `vmhook.hpp:6959`) add
  `auto set_enabled(bool) noexcept -> void;` and `auto enabled() const noexcept -> bool;`. When
  disabled, the entry stays in `g_hooked_methods`/`g_hooked_i2i_entries` and the trampoline still
  fires, but `common_detour` checks a per-method `std::atomic<bool> enabled` and, if false,
  resumes the original method immediately without invoking the user `std::function`. Re-enabling
  is a single relaxed store. No patch/unpatch, no `std::function` teardown, so it is safe to call
  from any thread while the method is executing.
- **why it removes the pain:** this is *exactly* the `enabled`-atomic + `should_fire()` gate
  NPNOQOL re-implemented by hand in `module.hpp:52` / `module.cpp:55-59` and re-checked in every
  single detour. Folding it into the handle means "disable a feature" is one library call that is
  GC-/thread-safe by construction, and "re-enable" works without the non-idempotent re-install
  problem. It removes both the per-module gating boilerplate and the one-shot install guards.

### [M] Make `vmhook::hook<T>()` / `scoped_hook<T>()` idempotent (return the existing handle)
- **what to add upstream:** before installing, look up `(Method*, detour-target)` in
  `g_hooked_methods`; if an identical hook already exists, return the existing `hook_handle` /
  `true` and do not push a second entry or re-patch. Add a `bool hook_exists<T>(name, sig)` query.
- **why it removes the pain:** kills the need for the `tab_hook_attempted.exchange(true)`
  one-shot guard (`hypixel_module.cpp:556-560`) and makes deferred/retried installs safe. Today a
  retry-on-next-tick install loop (which the descriptor-resolution code is one failure away from
  needing) would silently double-register.

### [M] First-class deferred install: `hook_on_class_loaded<T>(name, sig, cb)`
- **what to add upstream:** a variant of `hook` that, if the target class is not yet loaded,
  registers a class-load watcher (the fork already has `on_class_loaded` plumbing, upstream
  `vmhook.hpp:7018-7022`) and performs the actual `hook<T>()` the first time the class appears —
  returning a `hook_handle` that becomes `installed()` at that point. Resolve the method by
  descriptor at fire time, not at call time.
- **why it removes the pain:** removes the hand-rolled "defer to a live-world tick + one-shot
  guard + descriptor re-resolution" dance in `hypixel_module.cpp:362-402,551-561`. The library
  knows when a class loads; the consumer should not have to bolt deferral onto a per-frame tick
  detour.

### [M] Port `reanchor_classes_via_oop` (and its three primitives) upstream, generically
- **what to add upstream:** lift `reanchor_classes_via_oop(void* anchor_oop, initializer_list<const char*>)`,
  `set_host_classloader_via_oop`, `find_class_via_oop`, and `override_class_lookup` from the fork
  (`npnoqol/.../vmhook.hpp:10333-10416`) into upstream verbatim — they are written in fully
  HotSpot-generic terms ("host classloader", "anchor OOP"), with zero Minecraft/Lunar specifics in
  the API.
- **why it removes the pain:** any multi-classloader host (Forge LaunchClassLoader, Lunar Genesis,
  app servers, OSGi, agent-isolated loaders) currently makes upstream `find_class` bind to the
  wrong class copy, so hooks/SDK wrappers no-op or throw `ClassCastException`. This is a hard
  blocker the fork already solved.
- **fork reference:** `vmhook::reanchor_classes_via_oop`, `vmhook::set_host_classloader_via_oop`,
  `vmhook::find_class_via_oop`, `vmhook::override_class_lookup`
  (`npnoqol/npnoqol/ext/vmhook/vmhook.hpp:10333-10416`).

### [S] Document the non-`noexcept` detour constraint and/or accept `noexcept` detours
- **what to add upstream:** add `function_traits` specialisations for
  `return_type(*)(args...) noexcept` (and the `std::function`/member variants), so a `noexcept`
  detour can be passed to `hook`/`scoped_hook`. Failing that, put a one-line constraint in the
  `hook<T>()` doc-comment.
- **why it removes the pain:** removes the `// NB: detours are not noexcept` footgun
  (`hypixel_module.cpp:270-274`) and lets consumers keep their callbacks `noexcept` for the usual
  optimisation/clarity reasons.

### [S] Structured hook-install failure reason
- **what to add upstream:** a `hook_result` (or `expected<hook_handle, hook_error>`) with a reason
  enum: `class_not_loaded`, `method_not_found`, `signature_mismatch`, `already_hooked`,
  `patch_failed`. Keep the `bool` overload for source compat.
- **why it removes the pain:** the descriptor-resolution path
  (`hypixel_module.cpp:362-402`) currently can only see "it returned false / it threw", forcing
  the chat-printed "dump every String method" debugger (`report_tab_name_failure`,
  `:342-359`). A structured reason lets the consumer branch (retry next tick vs. give up vs. try
  the other descriptor) without a release-only diagnostic.

## New test scenarios (mostly jvm_integration — exercise the real usage)

### [jvm_integration] test_disable_reenable_hot_hook_no_uaf_no_double_register
- **scenario:** install a hook on a method, then from a *second thread* repeatedly call the new
  `set_enabled(false)`/`set_enabled(true)` while the first thread hammers the hooked method in a
  tight loop (the render-thread-vs-UI-toggle situation NPNOQOL faced for camera_no_clip /
  getDisplayName). Mirror the NPNOQOL pattern: a per-frame method gated on/off at runtime.
- **asserts:** no crash/UAF; while disabled the original method result is observed (detour did not
  run); while enabled the detour result is observed; `g_hooked_methods` size is constant across
  thousands of toggles (no growth, proving disable≠uninstall and re-enable≠re-install).

### [jvm_integration] test_double_hook_same_method_is_idempotent
- **scenario:** call `vmhook::hook<T>("m","()V",&cb)` twice on the same method (the situation the
  `tab_hook_attempted` guard exists to prevent). Then call the method once.
- **asserts:** `hook_exists<T>(...)` is true after the first call; the second call returns the
  same logical hook and does **not** add a second `g_hooked_i2i_entries`/`g_hooked_methods`
  entry; the detour fires exactly once per invocation (not twice); a single
  `shutdown_hooks()`/handle teardown fully restores the method.

### [jvm_integration] test_hook_on_class_loaded_defers_until_class_appears
- **scenario:** request `hook_on_class_loaded<T>("net/test/LazilyLoaded","m","()I",&cb)` before
  the class is referenced; assert the handle reports not-yet-installed; then trigger loading the
  class and call the method (reproduces `GuiPlayerTabOverlay` being absent at ctor time,
  `hypixel_module.cpp:551-561`).
- **asserts:** before load: `installed()==false`, original behaviour intact; after first load:
  `installed()==true` and the detour fires; resolution-by-descriptor picked the right method.

### [jvm_integration] test_reanchor_classes_via_oop_binds_host_loader_copy
- **scenario:** load two copies of the same class name under two different classloaders (a
  child/isolated loader holding the "host" copy, plus the system loader). Take an anchor OOP whose
  class lives in the child loader, call `reanchor_classes_via_oop(anchor, {"net/test/Dup"})`, then
  resolve the class through the normal SDK/find_class path (reproduces Lunar Genesis vs JDK copy,
  `hypixel_module.cpp:44-70`).
- **asserts:** after reanchor, `find_class("net/test/Dup")` returns the **child-loader** Klass*,
  not the system one; a method hooked on the reanchored class fires; an object built against it
  and handed back via `return_value.set(oop)` does not trigger a ClassCastException in Java.

### [jvm_integration] test_detour_returned_oop_survives_gc
- **scenario:** in a detour, return an OOP that was built on a *different* thread and stashed in a
  `global_ref` (the nametag/tab/skin pattern, `hypixel_module.cpp:653-675,482,169`). Force several
  GCs between building the object and the detour returning it.
- **asserts:** the returned OOP is still valid Java after GC (object pinned by the `global_ref`);
  dropping the `global_ref` and forcing GC then reclaims it. Validates the object-lifetime-vs-
  detour-lifetime contract a returning detour depends on.

### [unit] test_noexcept_detour_compiles_or_is_clearly_rejected
- **scenario:** attempt to pass a `noexcept` function-pointer detour to `hook`/`scoped_hook`
  (the case behind `// NB: detours are not noexcept`, `hypixel_module.cpp:270-274`).
- **asserts:** either it compiles and fires identically to the non-`noexcept` version (after
  adding the `function_traits` specialisation), or a `static_assert` produces a clear diagnostic
  message naming the constraint — not an inscrutable "no matching overload".

## Verdict: should this be ported upstream?

**Yes — port the general mechanisms, not the Minecraft specifics.** This angle surfaces a clear
thesis: NPNOQOL never once used vmhook's per-module hook install/uninstall, even though
`scoped_hook`/`hook_handle` exist. It instead installs every detour once and gates with an
`enabled` atomic, because (a) `hook_handle::stop()` is documented-unsafe to call while a hot
method may be executing, (b) re-`hook()` is not idempotent, and (c) there is no disarm-but-keep
primitive. That is upstream telling you, through a real consumer's avoidance behaviour, that the
hook-*lifecycle* API is missing its most-wanted operation: **safe runtime enable/disable of an
installed hook.** Adding `hook_handle::set_enabled()` plus hook idempotency is general,
HotSpot-agnostic, and would let the *next* consumer use the library as intended instead of
rebuilding the gating layer.

The classloader-pinning family (`reanchor_classes_via_oop` et al.) is an unambiguous port: it is
already written in host-generic language, it is a hard blocker on any multi-loader JVM (Forge,
OSGi, app servers — not just Lunar), and the fork has battle-tested it. The deferred
`hook_on_class_loaded`, the non-`noexcept` detour fix, and structured install-failure reasons are
smaller quality-of-life ports with no Minecraft coupling. The Minecraft-specific parts — the
gametype/locraw state machine, NetworkPlayerInfo skin swapping, the BELOW_NAME scoreboard slot —
stay in the consumer; none of that belongs upstream.
