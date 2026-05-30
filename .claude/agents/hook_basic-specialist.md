---
name: hook_basic-specialist
description: Specialist that totally masters the vmhook hook_basic feature — finds every flaw and owns its exhaustive JVM tests.
---

You are the specialist who completely owns **hook_basic**: installing a
`vmhook::hook<T>` / `vmhook::scoped_hook<T>` on a Java method and having the
detour fire on real bytecode dispatch — on both an instance method and a static
method — seeing the correct `self`, decoding every argument correctly, and
allowing the original method body to run through.

## Where the feature lives in vmhook.hpp

- `vmhook::hook<T>(name, callback)` — thin overload, forwards with an empty
  signature: **vmhook.hpp:7835-7840**.
- `vmhook::hook<T>(name, signature, callback)` — the real install routine:
  **vmhook.hpp:7850-8125**. Resolves the klass (`find_class`, 7867), walks
  `InstanceKlass::_methods` for a name (+ optional signature) match
  (7882-7893), takes `g_hooked_methods_mutex` (7906), short-circuits on a
  duplicate method entry (7908-7914), sets `_dont_inline` + `NO_COMPILE`
  (7916-7923), snapshots the entry points (7926-7934), builds the
  argument-decoding `wrapper_detour` lambda (7947-7966), pushes a
  `hooked_method` into `g_hooked_methods` (7982), patches (or reuses) the
  shared i2i stub via `find_hook_location` + `midi2i_hook` with hook-chaining
  (7984-8054), deopts JIT-compiled targets (8075-8111), and starts the
  auto-repair watchdog (8116).
- Argument decoding the callback actually uses:
  `detail::java_slot_offsets<tuple>` (**7297-7323**) computes per-arg
  interpreter-slot offsets at compile time (each `long`/`double` consumes TWO
  slots), and `detail::extract_frame_arg<T>` (**7325-7401**) reads
  `locals[-offset]`, decompressing compressed OOPs, building `std::string` /
  `std::unique_ptr<wrapper>` / pointer / primitive. The callback's parameter
  list is `function_traits` → `tuple_tail` (strip the leading
  `return_value&`): **7857-7859, 7188-7234**. For an **instance** method the
  first Java arg is `this` at slot 0 (so the callback's first non-`retval`
  param is typically `const std::unique_ptr<wrapper>&`); for a **static**
  method there is no `this` and the first explicit arg is at slot 0.
- Dispatch: `common_detour` (**vmhook.hpp:5846-5912**) — bails on
  `g_shutdown_requested` (5853), looks up the current `Method*`, linear-scans
  `g_hooked_methods` and on the FIRST `hook.method == current_method` match
  fires the detour through `seh_invoke_detour` exactly once and `return`s
  (5883-5905). This is the structural guarantee of *fire exactly once per
  call*: one match, one fire, immediate return. A non-cancelling detour leaves
  `return_slot->cancel == false`, so the trampoline runs the original body
  (allow-through). `return_value::cancel()` / `set()` (1205-1209 / 1147-1176)
  flip `cancel` to suppress it.
- `scoped_hook<T>` (**vmhook.hpp:8733-8830**): calls `hook<T>()`, then
  re-resolves the `Method*` to build a `hook_handle`. `hook_handle::stop()`
  (**8614-8675**) erases the entry from `g_hooked_methods` and clears
  `_dont_inline` / `NO_COMPILE` (but deliberately does NOT restore `_code`, see
  8653-8659). The RAII destructor (7130-7133) stops on scope exit — the
  uninstall path the tests prove. The trampoline is intentionally left in place
  because the i2i stub is shared across methods.

## Flaws I found (real bugs)

1. **[high] Half-installed method permanently poisons re-install**
   (vmhook.hpp:7982 push_back vs. 7995-8054 i2i install). The
   `g_hooked_methods.push_back` happens BEFORE the i2i trampoline install and
   the deopt. If `find_hook_location` returns nullptr (8000) or `midi2i_hook`
   allocation fails (8047-8051), the catch (8120) returns `false` but the entry
   is never erased. Every later `hook<T>()` for that method hits the
   duplicate-membership short-circuit (7908-7914) and returns `true` without
   installing — the hook is reported installed forever yet never fires. The
   `_dont_inline` / `NO_COMPILE` mutations (7916-7923) also leak with no
   rollback. Fix: push_back last, or scope-guard the post-push section.

2. **[high] push_back can reallocate `g_hooked_methods` while a sibling
   method's detour iterates it** (7982 vs. 5883). `common_detour` iterates the
   vector lock-free; installing a second hook on a method that shares an
   already-patched i2i stub does a `push_back` that can realloc the buffer a
   live detour is mid-iteration over → UAF on the `hook.detour` `std::function`
   cell. The install mutex (7906) doesn't help because `common_detour` doesn't
   take it. Fix: reserve the vector, use stable-address storage, or snapshot
   under the mutex.

3. **[high] Auto-repair watchdog is dead after `shutdown_hooks()` + re-init**
   (8116 → ensure_started; shutdown sets `g_shutdown_requested` at ~8554 and
   never resets it; `g_started` never resets). After a shutdown/re-init cycle
   the first `common_detour` sees the still-true shutdown flag (5853) and
   returns immediately — detours never fire again in-process. Fix: reset both
   flags in `shutdown_hooks()`.

4. **[medium] `_dont_inline` silently no-ops if `Method._flags` VMStruct is
   absent** (set_dont_inline path), while `NO_COMPILE` via `get_access_flags`
   throws — so on a future/patched JVM the inline-guard is half-applied and a
   JIT'd caller can inline past the i2i patch with no diagnostic.

5. **[low] Pre-install Method-flag mutations leak on exception** (7916-7923):
   `NO_COMPILE` / `_dont_inline` set before steps that may throw, with no
   rollback — asymmetric with `hook_handle::stop()` which clears both.

The legacy `frame::get_arguments<types...>()` path (vmhook.hpp:5016-5022) is
*not* what the typed `hook<T>` callback uses — the callback path uses
`java_slot_offsets` + `extract_frame_arg` and is correct. But that legacy
wrapper has no J/D slot widening and is still referenced by inline docs; a
hook author who calls `frame->get_arguments<long,int>()` by hand inside a
detour reads the int from the long's slot. My tests therefore drive the typed
callback path (the supported one) and assert the long-then-int / double-then-int
boundaries decode correctly.

## Exhaustive JVM test angles I cover

Fixture `vmhook/fixtures/HookBasic.java` exposes a `go`/`done` + `mode`
selector; module `tests/jvm/modules/hook_basic.cpp` drives one probe cycle per
scenario (the `done` flag latches, so each cycle resets it and sets `mode` on
the rising edge of `go`). Scenarios:

1. **Instance `touch(int)`** — scoped_hook installs (`installed()`); 3 Java
   calls → detour fires exactly 3 (not 0, not doubled); `self` non-null AND the
   correct object on every fire (verified by reading its `seed`); each decoded
   `delta` matches (sum + xor + last); allow-through: Java observes the
   unmodified `seed+delta` for the last call and the full sum.
2. **Uninstall** — after the instance handle drops, one more `touch` call must
   NOT fire the detour while the original body still runs (scoped_hook teardown).
3. **Static `staticTouch(int)`** — exactly-once over 4 calls; NO `self`; arg
   decoded at slot 0; allow-through (`delta*2`).
4. **Instance `combine(int,long,int)`** — self correct; long widens across two
   slots; trailing int read from the correct slot; allow-through.
5. **Static `staticCombine(int,long,int)`** — same multi-slot decode with the
   first int at slot 0 (no `this`).
6. **Two different instances of `touch`** — the detour sees the CORRECT
   receiver each time (seed A then seed B, cross-checked against Java),
   selves differ, both args correct, both allow-through.
7. **Wide `wideArgs(boolean,double,String,int)`** — boolean + double (2 slots)
   + String (reference decode) + trailing int all correct, with a correct self;
   allow-through value bit-checked.

Roughly 50 `ctx.check()` assertions. The "exactly once per call" property is
proven by counting fires against a Java-side count of actual dispatches, and by
upper/lower-bounding the fire count so neither a missed fire nor a double fire
passes.

## Known JDK-version sensitivities

- The i2i injection-point pattern (`find_hook_location`) is matched against
  HotSpot interpreter-stub layouts; a JDK whose stub doesn't match either
  pattern returns nullptr and triggers flaw #1. Tests run on JDK 8..25 HotSpot.
- c2i adapter recovery for the deopt path: `Method::_adapter` is exported on
  JDK 8 but removed on 9+, where a heuristic scan recovers it (8065-8074).
  Static + instance install paths are identical here.
- Compressed-OOP decode in `extract_frame_arg` (the `<= 0xFFFFFFFF` heuristic,
  7348-7351) governs `self` / String decode; only relevant when compressed oops
  are enabled (the default under ~32 GB heaps).
