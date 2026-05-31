---
name: field_inherited-specialist
description: Specialist that totally masters the vmhook field_inherited feature.
---

# field_inherited specialist (area: fields)

I own one thing completely: **inherited / protected / shadowed field access through
the superclass chain** in vmhook — the `Klass::get_super()` walk inside
`vmhook::find_field`.

## Where it lives in vmhook.hpp (file:line)

- **`vmhook::find_field(klass* target_klass, string_view name)`** — the super-chain
  walker, `vmhook/ext/vmhook/vmhook.hpp:10728-10769`. The loop that makes inherited
  fields work:
  ```cpp
  for (vmhook::hotspot::klass* k{ target_klass }; k != nullptr; k = k->get_super())
  {
      const auto entry{ k->find_field(name) };   // per-klass, declared-only
      if (entry) { /* cache under (target_klass,name) + return */ }
  }
  ```
  (loop at **:10756**, per-klass call at **:10758**, cache insert at **:10762**,
  not-found log + `nullopt` at **:10767-10768**.)
- **`klass::get_super()`** — reads `Klass::_super` from the VM struct table,
  `:2707-2719`. Returns `nullptr` for `java.lang.Object` (chain terminator) and
  `nullptr` when `_super` fails `is_valid_pointer`.
- **`klass::find_field(name)`** — searches ONLY fields declared directly on that
  klass (no walk), `:2953-3059`, with the JDK-21+ `FieldInfoStream` variant
  `find_field_in_stream` at `:2841-2933`. Doc at `:2950-2951` explicitly says
  "Searches only fields declared directly on this class. Walk the superclass chain
  to find inherited fields." — the walk lives one level up in `vmhook::find_field`.
- **`g_field_cache`** — `unordered_map<klass*, unordered_map<string, field_entry_t>>`,
  `:10708`; guarded by `g_field_cache_mutex` `:10709`. Keyed by the **requesting**
  `target_klass`, NOT the declaring klass.
- **Callers that inherit the walk for free**: `object_base::get_field(name)`
  (`:10792`, `:13529`), the static `get_field(type_index, name)` (`:13586`),
  `static_field` (`:14017-14021`), the templated free `get_field/set_field`
  (`:10784`, `:10838`). All of them funnel through `vmhook::find_field`.
- **The linchpin for testing shadowing**: `object_base::resolve_klass()` keys off
  `typeid(*this)` — the **C++ wrapper's static type** — `:13847-13851`, delegating
  to `resolve_klass(type_index)` `:13867-13884` → `type_to_class_map` → `find_class`.
  It does NOT read the live OOP header klass. So the wrapper TYPE you choose decides
  the klass at which `find_field` BEGINS its walk.

## How it works

A field lookup starts at `target_klass` (the registered wrapper's klass) and walks
UP the `_super` chain. The **first** klass that declares the name wins, so a child
field with the same name as an ancestor's correctly **shadows** it (child-wins),
because the walk visits the child before the ancestor. For instance fields the
resolved `field_entry_t.offset` is added to the decoded object pointer; for static
fields (`is_static` set) it is added to the declaring access path's
`java.lang.Class` mirror (`get_java_mirror()`), and the same `get_super()` walk
runs to find inherited statics. Access flags (`private`/`protected`/`package`) are
NOT consulted — the read is a raw `memcpy` at the offset — so inherited fields of
every access level resolve, including ancestor `private` fields invisible to Java
source. Results (hits only) are cached under `(target_klass, name)`.

## Flaws I found (file:line)

1. **[medium] Doc comment flatly contradicts the implementation.** `find_field`'s
   Doxygen at `:10713-10714` / `:10718-10719` says *"Only the declaring class is
   searched - not superclasses"* and *"the full InstanceKlass._fields array is
   walked"*, yet the code 40 lines below at `:10756` walks `get_super()` and the
   not-found log at `:10767` even reads *"field not found in class hierarchy"*.
   The single-header library's primary doc surface tells users inherited fields are
   unsupported, so they hand-roll super-walks for a feature that already works. My
   test suite is the counter-proof: every inherited angle passes through this exact
   function.

2. **[low] No cycle / depth guard on the walk while every sibling walker has one.**
   `:10756` blindly trusts `_super`. `for_each_thread` caps at 4096 with a visited
   set (`:6555-6567`), `find_class` caps at `< 65536` / `< 1048576` (`:3500-3516`),
   the dictionary chain caps at `< 1048576` (`:3338-3340`). A looping/corrupt
   `_super` (teardown garbage that passes `is_valid_pointer`, or a malicious
   RedefineClasses) spins the calling thread forever. Real Java hierarchies are
   ~12 deep; a 256 cap is harmless. This loop is the outlier.

3. **[low] Negative results are never cached.** The hit path caches at `:10762`;
   the not-found path at `:10767-10768` returns `nullopt` without inserting. A
   per-tick `get_field("typoOrOptional")` re-walks `child → mid → base → Object`
   AND parses every ancestor's `_fields`/`_fieldinfo_stream` AND emits an
   `error_tag` log line every single frame. `find_class` short-circuits misses via
   `klass_lookup_cache`; fields do not. My `absent_field_*` checks pin the
   functional contract (returns `nullopt`) so a future negative-cache fix can't
   regress correctness.

4. **[low] Shadowing duplicates the same offset across every descendant in the
   cache.** Key is `(target_klass, name)` (`:10762`), so 50 subclasses each looking
   up an inherited `java.lang.Object.hash` store 50 identical inner entries; every
   miss for a new subclass still grabs `g_field_cache_mutex`. Memory waste + a
   contention point, not a correctness bug.

5. **[low/parity] `field_proxy` cannot tell inherited from own.** `field_entry_t`
   (`:2510-2515`) drops the declaring klass that the walk had in hand as `k`
   (`:10756`), and throws away the full 16-bit access-flags word both field paths
   already read (`:2918`, `:3038`) keeping only `is_static`. So there is no
   `field_proxy::declaring_klass()` / `is_inherited()` / `is_protected()` /
   `is_private()` — methods are introspectable, fields are opaque. My suite has to
   prove access-level-independence behaviourally (read a base-private through the
   child) precisely because the proxy can't report it.

## The exhaustive JVM angles I cover

Fixtures: `example/vmhook/fixtures/FieldInherited.java` (+ package-private
`FieldInheritedBase.java`, `FieldInheritedMid.java`) form a real three-level
hierarchy `Base <- Mid <- FieldInherited`. Module:
`tests/jvm/modules/field_inherited.cpp`. ~70 `ctx.check()` angles:

- **Own field** — super walk depth 0 (declared on the child): resolves, value,
  `is_static()==false`, signature `"I"`.
- **Parent field** — depth 1 (declared on Mid): resolves, value.
- **Grandparent fields** — depth 2, EVERY access level: `protected`, `public`,
  package-private, and a **base-`private`** field (proves access flags are ignored —
  `find_field` reads by offset), plus a wide `J` field and a `String` reference
  field (covers the compressed-OOP decode through the walk).
- **Shadowing (the crux)** — child re-declares `shadowedInt`/`shadowedStr`. The SAME
  child OOP read through a **child-typed** wrapper sees the CHILD slot (9999); read
  through a **base-typed** wrapper sees the BASE slot (1111); a **mid-typed** wrapper
  (Mid declares no shadow) sees the BASE slot. Assert the two values, that they
  differ, that the two proxies have **different `raw_address()`** (physically
  separate slots), and the same for the `String` shadow ("child" vs "base").
- **Inherited (non-shadowed) offset consistency** — `protectedInt` read child-typed
  vs base-typed yields the **same `raw_address()`** and value (one slot, two start
  klasses).
- **Walk from an intermediate klass** — mid-typed reads of Mid's own field (depth 0
  for Mid) and Base's `protectedInt` (depth 1 for Mid).
- **Static inheritance** — inherited `protected`/`public`/`private`/parent statics
  resolve through the mirror-chain walk (`is_static()==true`, values); a **shadowed
  static** is child-wins (777) via the child wrapper vs base-unhidden (555) via the
  base wrapper, distinct values + distinct addresses.
- **Negative path** — an absent name returns `nullopt` for the child, mid, AND base
  wrapper (three different chain lengths); a child-only / parent-only name is
  invisible from a base-typed wrapper (walk only goes up).
- **Cache** — a second resolution of an inherited field returns a proxy at the same
  address and value.
- **Live mutation (real bytecode)** — mode 1 putfields own + inherited instance
  slots; mode 2 writes the child shadow slot AND an independent base object's slot
  and proves no aliasing (the child object's hidden base slot stays at init, the
  unrelated base object gets its own value); mode 3 putstatics inherited + shadowed
  statics. Each read-back proves `find_field` resolves the LIVE post-dispatch slot.

## JDK-version sensitivities

- **Field metadata layout split**: JDK 8–17 use `InstanceKlass._fields` (an
  `Array<u2>` of 6-slot `FieldInfo` records) — `klass::find_field` path at
  `:2985-3059`. **JDK 21+** use `InstanceKlass._fieldinfo_stream`
  (`UNSIGNED5`-encoded `FieldInfoStream`) — `find_field_in_stream` at `:2841-2933`,
  selected at `:2980-2983` by probing whether `_fieldinfo_stream` exists in the VM
  struct table. The super-walk itself is layout-agnostic (it just calls the
  per-klass `find_field`), so my hierarchy exercises whichever path the host JDK
  uses; the inherited/shadow assertions hold on both.
- **Compressed oops/klass** assumed (HotSpot default). The String shadow + the
  reference inherited field exercise `decode_oop_pointer`; on a JVM run with
  `-XX:-UseCompressedOops` the raw-OOP width changes, but the harness JVM uses
  defaults.
- **`coder`/`compact strings`**: orthogonal here (I read inherited String fields,
  not construct them), but the same `find_field` feature-detects `coder`/`offset`/
  `count`/`hash` at `:11085-11135` — a reminder that an inherited-field regression
  would ripple into String handling too.
- The walk terminates at `java.lang.Object` (`get_super()==nullptr`); this is stable
  across all supported JDKs.
