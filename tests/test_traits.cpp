// Compile-time-only test for the header's type traits.  Every static_assert
// here proves a property of the API surface the rest of the library relies
// on; the executable just succeeds when produced.
#include <vmhook/vmhook.hpp>

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

static_assert(vmhook::detail::is_vector_v<std::vector<int>>,
              "is_vector_v must recognise std::vector<int>");
static_assert(vmhook::detail::is_vector_v<const std::vector<int>&>,
              "is_vector_v must strip cv-ref before testing");
static_assert(!vmhook::detail::is_vector_v<int>,
              "is_vector_v must reject non-vector types");

static_assert(vmhook::detail::is_unique_ptr_v<std::unique_ptr<int>>,
              "is_unique_ptr_v must recognise std::unique_ptr<int>");
static_assert(!vmhook::detail::is_unique_ptr_v<int*>,
              "is_unique_ptr_v must reject raw pointers");

// Platform-detection sanity.
#if VMHOOK_OS_WINDOWS
static_assert(VMHOOK_OS_LINUX == 0, "exactly one OS macro should be 1");
#elif VMHOOK_OS_LINUX
static_assert(VMHOOK_OS_WINDOWS == 0, "exactly one OS macro should be 1");
#else
static_assert(false, "vmhook should detect Windows or Linux");
#endif

#if VMHOOK_COMPILER_MSVC + VMHOOK_COMPILER_GCC + VMHOOK_COMPILER_CLANG != 1
#  error "exactly one compiler macro should be 1"
#endif

int main()
{
    std::printf("vmhook traits: OK\n");
    return 0;
}
