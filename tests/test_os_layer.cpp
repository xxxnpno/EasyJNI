// Exercises the vmhook::os abstraction without involving the JVM.
// Validates:
//   * page_size / allocation_granularity return non-zero, power-of-two
//   * current_thread_id returns non-zero values that change across threads
//   * allocate_rwx then protect can flip a page to RW and back, and the
//     byte we wrote is preserved (so we know the protect path didn't tear
//     the mapping down).
//   * query_region returns at least some info for an allocated region.
//   * safe_read succeeds on a valid pointer and rejects an obviously bogus
//     one.
#include <vmhook/vmhook.hpp>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <thread>

static int failures{ 0 };

static auto check(const char* name, bool ok) -> void
{
    std::printf("%s %s\n", ok ? "[PASS]" : "[FAIL]", name);
    if (!ok)
    {
        ++failures;
    }
}

int main()
{
    const std::size_t page{ vmhook::os::page_size() };
    check("page_size_nonzero", page > 0);
    check("page_size_power_of_two", (page & (page - 1)) == 0);

    const std::size_t granularity{ vmhook::os::allocation_granularity() };
    check("alloc_granularity_nonzero", granularity > 0);

    const auto tid{ vmhook::os::current_thread_id() };
    check("current_thread_id_nonzero", tid != 0);

    std::atomic<std::uint64_t> other_tid{ 0 };
    std::thread worker{ [&] {
        other_tid.store(static_cast<std::uint64_t>(vmhook::os::current_thread_id()));
    } };
    worker.join();
    check("current_thread_id_unique_per_thread", other_tid.load() != static_cast<std::uint64_t>(tid));

    void* const block{ vmhook::os::allocate_rwx(nullptr, page) };
    check("allocate_rwx_returns_ptr", block != nullptr);

    if (block)
    {
        auto* const bytes = static_cast<volatile unsigned char*>(block);
        bytes[0] = 0xAA;
        bytes[1] = 0x55;
        check("allocated_memory_writable", bytes[0] == 0xAA && bytes[1] == 0x55);

        const auto info = vmhook::os::query_region(block);
        check("query_region_locates_alloc", info.base != nullptr && info.size >= page);

        std::uint32_t old{};
        const bool flipped = vmhook::os::protect(block, page,
                                                 vmhook::os::memory_protection::read, &old);
        check("protect_to_readonly", flipped);

        const bool flipped_back = vmhook::os::protect(block, page,
                                                     vmhook::os::memory_protection::execute_rw, nullptr);
        check("protect_back_to_rwx", flipped_back);
        check("memory_survives_protect_cycle", bytes[0] == 0xAA && bytes[1] == 0x55);

        // safe_read sanity: reading into a buffer should succeed for the
        // valid block and fail for a high-canonical pointer.
        unsigned char dst[2]{};
        const bool ok_read = vmhook::os::safe_read(dst, block, sizeof(dst));
        check("safe_read_valid_block", ok_read && dst[0] == 0xAA && dst[1] == 0x55);

        const void* const bogus = reinterpret_cast<const void*>(static_cast<std::uintptr_t>(0xDEADBEEFDEAD'BEEFull));
        const bool ok_bogus = vmhook::os::safe_read(dst, bogus, sizeof(dst));
        check("safe_read_rejects_bogus", !ok_bogus);

        vmhook::os::release(block, page);
    }

    return failures == 0 ? 0 : 1;
}
