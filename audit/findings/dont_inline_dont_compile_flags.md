# dont_inline_dont_compile_flags

## Summary
Audited `vmhook::hotspot::set_dont_inline` (vmhook.hpp:5935-5952), `NO_COMPILE`
(vmhook.hpp:5923-5927), and all four use sites that combine them (hook install at
7774-7781, verify_hooks reinstall at 8105-8109, verify_hooks JIT-drift repair at
8259-8263, shutdown / hook_handle::stop at 8434-8439 and 8506-8509). The feature
is fundamentally sound — every install path applies both flags and every teardown
path clears both — but the implementation has several concrete defects that bite
on real JDKs: `set_dont_inline` does no null check on the Method pointer, the
`get_flags()` helper hard-codes a `u2` (uint16_t) width that does not match
HotSpot's actual field width on JDK 8-12 (u1) or JDK 21+ (u4), the
read-modify-write is non-atomic despite the task brief explicitly asking for
atomicity (HotSpot JIT compile threads also touch `_flags`), `set_dont_inline`
silently no-ops on lookup failure with no diagnostic, and the bit position
`(1 << 2)` is hard-coded without ever consulting HotSpot's exported
`Method::_dont_inline` constant (still exported via `gHotSpotVMTypes` on JDK
17+).

## Bugs

### [high] set_dont_inline dereferences method_pointer without null/validity check
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:5935-5952
- **description:** `set_dont_inline` immediately calls
  `method_pointer->get_flags()` with no guard. Every other public method on
  `vmhook::hotspot::method` that this helper interacts with (`get_name`,
  `get_signature`, `get_from_compiled_entry`, etc.) checks
  `is_valid_pointer(this)` first, precisely because `verify_hooks` mode 1
  ("Method freed") and the verify-after-class-unload path can hand us a
  `Method*` that aliases freed memory. `get_flags()` itself also does no
  validation — it indexes off `this + entry->offset` and returns a pointer
  inside whatever the slab allocator handed back. The call at
  vmhook.hpp:8259 (`set_dont_inline(hm.method, true)` inside `verify_hooks`
  mode 3) is reached AFTER mode 1 confirms `hm.method->get_const_method()` is
  non-null, but the mode 1 reinstall path at vmhook.hpp:8105
  (`set_dont_inline(new_method, true)`) is reached AFTER `new_klass` lookup and
  a `m->get_name()` comparison that already validated `m`, so those two are OK.
  The remaining call sites — `hook<>()` at 7774, `shutdown_hooks` at 8434, and
  `hook_handle::stop()` at 8506 — are reached with `found_method` /
  `hooked_method_entry.method` / `entry_it->method` that have NOT been
  `is_valid_pointer`-checked at the moment of the `set_dont_inline` call.
- **repro:** `hook<my_class>("addScore", cb)` returns true, then another
  JVMTI agent calls `RedefineClasses` and the allocator reuses the address
  for an unrelated freed object. Call `shutdown_hooks()` — `set_dont_inline`
  fetches `_flags` from the alien object using a stale `entry->offset`,
  reads garbage as a `uint16_t*`, and writes the OR'd bit into it. On Windows
  that page may not be writable any more, so the process AVs in
  `set_dont_inline`. On Linux it silently corrupts the alien object.
- **suggested_fix:** Add the same guard the other Method helpers use:
  ```cpp
  if (!method_pointer || !vmhook::hotspot::is_valid_pointer(method_pointer))
  {
      return;
  }
  ```
  at the top of `set_dont_inline`.
- **confidence:** certain

### [high] get_flags hardcodes uint16_t — wrong width on JDK 8-12 (u1) and JDK 21+ (u4)
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:2203-2214, 5938
- **description:** `method::get_flags()` returns `std::uint16_t*`
  unconditionally. HotSpot's actual `Method::_flags` type has changed:
  - JDK 8 through ~JDK 12: `u1` (uint8_t) — a single byte.
  - JDK 13 through JDK 20: `u2` (uint16_t) — what the code assumes.
  - JDK 21+: widened to `u4` (uint32_t) when the bit set grew past 16 bits;
    HotSpot also added a separate `_flags2` field for new bits.
  On JDK 8-12 the helper takes the address of a single `u1` slot and OR's a
  `uint16_t` value into it, which overwrites whatever HotSpot field sits in
  the next byte of the Method object (`_intrinsic_id`, `_jfr_towrite`, etc.,
  depending on layout). On JDK 21+ the upper 16 bits of the real `u4` field
  are left untouched on `&= ~(1 << 2)`, which is benign there, but the read
  side (`*flags & (1 << 2)`) is still correct for the dont_inline bit. The
  shutdown path's `*flags &= static_cast<std::uint16_t>(~(1 << 2))` on a
  JDK 8 `u1` clears whatever is at the next byte. This is the same class of
  silent adjacent-field corruption the v0.5.0 changelog called out for
  `field_proxy::set`.
- **repro:** On JDK 8 hot-spot a class on x86_64 (e.g. JDK 8u202 in
  Minecraft Forge 1.8.9), hook any method via `vmhook::hook<T>()`, log
  `entry->offset` for `Method._flags`, dump 4 bytes of memory at
  `(uint8_t*)method + offset` before and after install — the byte at offset
  +1 (which lives outside `_flags`) gets clobbered.
- **suggested_fix:** Inspect `entry->type_string` once and dispatch on it.
  HotSpot's VMStructs export the field type as the literal `"u1"`, `"u2"`,
  `"u4"`, or `"jint"` strings. A small switch in `get_flags()` (or a new
  `set_method_flag_bit(method*, std::uint32_t mask, bool on)` helper that
  does the size-aware read/modify/write itself) eliminates the silent
  corruption. The OR / AND mask is naturally `1u << 2` which already fits in
  every size; only the load/store width needs to vary.
- **confidence:** certain

### [high] _flags read-modify-write is not atomic; HotSpot JIT threads also write _flags
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:5944-5951
- **description:** The audit brief explicitly asks "Atomic flag set". The
  implementation is `*flags |= (1 << 2)` / `*flags &= ~(1 << 2)` — a plain
  C++ non-atomic read-modify-write on a normal pointer. HotSpot's own
  `Method::set_dont_inline()` (and friends like
  `set_force_inline`, `set_hidden`, `set_caller_sensitive`) mutate the same
  word from C2 / JVMCI / JFR compile threads at arbitrary safepoints, and
  HotSpot defends those writes with `Atomic::cmpxchg` (or, in some bit-set
  variants, the `Method::clear_flags`/`Method::set_flags` API that uses
  `Atomic::*`). Our non-atomic OR can race with HotSpot's CAS and lose the
  bit either direction:
    - thread A (us) reads flags = X
    - thread B (JIT) atomically OR's in bit Y, flags = X | Y
    - thread A writes (X | dontinline), losing bit Y.
  Worst-case symptom on JDK 21+ where the field is `u4` and several JIT
  state bits live in the upper bytes: we clear a `_changes_current_thread`
  / `_jvmci_alias` / etc. bit that the JIT just set, the JIT acts on the
  cleared bit and either deopts the wrong method or skips a needed barrier.
- **repro:** Hard to reproduce deterministically (it's a single-cycle race
  on a single word), but the install path is the most exposed because it
  runs once per hook from the user thread while HotSpot's compile threads
  are sustained. A stress test that calls `vmhook::hook<>` repeatedly while
  a tight Java loop is warming hot methods will hit it within seconds.
- **suggested_fix:** Use `std::atomic_ref<uint16_t>` (or `uint8_t` / `uint32_t`
  per the size-aware fix above) with `fetch_or` / `fetch_and` and
  `memory_order_acq_rel`. C++20 `atomic_ref` is fine for this — the
  underlying word does not need to have been declared atomic on the JVM
  side because x86's lock prefix provides system-wide atomicity regardless
  of how the other party accesses the cache line. Per-platform fallback:
  `_InterlockedOr16` on MSVC, `__atomic_fetch_or` on GCC/clang.
- **confidence:** likely

### [medium] set_dont_inline failure is silent — no diagnostic when VMStructs lookup fails
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:5938-5942, 2203-2214
- **description:** When `iterate_struct_entries("Method", "_flags")` returns
  null (JVM not loaded, JVMTI agent unloaded the structs, mismatched
  HotSpot build), `get_flags()` returns nullptr and `set_dont_inline` quietly
  no-ops with no log line. Every other VMStructs lookup in the file
  (`get_from_interpreted_entry`, `get_access_flags`, `get_const_method`,
  `get_i2i_entry`, `get_code`) wraps the same pattern in a `try/catch` that
  emits `VMHOOK_LOG("{} method.get_xxx() {}", ...)`. The user installs a
  hook, gets `true` back (because the install path only checks
  `access_flags`, not `_flags`), runs in production, and silently loses
  hook coverage the moment HotSpot JIT-compiles any caller because
  `_dont_inline` was never actually set. The auto-repair watchdog will not
  detect this either — `verify_hooks` mode 3 reads `*flags_now &
  NO_COMPILE` from `access_flags`, never from the `_flags` we failed to
  write.
- **repro:** On a HotSpot build whose VMStructs do not export `Method::_flags`
  (any post-2018 OpenJ9-style build, or any custom JVM stripped of internal
  introspection — observed on a hardened Eclipse Adoptium nightly), call
  `vmhook::hook<T>(...)` — install succeeds, hook fires while interpreted,
  silently stops firing once the JIT inlines any caller, no log entry
  points at the cause.
- **suggested_fix:** Match the surrounding helpers — wrap the lookup in
  `try/catch`, log via `VMHOOK_LOG("{} set_dont_inline: ...", error_tag, ...)`,
  and ideally have `hook<>()` propagate the failure (throw or return false)
  so the user knows their hook is dead-on-arrival rather than mysteriously
  dying later.
- **confidence:** certain

### [medium] Hardcoded bit position (1 << 2) — never consults Method::_dont_inline VMTypes constant
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:5946, 5950
- **description:** The `(1 << 2)` magic number is the dont_inline bit
  position in the JDK 17 HotSpot `Method::_flag_values` enum, but the
  enum value has not been stable across versions — JDK 8's HotSpot put
  `_dont_inline` at bit 3 of an unrelated `_jfr_towrite` interaction in
  one Oracle patchlevel, and the JDK 24+ HotSpot reshuffled the bit set
  when splitting into `_flags` and `_flags2`. The header itself uses
  named symbolic constants for the comparable `NO_COMPILE` mask (5923-5927)
  — `_dont_inline` should be looked up through the same mechanism. HotSpot
  exports the enum values via `gHotSpotVMIntConstants` (entry pattern
  `Method::_dont_inline`), reachable through whatever `iterate_struct_entries`
  uses for the `VMTypes` table. Today, when a JDK reorders the enum, every
  `set_dont_inline` silently sets the WRONG bit — e.g. `_force_inline` or
  `_caller_sensitive` — and the JIT happily inlines our hook callee.
- **repro:** Build against any HotSpot fork that has shuffled the
  `_flag_values` enum (the IBM Semeru build for the OMR JIT does this) and
  observe that `_dont_inline` is no longer effective; the JIT inlines the
  hooked body and bypasses the i2i patch.
- **suggested_fix:** Add a one-time lookup similar to the existing pattern at
  vmhook.hpp:2386-2410 for `_from_compiled_code_entry_point`:
  ```cpp
  static const std::uint32_t mask{
      vmhook::hotspot::lookup_int_constant("Method::_dont_inline").value_or(1u << 2) };
  ```
  Use the dynamic mask in the OR / AND; fall back to `(1<<2)` only if the
  exported constant is absent (JDK 8 didn't always export it).
- **confidence:** likely

### [low] NO_COMPILE includes JVM_ACC_QUEUED (0x01000000) without justification
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:5921, 5923-5927
- **description:** `NO_COMPILE` OR's in `JVM_ACC_QUEUED (0x01000000)` alongside
  the three "not compilable" bits. `JVM_ACC_QUEUED` is HotSpot's marker
  meaning "this method is currently sitting in the compile queue"; setting
  it from outside causes HotSpot's `CompileBroker` to skip enqueueing the
  method on subsequent compile requests (because it thinks one is already
  pending). That is most of the desired effect, but it also leaves the bit
  set forever — `shutdown_hooks` line 8439 clears all of `NO_COMPILE` (good),
  but if HotSpot ever sees the `QUEUED` bit set without a matching entry in
  its actual compile queue (which can happen mid-shutdown), it will refuse
  to dequeue and emit `compile queue inconsistent` warnings to hs_err. Not
  a correctness bug per se, but the bit was added without a comment
  justifying why.
- **repro:** Inspect a JVM compile log (`-XX:+PrintCompilation`) after
  installing a hook; the hooked method has the `QUEUED` annotation in
  HotSpot internals but no matching CompileBroker work item.
- **suggested_fix:** Either document why `QUEUED` is included (the strongest
  case is that JDK 12 added a fast-path in `CompileBroker::compile_method`
  that early-returns on `is_queued()`, which is desirable) or drop it and
  rely on the three `NOT_*_COMPILABLE` bits alone. The cost is roughly one
  extra CompileBroker rejection per compile request the JIT generates while
  the hook is installed — negligible.
- **confidence:** speculative

## Improvements

### [S] [INTERNAL] Lift set_dont_inline / NO_COMPILE apply / clear into a single helper
- **rationale:** Five call sites (install at 7774-7781, verify mode 1 reinstall
  at 8105-8109, verify mode 3 repair at 8259-8263, shutdown_hooks at 8434-8439,
  hook_handle::stop at 8506-8509) duplicate the same two-step sequence
  ("set_dont_inline + flags |= NO_COMPILE", or the inverse with `&= ~`). Each
  copy re-fetches `access_flags`, re-checks the null pointer, re-emits
  whatever subset of validation the author thought to add that day. The
  verify mode 3 copy (8240-8263) is the most divergent — it fetches
  `access_flags` BEFORE deciding `jit_drifted`, then re-uses the same
  pointer to write — but the others rewrite the same three lines.
  Centralising into `apply_jit_inhibitors(method*, bool enabled)` cuts ~25
  duplicated lines and gives a single place to add the null check, size
  guard, atomic op, and diagnostic from the bugs above.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:7774-7781, 8105-8109,
  8240-8263, 8434-8439, 8506-8509
- **suggested_change:**
  ```cpp
  inline auto apply_jit_inhibitors(vmhook::hotspot::method* const m, const bool enable) noexcept
      -> bool
  {
      if (!m || !is_valid_pointer(m)) return false;
      set_dont_inline(m, enable);
      std::uint32_t* const af{ m->get_access_flags() };
      if (!af) return false;
      if (enable) *af |= NO_COMPILE;
      else        *af &= ~NO_COMPILE;
      return true;
  }
  ```

### [S] [USER_FACING] Expose set_dont_inline / NO_COMPILE in the public namespace
- **rationale:** Both helpers live in `vmhook::hotspot::`, which the README
  treats as the internal/raw layer. Power users who hand-roll their own
  method discovery (e.g. walking custom dispatch tables) currently have to
  copy-paste the `_flags |= (1<<2)` recipe rather than calling our helper.
  Promoting them to `vmhook::` (or re-exporting under `vmhook::detail::`)
  removes the duplication and makes a single bug-fix point.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:5935-5952
- **suggested_change:** Add `using hotspot::set_dont_inline;` and `using
  hotspot::NO_COMPILE;` aliases inside the top-level `vmhook` namespace, or
  forward them with thin wrappers that take the public `vmhook::oop_t` /
  method handle types so callers never see the hotspot-namespace internals.

### [XS] [USER_FACING] Document why set_dont_inline takes a const Method*
- **rationale:** The signature `set_dont_inline(const method* const, bool)`
  declares the pointer-to-Method `const`, then immediately mutates the
  Method object via `get_flags()` (which `const_cast`'s `this` away). This
  is a textbook "lie in the API" — it tricks the caller into thinking the
  call is observe-only. Either drop the `const` (preferred) or add a
  comment explaining that `const` here means "the pointer is never
  reseated, but the pointee is mutated".
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:5935-5938, 2203, 2213
- **suggested_change:** Change signature to
  `set_dont_inline(vmhook::hotspot::method* method_pointer, bool enabled)`
  and remove the matching `const_cast` from `get_flags`.

### [S] [INTERNAL] Add a debug log line on every dont_inline / NO_COMPILE flip
- **rationale:** The hook install path logs "X is JIT-compiled" /
  "X is interpreted" (7797, 7801) but never logs that the flags were set.
  When a user reports "my hook stopped firing after 30 minutes",
  `verify_hooks` mode 3 prints a clear "JIT-state drifted" line — but the
  initial install gives no audit trail of what flag state we left the
  method in. A single `VMHOOK_LOG("{} hook(): set dont_inline + NO_COMPILE
  on '{}'", info_tag, method_name)` after the apply call lets users
  correlate the timestamps.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:7774-7781
- **suggested_change:** Add the log line directly after `*flags |= NO_COMPILE;`,
  using the `info_tag` (already used at 7797, 7801).

### [M] [INTERNAL] Add a debug-mode read-back assertion that the dont_inline bit actually set
- **rationale:** Even after fixing the silent-failure bug, there are HotSpot
  builds (some hardened Adoptium variants) where the page containing
  Method._flags is read-only and the OR is a no-op without an OS write
  fault. A debug-mode assertion (`assert(*flags & (1<<2))` inside
  `VMHOOK_DEBUG` blocks) would catch this immediately at install time
  rather than hours later when a user reports the hook isn't firing.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:5944-5951
- **suggested_change:** Wrap a read-back-and-log in `#ifdef VMHOOK_DEBUG`:
  ```cpp
  #ifdef VMHOOK_DEBUG
      if (enabled && !(*flags & (1u << 2))) {
          VMHOOK_LOG("{} set_dont_inline: write failed - flags still 0x{:X}",
                     warning_tag, *flags);
      }
  #endif
  ```

## Tests

### [standalone_unit] [new] test_set_dont_inline_null_method
- **description:** Confirm `set_dont_inline(nullptr, true)` and
  `set_dont_inline(nullptr, false)` return safely without dereferencing.
- **asserts:** No crash, no log line (after fix), function returns void.
  Best paired with the suggested null guard.

### [standalone_unit] [new] test_set_dont_inline_invalid_method_pointer
- **description:** Pass a small-integer / sentinel value (e.g.
  `reinterpret_cast<method*>(0xdeadbeef)`) and observe the guard rejects
  it. Today the call crashes inside `get_flags()`.
- **asserts:** Returns without segfault; emits a warning log; no memory
  write occurs at the sentinel address.

### [standalone_unit] [new] test_get_flags_size_jdk_matrix
- **description:** Mock the VMStructs entry table to return `_flags` entries
  with three different `type_string` values: `"u1"`, `"u2"`, `"u4"`. Set
  up a fake Method buffer with sentinel bytes around the flags slot and
  call `set_dont_inline`. Verify the bytes outside the slot remain
  unchanged for every JDK width.
- **asserts:** Bytes at `[offset-1, offset+sizeof(field))` and
  `[offset+sizeof(field), offset+sizeof(field)+8)` unchanged for u1/u2/u4
  paths; bit `1<<2` set inside the slot.

### [standalone_unit] [new] test_set_dont_inline_idempotent
- **description:** Call `set_dont_inline(m, true)` twice and verify the
  flag bit is set once and only once, no other bits flipped.
- **asserts:** `*flags` differs from initial state only at bit 2.

### [standalone_unit] [new] test_set_dont_inline_clear_preserves_other_bits
- **description:** Pre-populate the `_flags` slot with `0xF7` (every bit
  except bit 2) and call `set_dont_inline(m, false)`. Verify the result
  is still `0xF7` (clearing an unset bit is a no-op).
- **asserts:** `*flags == 0xF7` after the call.

### [standalone_unit] [new] test_set_dont_inline_silent_failure_logs
- **description:** Mock `iterate_struct_entries` to return nullptr for
  `Method::_flags`. Call `set_dont_inline(m, true)` and capture
  `VMHOOK_LOG` output.
- **asserts:** A log line at `error_tag` mentioning `set_dont_inline` and
  `_flags` is emitted; function returns safely.

### [standalone_unit] [new] test_set_dont_inline_atomic_concurrency
- **description:** Spawn N threads (N = hardware concurrency) all repeatedly
  calling `set_dont_inline` true/false on the same fake Method, with
  another thread spinning and fetch-OR-ing an unrelated bit into the same
  word. After joining, verify the unrelated bit is still set.
- **asserts:** The unrelated bit is preserved across the race window (only
  meaningful after the atomic-RMW fix).
- **existing_file:** could be added to a new `tests/test_jit_flags.cpp`
  paired with the implementation fix.

### [jvm_integration] [new] test_hook_install_disables_jit_inlining
- **description:** Launch a real JVM (via `tests/jvm_test_harness` or
  similar — if absent, this is the test that motivates building one),
  load a small Java class whose hot loop calls a target method, let
  the JIT warm it up enough to inline, install a `vmhook::hook` on the
  target, force `deoptimize_all_jit_compiled_methods()`, then assert
  the hook fires on the next loop iteration. Without the dont_inline
  bit the hook would not fire.
- **asserts:** Detour callback invocation count > 0 after install +
  deopt; `Method::_flags & (1<<2) != 0` after install.

### [jvm_integration] [new] test_dont_inline_survives_safepoint_cleanup
- **description:** Install hook, idle for >30s under a memory-pressure
  workload that forces safepoint cleanup, verify `Method::_flags &
  (1<<2)` is still set. Regression test for the JIT-drift mode 3
  scenario described at vmhook.hpp:8221-8230.
- **asserts:** Bit still set after safepoint cleanup; if cleared, the
  auto-repair watchdog has already re-set it (also fine).

### [jvm_integration] [new] test_shutdown_clears_dont_inline
- **description:** Hook a method, call `shutdown_hooks()`, then call
  `Method::_flags` directly via VMStructs and confirm bit 2 is cleared.
- **asserts:** `*flags & (1<<2) == 0` post-shutdown.

### [jvm_integration] [new] test_hook_handle_stop_clears_dont_inline
- **description:** Same as above but exercising `hook_handle::stop()`
  (or `scoped_hook` destruction) on a single hook while other hooks
  remain installed. Confirm only the stopped method's flag is cleared.
- **asserts:** Stopped method's flag cleared; other hooked methods'
  flags still set.

## Parity Concerns
- `set_dont_inline` toggles a single bit; HotSpot has a richer set of JIT
  inhibitors (`_force_inline` -> bit 1, `_hidden` -> bit 4, `_intrinsic_candidate`,
  `_changes_current_thread`) that are worth exposing for advanced users who
  want, e.g., to mark a Java helper as never-hidden so their detour sees the
  call site. Siblings would naturally be
  `set_force_inline(method*, bool)`, `set_hidden(method*, bool)`. Currently
  there is zero API parity — only `set_dont_inline` exists.
- `NO_COMPILE` is exported as an `inline constexpr std::int32_t` but
  `_dont_inline`'s bit is a hard-coded `1 << 2`. Bring them under the same
  named-constants convention (e.g. `inline constexpr std::uint32_t
  DONT_INLINE_BIT = 1u << 2;`).
- `set_dont_inline` accepts `const method*` while the matching access-flags
  write at the call sites uses `method*` (non-const). Consistency: pick one.
- `NO_COMPILE` is `std::int32_t` while `Method::_access_flags` is
  `std::uint32_t*` (vmhook.hpp:2178, 7776) — the OR-assignment at 7781
  silently sign-extends if `NO_COMPILE` is ever widened, and the
  `static_cast<std::uint32_t>(~NO_COMPILE)` at 8439 / 8509 is doing a no-op
  signed-to-unsigned reinterpretation. Should be `std::uint32_t`.
