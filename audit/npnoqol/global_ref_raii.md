# global_ref_raii — JNI global-ref RAII (vmhook::jni::global_ref)

> Note on focus paths: the focus referenced `hypixel_module.cpp` lines 80/113/149/176/265/482/653/662/670 at a top-level `src/` path. The real file lives at
> `C:/repos/cpp/npnoqol/npnoqol/src/feature/impl/module/hypixel_module.cpp` and its header
> `.../hypixel_module.hpp`. The cited line numbers match the real file exactly (verified below). `global_ref` is used in those two files and nowhere else in `npnoqol/src` (14 occurrences total) — it is the single concentrated GC-stability consumer.

## How NPNOQOL used it

The whole `hypixel_module` design is "do the heavy Java work on the 25 ms worker tick, stash the resulting Java objects, and let the render-thread detours return them with a bare lookup." That stash has to **outlive the tick that built it** and be touched again on a *different* thread (the render thread) one or more GCs later. A raw `vmhook::oop_t` (a decoded heap address) cannot do that: HotSpot relocates objects on GC, so an address captured this tick is a dangling/stale pointer by the time the detour reads it. NPNOQOL solved this by pinning every cross-boundary OOP in `vmhook::jni::global_ref`.

Concrete fork APIs consumed (all ADDED by the fork; none exist upstream):
- `vmhook::jni::global_ref` — move-only RAII pin. ctor `global_ref(void* raw_oop)` → `NewGlobalRef`; dtor → `DeleteGlobalRef`; `.oop()` re-derives the *current* (post-relocation) heap address; `explicit operator bool()`.
- Underlying primitives `vmhook::detail::jni_new_global_ref` (slot 21) / `jni_delete_global_ref` (slot 22), surfaced as `vmhook::jni::new_global_ref` / `delete_global_ref`, plus `vmhook::jni::oop_handle` (the "fake JNI handle" wrapper, fork header line 10473) that converts a raw decoded OOP into something `NewGlobalRef` will accept.

Call patterns in the consumer:

1. **Member-stored pin in a long-lived registry** — `skin_entry::network_player_info` is a `vmhook::jni::global_ref` (`hypixel_module.cpp:80`). The skin registry is a `static std::unordered_map` that lives for the process; the pinned `NetworkPlayerInfo` it owns is swapped onto live entities ticks later.
2. **Factory returns a pin by value (move-only)** — `build_network_player_info(...) -> vmhook::jni::global_ref` constructs a fresh `NetworkPlayerInfo` via `vmhook::make_unique` and returns it pinned: `return vmhook::jni::global_ref{ info->get_instance() };` (`hypixel_module.cpp:113` signature, `:149` return).
3. **Build-outside-lock, then move-into-map** — `resolve_skin_network_player_info` builds the pin off the mutex, then `entry.network_player_info = std::move(built);` and hands back the *live* address with `.oop()` (`hypixel_module.cpp:176` build, `:169`/`:189` read via `.oop()`).
4. **Pinned snapshot value in an atomically-published struct** — `tab_snapshot::player_list` is a `global_ref` holding a de-duplicated `ArrayList` (`hypixel_module.cpp:265`); built on the worker (`fresh->player_list = vmhook::jni::global_ref{ list->get_instance() };`, `:482`) and read by the render-thread detour with `snapshot->player_list.oop()` (`:321`).
5. **Pinned nametag components keyed by entity id** — `using nametag_snapshot = std::unordered_map<std::int32_t, vmhook::jni::global_ref>;` (`hypixel_module.hpp:55`). Components are built per-player (Adventure `Component` on Lunar `:662`, `ChatComponentText` otherwise `:670`) into a `global_ref` (`:653`) and returned to the render thread by `on_bridge_get_display_name` via `it->second.oop()` (`:742`).

## Pain points encountered (the real-world lessons)

### A bare oop_t saved past the detour is a dangling pointer after the next GC
- **evidence:** `hypixel_module.cpp:73-83`, the `skin_entry` struct stores `vmhook::jni::global_ref network_player_info{};` with the comment *"build a NetworkPlayerInfo from those textures (once, pinned against GC) and swap it onto the entity"*. The factory at `:149` ends `return vmhook::jni::global_ref{ info->get_instance() };` — the value `make_unique` produced is immediately promoted to a global ref, because the `std::unique_ptr<sdk::network_player_info>` wrapper only holds a raw decoded OOP that dies with the tick.
- **underlying vmhook gap:** Upstream is explicit that this is unsupported: `oop_type_t` is *"NOT a JNI global reference - no GC handles are created and the pointer remains valid only for the duration of the hook"* (upstream `vmhook.hpp:12828`), and `object_base`'s note repeats *"valid for the duration of the hook invocation only"* (`:12882`). Upstream gives you `make_unique`, `register_class`, `method_proxy::call`, and `oop_t`, but **nothing to keep an OOP alive past the call** — the moment your detour returns or you cross a tick, anything you saved is GC-unsafe. NPNOQOL had to build the missing lifetime primitive itself.
- **severity:** blocker — without a pin, every "compute on worker tick, consume on render thread" pattern (the core architecture of this module) is a use-after-relocation.

### Re-reading a pinned object must return the *relocated* address, not the address you pinned at
- **evidence:** `hypixel_module.cpp:169` returns `it->second.network_player_info.oop()` and `:321` returns `snapshot->player_list.oop()`. These are read on later ticks / the render thread. The fork's `oop()` is implemented as `return this->handle_ ? *reinterpret_cast<void**>(this->handle_) : nullptr;` (fork header `:10648`) — it dereferences the JNI handle slot every call, so it observes the address HotSpot rewrote into the handle after a relocating GC.
- **underlying vmhook gap:** Upstream has no concept of "the live address of a pinned object." Even the raw building block it ships, `jni_oop_handle` (`vmhook.hpp:8773`: `handle_storage = oop; return &handle_storage;`), is a *throwaway stack* handle — its storage is gone the instant the call returns, so it can't be re-dereferenced later. The fork's `global_ref` is precisely the piece that gives the handle a stable home and a `.oop()` accessor that survives relocation. Upstream ships the half (the fake-handle trick) that is useless without the other half (durable storage + accessor).
- **severity:** major — a naive cache that stored the pinned address as a plain `void*` would still hand the render thread a stale pointer after GC; the indirection through `.oop()` is load-bearing.

### Cross-thread handoff needs an owning, move-only handle, not a copyable address
- **evidence:** `hypixel_module.cpp:176-189` builds `vmhook::jni::global_ref built{ ... }` outside the mutex, then `entry.network_player_info = std::move(built);` inside it; `:675` `fresh->emplace(player->get_entity_id(), std::move(component));`; `:186`/`:483`/`:681` all move pins into atomically-published `shared_ptr<const ...>` snapshots. The structures are produced on the worker thread and consumed on the render thread.
- **underlying vmhook gap:** A correct cross-thread Java-object handoff requires exactly-once ownership of the `DeleteGlobalRef` (double-free of a global ref corrupts the JNI handle table; a leak pins the heap forever). Upstream offers no ownership type at all — a developer using plain `oop_t` in a `shared_ptr` snapshot would either never release (heap leak) or release on the wrong thread. The fork encodes the ownership in a move-only type so the snapshot's destruction releases the pin once. The move semantics also matter because the pins live inside `std::unordered_map` values that get rehashed/relocated.
- **severity:** major.

### Keying caches by raw OOP is unsafe; the consumer had to use stable ids and document the OOP-keyed exception
- **evidence:** the nametag snapshot is deliberately keyed by entity id, not OOP — `hypixel_module.hpp:52-55`: *"Keyed by entity id rather than the raw OOP so a GC relocation / address reuse can't return the wrong component."* Where the consumer *does* key by OOP (`tab_snapshot::lines`, `:264`), it adds a written safety argument (`:259-261`): *"Keying `lines` by the live NetworkPlayerInfo OOP is safe: those objects are strongly held by playerInfoMap and the snapshot is rebuilt every tick, so a rare GC relocation costs at most one entry its line for <=25ms."*
- **underlying vmhook gap:** This is the *flip side* of the missing pin: because upstream gives no identity-stable handle, the consumer cannot safely use an OOP as a map key and must hand-reason about every cache. A pin (or a pin-derived stable identity) is what would let a cache key be both stable and valid. Note even `global_ref` does not solve OOP-as-key on its own (two pins to the same object expose different addresses post-GC) — see the improvement below.
- **severity:** annoyance bordering on major — it works, but every cache site carries a bespoke correctness proof a library primitive should have absorbed.

### The pin's value is itself a moving target — registry "never evicts" because eviction can't prove the object is dead
- **evidence:** `hypixel_module.cpp:88-90`: *"Entries pin a NetworkPlayerInfo that live entities may still reference, so we never evict — once the cap is hit, new nicks simply don't get a skin override."* Hence the hard `skin_registry_max{ 512 }` cap (`:90`) and the no-evict policy (`:97-100`).
- **underlying vmhook gap:** A global ref keeps the object alive forever, so the consumer cannot tell "is anything still using this?" Upstream provides no weaker handle (e.g. a weak global ref / `NewWeakGlobalRef`, slot 226) that would let a cache observe collection and self-trim. Lacking it, NPNOQOL trades correctness for an unbounded-pin-with-a-cap heuristic.
- **severity:** annoyance — but it is a permanent heap-pin policy chosen *because* the library only offers the strong-pin primitive.

## Upstream improvements this implies

### [S] Port `vmhook::jni::global_ref` verbatim (the move-only GC pin)
- **what to add upstream:** the exact fork class — `class global_ref final` with `explicit global_ref(void* raw_oop)` (→ `jni_oop_handle` then `NewGlobalRef`), `~global_ref()` (→ `DeleteGlobalRef`), deleted copy, defaulted move ctor/assign that null out the source, `void* oop() const` that returns `*reinterpret_cast<void**>(handle_)`, and `explicit operator bool()`. Plus the three free helpers `jni::new_global_ref` / `jni::delete_global_ref` / `jni::oop_handle` it sits on (slots 21/22; the fake-handle wrapper). All five primitives are HotSpot-generic — zero Minecraft in them.
- **why it removes the pain:** it is the single missing lifetime type. It directly fixes pains 1–3: a developer can hold a Java object across ticks/threads with correct exactly-once release and read its live (relocated) address via `.oop()`. It is the natural companion to the `make_unique` / `oop_t` API upstream already ships, and closes the gap upstream's own docstrings flag ("valid for the duration of the hook only").
- **fork reference:** fork `vmhook.hpp:10598-10658` (`class global_ref`), `:10565`/`:10571` (`new_global_ref`/`delete_global_ref`), `:10475` (`oop_handle`), `:9236`/`:9259` (`jni_new_global_ref`/`jni_delete_global_ref`).

### [S] Add a `make_global` / `make_unique`-pinned convenience and document the pin contract on `oop_t`
- **what to add upstream:** (a) a free helper `inline auto pin(vmhook::oop_t) -> vmhook::jni::global_ref;` and a `template<class W> global_ref pin(const std::unique_ptr<W>&)` so the ubiquitous `global_ref{ wrapper->get_instance() }` (consumer `:149`,`:482`,`:662`,`:670`) becomes `vmhook::pin(wrapper)`. (b) Amend the `oop_t` docstring (`vmhook.hpp:12822-12840`) to point at `global_ref` as *the* way to outlive a hook, so the next user doesn't re-derive the lesson.
- **why it removes the pain:** removes the most-repeated boilerplate and turns the "you stored an OOP, now it's dangling" footgun into a discoverable, documented path.
- **fork reference:** none — this is an ergonomics layer over the ported `global_ref`.

### [M] Add a weak pin (`weak_global_ref`) so caches can self-evict
- **what to add upstream:** a `weak_global_ref` RAII over `NewWeakGlobalRef`/`DeleteWeakGlobalRef` (JNI slots 226/227) with a `bool expired()` (via `IsSameObject(handle, nullptr)`) and an `oop()` that returns nullptr once collected. Same move-only shape as `global_ref`.
- **why it removes the pain:** lets a long-lived cache (the skin registry, pain 5) pin weakly and trim entries whose objects HotSpot has reclaimed, replacing the never-evict + hard-cap heuristic with correct lifetime tracking. Still fully HotSpot-generic.
- **fork reference:** none — the fork only added the strong pin; this is the logical next step the consumer's no-evict comment (`:88-90`) is begging for.

### [M] Add a stable identity helper for OOP-keyed maps (`object_id`)
- **what to add upstream:** a small helper that derives a relocation-stable identity for use as a map key — e.g. wrap `global_ref` with an `identity_hash()` that calls JNI `GetObjectClass`/`System.identityHashCode` once and caches it, or document `global_ref` + an explicit id key as the supported pattern. Pair it with guidance that raw `oop_t` must never be a hash key.
- **why it removes the pain:** directly retires the two bespoke correctness proofs in the consumer (`hypixel_module.hpp:52-55` chose entity-id keying; `hypixel_module.cpp:259-261` had to argue OOP-keying was "safe enough for <=25ms"). A library-blessed stable key means consumers stop hand-reasoning about GC for every cache.
- **fork reference:** none directly; conceptually adjacent to `reanchor_classes_via_oop` (fork `:10387`) which already accepts an "anchor OOP" as a stable-enough handle for classloader pinning.

## New test scenarios (mostly jvm_integration — exercise the real usage)

### [jvm_integration] global_ref_survives_explicit_gc
- **scenario:** Allocate a Java object (e.g. `new java.lang.Object()` or a small array) via `make_unique`/JNI, pin it in a `vmhook::jni::global_ref`, drop every other reference, then force `System.gc()` several times (and ideally provoke a relocating young-gen collection). 
- **asserts:** after GC, `ref.oop() != nullptr`; calling a method on `ref.oop()` (e.g. `Object.hashCode()` / reading an array length) succeeds and returns the same logical value as before GC; the *numeric* address from `.oop()` is allowed to differ pre/post GC (proves relocation is being tracked, not that the object happens not to move).

### [jvm_integration] bare_oop_dangles_without_pin (negative / contrast)
- **scenario:** Capture a raw `oop_t` of a freshly allocated, otherwise-unreferenced object; force GC; demonstrate via the pinned twin that the object moved. (Don't deref the bare OOP — assert indirectly.)
- **asserts:** a `global_ref` to the same object reports a *different* `.oop()` after GC than the originally captured bare value — documenting, as an executable spec, why `oop_t` cannot be stored across GC and `global_ref` must be used.

### [unit] global_ref_move_only_semantics
- **scenario:** Default-construct (empty), construct from an OOP, move-construct, move-assign, and self-move-assign a `global_ref`; verify copy is statically disabled.
- **asserts:** `static_assert(!std::is_copy_constructible_v<global_ref> && !std::is_copy_assignable_v<global_ref>)`; moved-from instance is falsy (`!ref`) and its `.oop()` is nullptr; the moved-to instance owns the handle; self-move leaves the handle intact; destroying both does not double-free (run under a JNI stub counting NewGlobalRef/DeleteGlobalRef calls → exactly balanced, one delete per non-empty ref).

### [unit] global_ref_null_and_empty_are_safe
- **scenario:** Construct `global_ref{ nullptr }` and a default `global_ref{}`; call `.oop()` and `operator bool`; let them destruct.
- **asserts:** both are falsy, `.oop()` returns nullptr, no `NewGlobalRef`/`DeleteGlobalRef` is issued for the null path (matches fork ctor early-return at `:10605` and dtor null-guard at `:10616`).

### [jvm_integration] global_ref_cross_thread_handoff
- **scenario:** On thread A, build a pin and publish it inside a `std::shared_ptr<const Snapshot>` via an atomic (mirroring `tab_lines`/`nametags`). On thread B, load the snapshot and use `.oop()` to call a Java method. Force GC between publish and consume.
- **asserts:** thread B sees a valid, relocated `.oop()` and the JNI call succeeds; when the last `shared_ptr` is dropped, exactly one `DeleteGlobalRef` fires (no leak, no double-free), regardless of which thread drops it.

## Verdict: should this be ported upstream?

**Yes — port the general mechanism (the strong GC pin), and ideally the weak pin too.** `global_ref` is not Minecraft-specific in any way: it is a textbook "pin a HotSpot OOP across a relocating GC" primitive, and upstream's own documentation already concedes the gap it fills ("`oop_t` ... is NOT a JNI global reference ... valid for the duration of the hook invocation only"). Upstream ships every neighbour of this feature — `make_unique` to allocate Java objects, `oop_t`/`object_base` to wrap them, `method_proxy::call` to invoke them — yet gives no way to *keep one alive past the call*, which makes any non-trivial "build on one thread/tick, consume on another" workload unsafe by construction. That is a HotSpot-generic deficiency, not a downstream quirk. The right scope is: port `global_ref` + the slot-21/22 helpers + the `oop_handle` wrapper verbatim (they are already generic), add the `pin()` convenience and the `oop_t` docstring pointer, and follow up with `weak_global_ref` so long-lived caches can self-trim instead of pinning the heap forever behind a hard cap. Leave behind only the Minecraft specifics (NetworkPlayerInfo skin swapping, the Lunar classloader anchoring) — those belong in the consumer; the pin itself belongs upstream.
