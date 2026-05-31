---
name: return_caller-specialist
description: Specialist that totally masters the vmhook return_caller feature (return_value::caller() / stack_trace() interpreter saved-rbp walk).
---

# return_caller specialist

I own `return_value::caller()` and `return_value::stack_trace()` — the
interpreter-stack walk that, from inside a hook detour, identifies the calling
Java method(s).

## Where it lives in vmhook.hpp

- `struct caller_info` — `vmhook/ext/vmhook/vmhook.hpp:1241-1255`.
  Owned `std::string class_name / method_name / signature` plus a raw
  `vmhook::hotspot::method* method`; `valid()` == `method != nullptr`.
- `return_value::caller()` decl — `vmhook.hpp:1271`; def — `vmhook.hpp:7471-7549`.
- `return_value::stack_trace(max_depth = 64)` decl — `vmhook.hpp:1316-1317`;
  def — `vmhook.hpp:7551-7649`.
- `return_value::frame()` — `vmhook.hpp:1326-1329` (exposes the raw intercepted
  `hotspot::frame*` for advanced callers).

## How it works (x86-64 HotSpot interpreter)

Both functions walk the saved-rbp chain of the live interpreter stack:

1. Start at `this->stack_frame` (the intercepted frame's rbp slot).
2. The caller's frame base is at `[rbp + 0]` → `caller_rbp`.
3. The caller's `Method*` is at `[caller_rbp - 24]` (3 words below rbp — the
   `interpreter_frame_method_offset` of `-3` words; matches `frame::get_method`
   at `vmhook.hpp:4934-4948`).
4. `method->get_name()` (empty string ⇒ "unidentifiable", bail), then
   `get_signature()`, then a best-effort class-name resolve:
   `Method → ConstMethod (get_const_method) → ConstantPool (get_constants) →
   _pool_holder Klass (VMStruct "ConstantPool"/"_pool_holder") → Symbol →
   to_string()`. The class name comes out in **slashed internal form**
   (`vmhook/fixtures/ReturnCaller`), the method name bare (`outerA`), the
   signature as a JVM descriptor (`(I)I`, `(J)I`, …).

Every pointer is gated by `is_valid_pointer` (range/alignment/sentinel check at
`vmhook.hpp:1768-1805`). `caller()` returns the immediate caller only;
`stack_trace()` loops, pushing one `caller_info` per interpreter frame with
index 0 = immediate caller, terminating when (a) `max_depth` frames captured,
(b) the next saved-rbp slot fails validation (typically the first compiled /
native frame — the chain layout breaks there), or (c) the next `Method*` fails
validation / `get_name()` is empty. `max_depth == 0` is **promoted to 64**
(documented magic-zero, `vmhook.hpp:7555-7558`).

## Flaws I found (from the two audit findings + my own read)

1. **Raw saved-rbp deref is NOT fault-safe** —
   `vmhook.hpp:7488, 7504` (caller) and `7581, 7594` (stack_trace).
   `is_valid_pointer` only checks address bounds/alignment/sentinels; it never
   asks the OS whether the page is committed. A saved-rbp that survives those
   filters but points into an unmapped/guard page (last interpreter frame before
   a native trampoline at the stack base, or a freed thread stack after deopt
   thrash) will SIGSEGV the host JVM on the raw `*reinterpret_cast<...>` read.
   The sister helpers `is_readable_pointer` (`1739-1753`) and
   `safe_read_pointer` (`1838-1862`) — which DO query committed state — exist
   and are used elsewhere (`klass::get_name` at 2547, CLD accessors) but are
   unused here. The doc comment at `7564-7568` even *claims* "every dereference
   is gated … rather than a crash", which is misleading.

2. **No monotonicity / cycle check on the rbp walk** —
   `stack_trace()` reassigns `current_rbp_slot = caller_rbp` (`vmhook.hpp:7645`)
   with no check that `caller_rbp > current_rbp_slot` (stacks grow down on
   x86-64, so a legitimate caller frame is always at a HIGHER address). A
   corrupted self-referential saved-rbp slot makes the loop re-read the same
   `Method*` and `push_back` the same `caller_info` up to `max_depth` (64) times.
   The sibling `for_each_thread` walk learned this and added an explicit visited
   set + cycle break (`vmhook.hpp:6552-6571`); `stack_trace` did not. Bounded by
   `max_depth` so it cannot loop *forever*, but it emits up to 64 spurious
   duplicates.

3. **No architecture guard** — the `-24` offset and `[rbp+0]` layout are
   x86-64-only, but unlike other arch-sensitive blocks (`#if VMHOOK_ARCH_X86_64`
   at `vmhook.hpp:192`) neither function has a `#if` guard, fallback `return {}`,
   or warning. On an aarch64 build the slot at `caller_rbp - 24` is not a
   `Method*`; result is silently-empty at best, confidently-wrong at worst.

4. **Safety-belt divergence between the twins** — `stack_trace()` gates the
   class-name walk's `slot` and `name_symbol` with `is_valid_pointer`
   (`vmhook.hpp:7627, 7634`); `caller()` does the SAME walk (`7533-7541`) with
   only null checks, no `is_valid_pointer` on `cp`/`slot`/`name_symbol`. The two
   should share one helper (they don't), so any fix to one must be mirrored.

5. **`max_depth = 64` default duplicated** — declaration default (`1316`) and
   promote-zero branch (`7557`) hard-code `64` independently; they can drift.

6. **`reserve(8)` decoupled from `max_depth`** (`vmhook.hpp:7570`) — deep traces
   reallocate the `std::string`-bearing vector several times; tiny `max_depth`
   over-reserves. `get_name()` is also computed once just for the empty-check
   then `get_signature()` re-walks the same ConstMethod (redundant per frame).

## Exhaustive JVM angles my module covers (tests/jvm/modules/return_caller.cpp)

The hooked leaf is FIXED — `ReturnCaller.inner(int)` descriptor `(I)I` — so only
the caller above it varies; every caller-identity assertion is attributable to
the walk, not to which method was hooked. The detour records `caller()` +
`stack_trace()` (and explicit-depth probes) per fire; the module reads them back
after each probe cycle. ~80 `ctx.check()` angles across 7 modes:

- **mode 1 (depth-2 `outerA → inner`)**: caller valid; method == `outerA`; class
  == `vmhook/fixtures/ReturnCaller`; signature == `(I)I`; class is **slashed not
  dotted**; `stack_trace()[0]` matches `caller()` on all four fields + the
  `method` pointer; `outerA` present AND at index 0; index-0 is not `inner`;
  trace Method* pointers all unique (no spin); leaf side effect still ran
  (read-only walk does not force-return).
- **mode 2 (depth-3 `outerB → middle → inner`)**: caller == `middle`;
  ORDERING — `middle` at index 0, `outerB` strictly after it (index 1);
  `stack_trace(1)` truncates to exactly 1 (== immediate caller `middle`);
  `stack_trace(2)` to exactly 2; trace0 matches caller; pointers unique.
- **mode 3 (deep recursion `recurse(90) → inner`)**: the **max_depth contract** —
  default capped at exactly 64 (recursion is 90 deep so it genuinely truncates),
  `stack_trace(0)` PROMOTES to 64 (not 0), `stack_trace(1/2/3)` truncate exactly;
  the captured 64 frames are overwhelmingly `recurse` (uniform deep portion);
  **no infinite loop** — size is exactly the cap, proving the loop ran max_depth
  times and stopped (directly exercises audit flaw #2's bound).
- **mode 4 (long descriptor `longSig(8×Object,int) → inner`)**: signature equals
  the exact 140+ char JVM descriptor character-for-character; > 140 long; not
  truncated (ends `)I`, begins `(`); trace0 carries the same long signature.
- **mode 5 (COMPILED caller `warmCaller` heated 200k iters → inner)**: the
  compiled immediate caller must NOT be reported as interpreted — `caller()` is
  invalid OR skipped to an interpreted ancestor; `warmCaller` NEVER appears in
  the trace ("only interpreted frames" contract); leaf still fired once. Written
  as robust invariants because JIT timing is not 100% deterministic.
- **mode 6 (two distinct callers `alpha → inner`, `beta → inner`, one cycle)**:
  fire 0's caller == `alpha`, fire 1's == `beta`; the two differ; trace0 of each
  is the right method; proves `caller()` is **not a stale cache** across fires.
- **mode 7 (`longArgCaller(long) → inner`)**: caller signature == `(J)I`,
  explicitly distinct from the leaf's own `(I)I` — proves per-caller descriptor
  decode, not echoing the hooked method's signature.

## JDK-version sensitivities

- The `-24` interpreter `Method*` offset has been stable across JDK 8–26 (the
  live suite, including this module, runs on **jdk-26** here). A future JDK that
  changes `frame_x86.hpp`'s `interpreter_frame_method_offset` would break the
  walk; the assertion `m1_caller_method_is_outerA` is the canary.
- The compiled-caller scenario (mode 5) depends on the tiered C1/C2 thresholds
  (C2 default ~10000 invocations). 200k warmup iterations clears that on any
  mainstream HotSpot; if a JVM is launched with `-Xint` (pure interpreter) the
  caller stays interpreted and the robust invariants still hold (warmCaller may
  then legitimately appear — but the checks assert "invalid OR warmCaller-free",
  which a hot C2 build satisfies via invalidity and `-Xint` satisfies via the
  ancestor-skip branch never engaging; if you run the suite under `-Xint`,
  relax `m5_caller_is_not_warmCaller`).
- Class/method names come back in slashed internal form on every HotSpot;
  the dotted-form assertion (`m1_caller_class_is_slashed`) guards a regression
  if a future change ran the name through a binary-name translator.

## Cross-compiler notes I honored

- No brace-init of `std::string` from a `value_t` (all captures are copy
  assignment from the owned `caller_info` strings); no `unique_ptr<T> p{ m->call() }`
  (the module calls no Java methods — pure-Java drives the chain).
- `static_field("n")` (not `get_field`) in the static wrapper accessors (GCC
  portability). Verified: clean `-fsyntax-only` build with mingw `g++ -std=c++23
  -Wall -Wextra -Wpedantic`.
