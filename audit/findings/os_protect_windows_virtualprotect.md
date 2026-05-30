# os_protect_windows_virtualprotect

## Summary
The Windows path of `vmhook::os::protect` calls `VirtualProtect` directly, relying on its native behavior of internally aligning the address range to page boundaries. Mapping from the portable `memory_protection` enum to `PAGE_*` constants is straightforward but lossy: any unmapped/out-of-range enum value silently revokes access via `PAGE_NOACCESS`, the `old_prot` value is not round-trippable into a future `protect()` call, and several common protections (PAGE_EXECUTE only, PAGE_WRITECOPY, PAGE_GUARD interaction) are not addressable through the wrapper.

## Bugs

### [LOW] `to_native_protect` defaults to `PAGE_NOACCESS` on unknown enum values
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:607-618
- **description:** The Windows `to_native_protect` switch returns `PAGE_NOACCESS` for any `memory_protection` value not in the enum. A caller who casts a stale/uninitialized `std::uint32_t` (or who reads `old_prot` from one call and tries to feed it back into a `memory_protection`-typed parameter) will silently *remove* access from the page instead of getting a diagnostic. The same fall-through pattern exists for the POSIX peer at lines 620-631 (defaults to `PROT_NONE`). The `noexcept` `auto` signature gives the compiler no way to flag the missing default. Severity is low because no in-tree caller hits this today, but it is a foot-gun for downstream users (and the test at lines 285-327 only walks the legal enum values).
- **repro:** `vmhook::os::protect(p, page, static_cast<vmhook::os::memory_protection>(99), nullptr);` returns `true`, page is now `PAGE_NOACCESS`, subsequent reads fault.
- **suggested_fix:** Mark the switch `[[unlikely]]` default with `VMHOOK_LOG` + return `false`, or use `if constexpr` + `static_assert(false)` style with a tagged dispatch. Simplest patch: drop the fallthrough `return PAGE_NOACCESS;` and replace with `VMHOOK_LOG(warn, ...); return PAGE_NOACCESS;` so at least it shows up in the log.
- **confidence:** certain

### [LOW] `old_prot` is not portable nor round-trippable through `protect()`
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:638-666
- **description:** The wrapper writes `*old_prot = static_cast<std::uint32_t>(prev)` on Windows (a raw `PAGE_*` constant), but writes `*old_prot = 0` on POSIX. The same `protect()` function only *accepts* the portable `memory_protection` enum, so `old_prot` is opaque to the wrapper and cannot be fed back to restore the previous protection. Every in-tree caller (lines 5499-5503, 5512-5513, 5533-5540, 5619-5627, 5671-5679) declares `std::uint32_t old_protect{}`, passes it, and either ignores the result or overwrites it with the *next* call's previous value — the variable is effectively dead. This silently misleads consumers reading the header: the parameter name `old_prot` suggests "save this, pass it back later" but doing so is broken on Windows (raw `PAGE_*` doesn't fit the enum) and meaningless on POSIX (always zero).
- **repro:** `std::uint32_t op{}; protect(p, n, memory_protection::read, &op); protect(p, n, static_cast<memory_protection>(op), nullptr);` — on Windows, `op == PAGE_READONLY == 2`, which casts to `memory_protection::read_write` and unintentionally re-enables writes. On POSIX, `op == 0`, which casts to `memory_protection::no_access` and revokes access entirely.
- **suggested_fix:** Either (a) change `old_prot` to a `memory_protection*` output and translate `prev` back through an inverse `from_native_protect` helper, or (b) return a small POD `protect_result { bool ok; memory_protection previous; }`. If neither is desirable, document explicitly that `old_prot` is platform-native, opaque, and unusable as input to `protect()`.
- **confidence:** certain

### [LOW] No surfacing of `GetLastError` on `VirtualProtect` failure
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:645-652
- **description:** When `VirtualProtect` fails (region mismatch, PAGE_GUARD on a region that disallows it, span across distinct VirtualAlloc regions, etc.) the wrapper drops `GetLastError()` on the floor and just returns `false`. Every caller swallows this — `midi2i_hook::verify_and_repair` (5620-5623) bails out silently, and the install path (5500-5503) doesn't even check the return value. Without an error code in the log, post-mortem debugging of a hook that "just didn't install" requires attaching a debugger.
- **repro:** Hook a function whose page is `PAGE_EXECUTE_WRITECOPY` (CRT/.text in some PE images); `VirtualProtect` returns `false`, wrapper returns `false`, no log line.
- **suggested_fix:** On failure, emit `VMHOOK_LOG("{} os::protect: VirtualProtect(0x{:X}, {}, 0x{:X}) failed, GetLastError={}", vmhook::warn_tag, ...);` before returning false. Same treatment for the POSIX path with `errno`.
- **confidence:** likely

### [LOW] Spanning multiple VirtualAlloc regions fails silently
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:645-652
- **description:** `VirtualProtect` on Windows is documented to fail if the address range spans multiple distinct allocation regions (different `VirtualAlloc` base pointers). The wrapper does not detect this, does not split the request, and does not log. POSIX `mprotect` does not have this limitation, so the same call succeeds on Linux/macOS and fails on Windows — a portability footgun for any user building a multi-allocation patcher on top of `os::protect`.
- **repro:** Allocate two separate `VirtualAlloc` blocks back-to-back (force the OS to give you adjacent regions), call `protect(block_a, block_a_size + block_b_size, ...)`. Returns false on Windows, true on Linux.
- **suggested_fix:** Either (a) document the limitation in the doxygen comment at line 634-637, or (b) walk the range with `VirtualQuery` and split into per-region `VirtualProtect` calls. (a) is the right call for a hook library — splitting is a layering violation and the in-tree usage is always single-region.
- **confidence:** likely

## Improvements

### [LOW] [USER_FACING] Document Windows VirtualProtect behavior in the doxygen comment
- **rationale:** The current doxygen at lines 634-637 ("Changes the protection of a memory region in place") doesn't mention that (1) Windows `VirtualProtect` auto-aligns to page boundaries, (2) the call fails across distinct allocation regions, (3) `old_prot` is platform-native and not portable, and (4) `PAGE_GUARD` / `PAGE_WRITECOPY` / execute-only are not addressable. Users reading the header have to read the `VirtualProtect` MSDN page to know any of this.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:634-637
- **suggested_change:** Expand to ~6 lines: list the per-platform contract, note that `old_prot` is opaque/platform-native, and warn that on Windows the request must lie within a single allocation region.

### [LOW] [USER_FACING] Add `execute_only` and `write_copy` enum members
- **rationale:** The current 5-member enum covers the common cases but excludes two Windows-native protections that hookers and patchers commonly need: `PAGE_EXECUTE` (read-disallowed, execute-only — useful for stripping read access from JIT code) and `PAGE_WRITECOPY` / `PAGE_EXECUTE_WRITECOPY` (the natural state of mapped PE `.text` sections). On POSIX, `PROT_EXEC` alone maps cleanly to `execute_only`; `write_copy` would be a no-op alias for `read_write` on POSIX (private mapping is already CoW). Without these, callers cannot exactly restore the page's original protection after a write.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:447-454, 607-617
- **suggested_change:** Add `execute_only = 5` (→ `PAGE_EXECUTE` / `PROT_EXEC`) and optionally `write_copy = 6` (→ `PAGE_WRITECOPY` / `PROT_READ|PROT_WRITE`). Update `to_native_protect` and add a test arm to `test_protect_all_enum_values`.

### [LOW] [INTERNAL] Reuse a single `old_prot` only after reading it
- **rationale:** The pattern at lines 5499-5513 declares one `old_protect` variable and passes it to two unrelated `protect()` calls back-to-back. The first call's stored "previous" is overwritten by the second call before anyone could use it. This is technically correct (no one *does* use it) but it teaches the wrong pattern to anyone copying the snippet into their own code: it looks like "save then restore" but doesn't.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:5499-5513, 5533-5540, 5619-5627, 5671-5679
- **suggested_change:** Either drop the local entirely and pass `nullptr` (the in-tree callers never read it), or rename it `unused_old_protect` with a `// VirtualProtect requires a non-null lpflOldProtect parameter` comment so a reader knows it is intentionally discarded.

### [MEDIUM] [USER_FACING] Return a richer result type
- **rationale:** `bool` strips the diagnostic. A `struct protect_result { bool ok; memory_protection previous; std::uint32_t native_error; };` (or a `std::expected<memory_protection, std::uint32_t>` once the project moves past the C++17 floor) lets the caller (a) log the underlying error code without needing `GetLastError()` plumbing, and (b) actually restore the previous protection portably. Existing callers that ignore the return continue to compile (struct → bool conversion is fine via an `explicit operator bool`).
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:638-667
- **suggested_change:** Introduce `struct protect_result` in the `os` namespace, change `protect()`'s return type, keep the old `bool old_prot=nullptr` overload as a deprecated thin shim that calls through. Update the four in-tree callers to use the new type only where they need it.

### [LOW] [INTERNAL] Tighten arithmetic on the POSIX path to avoid overflow on near-top-of-address-space addresses
- **rationale:** Lines 656-659 compute `end = base + size` and `end - base + ps - 1` without checking for overflow. If `address + size` overflows (size = SIZE_MAX or address near `0xFFFFFFFFFFFFFFFF`), `end` wraps and `aligned_size` is bogus. Not a Windows-path bug — `VirtualProtect` does its own bounds check — but the parity matters because `os::protect` is the same function for both platforms.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:653-666
- **suggested_change:** Add `if (size > SIZE_MAX - ps || base > UINTPTR_MAX - size) return false;` before the alignment math.

## Tests

### [unit] [extend_existing] test_protect_windows_old_prot_value
- **description:** Confirm that on Windows, `old_prot` is populated with a `PAGE_*` constant (not zero), and that the same call on POSIX leaves it as zero. Codifies the platform-specific contract so any future refactor that "unifies" the semantics breaks loudly.
- **asserts:** After `protect(p, page, read, &op)` returning `true`: on Windows `op == PAGE_EXECUTE_READWRITE` (from prior `allocate_rwx`); on POSIX `op == 0`.
- **existing_file:** tests/test_os_protect_interaction.cpp

### [unit] [extend_existing] test_protect_old_prot_roundtrip_is_broken_today
- **description:** Document the current (broken) round-trip semantics so anyone "fixing" `old_prot` to be the portable enum has to update the test. Allocate RWX, flip to read with `op` capture, attempt to cast `op` to `memory_protection`, assert it is *not* equivalent to the previous protection. This is a regression-guard test, not a feature test.
- **asserts:** `static_cast<memory_protection>(op) != memory_protection::execute_rw` on every platform.
- **existing_file:** tests/test_os_protect_interaction.cpp

### [unit] [new] test_protect_invalid_enum_value_does_not_silently_no_access
- **description:** Pass a cast-from-int `memory_protection` outside the defined range and verify the wrapper either rejects the call (preferred) or at minimum logs a warning rather than silently revoking access. Today the call succeeds and sets `PAGE_NOACCESS` — this test will fail until the bug is fixed.
- **asserts:** `protect(p, page, static_cast<memory_protection>(99), nullptr)` returns `false` OR after the call the page is still readable.
- **existing_file:** tests/test_os_protect_interaction.cpp (add new test, gate behind a "negative test" comment)

### [unit] [extend_existing] test_protect_windows_spans_multiple_allocations
- **description:** Allocate two adjacent regions via `VirtualAlloc` (force adjacency by reserving + committing in two halves) and ask `protect()` to cover both with one call. On Windows the call must fail (and ideally log `GetLastError() == ERROR_INVALID_PARAMETER`). Without this test the "single-region-only" contract isn't enforced anywhere.
- **asserts:** Windows: returns `false`. POSIX: returns `true` (mprotect supports multi-region requests). Gate with `#if VMHOOK_OS_WINDOWS`.
- **existing_file:** tests/test_os_protect_interaction.cpp

### [unit] [new] test_protect_executes_after_flip_to_execute_read
- **description:** Allocate RWX, write a tiny `ret` instruction (`0xC3` on x86-64), flip to `execute_read`, cast to a `void(*)()`, call it. Today the path is exercised transitively by hook install but no unit test confirms that an `execute_read` page actually executes. Skip on iOS/Apple-arm64 (W^X without JIT entitlement).
- **asserts:** No crash; control returns after the call. Verify by mutating an `int volatile` before/after the function pointer call.
- **existing_file:** none — extend tests/test_os_protect_interaction.cpp

### [unit] [extend_existing] test_protect_writes_old_prot_only_on_success
- **description:** Verify the `old_prot` output is left untouched on failure (e.g. when address is nullptr or VirtualProtect rejects the request). Today the early-return `if (!address || size == 0)` branch doesn't touch `*old_prot`, but neither does the post-call `if (ok && old_prot)` guard on the Windows side — confirm the behavior is consistent across platforms.
- **asserts:** `std::uint32_t op{0xDEADBEEF}; protect(nullptr, page, read, &op); op == 0xDEADBEEF;` on every platform.
- **existing_file:** tests/test_os_protect_interaction.cpp

## Parity Concerns
- `old_prot` semantic mismatch: Windows path stores a `PAGE_*` constant, POSIX path stores `0`. Either both should be portable (a `memory_protection` enum), both should be platform-native, or the parameter should be removed.
- POSIX path silently swallows `mprotect` returning `EACCES` / `ENOMEM` / `EINVAL` the same way Windows swallows `GetLastError()`. Fix both paths together to keep parity.
- POSIX path explicitly page-aligns; Windows path relies on `VirtualProtect`'s built-in alignment. The user contract (does `protect()` accept unaligned addresses?) is implicitly "yes" by the test at line 62-109 but isn't documented in the header. A reader checking only the Windows arm would not know.
- The enum is missing two protections that Windows users routinely need (`PAGE_EXECUTE`, `PAGE_WRITECOPY`); POSIX has no analog for the latter but does for the former (`PROT_EXEC`).
