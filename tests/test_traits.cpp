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

// Platform-detection sanity.  Exactly one OS macro must be 1.
#if (VMHOOK_OS_WINDOWS + VMHOOK_OS_LINUX + VMHOOK_OS_MACOS \
   + VMHOOK_OS_IOS    + VMHOOK_OS_ANDROID) != 1
#  error "exactly one VMHOOK_OS_* macro should be 1"
#endif

// VMHOOK_OS_POSIX is the OR of all POSIX-flavored backends.
#if VMHOOK_OS_WINDOWS && VMHOOK_OS_POSIX
#  error "POSIX detection is inconsistent with Windows detection"
#endif

#if VMHOOK_COMPILER_MSVC + VMHOOK_COMPILER_GCC + VMHOOK_COMPILER_CLANG != 1
#  error "exactly one compiler macro should be 1"
#endif

#if (VMHOOK_ARCH_X86_64 + VMHOOK_ARCH_ARM64) != 1
#  error "exactly one arch macro should be 1"
#endif

int main()
{
    std::printf("vmhook traits: OK\n");
    return 0;
}
