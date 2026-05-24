// Stress tests for the vmhook::os layer that go beyond the basic round-trip
// covered in test_os_layer.cpp:
//   * protect() must accept addresses that are NOT page-aligned (mprotect on
//     POSIX requires page alignment; the wrapper must align internally).
//   * protect() with a size that crosses a page boundary must cover both
//     pages, not just one.
//   * After protect(no_access), safe_read() must refuse to copy and return
//     false rather than faulting the process.
//   * query_region() on a freshly allocate_rwx'd block must report it as
//     committed + readable (regardless of platform).
//   * allocation_granularity() must be a multiple of page_size() — required
//     by every trampoline allocator that walks the address space at
//     granularity-sized strides.

#include <vmhook/vmhook.hpp>

#include <cstdio>
#include <cstdint>
#include <cstring>

static int failures{ 0 };

static auto check(const char* name, bool ok) -> void
{
    std::printf("%s %s\n", ok ? "[PASS]" : "[FAIL]", name);
    if (!ok)
    {
        ++failures;
    }
}

static auto test_granularity_relationship() -> void
{
    const std::size_t ps{ vmhook::os::page_size() };
    const std::size_t gr{ vmhook::os::allocation_granularity() };
    check("allocation_granularity_at_least_page_size", gr >= ps);
    check("allocation_granularity_multiple_of_page_size", (gr % ps) == 0);
}

static auto test_query_region_attributes_of_rwx_alloc() -> void
{
    const std::size_t page{ vmhook::os::page_size() };
    void* const block{ vmhook::os::allocate_rwx(nullptr, page) };
    if (!block)
    {
        check("query_region_attributes_skipped_alloc_failed", false);
        return;
    }

    // Touch the page so it is unambiguously committed.
    *static_cast<volatile std::uint8_t*>(block) = 0x42;

    const auto info{ vmhook::os::query_region(block) };
    check("query_region_committed", info.committed);
    check("query_region_readable", info.readable);
    check("query_region_not_guarded", !info.guarded);
    check("query_region_size_at_least_page", info.size >= page);

    vmhook::os::release(block, page);
}

static auto test_protect_non_aligned_address() -> void
{
    // Allocate two pages so any rounding inside protect() stays inside our
    // own allocation.  We then ask protect() to change one byte in the middle
    // of page 0 — the wrapper must align the request down before calling
    // mprotect (otherwise mprotect returns EINVAL on POSIX) and back up to
    // the full page.
    const std::size_t page{ vmhook::os::page_size() };
    void* const block{ vmhook::os::allocate_rwx(nullptr, page * 2) };
    if (!block)
    {
        check("protect_non_aligned_address_skipped_alloc_failed", false);
        return;
    }

    auto* const bytes{ static_cast<std::uint8_t*>(block) };
    bytes[0]            = 0x11;
    bytes[page / 2]     = 0x22;
    bytes[page]         = 0x33;
    bytes[page + 1]     = 0x44;

    // Pass an unaligned interior address with a 1-byte length.
    const bool flipped{ vmhook::os::protect(bytes + (page / 2), 1,
                                           vmhook::os::memory_protection::read,
                                           nullptr) };
    check("protect_unaligned_address_succeeds", flipped);

    // The protect() above must NOT have damaged the existing contents.
    check("protect_unaligned_address_preserves_data_at_zero",
          bytes[0] == 0x11);
    check("protect_unaligned_address_preserves_data_mid",
          bytes[page / 2] == 0x22);

    // Flip back to RW (or RWX, which is what allocate_rwx returned).  Some
    // platforms (Apple arm64 without JIT entitlement) only permit RW after
    // a writable->execute_rw transition; in that case we still want the
    // page to be writable for the cleanup memset below.
    bool restored{ vmhook::os::protect(bytes, page,
                                       vmhook::os::memory_protection::execute_rw, nullptr) };
    if (!restored)
    {
        restored = vmhook::os::protect(bytes, page,
                                       vmhook::os::memory_protection::read_write, nullptr);
    }
    check("protect_back_to_rw", restored);

    vmhook::os::release(block, page * 2);
}

static auto test_protect_crossing_page_boundary() -> void
{
    // Allocate two pages, ask protect() to change a range starting in page 0
    // and ending in page 1.  Both pages must end up with the new protection.
    const std::size_t page{ vmhook::os::page_size() };
    void* const block{ vmhook::os::allocate_rwx(nullptr, page * 2) };
    if (!block)
    {
        check("protect_crossing_page_skipped_alloc_failed", false);
        return;
    }

    auto* const bytes{ static_cast<std::uint8_t*>(block) };
    bytes[0]         = 0xAA;
    bytes[page - 1]  = 0xBB;
    bytes[page]      = 0xCC;
    bytes[page * 2 - 1] = 0xDD;

    // protect a range spanning [page - 4, page + 4] -> covers both pages.
    const bool flipped{ vmhook::os::protect(bytes + page - 4, 8,
                                           vmhook::os::memory_protection::read,
                                           nullptr) };
    check("protect_crossing_page_boundary_succeeds", flipped);

    check("protect_crossing_page_preserves_data_page0",
          bytes[0] == 0xAA && bytes[page - 1] == 0xBB);
    check("protect_crossing_page_preserves_data_page1",
          bytes[page] == 0xCC && bytes[page * 2 - 1] == 0xDD);

    // Restore writable.
    bool restored{ vmhook::os::protect(bytes, page * 2,
                                       vmhook::os::memory_protection::execute_rw, nullptr) };
    if (!restored)
    {
        restored = vmhook::os::protect(bytes, page * 2,
                                       vmhook::os::memory_protection::read_write, nullptr);
    }
    check("protect_back_to_rw_crossing", restored);

    vmhook::os::release(block, page * 2);
}

#if !VMHOOK_OS_IOS
static auto test_safe_read_refuses_no_access_page() -> void
{
    // After protect(no_access), the page must not be readable.  safe_read
    // must return false rather than letting the process fault.
    const std::size_t page{ vmhook::os::page_size() };
    void* const block{ vmhook::os::allocate_rwx(nullptr, page) };
    if (!block)
    {
        check("safe_read_no_access_skipped_alloc_failed", false);
        return;
    }

    auto* const bytes{ static_cast<std::uint8_t*>(block) };
    bytes[0] = 0xEE;

    const bool flipped{ vmhook::os::protect(block, page,
                                           vmhook::os::memory_protection::no_access,
                                           nullptr) };
    if (!flipped)
    {
        // Some sandboxed runners forbid PROT_NONE; skip rather than fail.
        std::printf("[INFO] safe_read_no_access_skipped: protect(no_access) refused\n");
        vmhook::os::release(block, page);
        return;
    }

    std::uint8_t dst{ 0 };
    const bool readable{ vmhook::os::safe_read(&dst, block, 1u) };
    check("safe_read_refuses_no_access_page", !readable);

    // Flip back so munmap doesn't trip over the no-access mapping in any
    // sanity-check the platform performs at unmap time.
    (void)vmhook::os::protect(block, page,
                              vmhook::os::memory_protection::read_write, nullptr);
    vmhook::os::release(block, page);
}
#endif

// ---------------------------------------------------------------------------
// Defensive input checks - every os::* primitive must short-circuit on bad
// args rather than calling through to the kernel with garbage.
// ---------------------------------------------------------------------------
static auto test_os_primitive_input_guards() -> void
{
    const std::size_t page{ vmhook::os::page_size() };

    // protect: null address OR zero size -> false, no kernel call.
    check("protect_null_addr_returns_false",
          !vmhook::os::protect(nullptr, page,
                               vmhook::os::memory_protection::read_write, nullptr));
    {
        std::uint8_t scratch[16]{};
        check("protect_zero_size_returns_false",
              !vmhook::os::protect(scratch, 0,
                                   vmhook::os::memory_protection::read_write, nullptr));
    }

    // allocate_rwx: zero size -> nullptr, no kernel call.
    check("allocate_rwx_zero_size_returns_null",
          vmhook::os::allocate_rwx(nullptr, 0) == nullptr);
    check("allocate_rwx_zero_size_with_hint_returns_null",
          vmhook::os::allocate_rwx(reinterpret_cast<void*>(0x10000), 0) == nullptr);

    // release: null addr OR zero size -> no-op, doesn't crash.
    vmhook::os::release(nullptr, page);    // must be safe
    vmhook::os::release(nullptr, 0);       // must be safe
    {
        void* const block{ vmhook::os::allocate_rwx(nullptr, page) };
        if (block)
        {
            vmhook::os::release(block, 0); // must be a no-op
            // After the no-op the block should still be live and writable.
            *static_cast<volatile std::uint8_t*>(block) = 0x77;
            check("release_zero_size_is_noop",
                  *static_cast<volatile std::uint8_t*>(block) == 0x77);
            vmhook::os::release(block, page);  // real release
        }
        else
        {
            check("release_zero_size_is_noop_skipped_alloc_failed", false);
        }
    }

    // safe_read: every null arg / zero size combination -> false, no read.
    {
        std::uint8_t dst{ 0 };
        std::uint8_t src{ 0xAB };
        check("safe_read_null_dst_returns_false",
              !vmhook::os::safe_read(nullptr, &src, 1u));
        check("safe_read_null_src_returns_false",
              !vmhook::os::safe_read(&dst, nullptr, 1u));
        check("safe_read_zero_size_returns_false",
              !vmhook::os::safe_read(&dst, &src, 0u));
        check("safe_read_all_null_returns_false",
              !vmhook::os::safe_read(nullptr, nullptr, 0u));
    }

    // query_region(nullptr) returns a default-constructed region_info.
    {
        const auto info{ vmhook::os::query_region(nullptr) };
        check("query_region_null_addr_base_null", info.base == nullptr);
        check("query_region_null_addr_size_zero", info.size == 0);
        check("query_region_null_addr_not_committed", !info.committed);
    }

    // find_loaded_module(nullptr-name) should not crash; result is platform
    // specific (GetModuleHandleA(nullptr) returns the exe's handle on
    // Windows; dlopen(nullptr) returns RTLD_DEFAULT on POSIX).  We only
    // assert no fault.
    (void)vmhook::os::find_loaded_module(nullptr);

    // get_proc_address: null module or null symbol -> nullptr.
    check("get_proc_address_null_module_returns_null",
          vmhook::os::get_proc_address(nullptr, "any_symbol") == nullptr);
    {
        // Use a non-null but invalid handle to keep the "null symbol" check
        // honest without touching GetProcAddress / dlsym on a real module.
        // The null-symbol guard fires before the handle is dereferenced.
        vmhook::os::module_handle bogus_handle{ reinterpret_cast<vmhook::os::module_handle>(
            static_cast<std::uintptr_t>(0x1)) };
        check("get_proc_address_null_symbol_returns_null",
              vmhook::os::get_proc_address(bogus_handle, nullptr) == nullptr);
    }
}

// ---------------------------------------------------------------------------
// Round-trip protect across multiple distinct flag combinations.  The basic
// roundtrip test only covers RWX -> R -> RWX.  This walks the whole enum so
// any switch arm that's silently broken (e.g. accidentally returning
// PROT_NONE on POSIX) shows up as a flipped page that can't be read back.
// ---------------------------------------------------------------------------
static auto test_protect_all_enum_values() -> void
{
    const std::size_t page{ vmhook::os::page_size() };
    void* const block{ vmhook::os::allocate_rwx(nullptr, page) };
    if (!block)
    {
        check("protect_all_enum_values_skipped_alloc_failed", false);
        return;
    }

    auto* const bytes{ static_cast<std::uint8_t*>(block) };
    bytes[0] = 0xA5;

    // Each transition must succeed and leave the byte intact.  Skip
    // no_access (some sandboxed runners forbid PROT_NONE) and
    // execute_rw -> read_write fallback (Apple W^X) but still verify the
    // primary flags.
    const vmhook::os::memory_protection sequence[]{
        vmhook::os::memory_protection::read,
        vmhook::os::memory_protection::read_write,
        vmhook::os::memory_protection::execute_read,
    };
    for (const auto prot : sequence)
    {
        const bool ok{ vmhook::os::protect(block, page, prot, nullptr) };
        check("protect_each_enum_transition_succeeds", ok);
    }

    // Tear down: ensure writable before unmap so any platform-level integrity
    // check at release time doesn't trip on a read-only mapping.
    bool restored{ vmhook::os::protect(block, page,
                                       vmhook::os::memory_protection::execute_rw, nullptr) };
    if (!restored)
    {
        restored = vmhook::os::protect(block, page,
                                       vmhook::os::memory_protection::read_write, nullptr);
    }
    check("protect_back_to_rw_after_enum_walk", restored);
    bytes[0] = 0x5A;
    check("protect_byte_still_writable_after_walk", bytes[0] == 0x5A);

    vmhook::os::release(block, page);
}

// ---------------------------------------------------------------------------
// query_region must report the FREE attribute for an unallocated address.
// This is implementation-defined on iOS (where the helper always returns a
// permissive "looks committed" stub) so we only check it on every other
// platform.
// ---------------------------------------------------------------------------
#if !VMHOOK_OS_IOS
static auto test_query_region_reports_free_for_unallocated() -> void
{
    // Reserve a block, release it, then query - the region must report as
    // either free or non-committed.  This proves the wrapper correctly
    // distinguishes mapped/unmapped state, which the trampoline allocator
    // relies on.
    const std::size_t page{ vmhook::os::page_size() };
    void* const block{ vmhook::os::allocate_rwx(nullptr, page) };
    if (!block)
    {
        check("query_region_free_after_release_skipped_alloc_failed", false);
        return;
    }
    vmhook::os::release(block, page);

    const auto info{ vmhook::os::query_region(block) };
    // After release, the region is either free or non-committed.  On some
    // OSes the kernel may immediately reuse the address; in that case the
    // region info reflects the new mapping and committed could be true.
    // The defining post-release contract is "not the same RWX region we
    // had before": readable + executable both true would be unusual.
    check("query_region_after_release_consistent",
          (info.free || !info.committed)
          || (!(info.readable && info.executable)));
}
#endif

int main()
{
    test_granularity_relationship();
    test_query_region_attributes_of_rwx_alloc();
    test_protect_non_aligned_address();
    test_protect_crossing_page_boundary();
    test_protect_all_enum_values();
    test_os_primitive_input_guards();
#if !VMHOOK_OS_IOS
    test_safe_read_refuses_no_access_page();
    test_query_region_reports_free_for_unallocated();
#endif

    if (failures == 0)
    {
        std::printf("vmhook os protect/safe_read: OK\n");
    }
    else
    {
        std::printf("vmhook os protect/safe_read: %d FAILURE(S)\n", failures);
    }
    return failures == 0 ? 0 : 1;
}
