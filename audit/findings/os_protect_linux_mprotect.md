# os_protect_linux_mprotect

## Summary
The Linux `mprotect` path correctly aligns the base down and rounds the length
up to a page multiple, but it silently drops the kernel error reason (errno),
always writes `0` into `*old_prot` on success which is misleading, and the
alignment arithmetic is unsafe at the address-space top where `base + size`
wraps. Test coverage of the alignment math is decent, but the boundary cases
(overflow at end-of-address-space, exact-page-sized regions, non-power-of-two
`sysconf` returns, EINVAL/EACCES propagation, and the unspecified-enum
fall-through to `PROT_NONE`) are not covered.

## Bugs

### [HIGH] Integer overflow in `end = base + size` and `end - base + ps - 1`
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:655-660
- **description:** `const std::uintptr_t end{ base + size };` wraps to 0 (or
  near 0) when `address + size` exceeds `UINTPTR_MAX`. After base is aligned
  down, `end - base + ps - 1` then produces a tiny (or huge underflowed)
  `aligned_size` and either silently mprotects the wrong region or returns
  success after mprotecting zero pages. The further expression
  `(end - base + ps - 1) & ~(ps - 1)` also overflows when
  `end - base > SIZE_MAX - ps + 1`. Neither path returns an error to the
  caller — the wrapper just hands a corrupt length to the kernel.
- **repro:** Call `vmhook::os::protect(reinterpret_cast<void*>(UINTPTR_MAX -
  8), 64, memory_protection::read)`. `end` becomes a small wrap value, the
  computed `aligned_size` is meaningless, and the syscall is invoked with a
  garbage length.
- **suggested_fix:** Detect overflow before computing `end`:
  ```cpp
  if (size > std::numeric_limits<std::uintptr_t>::max() - base) return false;
  const std::uintptr_t end{ base + size };
  // ...likewise guard end - base + ps - 1 against SIZE_MAX wrap.
  ```
- **confidence:** certain

### [MEDIUM] errno is dropped — caller cannot tell EACCES from ENOMEM from EINVAL
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:660-665
- **description:** The wrapper returns `bool` and discards `errno`. Callers
  (e.g. the trampoline installers around lines 5500-5680) have no way to
  distinguish "kernel forbade RWX under W^X / SELinux" from "address not
  mapped" from "page rounding bug". This silently breaks diagnostics on every
  hook-install failure and makes the documented `VMHOOK_LOG_FILE` channel
  useless for protect failures.
- **repro:** Run on an SELinux-confined process where `mprotect(PROT_EXEC)` is
  denied. The hook installer logs "protect failed" with no kernel reason.
- **suggested_fix:** Either return an `expected<void, int>` style result, or
  on failure stash `errno` into a thread-local `last_protect_errno()` accessor
  and log it via the existing `VMHOOK_LOG` macro when the syscall fails.
- **confidence:** likely

### [MEDIUM] `*old_prot = 0` on success is a lie on POSIX
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:660-665
- **description:** On Windows the wrapper faithfully writes the kernel's
  previous protection back into `*old_prot` (line 650). On Linux it writes
  literal `0`, which collides with `memory_protection::no_access` and is
  indistinguishable from "previous protection was PROT_NONE". A caller that
  does `protect(p, n, RW, &saved); ...; protect(p, n, saved, nullptr);` will
  silently downgrade the page to no-access on Linux instead of restoring it.
- **repro:** Read trampoline restore sites at lines 5512, 5539, 5626, 5678 —
  they pass `&old_protect` for round-tripping. On Linux that round-trip is
  fictitious; only the hard-coded `execute_read` value saves them.
- **suggested_fix:** Either (a) read `/proc/self/maps` for the target page on
  the Linux path and report the real flags, or (b) explicitly document that
  `old_prot` is undefined on POSIX and stop writing `0`, so misuse fails fast
  instead of silently mprotecting to `PROT_NONE`. Option (a) is preferable;
  option (b) at minimum prevents the silent footgun.
- **confidence:** certain

### [LOW] `~(ps - 1)` assumes `sysconf(_SC_PAGESIZE)` is a power of two
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:655-659
- **description:** Both masks `~(static_cast<std::uintptr_t>(ps) - 1)` and
  `~(ps - 1)` require `ps` to be a power of two. POSIX does not strictly
  guarantee this (in practice every Linux/BSD architecture does, but the
  spec is silent), and a hypothetical hostile sysconf override (LD_PRELOAD
  shim, container shim) could return e.g. 6144. The result is mis-aligned
  base/length and an EINVAL from mprotect — but the wrapper has already
  swallowed errno (see above).
- **repro:** `LD_PRELOAD` an `sysconf` interceptor returning 6144. Any
  `protect()` call mis-aligns silently.
- **suggested_fix:** Add a one-time `assert((ps & (ps - 1)) == 0)` in
  `page_size()` (lines 476-486) or fall back to 4096 when the returned value
  is not a power of two.
- **confidence:** speculative

### [LOW] Default-case fall-through silently downgrades unknown enum values to `PROT_NONE`
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:620-631
- **description:** If a caller passes a `memory_protection` constructed via
  `static_cast<memory_protection>(99)` (e.g. forwarded from a future enum
  value or fuzz input), `to_native_protect()` falls through the switch and
  returns `PROT_NONE`. Combined with the `*old_prot = 0` bug above, the
  region becomes silently unreadable with the wrapper still returning `true`
  because mprotect to PROT_NONE succeeds. Same fall-through exists on the
  Windows path with `PAGE_NOACCESS`.
- **repro:** `protect(p, n, static_cast<memory_protection>(7))` succeeds and
  silently strips all access.
- **suggested_fix:** Mark the switch with `default: return -1;` (or
  `0xFFFFFFFF` on Windows) and have `protect()` short-circuit return `false`
  when `to_native_protect` reports the sentinel. Alternatively, add
  `[[unlikely]]` plus a fatal assert in debug builds.
- **confidence:** likely

## Improvements

### [S] [USER_FACING] Surface mprotect errno through a thread-local accessor
- **rationale:** Users debugging hook install failures on Linux currently see
  a bare `false` from `protect()`. Adding a `last_os_error()` accessor (modeled
  on `GetLastError` / errno) costs ~6 lines and turns "it doesn't work" into
  actionable EACCES/EINVAL/ENOMEM strings.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:638-667
- **suggested_change:** On failure, capture `errno` (Linux) or
  `::GetLastError()` (Windows) into a `thread_local std::uint32_t` and expose
  `vmhook::os::last_protect_error() noexcept -> std::uint32_t`. Existing
  callers stay binary compatible.

### [XS] [USER_FACING] Reject `size > PTRDIFF_MAX` early with a clear log line
- **rationale:** Today an oversized `size` flows into the overflow path and
  produces undefined behavior. A one-line guard at the top of `protect()`
  turns it into a deterministic `false` and a log entry, matching the
  null/zero-size pattern already there.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:641-644
- **suggested_change:**
  ```cpp
  if (!address || size == 0
      || size > static_cast<std::size_t>(std::numeric_limits<std::ptrdiff_t>::max()))
      return false;
  ```

### [S] [INTERNAL] Use shared `align_down` / `align_up` helpers
- **rationale:** The two masking expressions on lines 658-659 are duplicated
  almost verbatim inside the trampoline allocator (around line 4797-4805).
  Extracting two `constexpr` helpers reduces the risk of one site getting
  fixed for overflow while the other is left vulnerable.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:655-659, vmhook/ext/vmhook/vmhook.hpp:4797-4805
- **suggested_change:** Add `inline constexpr auto align_down(uintptr_t v,
  size_t a) noexcept` and `align_up_checked(uintptr_t v, size_t a) noexcept ->
  std::optional<uintptr_t>` near `page_size()` and call from both sites.

### [XS] [USER_FACING] Document the `old_prot` semantics in the doxygen block
- **rationale:** The contract for `old_prot` differs between Windows
  (meaningful previous flags) and POSIX (currently always 0, see bug above).
  Users reading the header have no way to know that, and the comment at
  lines 634-637 makes no mention of platform skew.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:634-637
- **suggested_change:** Expand to: `@param old_prot Receives the previous
  protection on Windows. On POSIX the kernel does not return previous
  protection; this parameter is left untouched.` (after switching the impl to
  stop writing 0).

### [S] [INTERNAL] Mark `protect()` `[[nodiscard]]`
- **rationale:** Several call sites at lines 5500-5513 ignore the return value
  of `protect()`. Adding `[[nodiscard]]` would either flag those (forcing an
  explicit `(void)`) or surface a real bug where a failed protect change is
  followed by a write that segfaults.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:638-639
- **suggested_change:** Change signature to
  `[[nodiscard]] inline auto protect(...) noexcept -> bool`.

## Tests

### [unit] [extend_existing] test_protect_alignment_overflow_at_address_top
- **description:** Pass `address = reinterpret_cast<void*>(UINTPTR_MAX - 7),
  size = 64` and assert the wrapper returns `false` rather than computing a
  wrapped length. Use a sentinel range that is guaranteed unmapped so the
  call cannot accidentally succeed.
- **asserts:** `protect(top_minus_7, 64, read) == false`.
- **existing_file:** tests/test_os_protect_interaction.cpp

### [unit] [extend_existing] test_protect_exact_page_size
- **description:** Allocate one page, call `protect(block, page_size,
  read_write)` and confirm success — the rounding math
  `(end - base + ps - 1) & ~(ps - 1)` must produce exactly one page, not
  two.
- **asserts:** Wrapper returns true, `query_region(block).size >= page` and
  the page after `block` is still in its original state.
- **existing_file:** tests/test_os_protect_interaction.cpp

### [unit] [extend_existing] test_protect_size_exceeding_ptrdiff_max
- **description:** Pass `size = SIZE_MAX / 2 + 1` with a valid address and
  expect `false` without crashing.
- **asserts:** `protect(p, SIZE_MAX/2+1, read) == false`.
- **existing_file:** tests/test_os_protect_interaction.cpp

### [unit] [extend_existing] test_protect_old_prot_not_silently_zero_on_posix
- **description:** On POSIX, allocate a page, call `protect(p, page, read,
  &old)` (where `old` is pre-seeded to `0xDEADBEEF`), and assert that `old`
  is either left untouched or set to a documented sentinel — not `0`, which
  collides with `memory_protection::no_access`.
- **asserts:** After fix, `old != 0` (or `old == 0xDEADBEEF`).
- **existing_file:** tests/test_os_protect_interaction.cpp

### [unit] [new] test_protect_unknown_enum_value_rejected
- **description:** Cast an out-of-range integer to `memory_protection` and
  call `protect()`. Expect `false`, no kernel call, page unchanged.
- **asserts:** Pre-write byte survives, wrapper returns false.
- **existing_file:** tests/test_os_protect_interaction.cpp

### [unit] [new] test_protect_errno_surfaced_after_failure
- **description:** Call `protect()` on a deliberately unmapped page so
  mprotect returns `ENOMEM`, then assert `vmhook::os::last_protect_error()`
  returns `ENOMEM`. Linux-only.
- **asserts:** `last_protect_error() == ENOMEM`.
- **existing_file:** tests/test_os_protect_interaction.cpp (new function;
  requires the `last_protect_error` accessor improvement above)

### [unit] [extend_existing] test_protect_old_prot_pointer_null_safe
- **description:** Pass `old_prot = nullptr` (already implicit on most sites
  via the default arg) and confirm no dereference happens on the success
  path. Equivalent assert for failure path with `nullptr`.
- **asserts:** No crash, return value matches operation.
- **existing_file:** tests/test_os_protect_interaction.cpp

## Parity Concerns
- Windows `protect()` (lines 645-652) returns meaningful previous protection
  via `VirtualProtect`; Linux path writes literal `0` (lines 661-664). This
  asymmetry is hidden from the user.
- Windows path does not need page alignment (VirtualProtect handles it);
  Linux path requires manual alignment. The wrapper hides the difference but
  the alignment math is overflow-unsafe (see HIGH bug). Document the
  contract: "any address+size accepted; rounded to page on POSIX".
- macOS (Apple arm64) shares the POSIX path but has additional W^X
  constraints described at lines 687-693 for `allocate_rwx`. `protect()`
  itself has no parallel comment — a caller transitioning a JIT page from RW
  to RX on Apple Silicon without the JIT entitlement will get EACCES with no
  diagnostic. Consider a note in the doxygen block at lines 634-637.
- `to_native_protect` switch is duplicated between Windows (lines 607-618)
  and POSIX (lines 620-631); both share the same silent fall-through bug.
  Consider a single table-driven helper.
