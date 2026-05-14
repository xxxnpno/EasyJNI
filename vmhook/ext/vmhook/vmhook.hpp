#pragma once

/*
    VMHook - Single-header, header-only C++20/23 library for hooking Java methods
    and accessing fields at runtime via HotSpot VMStructs.

    Purpose:
        Provides a zero-JNI, zero-JVMTI API to intercept Java method calls
        and read/write instance/static fields by walking HotSpot's internal
        metadata tables (gHotSpotVMStructs / gHotSpotVMTypes) exported by
        jvm.dll on Windows or libjvm.so on Linux.

    Dependencies:
        - C++20-or-newer compiler. <print>/std::format are used when available
          (MSVC 19.36+, libstdc++ 14+, libc++ 18+); otherwise the header falls
          back to std::format-only or std::ostringstream formatting.
        - Tested with MSVC 19.4x, GCC 13+, Clang 16+ on Windows and Linux.
        - A running HotSpot JVM (Temurin, Corretto, Liberica, Microsoft, etc.)
        - The JVM's runtime shared library loaded in the current process with
          gHotSpotVMStructs / gHotSpotVMTypes exported (jvm.dll / libjvm.so).

    Thread safety:
        - read-only operations (find_class, find_field, get_field)
          are NOT thread-safe; the caller must synchronise access.
        - hook installation and shutdown_hooks() MUST be called from a single thread.
        - The internal caches (klass_lookup_cache, g_field_cache, type_to_class_map)
          use std::unordered_map which is NOT thread-safe for concurrent mutation.

    Template requirements:
        - get_field<T>, set_field<T>:
          T must be std::is_trivially_copyable_v<T> == true.
        - register_class<T>: T must be a complete class type with a virtual table
          (typeid(*this) is used to resolve the class name).
        - hook<T>: T must have been registered via register_class<T>() beforehand.

    Complexity guarantees:
        - find_class(class_name):       O(N*M) worst case, where N = number of loaded
                                        classloaders, M = average classes per classloader.
                                        O(1) after first lookup (cached in klass_lookup_cache).
        - find_field(klass, name):      O(F) where F = number of fields on the klass.
                                        O(1) after first lookup (cached in g_field_cache).
        - get_field<T> / set_field<T>:  O(1) after field is cached; O(F) on first call.
        - hook<T>(method_name, detour): O(M) where M = number of methods on the klass.
        - shutdown_hooks():             O(H) where H = number of active hooks.
        - klass.get_methods_count():    O(1).
        - klass.find_field(name):       O(F) where F = number of fields on the klass.
        - dictionary.find_klass(name):  O(B*E) where B = bucket count, E = entries per bucket.
        - class_loader_data_graph.find_klass(name): O(N*M) full graph walk.

    Exception safety:
        - All public API functions catch vmhook::exception internally and report
          the message through VMHOOK_LOG before returning a safe default value
          (nullptr, std::nullopt, false, or T{}). Release builds compile
          VMHOOK_LOG to a no-op unless VMHOOK_DEBUG_LOGS is overridden.
        - Internal helpers (iterate_struct_entries, iterate_type_entries, etc.)
          are noexcept and return nullptr on failure.
        - midi2i_hook constructor may throw vmhook::exception on allocation failure;
          the caller (hook<T>) catches it and returns false.
*/

#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>
#include <unordered_map>
#include <typeindex>
#include <memory>
#include <mutex>
#include <thread>
#include <chrono>
#include <algorithm>
#include <atomic>
#include <type_traits>
#include <tuple>
#include <cstring>
#include <optional>
#include <variant>
#include <functional>
#include <limits>

// ---------------------------------------------------------------------------
// Platform / compiler detection
//
// Five OS macros are exposed.  Exactly one is set to 1; the others are 0.
// Two convenience aggregates are also defined:
//
//   VMHOOK_OS_POSIX   = Linux | macOS | iOS | Android  (true POSIX backends)
//   VMHOOK_OS_APPLE   = macOS | iOS
//
// The header is callable (and the standalone unit tests run) on every
// platform listed here.  Runtime functionality (HotSpot interpreter
// hooking, gHotSpotVMStructs lookup) requires a HotSpot JVM in-process;
// that exists on Windows / Linux / macOS desktop JDKs but not on iOS or
// Android, where the platform VMs (Apple's restricted no-JIT environment
// and Android's ART) replace HotSpot.  See README / CONTRIBUTING for the
// platform-capability matrix.
// ---------------------------------------------------------------------------

#if defined(__ANDROID__)
    #define VMHOOK_OS_WINDOWS 0
    #define VMHOOK_OS_LINUX   0
    #define VMHOOK_OS_MACOS   0
    #define VMHOOK_OS_IOS     0
    #define VMHOOK_OS_ANDROID 1
#elif defined(_WIN32) || defined(_WIN64)
    #define VMHOOK_OS_WINDOWS 1
    #define VMHOOK_OS_LINUX   0
    #define VMHOOK_OS_MACOS   0
    #define VMHOOK_OS_IOS     0
    #define VMHOOK_OS_ANDROID 0
#elif defined(__APPLE__)
    #include <TargetConditionals.h>
    #if defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
        #define VMHOOK_OS_WINDOWS 0
        #define VMHOOK_OS_LINUX   0
        #define VMHOOK_OS_MACOS   0
        #define VMHOOK_OS_IOS     1
        #define VMHOOK_OS_ANDROID 0
    #else
        #define VMHOOK_OS_WINDOWS 0
        #define VMHOOK_OS_LINUX   0
        #define VMHOOK_OS_MACOS   1
        #define VMHOOK_OS_IOS     0
        #define VMHOOK_OS_ANDROID 0
    #endif
#elif defined(__linux__)
    #define VMHOOK_OS_WINDOWS 0
    #define VMHOOK_OS_LINUX   1
    #define VMHOOK_OS_MACOS   0
    #define VMHOOK_OS_IOS     0
    #define VMHOOK_OS_ANDROID 0
#else
    #define VMHOOK_OS_WINDOWS 0
    #define VMHOOK_OS_LINUX   0
    #define VMHOOK_OS_MACOS   0
    #define VMHOOK_OS_IOS     0
    #define VMHOOK_OS_ANDROID 0
    #error "vmhook supports Windows, Linux, macOS, iOS, or Android (x86_64 / arm64)."
#endif

#define VMHOOK_OS_POSIX (VMHOOK_OS_LINUX | VMHOOK_OS_MACOS | VMHOOK_OS_IOS | VMHOOK_OS_ANDROID)
#define VMHOOK_OS_APPLE (VMHOOK_OS_MACOS | VMHOOK_OS_IOS)

#if defined(__x86_64__) || defined(_M_X64)
    #define VMHOOK_ARCH_X86_64 1
    #define VMHOOK_ARCH_ARM64  0
#elif defined(__aarch64__) || defined(_M_ARM64)
    #define VMHOOK_ARCH_X86_64 0
    #define VMHOOK_ARCH_ARM64  1
#else
    #define VMHOOK_ARCH_X86_64 0
    #define VMHOOK_ARCH_ARM64  0
    #error "vmhook supports x86_64 or arm64 only."
#endif

// HotSpot runtime hooking is x86_64-only: the trampoline emits Microsoft
// x64 / System V AMD64 bytes and walks the HotSpot interpreter frame
// layout, which differs on arm64.  On arm64 hosts the header still
// compiles and the OS layer is fully functional, but `vmhook::hook<T>`
// returns false at runtime.  Set VMHOOK_RUNTIME_HOOKING_AVAILABLE to 0
// in that build configuration so consumers can gate their use of the
// runtime API.
#if VMHOOK_ARCH_X86_64 && !VMHOOK_OS_IOS
    #define VMHOOK_RUNTIME_HOOKING_AVAILABLE 1
#else
    #define VMHOOK_RUNTIME_HOOKING_AVAILABLE 0
#endif

#if defined(_MSC_VER) && !defined(__clang__)
    #define VMHOOK_COMPILER_MSVC 1
#else
    #define VMHOOK_COMPILER_MSVC 0
#endif

#if defined(__clang__)
    #define VMHOOK_COMPILER_CLANG 1
#else
    #define VMHOOK_COMPILER_CLANG 0
#endif

#if defined(__GNUC__) && !defined(__clang__)
    #define VMHOOK_COMPILER_GCC 1
#else
    #define VMHOOK_COMPILER_GCC 0
#endif

// std::format requires GCC 13+ / Clang 14+ / MSVC 19.29+
#if __has_include(<format>)
    #include <format>
    #define VMHOOK_HAS_STD_FORMAT 1
#else
    #define VMHOOK_HAS_STD_FORMAT 0
#endif

// std::print/std::println require GCC 14+ / Clang 18+ (libc++) / MSVC 19.37+
#if __has_include(<print>) && (defined(__cpp_lib_print) && __cpp_lib_print >= 202207L)
    #include <print>
    #define VMHOOK_HAS_STD_PRINT 1
#else
    #define VMHOOK_HAS_STD_PRINT 0
#endif

// C++23 deducing-this support test.  The feature itself is implemented in
// MSVC 19.32+ / GCC 14+ / Clang 18+, but GCC's overload resolution still
// considers explicit-object member functions in *static-call* contexts
// (where no implicit object is available) and then errors with "cannot
// call without object".  Upstream LLVM Clang and MSVC correctly exclude
// them from static-call overload resolution.
//
// The Android NDK Clang shows the same overload-resolution behavior as
// GCC here even though it self-identifies as Clang, so we also exclude
// __ANDROID__.
//
// The deducing-this path is therefore enabled only on MSVC and non-NDK
// Clang — that is where vmhook::object<T>::get_field can be invoked from
// both instance and static methods uniformly.
#if defined(__cpp_explicit_this_parameter) && __cpp_explicit_this_parameter >= 202110L \
    && (defined(__clang__) || defined(_MSC_VER)) \
    && !defined(__ANDROID__)
    #define VMHOOK_HAS_DEDUCING_THIS 1
#else
    #define VMHOOK_HAS_DEDUCING_THIS 0
#endif

#if VMHOOK_OS_WINDOWS
    // <windows.h> defines macros (min, max, ERROR, etc.) that clash with C++.
    // We guard them and undefine the worst offenders right after include.
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
    #include <tlhelp32.h>   // CreateToolhelp32Snapshot, THREADENTRY32, ...
#elif VMHOOK_OS_POSIX
    #include <cerrno>
    #include <cstdio>
    #include <dlfcn.h>
    #include <fcntl.h>
    #include <setjmp.h>     // sigjmp_buf, sigsetjmp, siglongjmp — POSIX (not in <csetjmp>)
    #include <signal.h>     // POSIX sigaction, SIGSEGV, etc. (not in <csignal> for SA_*)
    #include <sys/mman.h>
    #include <unistd.h>
    #if VMHOOK_OS_LINUX || VMHOOK_OS_ANDROID
        // Linux-and-Android-only helpers we use for fast safe memory reads.
        #include <sys/syscall.h>
        #include <sys/uio.h>
    #endif
    #if VMHOOK_OS_APPLE
        #include <mach/mach.h>
        #include <pthread.h>
        #include <libkern/OSCacheControl.h>   // sys_icache_invalidate
        // mach_vm.h is "unsupported" in the iOS SDK (it builds but the
        // mach_vm_* APIs are not callable from a user-space iOS process).
        // On macOS the same header lives at <mach/mach_vm.h>.  Only include
        // it when we are actually going to call it.
        #if VMHOOK_OS_MACOS
            #include <mach/mach_vm.h>
        #endif
    #endif
#endif

#ifndef VMHOOK_DEBUG_LOGS
    #ifdef NDEBUG
        #define VMHOOK_DEBUG_LOGS 0
    #else
        #define VMHOOK_DEBUG_LOGS 1
    #endif
#endif

namespace vmhook::detail
{
    /*
        @brief Format a log line using std::format if available, otherwise stream-format.
        @details
        Provides a single API used by VMHOOK_LOG so callers do not need to know whether
        the host toolchain has shipped std::format / std::print.  When std::format is
        present we use it; otherwise we ignore the format specifiers and concatenate
        the format string verbatim, which is enough for diagnostic output.
    */
#if VMHOOK_HAS_STD_FORMAT
    template <typename... args_t>
    inline auto format_log(std::string_view fmt, args_t&&... args)
        -> std::string
    {
        try
        {
            return std::vformat(fmt, std::make_format_args(args...));
        }
        catch (...)
        {
            return std::string{ fmt };
        }
    }
#else
    template <typename... args_t>
    inline auto format_log(std::string_view fmt, args_t&&...)
        -> std::string
    {
        return std::string{ fmt };
    }
#endif

    inline auto emit_log_line(std::string const& line) noexcept
        -> void
    {
        try
        {
            std::cout << line << '\n';
            std::cout.flush();
        }
        catch (...)
        {
            // Never let logging escape.
        }
    }
}

#if VMHOOK_DEBUG_LOGS
    #define VMHOOK_LOG(...) ::vmhook::detail::emit_log_line(::vmhook::detail::format_log(__VA_ARGS__))
#else
    #define VMHOOK_LOG(...) do {} while (false)
#endif

namespace vmhook
{
    /*
        @brief Log-prefix tags used in diagnostic messages emitted by this library.
        @details  These are prepended to every VMHOOK_LOG() call so that output from
        VMHook can be filtered in the host process console.
    */
    inline constexpr std::string_view error_tag{ "[VMHook ERROR]" };
    inline constexpr std::string_view warning_tag{ "[VMHook WARNING]" };
    inline constexpr std::string_view info_tag{ "[VMHook INFO]" };

    /*
        @brief Exception type thrown internally by VMHook to report unrecoverable errors.
        @details  All public API functions catch vmhook::exception and log the message
        through VMHOOK_LOG before returning a safe default value, so callers never
        see uncaught exceptions escaping the library boundary.
    */
    class exception final : public std::exception
    {
    public:
        explicit exception(const std::string_view msg)
            : message{ msg }
        {

        }

        auto what() const noexcept
            -> const char* override
        {
            return this->message.c_str();
        }

    private:
        std::string message;
    };

    // -------------------------------------------------------------------------
    // OS abstraction layer
    //
    // Wraps Windows-only and POSIX-only primitives behind a portable surface so
    // the HotSpot probing/hooking code can stay platform-agnostic.  All members
    // are noexcept and never throw; failure is reported via returned values.
    // -------------------------------------------------------------------------
    namespace os
    {
#if VMHOOK_OS_WINDOWS
        using module_handle = ::HMODULE;
        using thread_id_t   = ::DWORD;
#else
        using module_handle = void*;
        using thread_id_t   = std::uint64_t;
#endif

        /*
            @brief Memory-protection flags expressed in portable terms.
        */
        enum class memory_protection : std::uint32_t
        {
            no_access      = 0,
            read           = 1,
            read_write     = 2,
            execute_read   = 3,
            execute_rw     = 4,
        };

        /*
            @brief Information about a single VM memory region (allocated or free).
            @details
            Returned by query_region() and used by the trampoline allocator to find
            a free region within +/- 2 GiB of the hook target.
        */
        struct region_info
        {
            void*       base{ nullptr };
            std::size_t size{ 0 };
            bool        committed{ false };
            bool        free{ false };
            bool        readable{ false };
            bool        executable{ false };
            bool        guarded{ false };
        };

        /*
            @brief Returns the host CPU page size in bytes.
        */
        inline auto page_size() noexcept -> std::size_t
        {
#if VMHOOK_OS_WINDOWS
            SYSTEM_INFO si{};
            ::GetSystemInfo(&si);
            return static_cast<std::size_t>(si.dwPageSize);
#else
            const long ps{ ::sysconf(_SC_PAGESIZE) };
            return ps > 0 ? static_cast<std::size_t>(ps) : static_cast<std::size_t>(4096);
#endif
        }

        /*
            @brief Returns the host VM allocation granularity (aligned-to bytes).
        */
        inline auto allocation_granularity() noexcept -> std::size_t
        {
#if VMHOOK_OS_WINDOWS
            SYSTEM_INFO si{};
            ::GetSystemInfo(&si);
            return static_cast<std::size_t>(si.dwAllocationGranularity);
#else
            return page_size();
#endif
        }

        /*
            @brief Maximum user-space address; pointers above this are kernel/non-canonical.
        */
        inline constexpr std::uintptr_t user_address_ceiling{ 0x00007FFFFFFFFFFFull };

        /*
            @brief Minimum sane user-space address; pointers below this are noise.
        */
        inline constexpr std::uintptr_t user_address_floor{ 0xFFFFull };

        /*
            @brief Looks up a previously loaded shared library by leaf filename.
            @details
            Tries the platform's "find loaded module" call (GetModuleHandle on
            Windows, dlopen(name, RTLD_NOW | RTLD_NOLOAD) on Linux).  Returns
            nullptr if the module is not currently loaded into the process.
        */
        inline auto find_loaded_module(const char* name) noexcept -> module_handle
        {
#if VMHOOK_OS_WINDOWS
            return ::GetModuleHandleA(name);
#else
            // RTLD_NOLOAD: only return handle if already loaded.
            return ::dlopen(name, RTLD_LAZY | RTLD_NOLOAD);
#endif
        }

        /*
            @brief Locates the JVM runtime shared library currently loaded.
            @details
            Tries the platform-specific candidate names until one resolves to
            a non-null module.  On Android and iOS this normally returns
            nullptr because those platforms ship their own VM (ART /
            JavaScriptCore-derived) rather than a HotSpot libjvm; the
            higher-level vmhook APIs degrade gracefully in that case.
        */
        inline auto find_jvm_module() noexcept -> module_handle
        {
#if VMHOOK_OS_WINDOWS
            static const char* const candidates[]{ "jvm.dll" };
#elif VMHOOK_OS_MACOS
            static const char* const candidates[]{
                "libjvm.dylib",
                "@rpath/libjvm.dylib",
                "libjvm.so",
            };
#elif VMHOOK_OS_LINUX || VMHOOK_OS_ANDROID
            static const char* const candidates[]{ "libjvm.so", "libjvm.so.0" };
#else
            static const char* const candidates[]{ "libjvm.dylib", "libjvm.so" };
#endif
            for (const char* name : candidates)
            {
                if (module_handle const handle{ find_loaded_module(name) })
                {
                    return handle;
                }
            }
            return nullptr;
        }

        /*
            @brief Resolves an exported symbol from a previously loaded module.
        */
        inline auto get_proc_address(module_handle module, const char* symbol) noexcept -> void*
        {
            if (!module || !symbol)
            {
                return nullptr;
            }
#if VMHOOK_OS_WINDOWS
            return reinterpret_cast<void*>(::GetProcAddress(module, symbol));
#else
            return ::dlsym(module, symbol);
#endif
        }

        /*
            @brief Returns the OS-level identifier of the calling thread.
            @details
            Win32 thread ID on Windows, kernel TID (gettid) on Linux/Android,
            mach thread port number on Apple platforms.  The value space
            differs per platform; the type is uint64_t so all of them fit.
        */
        inline auto current_thread_id() noexcept -> thread_id_t
        {
#if VMHOOK_OS_WINDOWS
            return ::GetCurrentThreadId();
#elif VMHOOK_OS_LINUX || VMHOOK_OS_ANDROID
            return static_cast<thread_id_t>(::syscall(SYS_gettid));
#elif VMHOOK_OS_APPLE
            std::uint64_t tid{ 0 };
            ::pthread_threadid_np(nullptr, &tid);
            return static_cast<thread_id_t>(tid);
#else
            return 0;
#endif
        }

#if VMHOOK_OS_WINDOWS
        inline auto to_native_protect(memory_protection prot) noexcept -> DWORD
        {
            switch (prot)
            {
            case memory_protection::no_access:    return PAGE_NOACCESS;
            case memory_protection::read:         return PAGE_READONLY;
            case memory_protection::read_write:   return PAGE_READWRITE;
            case memory_protection::execute_read: return PAGE_EXECUTE_READ;
            case memory_protection::execute_rw:   return PAGE_EXECUTE_READWRITE;
            }
            return PAGE_NOACCESS;
        }
#else
        inline auto to_native_protect(memory_protection prot) noexcept -> int
        {
            switch (prot)
            {
            case memory_protection::no_access:    return PROT_NONE;
            case memory_protection::read:         return PROT_READ;
            case memory_protection::read_write:   return PROT_READ | PROT_WRITE;
            case memory_protection::execute_read: return PROT_READ | PROT_EXEC;
            case memory_protection::execute_rw:   return PROT_READ | PROT_WRITE | PROT_EXEC;
            }
            return PROT_NONE;
        }
#endif

        /*
            @brief Changes the protection of a memory region in place.
            @return true on success.
        */
        inline auto protect(void* address, std::size_t size, memory_protection prot,
                            std::uint32_t* old_prot = nullptr) noexcept -> bool
        {
            if (!address || size == 0)
            {
                return false;
            }
#if VMHOOK_OS_WINDOWS
            DWORD prev{};
            const BOOL ok{ ::VirtualProtect(address, size, to_native_protect(prot), &prev) };
            if (ok && old_prot)
            {
                *old_prot = static_cast<std::uint32_t>(prev);
            }
            return ok != 0;
#else
            // mprotect requires page-aligned base + length.
            const std::size_t ps{ page_size() };
            std::uintptr_t base{ reinterpret_cast<std::uintptr_t>(address) };
            const std::uintptr_t end{ base + size };
            base &= ~(static_cast<std::uintptr_t>(ps) - 1);
            const std::size_t aligned_size{ static_cast<std::size_t>(end - base + ps - 1) & ~(ps - 1) };
            const int rc{ ::mprotect(reinterpret_cast<void*>(base), aligned_size, to_native_protect(prot)) };
            if (rc == 0 && old_prot)
            {
                *old_prot = 0;
            }
            return rc == 0;
#endif
        }

        /*
            @brief Reserves and commits `size` bytes of writable, executable memory.
            @details
            On Windows uses VirtualAlloc with PAGE_EXECUTE_READWRITE.
            On Linux uses mmap with PROT_READ|PROT_WRITE|PROT_EXEC, MAP_ANONYMOUS|MAP_PRIVATE.
            `address_hint` is treated as a non-binding placement preference.
        */
        inline auto allocate_rwx(void* address_hint, std::size_t size) noexcept -> void*
        {
            if (size == 0)
            {
                return nullptr;
            }
#if VMHOOK_OS_WINDOWS
            return ::VirtualAlloc(address_hint, size,
                                  MEM_COMMIT | MEM_RESERVE,
                                  PAGE_EXECUTE_READWRITE);
#else
            // Try RWX first (succeeds on Linux, Android, x86_64 macOS, and
            // older iOS).  On Apple arm64 / current iOS the kernel enforces
            // W^X and refuses PROT_WRITE | PROT_EXEC simultaneously without
            // the JIT entitlement; fall back to RW so the caller at least
            // gets a usable buffer (the caller can mprotect to RX later via
            // os::protect, which is also entitlement-gated on Apple).
            void* result{ ::mmap(address_hint, size,
                                 PROT_READ | PROT_WRITE | PROT_EXEC,
                                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0) };
            if (result == MAP_FAILED)
            {
                result = ::mmap(address_hint, size,
                                PROT_READ | PROT_WRITE,
                                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
                if (result == MAP_FAILED)
                {
                    return nullptr;
                }
            }
            return result;
#endif
        }

        /*
            @brief Releases memory previously returned by allocate_rwx.
        */
        inline auto release(void* address, std::size_t size) noexcept -> void
        {
            if (!address)
            {
                return;
            }
#if VMHOOK_OS_WINDOWS
            (void)size;
            ::VirtualFree(address, 0, MEM_RELEASE);
#else
            ::munmap(address, size);
#endif
        }

        /*
            @brief Returns information about the VM region containing `address`.
            @details
            Used by safe_readable() and by the trampoline allocator.
        */
        inline auto query_region(const void* address) noexcept -> region_info
        {
            region_info info{};
            if (!address)
            {
                return info;
            }
#if VMHOOK_OS_WINDOWS
            MEMORY_BASIC_INFORMATION mbi{};
            if (::VirtualQuery(address, &mbi, sizeof(mbi)) == 0)
            {
                return info;
            }
            info.base = mbi.BaseAddress;
            info.size = mbi.RegionSize;
            info.committed = (mbi.State == MEM_COMMIT);
            info.free = (mbi.State == MEM_FREE);
            const DWORD prot{ mbi.Protect };
            info.readable = (prot & (PAGE_READONLY | PAGE_READWRITE
                                     | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE
                                     | PAGE_EXECUTE_WRITECOPY | PAGE_WRITECOPY)) != 0;
            info.executable = (prot & (PAGE_EXECUTE | PAGE_EXECUTE_READ
                                       | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
            info.guarded = (prot & PAGE_GUARD) != 0;
            return info;
#elif VMHOOK_OS_MACOS
            // Use mach_vm_region to walk the task's memory map.  Apple has no
            // /proc filesystem; mach_vm_region is the supported API on macOS.
            mach_vm_address_t region_addr{ reinterpret_cast<mach_vm_address_t>(address) };
            mach_vm_size_t    region_size{ 0 };
            vm_region_basic_info_data_64_t mach_info{};
            mach_msg_type_number_t info_count{ VM_REGION_BASIC_INFO_COUNT_64 };
            mach_port_t object_name{};
            const kern_return_t rc{
                ::mach_vm_region(::mach_task_self(),
                                 &region_addr,
                                 &region_size,
                                 VM_REGION_BASIC_INFO_64,
                                 reinterpret_cast<vm_region_info_t>(&mach_info),
                                 &info_count,
                                 &object_name) };
            if (rc != KERN_SUCCESS)
            {
                return info;
            }
            info.base = reinterpret_cast<void*>(region_addr);
            info.size = static_cast<std::size_t>(region_size);
            info.committed = true;
            info.readable  = (mach_info.protection & VM_PROT_READ)    != 0;
            info.executable= (mach_info.protection & VM_PROT_EXECUTE) != 0;
            return info;
#elif VMHOOK_OS_IOS
            // iOS does not expose mach_vm_region / /proc/self/maps.  Return
            // a permissive "looks committed" result so the higher-level
            // validity checks defer to the user-supplied pointer rather
            // than rejecting everything.
            info.base = const_cast<void*>(address);
            info.size = vmhook::os::page_size();
            info.committed = true;
            info.readable  = true;
            return info;
#else
            // Linux and Android both expose /proc/self/maps.
            const std::uintptr_t target{ reinterpret_cast<std::uintptr_t>(address) };
            std::ifstream maps{ "/proc/self/maps" };
            std::string line;
            std::uintptr_t prev_end{ 0 };
            while (std::getline(maps, line))
            {
                if (line.empty())
                {
                    continue;
                }
                std::uintptr_t begin{ 0 };
                std::uintptr_t end{ 0 };
                char perms[5]{};
                const int parsed{ std::sscanf(line.c_str(), "%lx-%lx %4s",
                                              &begin, &end, perms) };
                if (parsed < 3)
                {
                    continue;
                }
                if (target < begin)
                {
                    info.base = reinterpret_cast<void*>(prev_end);
                    info.size = static_cast<std::size_t>(begin - prev_end);
                    info.free = true;
                    return info;
                }
                if (target >= begin && target < end)
                {
                    info.base = reinterpret_cast<void*>(begin);
                    info.size = static_cast<std::size_t>(end - begin);
                    info.committed = true;
                    info.readable   = perms[0] == 'r';
                    info.executable = perms[2] == 'x';
                    return info;
                }
                prev_end = end;
            }
            info.base = reinterpret_cast<void*>(prev_end);
            info.size = (std::numeric_limits<std::uintptr_t>::max)() - prev_end;
            info.free = true;
            return info;
#endif
        }

#if VMHOOK_OS_LINUX || VMHOOK_OS_ANDROID
        namespace detail_signal
        {
            // Thread-local jmp-buf machinery to recover from SIGSEGV during
            // the safe_read sigsetjmp fallback on Linux / Android.  macOS
            // and iOS use mach_vm_read_overwrite so the signal machinery
            // is not needed there; Windows uses ReadProcessMemory.
            struct probe_state
            {
                bool          active{ false };
                volatile bool fault{ false };
                sigjmp_buf    env{};
            };

            inline thread_local probe_state* active_state{ nullptr };

            inline auto handler(int /*sig*/, siginfo_t* /*info*/, void* /*ctx*/) -> void
            {
                if (active_state)
                {
                    active_state->fault = true;
                    ::siglongjmp(active_state->env, 1);
                }
                // Not in a probe; let the default handler take over.
                struct sigaction sa{};
                sa.sa_handler = SIG_DFL;
                ::sigaction(SIGSEGV, &sa, nullptr);
            }

            inline auto install_once() noexcept -> bool
            {
                static const bool installed{ []() noexcept
                {
                    struct sigaction sa{};
                    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
                    sa.sa_sigaction = &handler;
                    ::sigemptyset(&sa.sa_mask);
                    return ::sigaction(SIGSEGV, &sa, nullptr) == 0
                        && ::sigaction(SIGBUS,  &sa, nullptr) == 0;
                }() };
                return installed;
            }
        } // namespace detail_signal
#endif

        /*
            @brief Reads `size` bytes from `src` into `dst` without faulting on bad pointers.
            @details
            On Windows uses ReadProcessMemory on the current process so the
            kernel performs the read with a fault-safe path.  On Linux /
            Android uses process_vm_readv (zero-copy) and falls back to a
            SIGSEGV-catching sigsetjmp probed read.  On macOS / iOS uses
            mach_vm_read_overwrite which kernel-validates the source.
            Returns true on success.
        */
        inline auto safe_read(void* dst, const void* src, std::size_t size) noexcept -> bool
        {
            if (!dst || !src || size == 0)
            {
                return false;
            }
#if VMHOOK_OS_WINDOWS
            SIZE_T transferred{ 0 };
            const BOOL ok{ ::ReadProcessMemory(::GetCurrentProcess(), src, dst,
                                               size, &transferred) };
            return ok && transferred == size;
#elif VMHOOK_OS_MACOS
            mach_vm_size_t transferred{ 0 };
            const kern_return_t rc{ ::mach_vm_read_overwrite(
                ::mach_task_self(),
                reinterpret_cast<mach_vm_address_t>(src),
                static_cast<mach_vm_size_t>(size),
                reinterpret_cast<mach_vm_address_t>(dst),
                &transferred) };
            return rc == KERN_SUCCESS && transferred == size;
#elif VMHOOK_OS_IOS
            // No mach_vm on iOS; do a best-effort memcpy.  Bad pointers
            // will fault — there is no user-callable fault-safe read API
            // on iOS without entitlements.
            std::memcpy(dst, src, size);
            return true;
#elif VMHOOK_OS_LINUX || VMHOOK_OS_ANDROID
            iovec local{ dst, size };
            iovec remote{ const_cast<void*>(src), size };
            const ssize_t n{ ::process_vm_readv(::getpid(), &local, 1, &remote, 1, 0) };
            if (n == static_cast<ssize_t>(size))
            {
                return true;
            }
            // Fall back to signal-protected read.
            if (!detail_signal::install_once())
            {
                return false;
            }
            detail_signal::probe_state state{};
            state.active = true;
            detail_signal::active_state = &state;
            bool success{ false };
            if (::sigsetjmp(state.env, 1) == 0)
            {
                std::memcpy(dst, src, size);
                success = true;
            }
            detail_signal::active_state = nullptr;
            return success && !state.fault;
#else
            (void)dst; (void)src; (void)size;
            return false;
#endif
        }

        /*
            @brief Hints the CPU/OS that an instruction range has been written.
        */
        inline auto flush_instruction_cache(void* address, std::size_t size) noexcept -> void
        {
            if (!address || size == 0)
            {
                return;
            }
#if VMHOOK_OS_WINDOWS
            ::FlushInstructionCache(::GetCurrentProcess(), address, size);
#elif VMHOOK_OS_APPLE
            // Apple ships sys_icache_invalidate as the user-callable API.
            // __builtin___clear_cache emits a reference to compiler-rt's
            // ___clear_cache, which iOS does not link by default.
            ::sys_icache_invalidate(address, size);
#elif defined(__GNUC__) || defined(__clang__)
            __builtin___clear_cache(static_cast<char*>(address),
                                    static_cast<char*>(address) + size);
#else
            (void)address;
            (void)size;
#endif
        }

        // ------------------------------------------------------------
        // Hardware data breakpoints
        // ------------------------------------------------------------
        // x86_64 exposes four debug registers (DR0–DR3) that fire a
        // #DB exception when a configured memory access matches.  On
        // Windows we set them via SetThreadContext and catch the
        // exception with a vectored exception handler; the watch
        // remains zero-overhead until the trap actually fires.
        //
        // Currently a Windows-only path; on other platforms the
        // companion vmhook::watch_static_field<> falls back to its
        // polling implementation.  The capability flag below lets
        // callers query support at compile time.

#if VMHOOK_OS_WINDOWS && VMHOOK_ARCH_X86_64
        #define VMHOOK_HAS_HW_DATA_BREAKPOINTS 1
#else
        #define VMHOOK_HAS_HW_DATA_BREAKPOINTS 0
#endif

        /*
            @brief Type of memory access the breakpoint should trap on.
        */
        enum class data_breakpoint_kind : std::uint8_t
        {
            write    = 0b01,  // only writes
            read_write = 0b11,  // reads and writes (no execute)
        };

        /*
            @brief Length of the memory window the breakpoint guards.
        */
        enum class data_breakpoint_length : std::uint8_t
        {
            one_byte    = 0b00,
            two_bytes   = 0b01,
            eight_bytes = 0b10,
            four_bytes  = 0b11,
        };

#if VMHOOK_HAS_HW_DATA_BREAKPOINTS
        namespace detail_dr
        {
            /*
                @brief Builds a DR7 control mask enabling the given slot
                       (0-3) for the given access kind / length.
                @details
                Per Intel SDM:
                  - L0/L1/L2/L3 (bits 0,2,4,6) enable the local breakpoint.
                  - R/W and LEN fields live at (16+4*slot) and (18+4*slot)
                    respectively.  We keep the global enables (G*) cleared
                    so the trap only applies to threads we explicitly
                    configure.
            */
            inline auto build_dr7(int slot, data_breakpoint_kind rw,
                                  data_breakpoint_length len) noexcept -> std::uint64_t
            {
                const std::uint64_t local_enable{ std::uint64_t{ 1 } << (slot * 2) };
                const std::uint64_t rw_bits     { static_cast<std::uint64_t>(rw)
                                                  << (16 + slot * 4) };
                const std::uint64_t len_bits    { static_cast<std::uint64_t>(len)
                                                  << (18 + slot * 4) };
                return local_enable | rw_bits | len_bits;
            }

            /*
                @brief Enumerates every thread of the current process.
                @details
                Uses Toolhelp on Windows.  Each thread that belongs to
                this process is passed to `callback(HANDLE)`; the handle
                is closed automatically when the callback returns.
            */
            template<typename callback_type>
            inline auto for_each_thread(callback_type&& callback) -> void
            {
                const HANDLE snap{ ::CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0) };
                if (snap == INVALID_HANDLE_VALUE)
                {
                    return;
                }
                THREADENTRY32 te{};
                te.dwSize = sizeof(te);
                if (::Thread32First(snap, &te))
                {
                    const DWORD self_pid{ ::GetCurrentProcessId() };
                    do
                    {
                        if (te.dwSize >= FIELD_OFFSET(THREADENTRY32, th32OwnerProcessID) + sizeof(te.th32OwnerProcessID)
                            && te.th32OwnerProcessID == self_pid)
                        {
                            const HANDLE thread{ ::OpenThread(
                                THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME,
                                FALSE, te.th32ThreadID) };
                            if (thread)
                            {
                                callback(thread);
                                ::CloseHandle(thread);
                            }
                        }
                        te.dwSize = sizeof(te);
                    } while (::Thread32Next(snap, &te));
                }
                ::CloseHandle(snap);
            }
        } // namespace detail_dr
#endif
    } // namespace os

    namespace hotspot
    {
        struct frame;

        /*
            @brief Shared memory slot written by the hook trampoline and read by the callback.
            @details
            The trampoline allocates one return_slot on the native stack before invoking
            the user callback.  If the callback calls return_value::set() or
            return_value::cancel(), cancel is set to true and retval holds the raw
            bit-pattern of the value to return.  The trampoline checks cancel after the
            callback returns and either restores normal execution or short-circuits to
            the stored retval.

            Complexity: O(1) — plain struct with brace-initialised fields.
            Exception safety: noexcept — no dynamic allocation.
            Thread safety: not thread-safe; owned by a single trampoline invocation.
        */
        struct return_slot
        {
            bool         cancel{ false };
            std::int64_t retval{ 0 };
        };

        // Forward declarations used by return_value::caller_info; full
        // definitions appear further down in the file.
        struct method;
        struct klass;
        struct symbol;
        struct const_method;
        struct constant_pool;
    }

    class object_base;

    /*
        @brief Handle passed as the first argument to every hook callback.
        @details
        Call set(value) to suppress the original Java method body and return a
        custom value to the caller.  Call cancel() to suppress without a return
        value (for void methods).  If neither is called the original method runs
        normally.

        Example:
            vmhook::hook<MyClass>("getScore",
                [](vmhook::return_value& ret, const std::unique_ptr<MyClass>& self) {
                    ret.set(std::int32_t{ 9999 });   // always return 9999
                });
    */
    class return_value
    {
    public:
        explicit return_value(vmhook::hotspot::return_slot* slot, vmhook::hotspot::frame* frame = nullptr) noexcept
            : return_slot{ slot }, stack_frame{ frame }
        {

        }

        template<typename value_type>
        auto set(const value_type value) noexcept
            -> void
        {
            static_assert(sizeof(value_type) <= sizeof(std::int64_t), "return type too large for hook slot");
            this->return_slot->cancel = true;
            this->return_slot->retval = 0;
            std::memcpy(&this->return_slot->retval, &value, sizeof(value_type));
        }

        auto cancel() noexcept
            -> void
        {
            this->return_slot->cancel = true;
        }

        /*
            @brief Mutates a Java method argument in-place on the interpreter stack.
            @details
            Writes value directly into the local-variable slot at the given index within
            the intercepted frame.  index 0 is 'this' for instance methods; index 0 is
            the first argument for static methods.  The frame pointer must be valid
            (i.e. the callback was registered via vmhook::hook with frame support enabled).

            Complexity: O(1).
            Exception safety: noexcept — writes to pre-allocated stack memory.
            Thread safety: not thread-safe; must only be called from the hook callback.

            @param index  Zero-based index of the local-variable slot to overwrite.
            @param value  New value to store; must be trivially copyable and fit in a
                          Java local-variable slot (up to 8 bytes).
            @return true if the write succeeded, false if frame is nullptr or index is
                    out of range.
        */
        template<typename value_type>
        auto set_arg(std::int32_t index, value_type&& value) noexcept
            -> bool;

        /*
            @brief Information about the method that invoked the hooked method.
            @details
            Populated by return_value::caller() when the call originates from
            another HotSpot interpreter frame.  All string fields are empty and
            method is nullptr when the caller frame is not interpreted (compiled,
            native, or unidentifiable).
        */
        struct caller_info
        {
            vmhook::hotspot::method* method{ nullptr };
            std::string class_name{};
            std::string method_name{};
            std::string signature{};

            /*
                @brief Returns true when the caller frame was identified.
            */
            auto valid() const noexcept -> bool
            {
                return this->method != nullptr;
            }
        };

        /*
            @brief Returns information about the caller of the hooked method.
            @details
            Walks the saved-rbp chain on the interpreter stack: the caller's
            frame base lives at [rbp], and its Method* lives at
            [caller_rbp - 24] in the HotSpot x64 interpreter layout.  We
            validate every pointer with the existing safe-read helpers before
            dereferencing, so an unfamiliar frame layout produces an empty
            caller_info rather than a crash.

            Complexity: O(1) once the VMStruct offsets are cached.
            Exception safety: noexcept — bad pointers map to an empty result.
            Thread safety: must be called only from the hook callback.
        */
        auto caller() const noexcept -> caller_info;

        /*
            @brief Returns the intercepted interpreter frame, or nullptr.
            @details
            Exposed for advanced use cases that need to walk the call stack
            themselves; callers should prefer caller() when only the
            immediate caller is required.
        */
        auto frame() const noexcept -> vmhook::hotspot::frame*
        {
            return this->stack_frame;
        }

    private:
        vmhook::hotspot::return_slot* return_slot{ nullptr };
        vmhook::hotspot::frame* stack_frame{ nullptr };
    };

    // --- Forward declarations ------------------------------------------------

    namespace hotspot
    {
        struct vm_struct_entry_t;
        struct vm_type_entry_t;
        struct symbol;
        struct constant_pool;
        struct const_method;
        struct method;
        struct klass;
        struct class_loader_data;
        struct class_loader_data_graph;
        struct dictionary;
        struct java_thread;
        struct frame;
        class  midi2i_hook;
        struct hooked_method;
        struct i2i_hook_data;
        struct field_entry_t;

        /*
            @brief Decodes a compressed 32-bit OOP into a full 64-bit heap pointer.
            @details
            HotSpot stores object references as 32-bit values when UseCompressedOops
            is enabled.  This function reverses that encoding.
            Defined later in the file after the OOP-compression constants are resolved.

            Complexity: O(1).
            Exception safety: noexcept.
        */
        static auto decode_oop_pointer(std::uint32_t compressed) noexcept
            -> void*;

        /*
            @brief Encodes a full 64-bit heap pointer as a compressed 32-bit OOP.
            @details
            Inverse of decode_oop_pointer.  Used when writing object references back
            into Java fields that store compressed OOPs.
            Defined later in the file after the OOP-compression constants are resolved.

            Complexity: O(1).
            Exception safety: noexcept.
        */
        static auto encode_oop_pointer(void* decoded) noexcept
            -> std::uint32_t;

        /*
            @brief Attaches the calling native thread to the JVM as a Java thread if needed.
            @details
            Checks whether the current OS thread is already registered in HotSpot's
            thread list.  If not, performs the attach dance so that JNI calls and heap
            allocations from C++ code are legal.
            Defined later in the file after java_thread is fully declared.

            Complexity: O(N) where N = number of active Java threads (thread-list walk).
            Exception safety: noexcept — returns false on failure.
        */
        static auto ensure_current_java_thread() noexcept
            -> bool;
    }

    namespace detail
    {
        inline auto find_call_stub_entry() noexcept -> void*;
    }

    /*
        @brief Maps C++ wrapper types to their corresponding internal Java class names.
        @details
        Populated by vmhook::register_class() when a C++ wrapper type is associated
        with a Java class name.  Used by vmhook::hook() and other APIs that need to
        look up the Java class corresponding to a given C++ wrapper type at runtime.
        Keys   - std::type_index values derived from typeid() of the C++ wrapper type.
        Values - internal JVM class names using '/' separators (e.g. "java/lang/String").
        @see vmhook::register_class, vmhook::hook
    */
    inline std::unordered_map<std::type_index, std::string> type_to_class_map{};

    /*
        @brief Factory function type that creates a std::unique_ptr<T> from a raw OOP.
        @details
        Populated by vmhook::register_class() alongside type_to_class_map.
        Used by field_proxy::get_as<T>() and frame::get_arguments() to construct
        C++ wrapper objects from decoded Java object references.
        Keys   - internal JVM class names (e.g. "java/lang/String").
        Values - lambda functions: +[](void* oop) { return std::make_unique<T>(oop); }
    */
    // The factory returns a heap-allocated wrapper as a raw pointer so the
    // factory's signature is free of `std::unique_ptr<object_base>`.
    // libstdc++ and libc++ both eagerly check sizeof(object_base) inside
    // unique_ptr's destructor static_assert when the lambda is parsed, even
    // though the unique_ptr never actually destructs (the caller calls
    // .release() immediately).  Returning the raw pointer avoids the
    // incomplete-type-in-destructor instantiation entirely.  Callers wrap
    // the returned pointer in a unique_ptr at the consumption site where
    // object_base is already complete.
    using type_factory_function_t = class object_base*(*)(void* instance);
    inline std::unordered_map<std::string, type_factory_function_t> g_type_factory_map{};

    template<class wrapper_type>
    static auto register_class(std::string_view class_name) noexcept
        -> bool;

    class object_base;
    template<typename derived = void> class object;
    class field_proxy;
    class collection;
    class list;

    inline auto read_java_string(void* string_oop)
        -> std::string;

    inline auto make_java_string(std::string_view value) noexcept
        -> void*;

    inline auto write_java_string(void* string_oop, std::string_view value) noexcept
        -> void;

    inline auto decode_array_oop(std::uint32_t compressed)
        -> void*;

    inline auto set_str_field(const field_proxy& field, std::string_view value) noexcept
        -> void;

    inline auto field_oop(const field_proxy& field) noexcept
        -> void*;

    inline auto set_bool_array(const field_proxy& field, const std::vector<bool>& values) noexcept
        -> void;

    inline auto set_str_array(const field_proxy& field, const std::vector<std::string>& values) noexcept
        -> void;

    template<typename element_type>
    inline auto set_prim_array(const field_proxy& field, const std::vector<element_type>& values) noexcept
        -> void;

    // --- Compile-time type traits --------------------------------------------

    namespace detail
    {
        /*
            @brief Type trait that detects whether a type is a specialisation of std::vector.
            @details
            Primary template inherits from std::false_type.  The partial specialisation for
            std::vector<value_type, allocator_type> inherits from std::true_type and
            exposes the element type as value_type_t.  cv-ref qualifiers are stripped via
            std::remove_cvref_t before the check is applied (see is_vector_v).

            Exception safety: noexcept — compile-time trait, no runtime cost.
        */
        template<typename type>
        struct is_vector : std::false_type {};

        template<typename value_type, typename allocator_type>
        struct is_vector<std::vector<value_type, allocator_type>> : std::true_type
        {
            using value_type_t = value_type;
        };

        /*
            @brief Convenience bool constant for vmhook::detail::is_vector<T>.
            @details
            Strips cv-ref qualifiers from type before testing so that
            is_vector_v<const std::vector<int>&> == true.
        */
        template<typename type>
        inline constexpr bool is_vector_v{ is_vector<std::remove_cvref_t<type>>::value };

        /*
            @brief Type trait that detects whether a type is a specialisation of std::unique_ptr.
            @details
            Primary template inherits from std::false_type.  The partial specialisation for
            std::unique_ptr<value_type, deleter_type> inherits from std::true_type and
            exposes the pointed-to type as value_type_t.  Used by hook argument dispatch
            to identify wrapper objects passed by unique_ptr and unwrap the raw OOP.

            Exception safety: noexcept — compile-time trait, no runtime cost.
        */
        template<typename type>
        struct is_unique_ptr : std::false_type {};

        template<typename value_type, typename deleter_type>
        struct is_unique_ptr<std::unique_ptr<value_type, deleter_type>> : std::true_type
        {
            using value_type_t = value_type;
        };

        /*
            @brief Convenience bool constant for vmhook::detail::is_unique_ptr<T>.
            @details
            Strips cv-ref qualifiers from type before testing so that
            is_unique_ptr_v<const std::unique_ptr<Foo>&> == true.
        */
        template<typename type>
        inline constexpr bool is_unique_ptr_v{ is_unique_ptr<std::remove_cvref_t<type>>::value };

    }

    // --- HotSpot internals ----------------------------------------------------

    namespace hotspot
    {
        // https://github.com/openjdk/jdk/blob/master/src/hotspot/share/runtime/vmStructs.hpp
        struct vm_type_entry_t
        {
            const char* type_name;
            const char* superclass_name;
            std::int32_t  is_oop_type_type;
            std::int32_t  is_integer_type;
            std::int32_t  is_unsigned;
            std::uint64_t size;
        };

        // https://github.com/openjdk/jdk/blob/master/src/hotspot/share/runtime/vmStructs.hpp
        struct vm_struct_entry_t
        {
            const char* type_name;
            const char* field_name;
            const char* type_string;
            std::int32_t  is_static;
            std::uint64_t offset;
            void* address;
        };

        /*
            @brief Returns the module handle of the HotSpot JVM library loaded in the process.
            @details
            Returns the handle for jvm.dll on Windows, or libjvm.so on Linux.  Cached
            after the first lookup; if no JVM library is present the function returns
            nullptr (callers must check before dereferencing the result).
        */
        inline auto get_jvm_module() noexcept
            -> vmhook::os::module_handle
        {
            static vmhook::os::module_handle module{ vmhook::os::find_jvm_module() };
            return module;
        }

        /*
            @brief Returns a pointer to the global array of HotSpot VM type entries.
            @details
            Resolves gHotSpotVMTypes from the JVM module via the OS dynamic-symbol
            API on first call and caches the typed pointer so subsequent calls are free.
        */
        inline auto get_vm_types() noexcept
            -> vmhook::hotspot::vm_type_entry_t*
        {
            static vmhook::hotspot::vm_type_entry_t* pointer{ []() noexcept
                -> vmhook::hotspot::vm_type_entry_t*
                {
                    void* const procedure_address{ vmhook::os::get_proc_address(
                        vmhook::hotspot::get_jvm_module(), "gHotSpotVMTypes") };
                    if (!procedure_address)
                    {
                        return nullptr;
                    }
                    return *reinterpret_cast<vmhook::hotspot::vm_type_entry_t**>(procedure_address);
                }() };
            return pointer;
        }

        /*
            @brief Returns a pointer to the global array of HotSpot VM struct entries.
            @details
            Resolves gHotSpotVMStructs from the JVM module via the OS dynamic-symbol
            API on first call and caches the typed pointer so subsequent calls are free.
        */
        inline auto get_vm_structs() noexcept
            -> vmhook::hotspot::vm_struct_entry_t*
        {
            static vmhook::hotspot::vm_struct_entry_t* pointer{ []() noexcept
                -> vmhook::hotspot::vm_struct_entry_t*
                {
                    void* const procedure_address{ vmhook::os::get_proc_address(
                        vmhook::hotspot::get_jvm_module(), "gHotSpotVMStructs") };
                    if (!procedure_address)
                    {
                        return nullptr;
                    }
                    return *reinterpret_cast<vmhook::hotspot::vm_struct_entry_t**>(procedure_address);
                }() };
            return pointer;
        }

        /*
            @brief Searches the gHotSpotVMTypes array for a type entry by name.
        */
        static auto iterate_type_entries(const char* const type_name) noexcept
            -> vmhook::hotspot::vm_type_entry_t*
        {
            for (vmhook::hotspot::vm_type_entry_t* entry{ vmhook::hotspot::get_vm_types() }; entry && entry->type_name; ++entry)
            {
                if (!std::strcmp(entry->type_name, type_name))
                {
                    return entry;
                }
            }
            return nullptr;
        }

        /*
            @brief Searches the gHotSpotVMStructs array for a field entry by type and field name.
        */
        static auto iterate_struct_entries(const char* const type_name, const char* const field_name) noexcept
            -> vmhook::hotspot::vm_struct_entry_t*
        {
            for (vmhook::hotspot::vm_struct_entry_t* entry{ vmhook::hotspot::get_vm_structs() }; entry && entry->type_name; ++entry)
            {
                if (!std::strcmp(entry->type_name, type_name) && !std::strcmp(entry->field_name, field_name))
                {
                    return entry;
                }
            }
            return nullptr;
        }

        /*
            @brief Checks whether a pointer refers to committed readable memory.
            @details
            Extends is_valid_pointer with an OS-region query to verify that the memory
            region containing pointer is actually committed and readable.  Implementation
            uses VirtualQuery on Windows and /proc/self/maps on Linux via os::query_region.
        */
        static auto is_readable_pointer(const void* const pointer) noexcept
            -> bool
        {
            const std::uintptr_t addr{ reinterpret_cast<std::uintptr_t>(pointer) };

            if (addr <= vmhook::os::user_address_floor
                || addr >= vmhook::os::user_address_ceiling
                || (addr & 0x7) != 0)
            {
                return false;
            }

            const vmhook::os::region_info info{ vmhook::os::query_region(pointer) };
            return info.committed && info.readable && !info.guarded;
        }

        /*
            @brief Checks whether a pointer is likely valid for dereferencing.
            @details
            Filters out null pointers, low sentinel values used by HotSpot to mark
            the end of linked lists, and kernel-space addresses above the user ceiling.
        */
        inline static auto is_valid_pointer(const void* const pointer) noexcept
            -> bool
        {
            const std::uintptr_t addr{ reinterpret_cast<std::uintptr_t>(pointer) };
            return addr > vmhook::os::user_address_floor && addr < vmhook::os::user_address_ceiling;
        }

        /*
            @brief Removes GC tag bits from a HotSpot pointer to recover the real address.
            @details
            Masking with user_address_ceiling strips high GC tag bits and recovers the
            underlying canonical user-space address.
        */
        inline static auto untag_pointer(const void* const pointer) noexcept
            -> const void*
        {
            return reinterpret_cast<const void*>(
                reinterpret_cast<std::uintptr_t>(pointer) & vmhook::os::user_address_ceiling);
        }

        /*
            @brief Reads a 32-bit pointer field from a JVM structure and zero-extends it.
        */
        template<typename structure_type>
        inline static auto read_pointer(const void* const base, const std::uint64_t offset) noexcept
            -> structure_type*
        {
            const std::uint32_t raw{ *reinterpret_cast<const std::uint32_t*>(reinterpret_cast<const std::uint8_t*>(base) + offset) };
            return reinterpret_cast<structure_type*>(static_cast<std::uintptr_t>(raw));
        }

        /*
            @brief Safely reads a pointer value from a memory address without faulting.
            @details
            Uses os::safe_read which maps to ReadProcessMemory on Windows and a
            fault-safe path on Linux.  Pre-checks filter out null, low, non-canonical,
            and unaligned addresses before crossing the OS boundary.
        */
        static auto safe_read_pointer(const void* const pointer) noexcept
            -> const void*
        {
            if (!pointer)
            {
                return nullptr;
            }

            const std::uintptr_t addr{ reinterpret_cast<std::uintptr_t>(pointer) };

            if (addr <= vmhook::os::user_address_floor
                || addr >= vmhook::os::user_address_ceiling
                || (addr & 0x7) != 0)
            {
                return nullptr;
            }

            const void* result{ nullptr };
            if (!vmhook::os::safe_read(&result, pointer, sizeof(result)))
            {
                return nullptr;
            }

            return result;
        }

        /*
            @brief Represents a HotSpot internal Symbol object.
            @details
            Symbols are interned strings used throughout the JVM to represent class names,
            method names, field names, and type signatures. The layout is resolved at
            runtime via gHotSpotVMStructs using the offsets of Symbol._length and Symbol._body.
        */
        struct symbol
        {
            /*
                @brief Converts this HotSpot Symbol to a std::string.
                @return A std::string containing a copy of the symbol's character data,
                        or an empty string on failure.
            */
            auto to_string() const
                -> std::string
            {
                static const vmhook::hotspot::vm_struct_entry_t* const length_entry{ vmhook::hotspot::iterate_struct_entries("Symbol", "_length") };
                static const vmhook::hotspot::vm_struct_entry_t* const body_entry{ vmhook::hotspot::iterate_struct_entries("Symbol", "_body") };

                try
                {
                    if (!length_entry)
                    {
                        throw vmhook::exception{ "Failed to find Symbol._length entry." };
                    }

                    if (!body_entry)
                    {
                        throw vmhook::exception{ "Failed to find Symbol._body entry." };
                    }

                    if (!vmhook::hotspot::safe_read_pointer(this))
                    {
                        return std::string{};
                    }

                    const std::uint16_t length{ *reinterpret_cast<const std::uint16_t*>(reinterpret_cast<const std::uint8_t*>(this) + length_entry->offset) };
                    const char* const body{ reinterpret_cast<const char*>(reinterpret_cast<const std::uint8_t*>(this) + body_entry->offset) };

                    if (!vmhook::hotspot::is_valid_pointer(body) || length == 0 || length > 0x1000)
                    {
                        return std::string{};
                    }

                    return std::string{ body, length };
                }
                catch (const std::exception& exception)
                {
                    VMHOOK_LOG("{} symbol.to_string() {}", vmhook::error_tag, exception.what());
                    return std::string{};
                }
            }
        };

        /*
            @brief Represents a HotSpot internal ConstantPool object.
            @details
            The constant pool holds all constants referenced by a Java class.
            The layout is resolved at runtime via gHotSpotVMStructs using the size
            of the ConstantPool type to locate the base of the pool entries array.
        */
        struct constant_pool
        {
            /*
                @brief Returns a pointer to the base of the constant pool entries array.
                @details
                ConstantPool entries are stored immediately after the fixed-size ConstantPool
                header in memory (no separate heap allocation):
                  [ ConstantPool header (size from gHotSpotVMTypes) ][ entry[0] ][ entry[1] ] ...
                Entry indices are 1-based: index 0 is unused, valid indices start at 1.
                Each entry is pointer-sized (8 bytes on x64): it may hold a Symbol*, a primitive
                constant, or other data depending on the tag byte in _tags.
                The size of the header is read at runtime from gHotSpotVMTypes so this works
                across all JDK versions regardless of header size changes.
            */
            auto get_base() const
                -> void**
            {
                static const vmhook::hotspot::vm_type_entry_t* const entry{ vmhook::hotspot::iterate_type_entries("ConstantPool") };

                try
                {
                    if (!entry)
                    {
                        throw vmhook::exception{ "Failed to find ConstantPool entry." };
                    }

                    return reinterpret_cast<void**>(reinterpret_cast<std::uint8_t*>(const_cast<vmhook::hotspot::constant_pool*>(this)) + entry->size);
                }
                catch (const std::exception& exception)
                {
                    VMHOOK_LOG("{} constant_pool.get_base() {}", vmhook::error_tag, exception.what());
                    return nullptr;
                }
            }
        };

        /*
            @brief Represents a HotSpot internal ConstMethod object.
            @details
            ConstMethod holds the immutable metadata of a Java method, including its name,
            signature, and a reference to the owning class constant pool.
            The layout is resolved at runtime via gHotSpotVMStructs.
        */
        struct const_method
        {
            /*
                @brief Returns a pointer to the constant pool of the owning class.
            */
            auto get_constants() const
                -> vmhook::hotspot::constant_pool*
            {
                static const vmhook::hotspot::vm_struct_entry_t* const entry{ vmhook::hotspot::iterate_struct_entries("ConstMethod", "_constants") };

                try
                {
                    if (!entry)
                    {
                        throw vmhook::exception{ "Failed to find ConstMethod._constants entry." };
                    }

                    return *reinterpret_cast<constant_pool**>(reinterpret_cast<std::uint8_t*>(const_cast<vmhook::hotspot::const_method*>(this)) + entry->offset);
                }
                catch (const std::exception& exception)
                {
                    VMHOOK_LOG("{} const_method.get_constants() {}", vmhook::error_tag, exception.what());
                    return nullptr;
                }
            }

            /*
                @brief Returns the symbol representing the name of this method.
            */
            auto get_name() const
                -> vmhook::hotspot::symbol*
            {
                static const vmhook::hotspot::vm_struct_entry_t* const entry{ vmhook::hotspot::iterate_struct_entries("ConstMethod", "_name_index") };

                try
                {
                    if (!entry)
                    {
                        throw vmhook::exception{ "Failed to find ConstMethod._name_index entry." };
                    }

                    const std::uint16_t index{ *reinterpret_cast<std::uint16_t*>(reinterpret_cast<std::uint8_t*>(const_cast<vmhook::hotspot::const_method*>(this)) + entry->offset) };
                    return reinterpret_cast<vmhook::hotspot::symbol*>(get_constants()->get_base()[index]);
                }
                catch (const std::exception& exception)
                {
                    VMHOOK_LOG("{} const_method.get_name() {}", vmhook::error_tag, exception.what());
                    return nullptr;
                }
            }

            /*
                @brief Returns the symbol representing the signature of this method.
            */
            auto get_signature() const
                -> vmhook::hotspot::symbol*
            {
                static const vmhook::hotspot::vm_struct_entry_t* const entry{ vmhook::hotspot::iterate_struct_entries("ConstMethod", "_signature_index") };

                try
                {
                    if (!entry)
                    {
                        throw vmhook::exception{ "Failed to find ConstMethod._signature_index entry." };
                    }

                    const std::uint16_t index{ *reinterpret_cast<std::uint16_t*>(reinterpret_cast<std::uint8_t*>(const_cast<vmhook::hotspot::const_method*>(this)) + entry->offset) };
                    return reinterpret_cast<vmhook::hotspot::symbol*>(get_constants()->get_base()[index]);
                }
                catch (const std::exception& exception)
                {
                    VMHOOK_LOG("{} const_method.get_signature() {}", vmhook::error_tag, exception.what());
                    return nullptr;
                }
            }
        };

        /*
            @brief Represents a HotSpot internal Method object.
            @details
            The Method object is the JVM's internal representation of a Java method.
            It holds all runtime metadata needed to invoke, compile, and profile a method,
            including its entry points, access flags, compilation flags, and a pointer
            to its immutable ConstMethod.
            The layout is resolved at runtime via gHotSpotVMStructs.
        */
        struct method
        {
            /*
                @brief Returns the interpreter-to-interpreter entry point of this method.
                @details
                The i2i entry is the native code stub invoked when an interpreted method
                calls another interpreted method. It is used as the hook location target
                in midi2i_hook.
            */
            auto get_i2i_entry() const
                -> void*
            {
                static const vmhook::hotspot::vm_struct_entry_t* const entry{ vmhook::hotspot::iterate_struct_entries("Method", "_i2i_entry") };

                try
                {
                    if (!entry)
                    {
                        throw vmhook::exception{ "Failed to find Method._i2i_entry entry." };
                    }

                    return *reinterpret_cast<void**>(reinterpret_cast<std::uint8_t*>(const_cast<vmhook::hotspot::method*>(this)) + entry->offset);
                }
                catch (const std::exception& exception)
                {
                    VMHOOK_LOG("{} method.get_i2i_entry() {}", vmhook::error_tag, exception.what());
                    return nullptr;
                }
            }

            /*
                @brief Returns the from-interpreted entry point of this method.
            */
            auto get_from_interpreted_entry() const
                -> void*
            {
                static const vmhook::hotspot::vm_struct_entry_t* const entry{ vmhook::hotspot::iterate_struct_entries("Method", "_from_interpreted_entry") };

                try
                {
                    if (!entry)
                    {
                        throw vmhook::exception{ "Failed to find Method._from_interpreted_entry entry." };
                    }

                    return *reinterpret_cast<void**>(reinterpret_cast<std::uint8_t*>(const_cast<vmhook::hotspot::method*>(this)) + entry->offset);
                }
                catch (const std::exception& exception)
                {
                    VMHOOK_LOG("{} method.get_from_interpreted_entry() {}", vmhook::error_tag, exception.what());
                    return nullptr;
                }
            }

            /*
                @brief Returns a pointer to the access flags of this method.
                @details
                Access flags encode Java visibility modifiers (public, private, static, etc.)
                as well as JVM-internal flags such as NO_COMPILE.
            */
            auto get_access_flags() const
                -> std::uint32_t*
            {
                static const vmhook::hotspot::vm_struct_entry_t* const entry{ vmhook::hotspot::iterate_struct_entries("Method", "_access_flags") };

                try
                {
                    if (!entry)
                    {
                        throw vmhook::exception{ "Failed to find Method._access_flags entry." };
                    }

                    return reinterpret_cast<std::uint32_t*>(reinterpret_cast<std::uint8_t*>(const_cast<vmhook::hotspot::method*>(this)) + entry->offset);
                }
                catch (const std::exception& exception)
                {
                    VMHOOK_LOG("{} method.get_access_flags() {}", vmhook::error_tag, exception.what());
                    return nullptr;
                }
            }

            /*
                @brief Returns a pointer to the internal method flags of this method.
                @details
                These flags encode HotSpot-internal method properties such as _dont_inline.
            */
            auto get_flags() const
                -> std::uint16_t*
            {
                static const vmhook::hotspot::vm_struct_entry_t* const entry{ vmhook::hotspot::iterate_struct_entries("Method", "_flags") };

                if (!entry)
                {
                    return nullptr;
                }

                return reinterpret_cast<std::uint16_t*>(reinterpret_cast<std::uint8_t*>(const_cast<vmhook::hotspot::method*>(this)) + entry->offset);
            }

            /*
                @brief Returns a pointer to the ConstMethod of this method.
            */
            auto get_const_method() const
                -> vmhook::hotspot::const_method*
            {
                static const vmhook::hotspot::vm_struct_entry_t* const entry{ vmhook::hotspot::iterate_struct_entries("Method", "_constMethod") };

                try
                {
                    if (!entry)
                    {
                        throw vmhook::exception{ "Failed to find Method._constMethod entry." };
                    }

                    return *reinterpret_cast<vmhook::hotspot::const_method**>(reinterpret_cast<std::uint8_t*>(const_cast<vmhook::hotspot::method*>(this)) + entry->offset);
                }
                catch (const std::exception& exception)
                {
                    VMHOOK_LOG("{} method.get_const_method() {}", vmhook::error_tag, exception.what());
                    return nullptr;
                }
            }

            /*
                @brief Returns the name of this method as a std::string.
            */
            auto get_name() const
                -> std::string
            {
                const vmhook::hotspot::const_method* const const_method_pointer{ this->get_const_method() };

                try
                {
                    if (!const_method_pointer)
                    {
                        throw vmhook::exception{ "ConstMethod is nullptr." };
                    }

                    const vmhook::hotspot::symbol* const symbol_pointer{ const_method_pointer->get_name() };
                    if (!symbol_pointer)
                    {
                        throw vmhook::exception{ "Symbol is nullptr." };
                    }

                    return symbol_pointer->to_string();
                }
                catch (const std::exception& exception)
                {
                    VMHOOK_LOG("{} method.get_name() {}", vmhook::error_tag, exception.what());
                    return std::string{};
                }
            }

            /*
                @brief Returns the signature of this method as a std::string.
            */
            auto get_signature() const
                -> std::string
            {
                const vmhook::hotspot::const_method* const const_method_pointer{ this->get_const_method() };

                try
                {
                    if (!const_method_pointer)
                    {
                        throw vmhook::exception{ "ConstMethod is nullptr." };
                    }

                    const vmhook::hotspot::symbol* const symbol_pointer{ const_method_pointer->get_signature() };
                    if (!symbol_pointer)
                    {
                        throw vmhook::exception{ "Symbol is nullptr." };
                    }

                    return symbol_pointer->to_string();
                }
                catch (const std::exception& exception)
                {
                    VMHOOK_LOG("{} method.get_signature() {}", vmhook::error_tag, exception.what());
                    return std::string{};
                }
            }

            /*
                @brief Returns the current nmethod pointer (_code field).
                @details Non-null when the method has been JIT-compiled; null when interpreted.
                         Writing null forces HotSpot dispatch to treat the method as uncompiled
                         without freeing the compiled code.
            */
            auto get_code() const noexcept
                -> void*
            {
                static const vmhook::hotspot::vm_struct_entry_t* const entry{ vmhook::hotspot::iterate_struct_entries("Method", "_code") };
                if (!entry)
                {
                    return nullptr;
                }

                return *reinterpret_cast<void**>(reinterpret_cast<std::uint8_t*>(const_cast<vmhook::hotspot::method*>(this)) + entry->offset);
            }

            /*
                @brief Overwrites the _code (nmethod) pointer for this method.
                @details
                Setting code to nullptr forces HotSpot to treat the method as
                interpreted even if a compiled version exists, without invalidating
                the nmethod.  Used during hook installation to suppress the
                compiled entry so that all calls route through the interpreter stub
                that we have already patched.

                Complexity: O(1).
                Exception safety: noexcept — writes a single pointer through a cached offset.

                @param code  New nmethod pointer, or nullptr to deoptimise.
            */
            auto set_code(void* const code) noexcept
                -> void
            {
                static const vmhook::hotspot::vm_struct_entry_t* const entry{ vmhook::hotspot::iterate_struct_entries("Method", "_code") };
                if (!entry)
                {
                    return;
                }

                *reinterpret_cast<void**>(reinterpret_cast<std::uint8_t*>(this) + entry->offset) = code;
            }

            /*
                @brief Overwrites _from_interpreted_entry.
                @details Reset to _i2i_entry during deoptimisation so interpreted callers
                         route through the interpreter entry stub (which we have patched).
            */
            auto set_from_interpreted_entry(void* const entry_point) noexcept
                -> void
            {
                static const vmhook::hotspot::vm_struct_entry_t* const entry{ vmhook::hotspot::iterate_struct_entries("Method", "_from_interpreted_entry") };
                if (!entry)
                {
                    return;
                }

                *reinterpret_cast<void**>(reinterpret_cast<std::uint8_t*>(this) + entry->offset) = entry_point;
            }

            /*
                @brief Returns the from-compiled entry point.
                @details The VMStruct field name changed across JDK versions:
                         JDK <= 20: "_from_compiled_code_entry_point"
                         JDK 21+:   "_from_compiled_entry"
                         Both names are tried at first call; the result is cached.
            */
            auto get_from_compiled_entry() const noexcept
                -> void*
            {
                static const vmhook::hotspot::vm_struct_entry_t* const entry{ []() noexcept
                    -> const vmhook::hotspot::vm_struct_entry_t*
                        {
                            const vmhook::hotspot::vm_struct_entry_t* found_entry{ vmhook::hotspot::iterate_struct_entries("Method", "_from_compiled_code_entry_point") };
                            if (!found_entry)
                            {
                                found_entry = vmhook::hotspot::iterate_struct_entries("Method", "_from_compiled_entry");
                            }

                            return found_entry;
                        }()
                };
                if (!entry)
                {
                    return nullptr;
                }

                return *reinterpret_cast<void**>(reinterpret_cast<std::uint8_t*>(const_cast<vmhook::hotspot::method*>(this)) + entry->offset);
            }

            /*
                @brief Overwrites the from-compiled entry point.
                @details Set to the c2i adapter during deoptimisation so compiled callers
                         transition to the interpreter (and reach our patched i2i stub).
            */
            auto set_from_compiled_entry(void* const entry_point) noexcept
                -> void
            {
                static const vmhook::hotspot::vm_struct_entry_t* const entry{ []() noexcept
                    -> const vmhook::hotspot::vm_struct_entry_t*
                    {
                        const vmhook::hotspot::vm_struct_entry_t* found_entry{ vmhook::hotspot::iterate_struct_entries("Method", "_from_compiled_code_entry_point") };
                        if (!found_entry)
                        {
                            found_entry = vmhook::hotspot::iterate_struct_entries("Method", "_from_compiled_entry");
                        }

                        return found_entry;
                    }()
                };
                if (!entry)
                {
                    return;
                }

                *reinterpret_cast<void**>(reinterpret_cast<std::uint8_t*>(this) + entry->offset) = entry_point;
            }

            /*
                @brief Returns the AdapterHandlerEntry* (_adapter field).
                @details Stores the calling-convention adapters (i2c / c2i) for this method.
                         Used to obtain the c2i entry when deoptimising a compiled method.
            */
            auto get_adapter() const noexcept
                -> void*
            {
                static const vmhook::hotspot::vm_struct_entry_t* const entry{ vmhook::hotspot::iterate_struct_entries("Method", "_adapter") };
                if (!entry)
                {
                    return nullptr;
                }

                return *reinterpret_cast<void**>(reinterpret_cast<std::uint8_t*>(const_cast<vmhook::hotspot::method*>(this)) + entry->offset);
            }
        };

        /*
            @brief Describes a Java field discovered from InstanceKlass._fields.
            @details
            - offset     Byte offset of the field's value within the object (instance fields)
                         or within the java.lang.Class mirror object (static fields).
            - is_static  true when JVM_ACC_STATIC (0x0008) is set on the field.
            - signature  JVM type descriptor, e.g. "I", "J", "Z", "Ljava/lang/String;"
        */
        struct field_entry_t
        {
            std::uint32_t offset;
            bool          is_static;
            std::string   signature;
        };

        /*
            @brief Represents a HotSpot internal Klass object.
            @details
            Klass is the JVM's internal representation of a Java class or interface.
            The layout is resolved at runtime via gHotSpotVMStructs.
        */
        struct klass
        {
            /*
                @brief Returns the symbol representing the internal name of this class.
                @return Pointer to the symbol containing the class name using '/' separators,
                        or nullptr on failure.
            */
            auto get_name() const
                -> vmhook::hotspot::symbol*
            {
                static const vmhook::hotspot::vm_struct_entry_t* const entry{ vmhook::hotspot::iterate_struct_entries("Klass", "_name") };

                try
                {
                    if (!entry)
                    {
                        throw vmhook::exception{ "Failed to find Klass._name entry." };
                    }

                    if (!vmhook::hotspot::is_valid_pointer(this))
                    {
                        return nullptr;
                    }

                    const void* const raw{ vmhook::hotspot::safe_read_pointer(reinterpret_cast<const std::uint8_t*>(this) + entry->offset) };
                    vmhook::hotspot::symbol* const symbol_pointer{ reinterpret_cast<vmhook::hotspot::symbol*>(const_cast<void*>(vmhook::hotspot::untag_pointer(raw))) };

                    return vmhook::hotspot::is_valid_pointer(symbol_pointer) ? symbol_pointer : nullptr;
                }
                catch (const std::exception& exception)
                {
                    VMHOOK_LOG("{} klass.get_name() {}", vmhook::error_tag, exception.what());
                    return nullptr;
                }
            }

            /*
                @brief Returns the next sibling klass in the ClassLoaderData klass linked list.
            */
            auto get_next_link() const
                -> vmhook::hotspot::klass*
            {
                static const vmhook::hotspot::vm_struct_entry_t* const entry{ vmhook::hotspot::iterate_struct_entries("Klass", "_next_link") };

                if (!entry)
                {
                    throw vmhook::exception{ "Failed to find Klass._next_link entry." };
                }

                if (!vmhook::hotspot::is_valid_pointer(this))
                {
                    return nullptr;
                }

                vmhook::hotspot::klass* const next{ *reinterpret_cast<vmhook::hotspot::klass**>(reinterpret_cast<std::uint8_t*>(const_cast<vmhook::hotspot::klass*>(this)) + entry->offset) };

                return vmhook::hotspot::is_valid_pointer(next) ? next : nullptr;
            }

            /*
                @brief Returns the number of methods declared by this InstanceKlass.
                @details
                Reads InstanceKlass._methods via its VMStruct offset, then reads
                the Array<Method*>::_length field at offset 0 of the array.
                @note This klass* must be an InstanceKlass* (i.e. a regular Java class).
            */
            auto get_methods_count() const noexcept
                -> std::int32_t
            {
                static const vmhook::hotspot::vm_struct_entry_t* const entry{ vmhook::hotspot::iterate_struct_entries("InstanceKlass", "_methods") };

                if (!entry || !vmhook::hotspot::is_valid_pointer(this))
                {
                    return 0;
                }

                void* const array{ *reinterpret_cast<void**>(reinterpret_cast<std::uint8_t*>(const_cast<klass*>(this)) + entry->offset) };

                if (!vmhook::hotspot::is_valid_pointer(array))
                {
                    return 0;
                }

                return *reinterpret_cast<std::int32_t*>(array);
            }

            /*
                @brief Returns a pointer to the first Method* in this InstanceKlass's methods array.
                @details
                Reads InstanceKlass._methods via its VMStruct offset. The Array<Method*> layout
                in HotSpot is: [int _length (4 bytes)] [4 bytes padding] [Method* _data[...]].
                Data starts at offset 8 from the array base pointer.
                @note This klass* must be an InstanceKlass* (i.e. a regular Java class).
            */
            auto get_methods_ptr() const noexcept
                -> vmhook::hotspot::method**
            {
                static const vmhook::hotspot::vm_struct_entry_t* const entry{ vmhook::hotspot::iterate_struct_entries("InstanceKlass", "_methods") };

                if (!entry || !vmhook::hotspot::is_valid_pointer(this))
                {
                    return nullptr;
                }

                void* const array{ *reinterpret_cast<void**>(reinterpret_cast<std::uint8_t*>(const_cast<vmhook::hotspot::klass*>(this)) + entry->offset) };

                if (!vmhook::hotspot::is_valid_pointer(array))
                {
                    return nullptr;
                }

                // HotSpot Array<T> layout on x64:
                //   +0  int32_t _length   (4 bytes)
                //   +4  int32_t _padding  (4 bytes alignment padding)
                //   +8  T       _data[0]   first element starts here
                return reinterpret_cast<vmhook::hotspot::method**>(reinterpret_cast<std::uint8_t*>(array) + 8);
            }

            /*
                @brief Returns the java.lang.Class mirror object associated with this klass.
                @details
                Reads Klass::_java_mirror, which is an OopHandle (introduced as such in JDK 17).
                An OopHandle wraps an oop* that points into an OopStorage slot.
                Dereferencing that slot yields the full 64-bit address of the
                java.lang.Class instance. Static field values are stored inside this mirror.
                @return Pointer to the java.lang.Class object, or nullptr on failure.
            */
            auto get_java_mirror() const noexcept
                -> void*
            {
                static const vmhook::hotspot::vm_struct_entry_t* const entry{ vmhook::hotspot::iterate_struct_entries("Klass", "_java_mirror") };

                if (!entry || !vmhook::hotspot::is_valid_pointer(this))
                {
                    return nullptr;
                }

                /*
                    The type and indirection level of _java_mirror changed across JDK versions.

                    JDK 8 through JDK 16:
                      _java_mirror is a plain `oop` (a direct 64-bit pointer to the
                      java.lang.Class object). One read is enough.
                      VMStructs type_string: "oop"

                    JDK 17+:
                      _java_mirror was changed to an OopHandle:
                        struct OopHandle { oop* _obj; };
                      The value at klass+offset is an oop* pointing into an OopStorage
                      arena. Dereferencing that slot yields the actual java.lang.Class oop.
                      VMStructs type_string: "OopHandle"

                    Detection: inspect entry->type_string at runtime so we stay
                    version-agnostic without any hardcoded JDK version numbers.
                */
                static const bool is_oop_handle{ entry->type_string && std::strcmp(entry->type_string, "OopHandle") == 0 };

                const void* const field_addr{ reinterpret_cast<const std::uint8_t*>(this) + entry->offset };

                if (is_oop_handle)
                {
                    // JDK 17+: two-level indirection through OopStorage.
                    //   klass + offset -> OopHandle._obj  (oop* into OopStorage)
                    //   *OopHandle._obj -> java.lang.Class oop  (full 64-bit, not compressed)
                    const void* const oop_storage_slot{ vmhook::hotspot::safe_read_pointer(field_addr) };
                    if (!vmhook::hotspot::is_valid_pointer(oop_storage_slot))
                    {
                        return nullptr;
                    }

                    return const_cast<void*>(vmhook::hotspot::safe_read_pointer(oop_storage_slot));
                }
                else
                {
                    // JDK 8 through JDK 16: the field is a direct full-width oop pointer.
                    // Read it as a 64-bit value; no further dereference needed.
                    const void* const mirror{ vmhook::hotspot::safe_read_pointer(field_addr) };
                    return vmhook::hotspot::is_valid_pointer(mirror) ? const_cast<void*>(mirror) : nullptr;
                }
            }

            /*
                @brief Returns the super-klass of this klass, or nullptr for java.lang.Object.
            */
            auto get_super() const noexcept -> klass*
            {
                static const vmhook::hotspot::vm_struct_entry_t* const entry{
                    vmhook::hotspot::iterate_struct_entries("Klass", "_super") };
                if (!entry || !vmhook::hotspot::is_valid_pointer(this))
                {
                    return nullptr;
                }
                auto* const super{
                    *reinterpret_cast<klass**>(reinterpret_cast<std::uint8_t*>(
                        const_cast<klass*>(this)) + entry->offset) };
                return vmhook::hotspot::is_valid_pointer(super) ? super : nullptr;
            }

            /*
                @brief Returns the size in bytes of one instance of this class.
                @details
                Reads Klass._layout_helper, which encodes instance layout information.
                For normal instance classes the layout helper is a positive integer
                whose value (with the low tag bit masked off) is the instance size in
                bytes.  A value of zero or negative indicates an array klass or an
                abstract / interface class that cannot be instantiated directly.

                Complexity: O(1) after the VMStruct offset is cached on first call.
                Exception safety: noexcept — returns 0 on any failure.

                @return Size in bytes of one heap-allocated instance, or 0 if the klass
                        is not a normal instance class.
            */
            auto get_instance_size() const noexcept
                -> std::size_t
            {
                static const vmhook::hotspot::vm_struct_entry_t* const entry{ vmhook::hotspot::iterate_struct_entries("Klass", "_layout_helper") };

                if (!entry || !vmhook::hotspot::is_valid_pointer(this))
                {
                    return 0;
                }

                const std::int32_t layout_helper{ *reinterpret_cast<const std::int32_t*>(reinterpret_cast<const std::uint8_t*>(this) + entry->offset) };
                if (layout_helper <= 0)
                {
                    return 0;
                }

                // Bit 0 of _layout_helper is a tag; mask it off to get the raw byte size.
                return static_cast<std::size_t>(layout_helper & ~1);
            }

            /*
                @brief Returns the mark-word prototype stored in this klass.
                @details
                Klass._prototype_header holds the template mark-word that HotSpot stamps
                into every freshly-allocated instance.  It encodes the default biased-locking
                epoch and the klass identity hash pattern before any actual locking occurs.
                Used during object allocation to initialise the mark word without
                a separate heap lookup.

                Complexity: O(1) after the VMStruct offset is cached on first call.
                Exception safety: noexcept — returns 1 (neutral mark word) on any failure.

                @return The prototype mark-word value, or 1 on failure.
            */
            auto get_prototype_header() const noexcept
                -> std::uintptr_t
            {
                static const vmhook::hotspot::vm_struct_entry_t* const entry{ vmhook::hotspot::iterate_struct_entries("Klass", "_prototype_header") };

                if (!entry || !vmhook::hotspot::is_valid_pointer(this))
                {
                    return 1;
                }

                return *reinterpret_cast<const std::uintptr_t*>(reinterpret_cast<const std::uint8_t*>(this) + entry->offset);
            }

            /*
                @brief Searches the InstanceKlass _fields array for a field by name.
                @param name The exact Java field name (e.g. "health", "x", "INSTANCE").
                @return A field_entry_t with offset, static flag, and type descriptor,
                        or std::nullopt when the field is not declared directly by this class.
                @details
                Parses InstanceKlass._fields, which is an Array<u2>.  Each field occupies
                exactly 6 consecutive u2 slots (FieldInfo::field_slots):
                  [0] access_flags  [1] name_index  [2] signature_index
                  [3] initval_index [4] low_packed   [5] high_packed
                The byte offset is recovered as: ((high_packed << 16) | low_packed) >> 2
                (FIELDINFO_TAG_SIZE = 2).
                Name and signature are resolved from the class constant pool.
                @note Searches only this class, not superclasses.  Walk the superclass
                      chain manually to locate inherited fields.
                @note Covers JDK 8 through JDK 21. JDK 22+ uses a different FieldInfoStream encoding.
            */
            /*
                @brief Decodes one UNSIGNED5 value from a byte stream (JDK 21+ FieldInfoStream).
                @details
                Algorithm (from src/hotspot/share/utilities/unsigned5.hpp):
                  sum = sum of (b_i - 1) * 64^i   for i = 0, 1, 2, ...
                  Stop after the first byte b_i where b_i < 192 (a "low byte").
                  Byte 0 is never emitted; it marks the stream End and returns ~0u.
            */
            inline static auto decode_u5(const std::uint8_t* data, int& stream_pos) noexcept
                -> std::uint32_t
            {
                std::uint32_t sum{ 0 };
                for (int byte_position{ 0 }; byte_position < 5; ++byte_position)
                {
                    const std::uint8_t current_byte{ data[stream_pos++] };
                    if (current_byte == 0)
                    {
                        --stream_pos;
                        return ~0u;  // End marker - never a valid value
                    }
                    sum += static_cast<std::uint32_t>(current_byte - 1) << (6 * byte_position);
                    if (current_byte < 192)
                    {
                        return sum;  // Low byte - sequence complete
                    }
                }
                return sum;
            }

            /*
                @brief Looks up a field by name from an InstanceKlass._fieldinfo_stream
                       (JDK 21+ FieldInfoStream format, Array<u1>).
                @details
                Stream grammar (from fieldInfo.hpp):
                  FieldInfoStream := j(num_java) k(num_injected) Field[j+k] End(0)
                  Field := name_idx sig_idx offset access_flags field_flags
                           [initval_idx  if field_flags & 0x01]
                           [gsig_idx     if field_flags & 0x04]
                           [group        if field_flags & 0x10]
                All integers encoded with UNSIGNED5 (see decode_u5).
            */
            auto find_field_in_stream(const std::string_view name, void** constant_pool_base) const noexcept
                -> std::optional<vmhook::hotspot::field_entry_t>
            {
                static const vmhook::hotspot::vm_struct_entry_t* const fis_entry{ vmhook::hotspot::iterate_struct_entries("InstanceKlass", "_fieldinfo_stream") };

                if (!fis_entry || !vmhook::hotspot::is_valid_pointer(this) || !vmhook::hotspot::is_valid_pointer(constant_pool_base))
                {
                    return std::nullopt;
                }

                // Read Array<u1>* pointer from InstanceKlass
                const void* const arr_ptr{ *reinterpret_cast<void* const*>(reinterpret_cast<const std::uint8_t*>(this) + fis_entry->offset) };

                if (!vmhook::hotspot::is_valid_pointer(arr_ptr))
                {
                    return std::nullopt;
                }

                // Array<u1> layout on x64 HotSpot:
                //   +0  int32_t _length   (4 bytes)
                //   +4  u1      _data[0]   data starts here (no padding: u1 has alignment 1)
                // Note: Array<Method*> uses +8 because 8-byte pointers need 8-byte alignment,
                // requiring 4 bytes of padding after _length.  u1 and u2 arrays do NOT need this.
                const std::int32_t length{ *reinterpret_cast<const std::int32_t*>(arr_ptr) };
                if (length <= 0 || length > 0x4000)
                {
                    return std::nullopt;
                }

                const std::uint8_t* const data{ reinterpret_cast<const std::uint8_t*>(arr_ptr) + 4 };

                std::int32_t stream_pos{ 0 };

                // Header: number of Java-declared fields, then number of VM-injected fields.
                const std::uint32_t num_java_fields{ decode_u5(data, stream_pos) };
                const std::uint32_t num_injected_fields{ decode_u5(data, stream_pos) };
                if (num_java_fields == ~0u || num_injected_fields == ~0u || num_java_fields + num_injected_fields > 4096u)
                {
                    return std::nullopt;
                }

                for (std::uint32_t field_index{ 0 }; field_index < num_java_fields + num_injected_fields && stream_pos < length; ++field_index)
                {
                    const std::uint32_t name_index{ decode_u5(data, stream_pos) };
                    if (name_index == ~0u)
                    {
                        break;
                    }

                    const std::uint32_t sig_index{ decode_u5(data, stream_pos) };
                    const std::uint32_t field_offset{ decode_u5(data, stream_pos) };
                    const std::uint32_t access_flags{ decode_u5(data, stream_pos) };
                    const std::uint32_t field_flags{ decode_u5(data, stream_pos) };

                    // Consume optional trailing entries whose presence is signalled by field_flags bits:
                    //   bit 0 (0x01): initval_index  - compile-time constant initialiser value
                    //   bit 2 (0x04): generic_sig_index - generic type signature (e.g. List<T>)
                    //   bit 4 (0x10): contended_group - @Contended padding group id
                    if (field_flags & 0x01u)
                    {
                        vmhook::hotspot::klass::decode_u5(data, stream_pos);
                    }
                    if (field_flags & 0x04u)
                    {
                        vmhook::hotspot::klass::decode_u5(data, stream_pos);
                    }
                    if (field_flags & 0x10u)
                    {
                        vmhook::hotspot::klass::decode_u5(data, stream_pos);
                    }

                    // Resolve the field name from the constant pool and compare.
                    if (name_index && vmhook::hotspot::is_valid_pointer(constant_pool_base[name_index]))
                    {
                        const vmhook::hotspot::symbol* const name_symbol{ reinterpret_cast<const vmhook::hotspot::symbol*>(constant_pool_base[name_index]) };
                        if (vmhook::hotspot::is_valid_pointer(name_symbol) && name_symbol->to_string() == name)
                        {
                            const bool is_static{ (access_flags & 0x0008u) != 0u };
                            std::string signature;
                            if (sig_index && vmhook::hotspot::is_valid_pointer(constant_pool_base[sig_index]))
                            {
                                const vmhook::hotspot::symbol* const signature_symbol{ reinterpret_cast<const vmhook::hotspot::symbol*>(constant_pool_base[sig_index]) };
                                if (vmhook::hotspot::is_valid_pointer(signature_symbol))
                                {
                                    signature = signature_symbol->to_string();
                                }
                            }
                            return vmhook::hotspot::field_entry_t{ field_offset, is_static, signature };
                        }
                    }
                }
                return std::nullopt;
            }

            /*
                @brief Searches the InstanceKlass field metadata for a field by name.
                @details
                Automatically selects the correct field storage format based on what
                is exported in gHotSpotVMStructs for this JDK build:

                JDK 8 through early JDK 21:
                  InstanceKlass._fields -> Array<u2>, 6 slots per field
                  Byte offset = ((high_packed << 16) | low_packed) >> FIELDINFO_TAG_SIZE(2)

                JDK 21.0.x+ and JDK 22+:
                  InstanceKlass._fieldinfo_stream -> Array<u1>, UNSIGNED5 compressed
                  Stream grammar: j(num_java) k(num_injected) Field[j+k] End(0)
                  Per field: name_idx sig_idx offset access_flags field_flags [optionals]

                @note Searches only fields declared directly on this class.
                      Walk the superclass chain to find inherited fields.
            */
            auto find_field(const std::string_view name) const noexcept
                -> std::optional<vmhook::hotspot::field_entry_t>
            {
                static const vmhook::hotspot::vm_struct_entry_t* const fields_entry{ vmhook::hotspot::iterate_struct_entries("InstanceKlass", "_fields") };
                static const vmhook::hotspot::vm_struct_entry_t* const fis_entry{ vmhook::hotspot::iterate_struct_entries("InstanceKlass", "_fieldinfo_stream") };
                static const vmhook::hotspot::vm_struct_entry_t* const constants_entry{ vmhook::hotspot::iterate_struct_entries("InstanceKlass", "_constants") };

                if (!vmhook::hotspot::is_valid_pointer(this) || !constants_entry)
                {
                    return std::nullopt;
                }

                // Resolve constant pool (needed by both paths)
                vmhook::hotspot::constant_pool* const constant_pool_ptr{ *reinterpret_cast<vmhook::hotspot::constant_pool**>(reinterpret_cast<std::uint8_t*>(const_cast<vmhook::hotspot::klass*>(this)) + constants_entry->offset) };

                if (!vmhook::hotspot::is_valid_pointer(constant_pool_ptr))
                {
                    return std::nullopt;
                }

                void** const constant_pool_base{ constant_pool_ptr->get_base() };
                if (!vmhook::hotspot::is_valid_pointer(constant_pool_base))
                {
                    return std::nullopt;
                }

                // -- JDK 21+ path: FieldInfoStream ----------------------------
                if (fis_entry)
                {
                    return vmhook::hotspot::klass::find_field_in_stream(name, constant_pool_base);
                }

                // -- JDK 8 through JDK 17 path: Array<u2> with 6-slot FieldInfo records --
                if (!fields_entry)
                {
                    return std::nullopt;
                }

                void* const fields_array{ *reinterpret_cast<void**>(reinterpret_cast<std::uint8_t*>(const_cast<vmhook::hotspot::klass*>(this)) + fields_entry->offset) };

                if (!vmhook::hotspot::is_valid_pointer(fields_array))
                {
                    return std::nullopt;
                }

                // Array<u2>: int32_t _length at +0, data at +8
                const std::int32_t array_length{ *reinterpret_cast<const std::int32_t*>(fields_array) };

                static const std::int32_t field_slots{ 6 };

                // In JDK 8, the _fields Array<u2> may include a trailing u2
                // storing _java_fields_count after all the 6-slot field records.
                // Use integer division to safely ignore any trailing partial slot.
                if (array_length <= 0 || array_length < field_slots)
                {
                    return std::nullopt;
                }

                // Array<u2> layout on x64 HotSpot:
                //   +0  int32_t _length   (4 bytes)
                //   +4  u2      _data[0]   data starts here (u2 needs 2-byte alignment;
                //                           offset 4 is already 2-byte aligned, no padding needed)
                const std::uint16_t* const data{ reinterpret_cast<const std::uint16_t*>(reinterpret_cast<const std::uint8_t*>(fields_array) + 4) };

                // FieldInfo::field_slots layout for each field record (JDK 8 through JDK 20):
                //   slot 0  u16  access_flags   - JVM_ACC_* flags (static flag = bit 3 / 0x0008)
                //   slot 1  u16  name_index     - constant-pool index of the field's name Symbol
                //   slot 2  u16  signature_index- constant-pool index of the type descriptor Symbol
                //   slot 3  u16  initval_index  - cp index of compile-time constant value (or 0)
                //   slot 4  u16  low_packed     - bits [1:0] FIELDINFO_TAG, bits [15:2] offset_low
                //   slot 5  u16  high_packed    - offset_high (upper 16 bits of the packed offset)
                for (std::int32_t field_slot_index{ 0 }; field_slot_index < array_length / field_slots; ++field_slot_index)
                {
                    const std::uint16_t name_index{ data[field_slot_index * field_slots + 1] };
                    if (!name_index)
                    {
                        continue;  // slot 1: name_index == 0 means VM-injected field, skip
                    }

                    const vmhook::hotspot::symbol* const name_symbol{ reinterpret_cast<const vmhook::hotspot::symbol*>(constant_pool_base[name_index]) };
                    if (!vmhook::hotspot::is_valid_pointer(name_symbol) || name_symbol->to_string() != name)
                    {
                        continue;
                    }

                    const std::uint16_t access_flags{ data[field_slot_index * field_slots + 0] };  // slot 0
                    const std::uint16_t sig_index{ data[field_slot_index * field_slots + 2] };  // slot 2
                    const std::uint16_t low_packed{ data[field_slot_index * field_slots + 4] };  // slot 4
                    const std::uint16_t high_packed{ data[field_slot_index * field_slots + 5] };  // slot 5

                    // Reconstruct the byte offset from the packed representation:
                    //   packed = (high_packed << 16) | low_packed
                    //   offset = packed >> FIELDINFO_TAG_SIZE   (FIELDINFO_TAG_SIZE = 2)
                    // The 2 lowest bits of packed are the FIELDINFO_TAG and carry no offset data.
                    const std::uint32_t packed{ (static_cast<std::uint32_t>(high_packed) << 16) | low_packed };
                    const std::uint32_t offset{ packed >> 2 };

                    const bool is_static{ (access_flags & 0x0008u) != 0u };

                    const vmhook::hotspot::symbol* const signature_symbol{ reinterpret_cast<const vmhook::hotspot::symbol*>(constant_pool_base[sig_index]) };
                    const std::string signature{ vmhook::hotspot::is_valid_pointer(signature_symbol) ? signature_symbol->to_string() : std::string{} };

                    return vmhook::hotspot::field_entry_t{ offset, is_static, signature };
                }

                return std::nullopt;
            }
        };

        /*
            @brief Represents a HotSpot ClassLoaderData node.
            @details
            Each ClassLoader in the JVM has a corresponding ClassLoaderData that
            tracks every Klass it has loaded. The ClassLoaderData nodes are chained
            together via _next into a global linked list whose head is held by
            ClassLoaderDataGraph::_head.
        */
        struct class_loader_data
        {
            /*
                @brief Returns the head of the klass linked list for this classloader.
                @details
                Reads ClassLoaderData::_klasses using its offset from gHotSpotVMStructs.
            */
            auto get_klasses() const
                -> vmhook::hotspot::klass*
            {
                static const vmhook::hotspot::vm_struct_entry_t* const entry{ vmhook::hotspot::iterate_struct_entries("ClassLoaderData", "_klasses") };

                try
                {
                    if (!entry)
                    {
                        throw vmhook::exception{ "Failed to find ClassLoaderData._klasses entry." };
                    }

                    if (!vmhook::hotspot::is_valid_pointer(this))
                    {
                        return nullptr;
                    }

                    // _klasses is a Klass* (full 8-byte native pointer), not a compressed OOP.
                    vmhook::hotspot::klass* const result{ *reinterpret_cast<vmhook::hotspot::klass**>(reinterpret_cast<std::uint8_t*>(const_cast<vmhook::hotspot::class_loader_data*>(this)) + entry->offset) };

                    return vmhook::hotspot::is_valid_pointer(result) ? result : nullptr;
                }
                catch (const std::exception& exception)
                {
                    VMHOOK_LOG("{} class_loader_data.get_klasses() {}", vmhook::error_tag, exception.what());
                    return nullptr;
                }
            }

            /*
                @brief Returns the next ClassLoaderData node in the global linked list.
            */
            auto get_next() const
                -> vmhook::hotspot::class_loader_data*
            {
                static const vmhook::hotspot::vm_struct_entry_t* const entry{ iterate_struct_entries("ClassLoaderData", "_next") };

                try
                {
                    if (!entry)
                    {
                        throw vmhook::exception{ "Failed to find ClassLoaderData._next entry." };
                    }

                    if (!vmhook::hotspot::is_valid_pointer(this))
                    {
                        return nullptr;
                    }

                    vmhook::hotspot::class_loader_data* const next{ reinterpret_cast<vmhook::hotspot::class_loader_data*>(const_cast<void*>(vmhook::hotspot::safe_read_pointer(reinterpret_cast<const std::uint8_t*>(this) + entry->offset))) };

                    return vmhook::hotspot::is_valid_pointer(next) ? next : nullptr;
                }
                catch (const std::exception& exception)
                {
                    VMHOOK_LOG("{} class_loader_data.get_next() {}", vmhook::error_tag, exception.what());
                    return nullptr;
                }
            }

            /*
                @brief Returns the Dictionary associated with this classloader.
            */
            auto get_dictionary() const
                -> vmhook::hotspot::dictionary*
            {
                static const vmhook::hotspot::vm_struct_entry_t* const entry{ vmhook::hotspot::iterate_struct_entries("ClassLoaderData", "_dictionary") };

                try
                {
                    if (!entry)
                    {
                        return nullptr;
                    }

                    if (!vmhook::hotspot::is_valid_pointer(this))
                    {
                        return nullptr;
                    }

                    vmhook::hotspot::dictionary* const dict{ reinterpret_cast<vmhook::hotspot::dictionary*>(const_cast<void*>(vmhook::hotspot::safe_read_pointer(reinterpret_cast<const std::uint8_t*>(this) + entry->offset))) };

                    return vmhook::hotspot::is_valid_pointer(dict) ? dict : nullptr;
                }
                catch (const std::exception& exception)
                {
                    VMHOOK_LOG("{} class_loader_data.get_dictionary() {}", vmhook::error_tag, exception.what());
                    return nullptr;
                }
            }
        };

        /*
            @brief Represents a HotSpot Dictionary - the per-classloader class registry.
            @details
            Each ClassLoaderData owns a Dictionary which is a hashtable mapping class
            names to their corresponding Klass* objects. It inherits from
            BasicHashtable<mtInternal> whose layout is:
            - offset 0: _table_size (int32) - number of buckets
            - offset 8: _buckets (HashtableBucket*) - pointer to the bucket array
            Each bucket slot holds a pointer to the head of a linked list of
            DictionaryEntry nodes. Each DictionaryEntry has:
            - offset 0:  _next (DictionaryEntry*) - next entry in the chain
            - offset 8:  _hash (uint32)
            - offset 16: _literal (Klass*) - the actual class
        */
        struct dictionary
        {
            /*
                @brief Returns the number of hash buckets in this dictionary.
                @details
                BasicHashtable<mtInternal> layout (x64):
                  +0  int32_t _table_size  - number of buckets at this offset
                  +4  (4 bytes alignment padding)
                  +8  HashtableBucket* _buckets
            */
            inline auto get_table_size() const noexcept
                -> std::int32_t
            {
                // _table_size is the first field of BasicHashtable at offset 0.
                return *reinterpret_cast<const std::int32_t*>(this);
            }

            /*
                @brief Returns a pointer to the bucket array of this dictionary.
                @details
                _buckets is the second field of BasicHashtable at offset +8 (after
                _table_size int32 + 4 bytes padding on x64).
                Each element is a pointer to the head of a singly-linked DictionaryEntry chain.
            */
            inline auto get_buckets() const noexcept
                -> const std::uint8_t*
            {
                // +8 bytes = sizeof(int32_t _table_size) + 4 bytes alignment padding.
                return *reinterpret_cast<const std::uint8_t* const*>(reinterpret_cast<const std::uint8_t*>(this) + 8);
            }

            /*
                @brief Searches this dictionary for a klass by its internal name.
            */
            auto find_klass(const std::string_view class_name) const
                -> vmhook::hotspot::klass*
            {
                const std::int32_t table_size{ this->get_table_size() };
                const std::uint8_t* const buckets{ this->get_buckets() };

                if (!vmhook::hotspot::is_valid_pointer(buckets) || table_size <= 0 || table_size > 0x186A0)
                {
                    return nullptr;
                }

                for (std::int32_t bucket_index{ 0 }; bucket_index < table_size; ++bucket_index)
                {
                    const std::uint8_t* dict_entry{ reinterpret_cast<const std::uint8_t*>(vmhook::hotspot::untag_pointer(vmhook::hotspot::safe_read_pointer(buckets + bucket_index * 8))) };

                    while (vmhook::hotspot::is_valid_pointer(dict_entry))
                    {
                        // DictionaryEntry (extends HashtableEntry<InstanceKlass*, mtClass>) layout:
                        //   +0   void*       _next    (next entry in the chain)
                        //   +8   uint32_t    _hash    (pre-computed hash, 4 bytes + 4 padding)
                        //   +16  Klass*      _literal, the actual class pointer
                        const void* const raw_klass{ vmhook::hotspot::safe_read_pointer(dict_entry + 16) };
                        const vmhook::hotspot::klass* const candidate_klass{ reinterpret_cast<const vmhook::hotspot::klass*>(vmhook::hotspot::untag_pointer(raw_klass)) };

                        if (vmhook::hotspot::is_valid_pointer(candidate_klass))
                        {
                            const vmhook::hotspot::symbol* const sym{ candidate_klass->get_name() };
                            if (vmhook::hotspot::is_valid_pointer(sym) && sym->to_string() == class_name)
                            {
                                return const_cast<vmhook::hotspot::klass*>(candidate_klass);
                            }
                        }

                        dict_entry = reinterpret_cast<const std::uint8_t*>(vmhook::hotspot::untag_pointer(vmhook::hotspot::safe_read_pointer(dict_entry)));
                    }
                }

                return nullptr;
            }
        };

        /*
            @brief Represents the HotSpot ClassLoaderDataGraph - the global registry of all classloaders.
            @details
            ClassLoaderDataGraph::_head is a static field holding the head of a global
            linked list of ClassLoaderData nodes, one per classloader registered in the JVM.
        */
        struct class_loader_data_graph
        {
            /*
                @brief Returns the head of the global ClassLoaderData linked list.
            */
            auto get_head() const
                -> vmhook::hotspot::class_loader_data*
            {
                static const vmhook::hotspot::vm_struct_entry_t* const entry{ vmhook::hotspot::iterate_struct_entries("ClassLoaderDataGraph", "_head") };

                try
                {
                    if (!entry)
                    {
                        throw vmhook::exception{ "Failed to find ClassLoaderDataGraph._head entry." };
                    }

                    vmhook::hotspot::class_loader_data* const head{ *reinterpret_cast<vmhook::hotspot::class_loader_data* const*>(entry->address) };

                    return vmhook::hotspot::is_valid_pointer(head) ? head : nullptr;
                }
                catch (const std::exception& exception)
                {
                    VMHOOK_LOG("{} class_loader_data_graph.get_head() {}", vmhook::error_tag, exception.what());
                    return nullptr;
                }
            }

            /*
                @brief Searches all loaded classloaders for a klass by its internal name.
                @param class_name The internal JVM class name using '/' separators
                                  (e.g. "java/lang/String", "net/minecraft/client/Minecraft").
                @return Pointer to the matching klass if found, nullptr otherwise.
                @details
                Walks the full ClassLoaderDataGraph using only HotSpot internal structures
                resolved via gHotSpotVMStructs, without any JNI or JVMTI calls.
            */
            auto find_klass(const std::string_view class_name) const
                -> vmhook::hotspot::klass*
            {
                // Adaptive strategy - detected once at startup via VMStructs presence:
                //   JDK 21+  exports _klasses  but not _dictionary
                //   JDK 8-17 exports _dictionary but not _klasses
                static const bool use_klasses{ vmhook::hotspot::iterate_struct_entries("ClassLoaderData", "_klasses") != nullptr };

                vmhook::hotspot::class_loader_data* class_loader_data{ this->get_head() };

                while (vmhook::hotspot::is_valid_pointer(class_loader_data) && class_loader_data)
                {
                    if (use_klasses)
                    {
                        // JDK 21+: walk the _klasses linked list
                        vmhook::hotspot::klass* current_klass{ class_loader_data->get_klasses() };
                        while (current_klass && vmhook::hotspot::is_valid_pointer(current_klass))
                        {
                            const vmhook::hotspot::symbol* const sym{ current_klass->get_name() };
                            if (vmhook::hotspot::is_valid_pointer(sym) && sym->to_string() == class_name)
                            {
                                return current_klass;
                            }

                            current_klass = current_klass->get_next_link();
                        }
                    }
                    else
                    {
                        // JDK 8-17: search via per-CLD Dictionary hashtable
                        // (may return null if _dictionary not in VMStructs for this JDK build)
                        vmhook::hotspot::dictionary* const dict{ class_loader_data->get_dictionary() };
                        if (vmhook::hotspot::is_valid_pointer(dict))
                        {
                            vmhook::hotspot::klass* const found_klass{ dict->find_klass(class_name) };
                            if (found_klass)
                            {
                                return found_klass;
                            }
                        }
                    }

                    vmhook::hotspot::class_loader_data* const next{ class_loader_data->get_next() };
                    class_loader_data = vmhook::hotspot::is_valid_pointer(next) ? next : nullptr;
                }

                // JDK 8 fallback: ClassLoaderData._dictionary not in VMStructs,
                // but SystemDictionary._dictionary (bootstrap CL) and
                // SystemDictionary._shared_dictionary (CDS) ARE exported as statics.
                // These cover all bootstrap-loaded classes (java.*, javax.*, sun.*, etc.)
                // and most Minecraft client classes (loaded by the same classloader chain).
                static const vmhook::hotspot::vm_struct_entry_t* const sd_main{ vmhook::hotspot::iterate_struct_entries("SystemDictionary", "_dictionary") };
                static const vmhook::hotspot::vm_struct_entry_t* const sd_shared{ vmhook::hotspot::iterate_struct_entries("SystemDictionary", "_shared_dictionary") };

                for (const vmhook::hotspot::vm_struct_entry_t* system_dictionary_entry : { sd_main, sd_shared })
                {
                    if (!system_dictionary_entry || !system_dictionary_entry->address)
                    {
                        continue;
                    }
                    vmhook::hotspot::dictionary* const dictionary_pointer{ *reinterpret_cast<vmhook::hotspot::dictionary**>(system_dictionary_entry->address) };
                    if (!vmhook::hotspot::is_valid_pointer(dictionary_pointer))
                    {
                        continue;
                    }
                    klass* const found_klass{ dictionary_pointer->find_klass(class_name) };
                    if (found_klass)
                    {
                        return found_klass;
                    }
                }

                return nullptr;
            }

            /*
                @brief Invokes a callback for every klass currently reachable
                       through the ClassLoaderData graph.
                @details
                Used by the class-load watcher to snapshot loaded classes.
                The callback receives the klass's internal name (with '/'
                separators) and the raw klass pointer.  Iteration is best-
                effort: unreadable nodes are skipped silently.

                Complexity: O(N) where N = total loaded classes.
                Exception safety: noexcept boundary — callback exceptions
                    are propagated as-is; iteration stops at the throw.
            */
            template<typename callback_type>
            auto for_each_klass(callback_type&& callback) const -> void
            {
                static const bool use_klasses{
                    vmhook::hotspot::iterate_struct_entries("ClassLoaderData", "_klasses") != nullptr };

                auto* class_loader_data{ this->get_head() };
                std::int32_t visited{ 0 };
                while (vmhook::hotspot::is_valid_pointer(class_loader_data)
                    && class_loader_data
                    && visited < 65536)
                {
                    ++visited;

                    if (use_klasses)
                    {
                        // JDK 21+: walk the _klasses linked list.
                        auto* current_klass{ class_loader_data->get_klasses() };
                        std::int32_t kl_visited{ 0 };
                        while (current_klass
                            && vmhook::hotspot::is_valid_pointer(current_klass)
                            && kl_visited < 1048576)
                        {
                            ++kl_visited;
                            const auto* const sym{ current_klass->get_name() };
                            if (vmhook::hotspot::is_valid_pointer(sym))
                            {
                                callback(sym->to_string(), current_klass);
                            }
                            current_klass = current_klass->get_next_link();
                        }
                    }

                    auto* const next{ class_loader_data->get_next() };
                    class_loader_data = vmhook::hotspot::is_valid_pointer(next) ? next : nullptr;
                }
            }
        };

        /*
            @brief Represents the possible execution states of a HotSpot JavaThread.
        */
        enum class java_thread_state : std::int8_t
        {
            _thread_uninitialized = 0,
            _thread_new = 2,
            _thread_new_trans = 3,
            _thread_in_native = 4,
            _thread_in_native_trans = 5,
            _thread_in_vm = 6,
            _thread_in_vm_trans = 7,
            /*
                @brief The thread is currently executing Java bytecode in the interpreter.
                @note This is the only state in which method hooks are safely intercepted.
            */
            _thread_in_Java = 8,
            _thread_in_Java_trans = 9,
            _thread_blocked = 10,
            _thread_blocked_trans = 11,
            _thread_max_state = 12
        };

        /*
            @brief Represents a HotSpot internal JavaThread object.
            @details
            JavaThread is the JVM's internal representation of a Java thread.
            On x64 HotSpot the current JavaThread pointer is always held in register r15,
            making it directly accessible from low-level hook stubs.
            The layout is resolved at runtime via gHotSpotVMStructs.
        */
        struct java_thread
        {
            /*
                @brief Returns the current execution state of this thread.
            */
            auto get_thread_state() const
                -> vmhook::hotspot::java_thread_state
            {
                static const vmhook::hotspot::vm_struct_entry_t* const entry{ vmhook::hotspot::iterate_struct_entries("JavaThread", "_thread_state") };

                try
                {
                    if (!entry)
                    {
                        throw vmhook::exception{ "Failed to find JavaThread._thread_state entry." };
                    }

                    return *reinterpret_cast<java_thread_state*>(reinterpret_cast<std::uint8_t*>(const_cast<vmhook::hotspot::java_thread*>(this)) + entry->offset);
                }
                catch (const std::exception& exception)
                {
                    VMHOOK_LOG("{} java_thread.get_thread_state() {}", vmhook::error_tag, exception.what());
                    return vmhook::hotspot::java_thread_state::_thread_uninitialized;
                }
            }

            /*
                @brief Sets the execution state of this thread.
                @warning Incorrect use of this function can corrupt the JVM thread state machine.
            */
            auto set_thread_state(const vmhook::hotspot::java_thread_state state) const
                -> void
            {
                static const vmhook::hotspot::vm_struct_entry_t* const entry{ vmhook::hotspot::iterate_struct_entries("JavaThread", "_thread_state") };

                try
                {
                    if (!entry)
                    {
                        throw vmhook::exception{ "Failed to find JavaThread._thread_state entry." };
                    }

                    *reinterpret_cast<java_thread_state*>(reinterpret_cast<std::uint8_t*>(const_cast<vmhook::hotspot::java_thread*>(this)) + entry->offset) = state;
                }
                catch (const std::exception& exception)
                {
                    VMHOOK_LOG("{} java_thread.set_thread_state() {}", vmhook::error_tag, exception.what());
                }
            }

            /*
                @brief Returns the current suspension flags of this thread.
            */
            auto get_suspend_flags() const
                -> std::uint32_t
            {
                static const vmhook::hotspot::vm_struct_entry_t* const entry{ vmhook::hotspot::iterate_struct_entries("JavaThread", "_suspend_flags") };

                try
                {
                    if (!entry)
                    {
                        throw vmhook::exception{ "Failed to find JavaThread._suspend_flags entry." };
                    }

                    return *reinterpret_cast<std::uint32_t*>(reinterpret_cast<std::uint8_t*>(const_cast<vmhook::hotspot::java_thread*>(this)) + entry->offset);
                }
                catch (const std::exception& exception)
                {
                    VMHOOK_LOG("{} java_thread.get_suspend_flags() {}", vmhook::error_tag, exception.what());
                    return 0;
                }
            }

            /*
                @brief Returns the next thread in HotSpot's global thread linked list.
                @details
                HotSpot maintains all JavaThread instances in an intrusive singly-linked list.
                The field was _next on JavaThread in older JDKs and moved to Thread in later
                JDKs.  This function probes both locations and uses whichever is present so
                that the code remains version-agnostic.

                Complexity: O(1) after the VMStruct offset is cached on first call.
                Exception safety: noexcept — returns nullptr on any failure.

                @return Pointer to the next JavaThread, or nullptr if this is the last entry.
            */
            auto get_next() const noexcept
                -> vmhook::hotspot::java_thread*
            {
                static const vmhook::hotspot::vm_struct_entry_t* const entry{ []()
                    -> const vmhook::hotspot::vm_struct_entry_t*
                    {
                        {
                            auto* found_entry{ vmhook::hotspot::iterate_struct_entries("JavaThread", "_next") };
                            if (found_entry)
                            {
                                return found_entry;
                            }
                        }
                        return vmhook::hotspot::iterate_struct_entries("Thread", "_next");
                    }()
                };

                if (!entry || !vmhook::hotspot::is_valid_pointer(this))
                {
                    return nullptr;
                }

                vmhook::hotspot::java_thread* const next_thread{ *reinterpret_cast<vmhook::hotspot::java_thread* const*>(reinterpret_cast<const std::uint8_t*>(this) + entry->offset) };
                return vmhook::hotspot::is_valid_pointer(next_thread) ? next_thread : nullptr;
            }

            /*
                @brief Returns the OS-level thread ID of this JavaThread.
                @details
                Walks JavaThread (or Thread) -> OSThread -> _thread_id.
                _osthread is probed on both JavaThread and Thread for JDK version
                compatibility.  On Windows the value is a Win32 thread ID; on Linux
                it is the kernel TID (same value returned by gettid()).

                Complexity: O(1) after VMStruct offsets are cached on first call.
                Exception safety: noexcept — returns 0 on any failure.

                @return The OS thread ID, or 0 if the field cannot be located.
            */
            auto get_os_thread_id() const noexcept
                -> vmhook::os::thread_id_t
            {
                static const vmhook::hotspot::vm_struct_entry_t* const osthread_entry{ []()
                    -> const vmhook::hotspot::vm_struct_entry_t*
                    {
                        if (auto* const entry{ vmhook::hotspot::iterate_struct_entries("JavaThread", "_osthread") })
                        {
                            return entry;
                        }

                        return vmhook::hotspot::iterate_struct_entries("Thread", "_osthread");
                    }()
                };
                static const vmhook::hotspot::vm_struct_entry_t* const thread_id_entry{ []()
                    -> const vmhook::hotspot::vm_struct_entry_t*
                    {
                        return vmhook::hotspot::iterate_struct_entries("OSThread", "_thread_id");
                    }()
                };

                if (!osthread_entry || !thread_id_entry || !vmhook::hotspot::is_valid_pointer(this))
                {
                    return 0;
                }

                void* const os_thread{ *reinterpret_cast<void* const*>(reinterpret_cast<const std::uint8_t*>(this) + osthread_entry->offset) };
                if (!os_thread || !vmhook::hotspot::is_valid_pointer(os_thread))
                {
                    return 0;
                }

                // HotSpot exports OSThread::_thread_id with a platform-specific underlying
                // type (32-bit DWORD on Windows, pid_t on Linux). Read the raw 32-bit slot
                // and zero-extend so callers always see a uniformly-typed value.
                return static_cast<vmhook::os::thread_id_t>(
                    *reinterpret_cast<const std::uint32_t*>(
                        reinterpret_cast<const std::uint8_t*>(os_thread) + thread_id_entry->offset));
            }

            /*
                @brief Bump-allocates byte_size bytes from this thread's TLAB.
                @details
                A Thread-Local Allocation Buffer (TLAB) is a private region of the Java heap
                assigned to each thread.  Allocating from the TLAB is a simple pointer bump:
                advance _top by byte_size and return the old _top.  No locking is required.
                If the TLAB has insufficient space this function returns nullptr and the caller
                must fall back to a slower global allocation path.

                The _tlab field is probed on both JavaThread and Thread for JDK version
                compatibility.

                Complexity: O(1).
                Exception safety: noexcept — returns nullptr on any failure or if the TLAB
                                  has insufficient space.

                @param byte_size  Number of bytes to allocate; must be > 0 and aligned
                                  to the JVM object alignment (typically 8 bytes).
                @return Pointer to the allocated region, or nullptr if allocation failed.
            */
            auto allocate_tlab(const std::size_t byte_size) const noexcept
                -> void*
            {
                static const vmhook::hotspot::vm_struct_entry_t* const tlab_entry{ []()
                    -> const vmhook::hotspot::vm_struct_entry_t*
                    {
                        {
                            auto* entry{ vmhook::hotspot::iterate_struct_entries("JavaThread", "_tlab") };
                            if (entry)
                            {
                                return entry;
                            }
                        }
                        return vmhook::hotspot::iterate_struct_entries("Thread", "_tlab");
                    }()
                };
                static const vmhook::hotspot::vm_struct_entry_t* const top_entry{ vmhook::hotspot::iterate_struct_entries("ThreadLocalAllocBuffer", "_top") };
                static const vmhook::hotspot::vm_struct_entry_t* const end_entry{ vmhook::hotspot::iterate_struct_entries("ThreadLocalAllocBuffer", "_end") };

                if (!tlab_entry || !top_entry || !end_entry || byte_size == 0)
                {
                    return nullptr;
                }

                std::uint8_t* const tlab{ reinterpret_cast<std::uint8_t*>(const_cast<vmhook::hotspot::java_thread*>(this)) + tlab_entry->offset };
                std::uint8_t** const top_address{ reinterpret_cast<std::uint8_t**>(tlab + top_entry->offset) };
                std::uint8_t** const end_address{ reinterpret_cast<std::uint8_t**>(tlab + end_entry->offset) };

                std::uint8_t* const top{ *top_address };
                std::uint8_t* const end{ *end_address };

                if (!vmhook::hotspot::is_valid_pointer(top) || !vmhook::hotspot::is_valid_pointer(end) || top > end)
                {
                    return nullptr;
                }

                if (static_cast<std::size_t>(end - top) < byte_size)
                {
                    return nullptr;
                }

                *top_address = top + byte_size;
                return top;
            }
        };

        // --- Thread management -----------------------------------------------

        /*
            @brief Cached JavaThread* for the calling OS thread.
            @details
            Set by ensure_current_java_thread() when the thread is first identified in the
            HotSpot thread list.  Thread-local so that each OS thread maintains its own
            cached pointer without synchronisation.
        */
        inline thread_local vmhook::hotspot::java_thread* current_java_thread{ nullptr };

        /*
            @brief Cached JNIEnv* for the calling OS thread.
            @details
            Set by attach_current_native_thread() after a successful
            AttachCurrentThread / AttachCurrentThreadAsDaemon call.  Thread-local for
            the same reason as current_java_thread.
        */
        inline thread_local void* current_jni_env{ nullptr };

        /*
            @brief The most recently observed JavaThread* across all OS threads.
            @details
            Updated atomically (memory_order_relaxed) by ensure_current_java_thread()
            and find_allocation_thread().  Used as a fast fallback by
            find_allocation_thread() when no thread-local cached pointer is available,
            avoiding a full thread-list walk in the common case.
        */
        inline std::atomic<vmhook::hotspot::java_thread*> last_java_thread{ nullptr };

        /*
            @brief Returns the first JavaThread in HotSpot's global thread list.
            @details
            Reads the head pointer from Threads._thread_list, which is a static field
            exported through gHotSpotVMStructs.  This is the entry point for walking
            the intrusive linked list of all live Java threads.

            Complexity: O(1).
            Exception safety: noexcept — returns nullptr if the list is empty or not found.

            @return Pointer to the head JavaThread, or nullptr on failure.
        */
        static auto find_any_java_thread() noexcept
            -> vmhook::hotspot::java_thread*
        {
            static const vmhook::hotspot::vm_struct_entry_t* const thread_list_entry{
                vmhook::hotspot::iterate_struct_entries("Threads", "_thread_list") };

            if (!thread_list_entry || !thread_list_entry->address)
            {
                return nullptr;
            }

            vmhook::hotspot::java_thread* const head{ *reinterpret_cast<vmhook::hotspot::java_thread**>(thread_list_entry->address) };
            return vmhook::hotspot::is_valid_pointer(head) ? head : nullptr;
        }

        /*
            @brief Finds the JavaThread whose OS thread ID matches os_thread_id.
            @details
            First walks the classic _thread_list linked list (up to 4 096 entries).
            If the thread is not found there, falls back to ThreadsSMRSupport._java_thread_list
            which is the Safe Memory Reclamation (SMR) snapshot used by JDK 10+.

            Complexity: O(N) where N = number of live Java threads.
            Exception safety: noexcept — returns nullptr if not found.

            @param os_thread_id  OS thread ID (Win32 thread ID on Windows, kernel TID on Linux).
            @return Matching JavaThread*, or nullptr if not found.
        */
        static auto find_java_thread_by_os_thread_id(const vmhook::os::thread_id_t os_thread_id) noexcept
            -> vmhook::hotspot::java_thread*
        {
            if (os_thread_id == 0)
            {
                return nullptr;
            }

            std::int32_t visited_threads{ 0 };
            for (vmhook::hotspot::java_thread* thread{ vmhook::hotspot::find_any_java_thread() };
                thread && vmhook::hotspot::is_valid_pointer(thread) && visited_threads < 4096;
                thread = thread->get_next(), ++visited_threads)
            {
                if (thread->get_os_thread_id() == os_thread_id)
                {
                    return thread;
                }
            }

            static const vmhook::hotspot::vm_struct_entry_t* const list_entry{
                vmhook::hotspot::iterate_struct_entries("ThreadsSMRSupport", "_java_thread_list") };
            static const vmhook::hotspot::vm_struct_entry_t* const length_entry{
                vmhook::hotspot::iterate_struct_entries("ThreadsList", "_length") };
            static const vmhook::hotspot::vm_struct_entry_t* const threads_entry{
                vmhook::hotspot::iterate_struct_entries("ThreadsList", "_threads") };

            if (!list_entry || !length_entry || !threads_entry || !list_entry->address)
            {
                return nullptr;
            }

            void* const list{ *reinterpret_cast<void**>(list_entry->address) };
            if (!list || !vmhook::hotspot::is_valid_pointer(list))
            {
                return nullptr;
            }

            const std::int32_t length{ *reinterpret_cast<const std::int32_t*>(reinterpret_cast<const std::uint8_t*>(list) + length_entry->offset) };
            if (length <= 0 || length > 4096)
            {
                return nullptr;
            }

            auto** const threads{ *reinterpret_cast<vmhook::hotspot::java_thread***>(reinterpret_cast<std::uint8_t*>(list) + threads_entry->offset) };
            if (!threads || !vmhook::hotspot::is_valid_pointer(threads))
            {
                return nullptr;
            }

            for (std::int32_t index{ 0 }; index < length; ++index)
            {
                vmhook::hotspot::java_thread* const thread{ threads[index] };
                if (thread && vmhook::hotspot::is_valid_pointer(thread) && thread->get_os_thread_id() == os_thread_id)
                {
                    return thread;
                }
            }

            return nullptr;
        }

        /*
            @brief Attaches the calling native (non-Java) thread to the JVM.
            @details
            Calls JNI_GetCreatedJavaVMs to locate the running JavaVM, then tries in order:
              1. GetEnv   — the thread is already attached; just record the JNIEnv.
              2. AttachCurrentThreadAsDaemon — preferred for injected native threads.
              3. AttachCurrentThread         — fallback.
            Stores the resulting JNIEnv* in current_jni_env on success.

            Complexity: O(1) — JNI vtable dispatches.
            Exception safety: noexcept — returns false on any failure.

            @return true if a JNIEnv was obtained, false otherwise.
        */
        static auto attach_current_native_thread() noexcept
            -> bool
        {
            vmhook::os::module_handle const jvm_module{ vmhook::hotspot::get_jvm_module() };
            if (!jvm_module)
            {
                return false;
            }

            using jint = int;
            struct JNIEnv__;
            struct JavaVM__;
            using JNIEnv = JNIEnv__;
            using JavaVM = JavaVM__;

            struct JNIInvokeInterface_
            {
                void* reserved0;
                void* reserved1;
                void* reserved2;
                jint (*DestroyJavaVM)(JavaVM*);
                jint (*AttachCurrentThread)(JavaVM*, void**, void*);
                jint (*DetachCurrentThread)(JavaVM*);
                jint (*GetEnv)(JavaVM*, void**, jint);
                jint (*AttachCurrentThreadAsDaemon)(JavaVM*, void**, void*);
            };

            struct JavaVM__
            {
                const JNIInvokeInterface_* functions;
            };

            using get_created_java_vms_t = jint (*)(JavaVM**, jint, jint*);
            auto* const get_created_java_vms{ reinterpret_cast<get_created_java_vms_t>(
                vmhook::os::get_proc_address(jvm_module, "JNI_GetCreatedJavaVMs")) };
            if (!get_created_java_vms)
            {
                return false;
            }

            JavaVM* vm{};
            jint vm_count{};
            if (get_created_java_vms(&vm, 1, &vm_count) != 0 || vm_count <= 0 || !vm || !vm->functions)
            {
                return false;
            }

            constexpr jint jni_version_1_8{ 0x00010008 };
            void* env{};
            if (vm->functions->GetEnv && vm->functions->GetEnv(vm, &env, jni_version_1_8) == 0)
            {
                vmhook::hotspot::current_jni_env = env;
                return true;
            }

            if (vm->functions->AttachCurrentThreadAsDaemon && vm->functions->AttachCurrentThreadAsDaemon(vm, &env, nullptr) == 0)
            {
                vmhook::hotspot::current_jni_env = env;
                return true;
            }

            if (vm->functions->AttachCurrentThread && vm->functions->AttachCurrentThread(vm, &env, nullptr) == 0)
            {
                vmhook::hotspot::current_jni_env = env;
                return true;
            }

            return false;
        }

        /*
            @brief Ensures the calling thread has a valid JavaThread* and JNIEnv*.
            @details
            Resolution order:
              1. Thread-local current_java_thread is already set — fast return.
              2. Search the HotSpot thread list for the current OS thread ID.
              3. Attach the thread via JNI (attach_current_native_thread) and retry
                 up to 64 times with thread yields while HotSpot registers it.

            Complexity: O(N) worst case on first call, where N = number of live Java threads.
            Exception safety: noexcept — returns false on failure.
            Thread safety: safe to call from any OS thread; state is stored thread-locally.

            @return true if current_java_thread is valid after the call, false otherwise.
        */
        static auto ensure_current_java_thread() noexcept
            -> bool
        {
            if (vmhook::hotspot::current_java_thread && vmhook::hotspot::is_valid_pointer(vmhook::hotspot::current_java_thread))
            {
                if (!vmhook::hotspot::current_jni_env)
                {
                    vmhook::hotspot::attach_current_native_thread();
                }

                return true;
            }

            const vmhook::os::thread_id_t current_os_thread_id{ vmhook::os::current_thread_id() };
            if (vmhook::hotspot::java_thread* const existing_thread{ vmhook::hotspot::find_java_thread_by_os_thread_id(current_os_thread_id) })
            {
                vmhook::hotspot::current_java_thread = existing_thread;
                vmhook::hotspot::last_java_thread.store(existing_thread, std::memory_order_relaxed);
                VMHOOK_LOG("{} ensure_current_java_thread(): adopted JavaThread 0x{:016X} for OS thread {}", vmhook::info_tag, reinterpret_cast<std::uintptr_t>(existing_thread), current_os_thread_id);
                return true;
            }

            if (!vmhook::hotspot::attach_current_native_thread())
            {
                VMHOOK_LOG("{} ensure_current_java_thread(): AttachCurrentThread failed for OS thread {}", vmhook::error_tag, current_os_thread_id);
                return false;
            }

            for (std::int32_t attempt{ 0 }; attempt < 64; ++attempt)
            {
                if (vmhook::hotspot::java_thread* const attached_thread{ vmhook::hotspot::find_java_thread_by_os_thread_id(current_os_thread_id) })
                {
                    vmhook::hotspot::current_java_thread = attached_thread;
                    vmhook::hotspot::last_java_thread.store(attached_thread, std::memory_order_relaxed);
                    VMHOOK_LOG("{} ensure_current_java_thread(): attached JavaThread 0x{:016X} for OS thread {}", vmhook::info_tag, reinterpret_cast<std::uintptr_t>(attached_thread), current_os_thread_id);
                    return true;
                }

                std::this_thread::yield();
            }

            VMHOOK_LOG("{} ensure_current_java_thread(): attached thread was not found in HotSpot thread list for OS thread {}", vmhook::error_tag, current_os_thread_id);
            return false;
        }

        /*
            @brief Returns a JavaThread suitable for TLAB allocation without ensure_current_java_thread.
            @details
            Faster than ensure_current_java_thread because it never attaches the thread.
            Resolution order:
              1. Thread-local current_java_thread — zero overhead.
              2. last_java_thread atomic — one relaxed load, suitable from injected threads.
              3. find_any_java_thread  — walks the thread list once.

            Complexity: O(1) in cases 1 and 2, O(N) in case 3.
            Exception safety: noexcept — returns nullptr if no thread can be found.
            Thread safety: safe for concurrent reads; writes to last_java_thread are relaxed.

            @return A valid JavaThread*, or nullptr if none is available.
        */
        static auto find_allocation_thread() noexcept
            -> vmhook::hotspot::java_thread*
        {
            if (vmhook::hotspot::current_java_thread && vmhook::hotspot::is_valid_pointer(vmhook::hotspot::current_java_thread))
            {
                vmhook::hotspot::last_java_thread.store(vmhook::hotspot::current_java_thread, std::memory_order_relaxed);
                return vmhook::hotspot::current_java_thread;
            }

            vmhook::hotspot::java_thread* const cached_thread{ vmhook::hotspot::last_java_thread.load(std::memory_order_relaxed) };
            if (cached_thread && vmhook::hotspot::is_valid_pointer(cached_thread))
            {
                return cached_thread;
            }

            vmhook::hotspot::java_thread* const discovered_thread{ vmhook::hotspot::find_any_java_thread() };
            if (discovered_thread)
            {
                vmhook::hotspot::last_java_thread.store(discovered_thread, std::memory_order_relaxed);
            }
            return discovered_thread;
        }

        /*
            @brief Allocates byte_size bytes from any thread's TLAB by scanning the SMR thread list.
            @details
            Used as a fallback when find_allocation_thread() returns nullptr.  Iterates
            ThreadsSMRSupport._java_thread_list and calls java_thread::allocate_tlab() on
            each entry until one succeeds.  Updates last_java_thread to the thread that
            provided the allocation so that future calls hit the fast path.

            Complexity: O(N) where N = number of live Java threads.
            Exception safety: noexcept — returns nullptr if no TLAB has sufficient space.

            @param byte_size  Number of bytes requested; must be > 0.
            @return Pointer to the allocated memory, or nullptr on failure.
        */
        static auto allocate_from_threads_list(const std::size_t byte_size) noexcept
            -> void*
        {
            static const vmhook::hotspot::vm_struct_entry_t* const list_entry{
                vmhook::hotspot::iterate_struct_entries("ThreadsSMRSupport", "_java_thread_list") };
            static const vmhook::hotspot::vm_struct_entry_t* const length_entry{
                vmhook::hotspot::iterate_struct_entries("ThreadsList", "_length") };
            static const vmhook::hotspot::vm_struct_entry_t* const threads_entry{
                vmhook::hotspot::iterate_struct_entries("ThreadsList", "_threads") };

            if (!list_entry || !length_entry || !threads_entry || !list_entry->address)
            {
                return nullptr;
            }

            void* const list{ *reinterpret_cast<void**>(list_entry->address) };
            if (!list || !vmhook::hotspot::is_valid_pointer(list))
            {
                return nullptr;
            }

            const std::int32_t length{ *reinterpret_cast<const std::int32_t*>(reinterpret_cast<const std::uint8_t*>(list) + length_entry->offset) };
            if (length <= 0 || length > 4096)
            {
                return nullptr;
            }

            auto** const threads{ *reinterpret_cast<vmhook::hotspot::java_thread***>(reinterpret_cast<std::uint8_t*>(list) + threads_entry->offset) };
            if (!threads || !vmhook::hotspot::is_valid_pointer(threads))
            {
                return nullptr;
            }

            for (std::int32_t index{ 0 }; index < length; ++index)
            {
                vmhook::hotspot::java_thread* const thread{ threads[index] };
                if (!thread || !vmhook::hotspot::is_valid_pointer(thread))
                {
                    continue;
                }

                void* const object{ thread->allocate_tlab(byte_size) };
                if (object)
                {
                    vmhook::hotspot::last_java_thread.store(thread, std::memory_order_relaxed);
                    return object;
                }
            }

            return nullptr;
        }

        // --- OOP and Klass pointer encoding / decoding -----------------------

        /*
            @brief Decodes a compressed OOP to a real 64-bit pointer.
            @param compressed The 32-bit compressed OOP value read from a JVM structure.
            @return The decoded real pointer, or nullptr if compressed is 0.
            @details
            HotSpot stores heap object pointers in 32-bit "compressed OOP" form to reduce
            memory usage. The formula to recover the real 64-bit address is:
              real_address = narrow_oop_base + (compressed << narrow_oop_shift)

            - narrow_oop_base  - base address of the Java heap (0 when -Xmx < 4 GB and
                                  heap starts at address 0, otherwise the heap start).
            - narrow_oop_shift - how many bits to left-shift the compressed value (typically
                                  0 for heap < 4 GB, 3 for heap up to 32 GB with 8-byte aligned oops).

            Both values are read from CompressedOops::_narrow_oop.{_base,_shift} via
            gHotSpotVMStructs so this works across all JDK versions.
        */
        static auto decode_oop_pointer(const std::uint32_t compressed) noexcept
            -> void*
        {
            if (!compressed)
            {
                return nullptr;
            }

            // VMStruct field names for the narrow OOP base/shift changed across versions:
            //   JDK  8-16: Universe::_narrow_oop._base/shift
            //   JDK 17-24: CompressedOops::_narrow_oop._base/shift
            //   JDK 25+  : CompressedOops::_base/shift  (_narrow_oop. prefix dropped)
            static const vmhook::hotspot::vm_struct_entry_t* const base_entry{ []()
                -> const vmhook::hotspot::vm_struct_entry_t*
                {
                    {
                        auto* entry{ vmhook::hotspot::iterate_struct_entries("CompressedOops", "_narrow_oop._base") };
                        if (entry)
                        {
                            return entry;
                        }
                    }
                    {
                        auto* entry{ vmhook::hotspot::iterate_struct_entries("CompressedOops", "_base") };
                        if (entry)
                        {
                            return entry;
                        }
                    }
                    return vmhook::hotspot::iterate_struct_entries("Universe", "_narrow_oop._base");
                }()
            };

            static const vmhook::hotspot::vm_struct_entry_t* const shift_entry{ []()
                -> const vmhook::hotspot::vm_struct_entry_t*
                {
                    {
                        auto* entry{ vmhook::hotspot::iterate_struct_entries("CompressedOops", "_narrow_oop._shift") };
                        if (entry)
                        {
                            return entry;
                        }
                    }
                    {
                        auto* entry{ vmhook::hotspot::iterate_struct_entries("CompressedOops", "_shift") };
                        if (entry)
                        {
                            return entry;
                        }
                    }
                    return vmhook::hotspot::iterate_struct_entries("Universe", "_narrow_oop._shift");
                }()
            };

            if (!base_entry || !shift_entry)
            {
                return nullptr;
            }

            const std::uint64_t narrow_oop_base{ *reinterpret_cast<const std::uint64_t*>(base_entry->address) };
            const std::uint32_t narrow_oop_shift{ *reinterpret_cast<const std::uint32_t*>(shift_entry->address) };

            // real_address = narrow_oop_base + (compressed << narrow_oop_shift)
            return reinterpret_cast<void*>(narrow_oop_base + (static_cast<std::uint64_t>(compressed) << narrow_oop_shift));
        }

        /*
            @brief Compresses a decoded OOP pointer back into HotSpot's narrow OOP form.
            @details
            This is the inverse of decode_oop_pointer() and is used when assigning
            object wrapper fields through field_proxy::set().
        */
        static auto encode_oop_pointer(void* const decoded) noexcept
            -> std::uint32_t
        {
            if (!decoded)
            {
                return 0;
            }

            static const vmhook::hotspot::vm_struct_entry_t* const base_entry{ []()
                -> const vmhook::hotspot::vm_struct_entry_t*
                {
                    {
                        auto* entry{ vmhook::hotspot::iterate_struct_entries("CompressedOops", "_narrow_oop._base") };
                        if (entry)
                        {
                            return entry;
                        }
                    }
                    {
                        auto* entry{ vmhook::hotspot::iterate_struct_entries("CompressedOops", "_base") };
                        if (entry)
                        {
                            return entry;
                        }
                    }
                    return vmhook::hotspot::iterate_struct_entries("Universe", "_narrow_oop._base");
                }()
            };

            static const vmhook::hotspot::vm_struct_entry_t* const shift_entry{ []()
                -> const vmhook::hotspot::vm_struct_entry_t*
                {
                    {
                        auto* entry{ vmhook::hotspot::iterate_struct_entries("CompressedOops", "_narrow_oop._shift") };
                        if (entry)
                        {
                            return entry;
                        }
                    }
                    {
                        auto* entry{ vmhook::hotspot::iterate_struct_entries("CompressedOops", "_shift") };
                        if (entry)
                        {
                            return entry;
                        }
                    }
                    return vmhook::hotspot::iterate_struct_entries("Universe", "_narrow_oop._shift");
                }()
            };

            if (!base_entry || !shift_entry)
            {
                return 0;
            }

            const std::uint64_t narrow_oop_base{ *reinterpret_cast<const std::uint64_t*>(base_entry->address) };
            const std::uint32_t narrow_oop_shift{ *reinterpret_cast<const std::uint32_t*>(shift_entry->address) };
            const std::uint64_t decoded_address{ reinterpret_cast<std::uint64_t>(decoded) };
            if (decoded_address < narrow_oop_base)
            {
                return 0;
            }

            return static_cast<std::uint32_t>((decoded_address - narrow_oop_base) >> narrow_oop_shift);
        }

        /*
            @brief Decodes a compressed Klass pointer to a real 64-bit pointer.
            @details
            Klass pointers use a separate compressed-pointer scheme from object OOPs,
            stored in CompressedKlassPointers::_narrow_klass.{_base,_shift}.
            The decoding formula is identical: real_address = base + (compressed << shift).
        */
        static auto decode_klass_pointer(const std::uint32_t compressed) noexcept
            -> void*
        {
            if (!compressed)
            {
                return nullptr;
            }

            // VMStruct field names changed the same way as for CompressedOops:
            //   JDK  8-16: Universe::_narrow_klass._base/shift
            //   JDK 17-24: CompressedKlassPointers::_narrow_klass._base/shift
            //   JDK 25+  : CompressedKlassPointers::_base/shift
            static const vmhook::hotspot::vm_struct_entry_t* const base_entry{ []()
                -> const vmhook::hotspot::vm_struct_entry_t*
                {
                    {
                        auto* entry{ vmhook::hotspot::iterate_struct_entries("CompressedKlassPointers", "_narrow_klass._base") };
                        if (entry)
                        {
                            return entry;
                        }
                    }
                    {
                        auto* entry{ vmhook::hotspot::iterate_struct_entries("CompressedKlassPointers", "_base") };
                        if (entry)
                        {
                            return entry;
                        }
                    }
                    return vmhook::hotspot::iterate_struct_entries("Universe", "_narrow_klass._base");
                }()
            };
            static const vmhook::hotspot::vm_struct_entry_t* const shift_entry{ []()
                -> const vmhook::hotspot::vm_struct_entry_t*
                {
                    {
                        auto* entry{ vmhook::hotspot::iterate_struct_entries("CompressedKlassPointers", "_narrow_klass._shift") };
                        if (entry)
                        {
                            return entry;
                        }
                    }
                    {
                        auto* entry{ vmhook::hotspot::iterate_struct_entries("CompressedKlassPointers", "_shift") };
                        if (entry)
                        {
                            return entry;
                        }
                    }
                    return vmhook::hotspot::iterate_struct_entries("Universe", "_narrow_klass._shift");
                }()
            };

            if (!base_entry || !shift_entry)
            {
                return nullptr;
            }

            const std::uint64_t base{ *reinterpret_cast<const std::uint64_t*>(base_entry->address) };
            const std::uint32_t shift{ *reinterpret_cast<const std::uint32_t*>(shift_entry->address) };

            return reinterpret_cast<void*>(base + (static_cast<std::uint64_t>(compressed) << shift));
        }

        /*
            @brief Compresses a decoded Klass pointer back into HotSpot's narrow Klass form.
            @details
            Inverse of decode_klass_pointer().  The formula is:
              compressed = (decoded_address - narrow_klass_base) >> narrow_klass_shift
            Used when writing class-pointer fields (e.g. the mark-word klass field in an
            object header) back into JVM structures.

            Complexity: O(1) after VMStruct entries are cached on first call.
            Exception safety: noexcept — returns 0 on failure or if decoded is null.

            @param decoded  Full 64-bit Klass pointer to compress.
            @return 32-bit compressed Klass pointer, or 0 on failure.
        */
        static auto encode_klass_pointer(void* const decoded) noexcept
            -> std::uint32_t
        {
            if (!decoded)
            {
                return 0;
            }

            static const vmhook::hotspot::vm_struct_entry_t* const base_entry{ []()
                -> const vmhook::hotspot::vm_struct_entry_t*
                {
                    {
                        auto* entry{ vmhook::hotspot::iterate_struct_entries("CompressedKlassPointers", "_narrow_klass._base") };
                        if (entry)
                        {
                            return entry;
                        }
                    }
                    {
                        auto* entry{ vmhook::hotspot::iterate_struct_entries("CompressedKlassPointers", "_base") };
                        if (entry)
                        {
                            return entry;
                        }
                    }
                    return vmhook::hotspot::iterate_struct_entries("Universe", "_narrow_klass._base");
                }()
            };
            static const vmhook::hotspot::vm_struct_entry_t* const shift_entry{ []()
                -> const vmhook::hotspot::vm_struct_entry_t*
                {
                    {
                        auto* entry{ vmhook::hotspot::iterate_struct_entries("CompressedKlassPointers", "_narrow_klass._shift") };
                        if (entry)
                        {
                            return entry;
                        }
                    }
                    {
                        auto* entry{ vmhook::hotspot::iterate_struct_entries("CompressedKlassPointers", "_shift") };
                        if (entry)
                        {
                            return entry;
                        }
                    }
                    return vmhook::hotspot::iterate_struct_entries("Universe", "_narrow_klass._shift");
                }()
            };

            if (!base_entry || !shift_entry)
            {
                return 0;
            }

            const std::uint64_t base{ *reinterpret_cast<const std::uint64_t*>(base_entry->address) };
            const std::uint32_t shift{ *reinterpret_cast<const std::uint32_t*>(shift_entry->address) };
            const std::uint64_t decoded_address{ reinterpret_cast<std::uint64_t>(decoded) };

            if (decoded_address < base)
            {
                return 0;
            }

            return static_cast<std::uint32_t>((decoded_address - base) >> shift);
        }

        /*
            @brief Checks whether a memory region matches a given byte pattern.
            @details
            A pattern byte of 0x00 acts as a wildcard and always matches.
        */
        inline static auto match_pattern(const std::uint8_t* const address, const std::uint8_t* const pattern, const std::size_t size)
            -> bool
        {
            for (std::size_t byte_index{ 0 }; byte_index < size; ++byte_index)
            {
                if (pattern[byte_index] == 0x00)
                {
                    continue;
                }
                if (address[byte_index] != pattern[byte_index])
                {
                    return false;
                }
            }
            return true;
        }

        /*
            @brief Scans a memory region for the first occurrence of a byte pattern.
            @details
            A pattern byte of 0x00 acts as a wildcard and always matches.
        */
        inline static auto scan(const std::uint8_t* const start, const std::size_t range, const std::uint8_t* pattern, const std::size_t size)
            -> std::uint8_t*
        {
            for (std::size_t scan_offset{ 0 }; scan_offset < range; ++scan_offset)
            {
                if (vmhook::hotspot::match_pattern(start + scan_offset, pattern, size))
                {
                    return const_cast<std::uint8_t*>(start + scan_offset);
                }
            }

            return nullptr;
        }

        /*
            @brief Determines the safe scannable size of a JVM generated code stub.
            @details
            Uses VirtualQuery to retrieve the memory region information and computes
            how many bytes remain from start to the end of the region, capped at 0x2000.
        */
        static auto find_stub_size(const std::uint8_t* start)
            -> std::size_t
        {
            const vmhook::os::region_info info{ vmhook::os::query_region(start) };
            if (!info.base || info.size == 0)
            {
                return static_cast<std::size_t>(0x2000);
            }
            const std::uint8_t* const region_end{
                reinterpret_cast<const std::uint8_t*>(info.base) + info.size };
            if (region_end <= start)
            {
                return static_cast<std::size_t>(0x2000);
            }
            const std::size_t remaining{ static_cast<std::size_t>(region_end - start) };
            return (std::min)(remaining, static_cast<std::size_t>(0x2000));
        }

        /*
            @brief Byte offset used to retrieve the local variables pointer from the interpreter frame.
            @details
            Determined at runtime by find_hook_location(). Defaults to -56.
        */
        inline constinit std::int8_t locals_offset{ -56 };

        /*
            @brief Locates the optimal injection point within a HotSpot i2i interpreter stub.
            @param i2i_entry Pointer to the beginning of the i2i interpreter stub to scan.
            @return Pointer to the injection point within the stub, or nullptr on failure.
            @details
            Scans the i2i stub for a known sequence of instructions that appears at a stable
            location across JVM builds, immediately before the interpreter begins executing
            the actual Java bytecode. All method arguments are fully set up at this point.
            Also scans backwards to find and cache the locals_offset value.
            @note Two patterns are tried in order:
                  1. Full pattern (JDK 8 through early JDK 21): 4-mov-spill + mov BYTE PTR [r15+??],??
                     Hook injected at the thread-state-write instruction (last 8 bytes).
                  2. Fallback (JDK 21 release / JDK 22+): just mov BYTE PTR [r15+??],??
                     Hook injected at the start of that instruction directly.
        */
        static auto find_hook_location(const void* i2i_entry)
            -> void*
        {
            /*
                Primary pattern (JDK 8 through early JDK 21 builds):
                  Four consecutive `mov [rsp+imm32], eax` instructions that spill the first
                  four Windows x64 integer arguments to the shadow area, followed by
                  `mov BYTE PTR [r15+imm32], imm8` which writes a thread-status byte.
                  Wildcard bytes (0x00) match any value.
            */
            static constexpr std::uint8_t pattern_full[]
            {
                0x89, 0x84, 0x24, 0x00, 0x00, 0x00, 0x00, // mov [rsp+??], eax
                0x89, 0x84, 0x24, 0x00, 0x00, 0x00, 0x00, // mov [rsp+??], eax
                0x89, 0x84, 0x24, 0x00, 0x00, 0x00, 0x00, // mov [rsp+??], eax
                0x89, 0x84, 0x24, 0x00, 0x00, 0x00, 0x00, // mov [rsp+??], eax
                0x41, 0xC6, 0x87, 0x00, 0x00, 0x00, 0x00, 0x00 // mov BYTE PTR [r15+??], ??
            };

            /*
                Fallback pattern (JDK 21 release builds, JDK 22+):
                  The 4-mov spill block is absent or different, but the
                  `mov BYTE PTR [r15+imm32], imm8` thread-status write is always present.
                  0x41 0xC6 0x87 = REX.B MOV r/m8,imm8 with ModRM selecting [r15+disp32].
                  All four displacement bytes and the imm8 are wildcards.
            */
            static constexpr std::uint8_t pattern_fallback[]
            {
                0x41, 0xC6, 0x87, 0x00, 0x00, 0x00, 0x00, 0x00 // mov BYTE PTR [r15+??], ??
            };

            static constexpr std::uint8_t locals_pattern[]
            {
                0x4C, 0x8B, 0x75, 0x00, 0xC3 // mov r14, QWORD PTR [rbp+??] ; ret
            };

            const std::uint8_t* const current{ reinterpret_cast<const std::uint8_t*>(i2i_entry) };
            const std::size_t scan_size{ vmhook::hotspot::find_stub_size(current) };

            // Try the full 4-spill + thread-state-write pattern first (JDK 8 through early JDK 21).
            // The injection point is at the START of the thread-state-write instruction,
            // which sits at the END of the full pattern (offset = sizeof - 8).
            std::uint8_t* injection_point{ nullptr };
            std::uint8_t* const full_match{ vmhook::hotspot::scan(current, scan_size, pattern_full, sizeof(pattern_full)) };
            if (full_match)
            {
                // The thread-state instruction is the last 8 bytes of the full match.
                injection_point = full_match + sizeof(pattern_full) - 8;
            }
            else
            {
                // Fallback: scan directly for `mov BYTE PTR [r15+??], ??` (JDK 21+).
                // The injection point is at the START of the matched instruction.
                std::uint8_t* const fallback_match{ vmhook::hotspot::scan(current, scan_size, pattern_fallback, sizeof(pattern_fallback)) };
                if (fallback_match)
                {
                    injection_point = fallback_match;
                }
            }

            try
            {
                if (!injection_point)
                {
                    throw vmhook::exception{ "Failed to find hook pattern (tried full and fallback)." };
                }

                // Scan backwards from the injection point to find the locals pointer load:
                //   mov r14, QWORD PTR [rbp+disp8]  ; ret
                // The displacement byte is the rbp-relative offset of the locals pointer.
                for (std::uint8_t* scan_ptr{ injection_point }; scan_ptr > current; --scan_ptr)
                {
                    if (vmhook::hotspot::match_pattern(scan_ptr, locals_pattern, sizeof(locals_pattern)))
                    {
                        // Byte [3] of `4C 8B 75 ??` is the signed 8-bit displacement.
                        locals_offset = static_cast<std::int8_t>(scan_ptr[3]);
                        break;
                    }
                }

                return injection_point;
            }
            catch (const std::exception& exception)
            {
                VMHOOK_LOG("{} find_hook_location() {}", vmhook::error_tag, exception.what());
                return nullptr;
            }
        }

        /*
            @brief Allocates a block of executable memory within a 32-bit relative jump
                   range of a given address.
            @details
            Walks the process address space and allocates inside a free region that is
            reachable by a 5-byte relative JMP.  HotSpot often reserves dense areas around
            code stubs, so blindly probing exact offsets can fail even when a usable
            nearby free region exists.  The implementation uses the platform-specific
            primitives wrapped behind vmhook::os::query_region / vmhook::os::allocate_rwx.
        */
        static auto allocate_nearby_memory(std::uint8_t* nearby_addr, const std::size_t size) noexcept
            -> std::uint8_t*
        {
            if (!nearby_addr || size == 0)
            {
                return nullptr;
            }

            const std::uintptr_t minimum_application_address{ static_cast<std::uintptr_t>(0x10000) };
            const std::uintptr_t maximum_application_address{ vmhook::os::user_address_ceiling };
            const std::uintptr_t allocation_granularity{ static_cast<std::uintptr_t>(
                vmhook::os::allocation_granularity()) };
            const std::uintptr_t target_address{ reinterpret_cast<std::uintptr_t>(nearby_addr) };
            const std::uintptr_t relative_limit{ static_cast<std::uintptr_t>(
                (std::numeric_limits<std::int32_t>::max)()) };

            const auto align_up = [](const std::uintptr_t value, const std::uintptr_t alignment) noexcept
                -> std::uintptr_t
            {
                return (value + alignment - 1) & ~(alignment - 1);
            };

            const auto align_down = [](const std::uintptr_t value, const std::uintptr_t alignment) noexcept
                -> std::uintptr_t
            {
                return value & ~(alignment - 1);
            };

            const std::uintptr_t search_min{
                (std::max)(minimum_application_address,
                    target_address > relative_limit
                        ? target_address - relative_limit
                        : minimum_application_address)
            };
            const std::uintptr_t search_max{
                target_address > maximum_application_address - relative_limit
                    ? maximum_application_address
                    : (std::min)(maximum_application_address, target_address + relative_limit)
            };

            auto try_allocate_in_region = [&](const std::uintptr_t region_base,
                                              const std::uintptr_t region_end) noexcept
                -> std::uint8_t*
            {
                if (region_end <= region_base || region_end - region_base < size)
                {
                    return nullptr;
                }

                const std::uintptr_t usable_begin{ (std::max)(region_base, search_min) };
                const std::uintptr_t usable_end{ (std::min)(region_end, search_max + 1) };
                if (usable_end <= usable_begin || usable_end - usable_begin < size)
                {
                    return nullptr;
                }

                const std::uintptr_t first_candidate{ align_up(usable_begin, allocation_granularity) };
                const std::uintptr_t last_candidate{ align_down(usable_end - size, allocation_granularity) };
                if (first_candidate > last_candidate)
                {
                    return nullptr;
                }

                const std::uintptr_t preferred_candidate{
                    target_address < first_candidate ? first_candidate :
                    target_address > last_candidate ? last_candidate :
                    align_down(target_address, allocation_granularity)
                };

                if (void* const allocated{ vmhook::os::allocate_rwx(
                        reinterpret_cast<void*>(preferred_candidate), size) })
                {
                    return reinterpret_cast<std::uint8_t*>(allocated);
                }

                if (preferred_candidate != first_candidate)
                {
                    if (void* const allocated{ vmhook::os::allocate_rwx(
                            reinterpret_cast<void*>(first_candidate), size) })
                    {
                        return reinterpret_cast<std::uint8_t*>(allocated);
                    }
                }

                if (preferred_candidate != last_candidate && first_candidate != last_candidate)
                {
                    if (void* const allocated{ vmhook::os::allocate_rwx(
                            reinterpret_cast<void*>(last_candidate), size) })
                    {
                        return reinterpret_cast<std::uint8_t*>(allocated);
                    }
                }

                return nullptr;
            };

            const std::size_t page_size{ vmhook::os::page_size() };
            for (std::uintptr_t current{ search_min }; current < search_max; )
            {
                const vmhook::os::region_info info{ vmhook::os::query_region(
                    reinterpret_cast<void*>(current)) };

                if (!info.base)
                {
                    current += page_size;
                    continue;
                }

                const std::uintptr_t region_base{ reinterpret_cast<std::uintptr_t>(info.base) };
                const std::uintptr_t region_size{ info.size };
                const std::uintptr_t region_end{ region_base + region_size };

                if (info.free)
                {
                    if (std::uint8_t* const allocated{ try_allocate_in_region(region_base, region_end) })
                    {
                        return allocated;
                    }
                }

                if (region_end <= current)
                {
                    break;
                }
                current = region_end;
            }

            return nullptr;
        }

        /*
            @brief Raw function pointer type for the low-level hook trampoline.
            @details
            Every hook trampoline generated by midi2i_hook calls through a pointer of this
            type.  The three arguments are passed in the System V / Microsoft x64 calling
            convention by the trampoline assembly:
              frame*        - rbp of the intercepted interpreter frame.
              java_thread*  - current JavaThread* (from r15 on HotSpot x64).
              return_slot*  - pre-allocated slot for the callback to record cancellation
                              and the return value.
        */
        using detour_function_t = void(*)(vmhook::hotspot::frame*, vmhook::hotspot::java_thread*, vmhook::hotspot::return_slot*);

        /*
            @brief Type-erased container for method arguments obtained via auto-detection.
            @details
            Returned by frame::get_arguments() (no template parameters). Stores raw decoded
            OOP pointers and class names. Call as<T>() to construct a wrapper on demand.
        */
        struct method_args
        {
            /*
                @brief One decoded argument from an intercepted Java method call.
                @details
                decoded_oop holds the full 64-bit heap pointer after OOP decompression,
                or zero for primitive arguments that do not have an OOP representation.
                class_name holds the internal JVM class name (slash-separated), used by
                method_args::as<T>() to construct the correct C++ wrapper type.
            */
            struct argument_entry
            {
                void* decoded_oop;
                std::string class_name;
            };

            /*
                @brief Returns the raw decoded OOP at the given index.
                @param index Zero-based argument index (0 = this / first parameter).
            */
            auto operator[](const std::size_t index) const noexcept
                -> void*
            {
                if (index >= this->arguments.size())
                {
                    return nullptr;
                }
                return this->arguments[index].decoded_oop;
            }

            /*
                @brief Returns the argument at the given index as a C++ wrapper.
                @tparam wrapper_type The C++ wrapper class (must be constructible from void* OOP).
                @param index Zero-based argument index.
                @return A new wrapper_type* constructed from the decoded OOP, or nullptr.
            */
            template<typename wrapper_type>
            auto as(const std::size_t index) const noexcept
                -> wrapper_type*
            {
                if (index >= this->arguments.size())
                {
                    return nullptr;
                }
                void* const raw{ this->arguments[index].decoded_oop };
                if (!raw)
                {
                    return nullptr;
                }
                return new wrapper_type{ raw };
            }

            /*
                @brief Returns the number of arguments in this container.
                @details
                Includes both reference-type (object) and primitive arguments.

                Complexity: O(1).
                Exception safety: noexcept.
            */
            auto size() const noexcept
                -> std::size_t
            {
                return this->arguments.size();
            }

            std::vector<argument_entry> arguments{};
        };


        /*
            @brief Represents a HotSpot interpreter frame on the call stack.
            @details
            In HotSpot's interpreter, each method invocation creates a frame on the
            native call stack. On x64 HotSpot, the base pointer (rbp) always points
            to the current frame. The Method pointer is at -24 bytes from rbp, and
            the local variables pointer is at locals_offset bytes from rbp.
        */
        struct frame
        {
        public:
            /*
                @brief Returns a pointer to the Method object of the currently executing method.
                @details
                On x64 HotSpot, `this` is rbp (the interpreter frame base pointer).
                The x64 interpreter frame layout relative to rbp is:
                  rbp + 0   saved caller rbp
                  rbp - 8   return address (pushed by call instruction)
                  rbp - 16  last_sp / expression stack bottom
                  rbp - 24  Method* pointer at this offset
                This corresponds to interpreter_frame_method_offset = -3 words = -24 bytes
                as defined in frame_x86.hpp.
            */
            inline auto get_method() const noexcept
                -> vmhook::hotspot::method*
            {
                // -24 bytes = -3 * sizeof(void*): the Method* slot in the interpreter frame.
                return *reinterpret_cast<vmhook::hotspot::method**>(reinterpret_cast<std::uint8_t*>(const_cast<vmhook::hotspot::frame*>(this)) - 24);
            }

            /*
                @brief Returns a pointer to the local variables array of the currently executing method.
                @details
                The frame slot at [rbp + locals_offset] encodes the locals pointer in one of two
                formats depending on the JDK era (see get_locals() implementation comment for
                the full derivation and proof from the hs_err crash dump).
                The actual offset within the frame is determined at hook time by scanning the i2i
                stub backwards for `mov r14, QWORD PTR [rbp+??]; ret`.  Defaults to -56.
            */
            inline auto get_locals() const noexcept
                -> void**
            {
                /*
                    How the locals pointer is encoded in the frame differs by JDK era.

                    JDK 8 through JDK 20:
                      The stub spills r14 (the locals register) directly into the frame
                      via `mov r14, QWORD PTR [rbp+locals_offset]; ret` in a helper.
                      So [rbp + locals_offset] IS the locals pointer - a valid stack
                      address that passes vmhook::hotspot::is_valid_pointer().

                    JDK 21+:
                      The stub computes and spills an INDEX instead:
                        mov rax, r14
                        sub rax, rbp        ; rax = r14 - rbp  (positive, locals are above rbp)
                        shr rax, 3          ; rax = (r14 - rbp) >> 3
                        push rax            ; [rbp - 56] = index
                      The raw value at [rbp + locals_offset] is therefore a small positive
                      integer (e.g. 3), NOT a pointer.  Recover via: r14 = rbp + index * 8.

                    Detection: if the value at the frame slot is a valid user-space pointer
                    (> 0xFFFF), treat it as a direct locals pointer (JDK 8-20).
                    Otherwise treat it as a slot index (JDK 21+).

                    Proof from hs_err crash dump for JDK 21:
                      RBP = 0x243f888    [rbp-56] = 3
                      R14 = 0x243f8a0    rbp + 3*8 = 0x243f8a0
                */
                const void* const frame_slot_value{ *reinterpret_cast<void* const*>(reinterpret_cast<const std::uint8_t*>(this) + locals_offset) };

                // JDK 8-20: direct pointer stored in the frame slot.
                if (vmhook::hotspot::is_valid_pointer(frame_slot_value))
                {
                    return const_cast<void**>(reinterpret_cast<const void* const*>(frame_slot_value));
                }

                // JDK 21+: the slot holds (r14 - rbp) >> 3 - a non-negative slot index.
                // Recover r14 = rbp + index * sizeof(void*).
                const std::uintptr_t slot_index{ reinterpret_cast<std::uintptr_t>(frame_slot_value) };
                if (slot_index < 0x1000u)
                {
                    return reinterpret_cast<void**>(reinterpret_cast<std::uint8_t*>(const_cast<vmhook::hotspot::frame*>(this)) + slot_index * sizeof(void*));
                }

                return nullptr;
            }

            /*
                @brief Retrieves all method arguments as a typed tuple.
                @tparam types The C++ types of the method arguments in declaration order.
                @return A std::tuple containing all arguments converted to their C++ types.
                @details
                For primitive types, the raw slot value is reinterpreted directly.
                For pointer types, the compressed OOP is decoded via decode_oop_pointer().
                For index 0 on instance methods, this holds the implicit `this` reference.
            */
            template<typename... types>
            auto get_arguments() const noexcept
                -> std::tuple<types...>
            {
                std::int32_t index{ 0 };
                return std::tuple<types...>{ this->get_argument<types>(index++)... };
            }

            /*
                @brief Retrieves all method arguments by auto-detecting types from the method signature.
                @return A method_args container with one entry per argument.
                @details
                Parses the method descriptor (e.g. "(ILjava/lang/String;)V") to determine
                how many arguments there are and which slots correspond to reference types.
                For reference-type arguments the factory registered via vmhook::register_class<T>()
                is used to construct a std::unique_ptr<T>. Primitive arguments are skipped
                (only reference-type args produce entries).

                Usage:
                    auto args = frame->get_arguments();
                    auto* player = args.as<test_target>(0);  // first ref arg
            */
            auto get_arguments() const noexcept
                -> vmhook::hotspot::method_args
            {
                vmhook::hotspot::method_args result{};

                vmhook::hotspot::method* const current_method{ this->get_method() };
                if (!current_method || !vmhook::hotspot::is_valid_pointer(current_method))
                {
                    return result;
                }

                const std::string descriptor{ current_method->get_signature() };
                if (descriptor.empty())
                {
                    return result;
                }

                // Parse the parameter section: everything between '(' and ')'
                const std::size_t open_paren{ descriptor.find('(') };
                const std::size_t close_paren{ descriptor.find(')') };
                if (open_paren == std::string::npos || close_paren == std::string::npos || close_paren <= open_paren)
                {
                    return result;
                }

                void** const locals{ this->get_locals() };
                if (!locals)
                {
                    return result;
                }

                std::int32_t slot_index{ 0 };
                for (std::size_t pos{ open_paren + 1 }; pos < close_paren; )
                {
                    const char ch{ descriptor[pos] };

                    if (ch == 'L')
                    {
                        // Reference type: find the semicolon end, decode OOP
                        const std::size_t semi{ descriptor.find(';', pos) };
                        if (semi == std::string::npos)
                        {
                            break;
                        }
                        const std::string class_name{ descriptor.substr(pos + 1, semi - pos - 1) };

                        void* raw_value{ locals[-slot_index] };
                        void* decoded{ nullptr };
                        if (raw_value)
                        {
                            const std::uintptr_t raw_bits{ reinterpret_cast<std::uintptr_t>(raw_value) };
                            if (raw_bits <= 0xFFFFFFFFull)
                            {
                                decoded = vmhook::hotspot::decode_oop_pointer(static_cast<std::uint32_t>(raw_bits));
                            }
                            else
                            {
                                decoded = raw_value;
                            }
                        }

                        if (decoded && vmhook::hotspot::is_valid_pointer(decoded))
                        {
                            result.arguments.push_back({ decoded, class_name });
                        }
                        else
                        {
                            result.arguments.push_back({ nullptr, class_name });
                        }

                        pos = semi + 1;
                    }
                    else if (ch == '[')
                    {
                        // Array type: skip array dimensions, then skip the base type
                        ++pos;
                        if (pos < close_paren && descriptor[pos] == 'L')
                        {
                            const std::size_t semi{ descriptor.find(';', pos) };
                            pos = (semi == std::string::npos) ? close_paren : semi + 1;
                        }
                        else
                        {
                            // Primitive array like [I, [Z - one char for base type
                            ++pos;
                        }
                    }
                    else
                    {
                        // Primitive: B, C, D, F, I, J, S, Z
                        ++pos;
                    }

                    // long and double occupy two local slots
                    if (ch == 'J' || ch == 'D')
                    {
                        ++slot_index;
                    }
                    ++slot_index;
                }

                return result;
            }

        private:
            /*
                @brief Retrieves a single method argument at the given index.
                @tparam type The C++ type to interpret the argument as.
                @param index Zero-based index into the local variables array.
                @return The argument value, or a default-constructed value on failure.
                @details
                HotSpot lays out the locals array in reverse order relative to the frame:
                  locals[0]  = argument 0  (this for instance methods)
                  locals[-1] = argument 1
                  locals[-2] = argument 2, ...
                So the slot for argument `index` is at locals[-index].
                - Pointer types: the slot holds a 32-bit compressed OOP (zero-extended to 64 bits
                  by the interpreter). vmhook::hotspot::decode_oop_pointer() reconstructs the real
                  address using narrow_oop_base + (compressed << narrow_oop_shift).
                - Primitive types (sizeof <= 8): the bits are copied verbatim via std::memcpy.
            */
            template<typename argument_type>
            auto get_argument(const std::int32_t index) const noexcept
                -> argument_type
            {
                void** const locals{ this->get_locals() };
                if (!locals)
                {
                    return argument_type{};
                }

                // locals[-index]: arguments are stored in descending slot order.
                void* raw_value{ locals[-index] };

                if constexpr (std::is_pointer_v<argument_type>)
                {
                    if (!raw_value)
                    {
                        return nullptr;
                    }
                    const std::uintptr_t raw_bits{ reinterpret_cast<std::uintptr_t>(raw_value) };

                    // HotSpot can expose object arguments either as:
                    //  - compressed oops (32-bit narrow value in the slot), or
                    //  - direct oop pointers (already decoded 64-bit address).
                    // Prefer decode only for narrow-looking values; otherwise use the
                    // direct pointer as-is.
                    if (raw_bits <= 0xFFFFFFFFull)
                    {
                        return reinterpret_cast<argument_type>(vmhook::hotspot::decode_oop_pointer(static_cast<std::uint32_t>(raw_bits)));
                    }

                    return reinterpret_cast<argument_type>(raw_value);
                }
                else if constexpr (sizeof(argument_type) <= sizeof(void*))
                {
                    argument_type result{};
                    std::memcpy(&result, &raw_value, sizeof(argument_type));
                    return result;
                }
                else
                {
                    return argument_type{};
                }
            }
        };

        /*
            @brief Installs a low-level hook on a HotSpot interpreter-to-interpreter (i2i) stub.
            @details
            midi2i_hook patches the i2i interpreter stub of a Java method at the injection
            point found by find_hook_location() with a 5-byte relative JMP instruction that
            redirects execution to an allocated trampoline stub. The trampoline saves all
            volatile registers, calls common_detour with (frame*, java_thread*, return_slot*), and
            then either returns the custom value or resumes normal execution depending on
            whether the detour set the cancel flag.

            The trampoline stub is allocated within 32-bit relative JMP range of the target.
            The stub layout is: [original 8 bytes] [assembly trampoline].
            Only one trampoline is allocated per unique i2i entry point, even if multiple
            methods share the same stub.
        */
        class midi2i_hook final
        {
        public:
            /*
                @brief Installs the hook on the given target address.
                @param target  Pointer to the injection point within the i2i stub.
                @param detour  The C++ function to call when the hook fires.
            */
            midi2i_hook(std::uint8_t* const target, const vmhook::hotspot::detour_function_t detour)
                : target{ target }
                , allocated{ nullptr }
                , allocated_size{ 0 }
                , error{ true }
            {
#if !VMHOOK_RUNTIME_HOOKING_AVAILABLE
                // The trampoline emits Windows/SysV x64 bytes and depends on
                // the HotSpot interpreter frame layout for that ABI.  arm64
                // and iOS (no JIT, no HotSpot) cannot use this path; leave
                // the hook in its error state so callers see a clean false.
                (void)target;
                (void)detour;
                return;
#else
                static constexpr std::int32_t HOOK_SIZE{ 8 };
                static constexpr std::int32_t JMP_SIZE{ 5 };
                static constexpr std::uint8_t JMP_OPCODE{ 0xE9 };

#if VMHOOK_OS_WINDOWS
                // ------------------------------------------------------------
                // Microsoft x64 calling convention trampoline.
                // Args: rcx, rdx, r8, r9.  Shadow space: 32 bytes.
                // Caller-saved: rax, rcx, rdx, r8, r9, r10, r11.
                // ------------------------------------------------------------
                static constexpr std::int32_t JE_OFFSET{ 0x32 };   // offset of je in assembly
                static constexpr std::int32_t JE_SIZE{ 6 };
                static constexpr std::int32_t RESUME_OFFSET{ 0x63 };
                static constexpr std::int32_t RESUME_JMP_OFFSET{ 0x73 };
                static constexpr std::int32_t RESUME_JMP_SIZE{ 5 };
                static constexpr std::int32_t DETOUR_ADDRESS_OFFSET{ 0x78 };

                // Stack layout after the two pushes:
                //   [rsp+0]  return_slot::cancel  (bool, 1 byte; rest zeroed)
                //   [rsp+8]  return_slot::retval  (int64_t)
                std::uint8_t assembly[]
                {
                    0x50,                                           // push rax
                    0x51,                                           // push rcx
                    0x52,                                           // push rdx
                    0x41, 0x50,                                     // push r8
                    0x41, 0x51,                                     // push r9
                    0x41, 0x52,                                     // push r10
                    0x41, 0x53,                                     // push r11
                    0x55,                                           // push rbp
                    0x6A, 0x00,                                     // push 0x0  ; return_slot::retval  (slot +8)
                    0x6A, 0x00,                                     // push 0x0  ; return_slot::cancel  (slot +0)

                    0x48, 0x89, 0xE9,                               // mov rcx, rbp   ; frame*
                    0x4C, 0x89, 0xFA,                               // mov rdx, r15   ; java_thread*
                    0x4C, 0x8D, 0x04, 0x24,                         // lea r8, [rsp]  ; return_slot*

                    0x48, 0x89, 0xE5,                               // mov rbp, rsp
                    0x48, 0x83, 0xE4, 0xF0,                         // and rsp, -16
                    0x48, 0x83, 0xEC, 0x20,                         // sub rsp, 0x20

                    0xFF, 0x15, 0x4D, 0x00, 0x00, 0x00,             // call [rip+0x4D]

                    0x48, 0x89, 0xEC,                               // mov rsp, rbp

                    0x80, 0x3C, 0x24, 0x00,                         // cmp byte ptr [rsp], 0
                    0x0F, 0x84, 0x00, 0x00, 0x00, 0x00,             // je resume  ; cancel==false

                    // cancel path (cancel==true, falls through):
                    0x48, 0x8B, 0x44, 0x24, 0x08,                   // mov rax, [rsp+8]    ; return_slot::retval
                    0x66, 0x48, 0x0F, 0x6E, 0xC0,                   // movq xmm0, rax      ; float/double return
                    0x48, 0x83, 0xC4, 0x10,                         // add rsp, 0x10       ; discard return_slot
                    0x5D,                                           // pop rbp
                    0x41, 0x5B,                                     // pop r11
                    0x41, 0x5A,                                     // pop r10
                    0x41, 0x59,                                     // pop r9
                    0x41, 0x58,                                     // pop r8
                    0x5A,                                           // pop rdx
                    0x59,                                           // pop rcx
                    0x48, 0x83, 0xC4, 0x08,                         // add rsp, 0x8        ; discard saved original rax
                    0x48, 0x8B, 0x5D, 0xF8,                         // mov rbx, [rbp-8]
                    0x48, 0x89, 0xEC,                               // mov rsp, rbp
                    0x5D,                                           // pop rbp
                    0x5E,                                           // pop rsi
                    0x48, 0x89, 0xDC,                               // mov rsp, rbx
                    0xFF, 0xE6,                                     // jmp rsi

                    // resume path (cancel==false):
                    0x48, 0x83, 0xC4, 0x10,                         // add rsp, 0x10       ; discard return_slot
                    0x5D,                                           // pop rbp
                    0x41, 0x5B,                                     // pop r11
                    0x41, 0x5A,                                     // pop r10
                    0x41, 0x59,                                     // pop r9
                    0x41, 0x58,                                     // pop r8
                    0x5A,                                           // pop rdx
                    0x59,                                           // pop rcx
                    0x58,                                           // pop rax
                    0xE9, 0x00, 0x00, 0x00, 0x00,                   // jmp target+HOOK_SIZE

                    // data slot: detour function pointer
                    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
                };
#else
                // ------------------------------------------------------------
                // System V AMD64 calling convention trampoline.
                // Args: rdi, rsi, rdx, rcx, r8, r9.  No shadow space.
                // Caller-saved: rax, rcx, rdx, rsi, rdi, r8, r9, r10, r11.
                //
                // Byte layout (the offsets below are runtime constants; the
                // values match the cumulative widths of the instructions
                // above each landmark):
                //
                //   off 0   push rax/rdi/rsi/rdx/rcx/r8/r9/r10/r11/rbp  (14)
                //   off 14  push 0, push 0                              (4)
                //   off 18  mov rdi,rbp ; mov rsi,r15 ; mov rdx,rsp     (9)
                //   off 27  mov rbp,rsp ; and rsp,-16                   (7)
                //   off 34  call [rip+disp32]                           (6)
                //   off 40  mov rsp,rbp                                 (3)
                //   off 43  cmp byte [rsp],0                            (4)
                //   off 47  je rel32                                    (6)  <- JE_OFFSET
                //   off 53  cancel path                                 (45)
                //   off 98  resume path                                 (23) <- RESUME_OFFSET
                //   off 116 jmp rel32 (inside resume path)              (5)  <- RESUME_JMP_OFFSET
                //   off 121 detour-function-pointer slot                (8)  <- DETOUR_ADDRESS_OFFSET
                //   off 129 end
                //
                // The 6-byte call uses disp32 = 121 − 40 = 81 = 0x51 so
                // [rip+0x51] dereferences the detour function pointer.
                // ------------------------------------------------------------
                static constexpr std::int32_t JE_OFFSET{ 0x2F };
                static constexpr std::int32_t JE_SIZE{ 6 };
                static constexpr std::int32_t RESUME_OFFSET{ 0x62 };
                static constexpr std::int32_t RESUME_JMP_OFFSET{ 0x74 };
                static constexpr std::int32_t RESUME_JMP_SIZE{ 5 };
                static constexpr std::int32_t DETOUR_ADDRESS_OFFSET{ 0x79 };

                std::uint8_t assembly[]
                {
                    0x50,                                           // push rax
                    0x57,                                           // push rdi
                    0x56,                                           // push rsi
                    0x52,                                           // push rdx
                    0x51,                                           // push rcx
                    0x41, 0x50,                                     // push r8
                    0x41, 0x51,                                     // push r9
                    0x41, 0x52,                                     // push r10
                    0x41, 0x53,                                     // push r11
                    0x55,                                           // push rbp
                    0x6A, 0x00,                                     // push 0  ; retval
                    0x6A, 0x00,                                     // push 0  ; cancel

                    0x48, 0x89, 0xEF,                               // mov rdi, rbp   ; arg1 frame*
                    0x4C, 0x89, 0xFE,                               // mov rsi, r15   ; arg2 java_thread*
                    0x48, 0x89, 0xE2,                               // mov rdx, rsp   ; arg3 return_slot*

                    0x48, 0x89, 0xE5,                               // mov rbp, rsp
                    0x48, 0x83, 0xE4, 0xF0,                         // and rsp, -16

                    0xFF, 0x15, 0x51, 0x00, 0x00, 0x00,             // call [rip+0x51]

                    0x48, 0x89, 0xEC,                               // mov rsp, rbp

                    0x80, 0x3C, 0x24, 0x00,                         // cmp byte ptr [rsp], 0
                    0x0F, 0x84, 0x00, 0x00, 0x00, 0x00,             // je resume  (offset filled below)

                    // cancel path (offset 0x35..0x61, 45 bytes):
                    0x48, 0x8B, 0x44, 0x24, 0x08,                   // mov rax, [rsp+8]
                    0x66, 0x48, 0x0F, 0x6E, 0xC0,                   // movq xmm0, rax
                    0x48, 0x83, 0xC4, 0x10,                         // add rsp, 0x10
                    0x5D,                                           // pop rbp
                    0x41, 0x5B,                                     // pop r11
                    0x41, 0x5A,                                     // pop r10
                    0x41, 0x59,                                     // pop r9
                    0x41, 0x58,                                     // pop r8
                    0x59,                                           // pop rcx
                    0x5A,                                           // pop rdx
                    0x5E,                                           // pop rsi
                    0x5F,                                           // pop rdi
                    0x48, 0x83, 0xC4, 0x08,                         // add rsp, 0x8 ; discard saved rax
                    0x48, 0x8B, 0x5D, 0xF8,                         // mov rbx, [rbp-8]
                    0x48, 0x89, 0xEC,                               // mov rsp, rbp
                    0x5D,                                           // pop rbp
                    0x5E,                                           // pop rsi
                    0x48, 0x89, 0xDC,                               // mov rsp, rbx
                    0xFF, 0xE6,                                     // jmp rsi

                    // resume path (offset 0x62..0x78, 23 bytes):
                    0x48, 0x83, 0xC4, 0x10,                         // add rsp, 0x10
                    0x5D,                                           // pop rbp
                    0x41, 0x5B,                                     // pop r11
                    0x41, 0x5A,                                     // pop r10
                    0x41, 0x59,                                     // pop r9
                    0x41, 0x58,                                     // pop r8
                    0x59,                                           // pop rcx
                    0x5A,                                           // pop rdx
                    0x5E,                                           // pop rsi
                    0x5F,                                           // pop rdi
                    0x58,                                           // pop rax
                    0xE9, 0x00, 0x00, 0x00, 0x00,                   // jmp target+HOOK_SIZE

                    // data slot (offset 0x79..0x80):
                    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
                };
#endif

                const std::size_t total_size{ static_cast<std::size_t>(HOOK_SIZE) + sizeof(assembly) };
                this->allocated = vmhook::hotspot::allocate_nearby_memory(target, total_size);
                this->allocated_size = total_size;

                try
                {
                    if (!this->allocated)
                    {
                        throw vmhook::exception{ "Failed to allocate memory for hook." };
                    }
                }
                catch (const std::exception& exception)
                {
                    VMHOOK_LOG("{} midi2i_hook::midi2i_hook {}", vmhook::error_tag, exception.what());
                    return;
                }

                const std::int32_t je_delta{ RESUME_OFFSET - (JE_OFFSET + JE_SIZE) };
                *reinterpret_cast<std::int32_t*>(assembly + JE_OFFSET + 2) = je_delta;

                const std::int32_t resume_jmp_delta{ static_cast<std::int32_t>(
                    target + HOOK_SIZE - (this->allocated + HOOK_SIZE + RESUME_JMP_OFFSET + RESUME_JMP_SIZE)) };
                *reinterpret_cast<std::int32_t*>(assembly + RESUME_JMP_OFFSET + 1) = resume_jmp_delta;

                *reinterpret_cast<vmhook::hotspot::detour_function_t*>(assembly + DETOUR_ADDRESS_OFFSET) = detour;

                std::memcpy(this->allocated, target, HOOK_SIZE);
                std::memcpy(this->allocated + HOOK_SIZE, assembly, sizeof(assembly));

                std::uint32_t old_protect{};
                vmhook::os::protect(this->allocated, total_size,
                                    vmhook::os::memory_protection::execute_read, &old_protect);
                vmhook::os::protect(target, JMP_SIZE,
                                    vmhook::os::memory_protection::execute_rw, &old_protect);

                target[0] = JMP_OPCODE;
                const std::int32_t jmp_delta{ static_cast<std::int32_t>(this->allocated - (target + JMP_SIZE)) };
                *reinterpret_cast<std::int32_t*>(target + 1) = jmp_delta;

                // Restore the target page's original protection.  We don't have a portable
                // way to spell the original native flags, so we apply execute_read which
                // matches the JVM's normal state for generated code.
                vmhook::os::protect(target, JMP_SIZE,
                                    vmhook::os::memory_protection::execute_read, &old_protect);
                vmhook::os::flush_instruction_cache(target, JMP_SIZE);
                vmhook::os::flush_instruction_cache(this->allocated, total_size);

                this->error = false;
#endif  // VMHOOK_RUNTIME_HOOKING_AVAILABLE
            }

            /*
                @brief Removes the hook and restores the original code at the target.
            */
            ~midi2i_hook()
            {
                if (this->error)
                {
                    return;
                }

                static constexpr std::uint8_t JMP_OPCODE{ 0xE9 };

                std::uint32_t old_protect{};
                if (this->target[0] == JMP_OPCODE
                    && vmhook::os::protect(this->target, 5,
                                           vmhook::os::memory_protection::execute_rw, &old_protect))
                {
                    std::memcpy(this->target, this->allocated, 5);
                    vmhook::os::protect(this->target, 5,
                                        vmhook::os::memory_protection::execute_read, &old_protect);
                    vmhook::os::flush_instruction_cache(this->target, 5);
                }

                vmhook::os::release(this->allocated, this->allocated_size);
            }

            inline auto has_error() const noexcept -> bool
            {
                return this->error;
            }

        private:
            std::uint8_t* target{ nullptr };
            std::uint8_t* allocated{ nullptr };
            std::size_t   allocated_size{ 0 };
            bool          error{ true };
        };

        /*
            @brief Stores the association between a HotSpot Method object and its C++ detour function.
            @details  Also holds the original entry points and _code pointer so shutdown_hooks()
                      can fully restore the method's state, including re-linking the nmethod if the
                      method was JIT-compiled when the hook was installed.
        */
        struct hooked_method
        {
            vmhook::hotspot::method* method{ nullptr };
            std::function<void(vmhook::hotspot::frame*, vmhook::hotspot::java_thread*, vmhook::hotspot::return_slot*)> detour;
            void* original_code{ nullptr };
            void* original_from_interpreted_entry{ nullptr };
            void* original_from_compiled_entry{ nullptr };
            bool     was_compiled{ false };
        };

        /*
            @brief Stores the association between an i2i entry point and its installed midi2i_hook.
            @details
            Since multiple Java methods can share the same i2i stub, only one trampoline
            is allocated per unique i2i entry point.
        */
        struct i2i_hook_data
        {
            void* i2i_entry{ nullptr };
            vmhook::hotspot::midi2i_hook* hook{ nullptr };
        };

        /*
            @brief Global list of all currently hooked Java methods and their detour functions.
        */
        inline std::vector<vmhook::hotspot::hooked_method> g_hooked_methods{};

        /*
            @brief Global list of all i2i entry points that have been patched and their hooks.
        */
        inline std::vector<vmhook::hotspot::i2i_hook_data> g_hooked_i2i_entries{};

        /*
            @brief Common detour function invoked by the trampoline stub for every intercepted method call.
            @param f      Pointer to the current HotSpot interpreter frame (rbp at hook site).
            @param thread Pointer to the current HotSpot JavaThread (r15).
            @param slot Pointer to the return slot on the trampoline stack.
            @details
            Single entry point for all midi2i_hook trampolines.  Finds the matching
            per-method detour in g_hooked_methods and dispatches it.

            The thread-state precondition check was removed because the injection point
            (the `mov BYTE PTR [r15+X], Y` instruction) is reached while the thread may
            still be in a transition state on JDK 21+ builds - the state is not
            necessarily _thread_in_Java yet at that exact instruction.  After the user
            detour returns we force the state to _thread_in_Java so the bytecode
            dispatch that follows finds the correct state.
        */
        static auto common_detour(vmhook::hotspot::frame* const frame_pointer, vmhook::hotspot::java_thread* const thread, vmhook::hotspot::return_slot* const slot)
            -> void
        {
            try
            {
                if (!thread || !vmhook::hotspot::is_valid_pointer(thread))
                {
                    throw vmhook::exception{ "JavaThread pointer is null or invalid." };
                }

                const method* const current_method{ frame_pointer->get_method() };
                struct current_thread_guard
                {
                    vmhook::hotspot::java_thread* previous;

                    explicit current_thread_guard(vmhook::hotspot::java_thread* const thread_pointer) noexcept
                        : previous{ vmhook::hotspot::current_java_thread }
                    {
                        vmhook::hotspot::current_java_thread = thread_pointer;
                        vmhook::hotspot::last_java_thread.store(thread_pointer, std::memory_order_relaxed);
                    }

                    ~current_thread_guard()
                    {
                        vmhook::hotspot::current_java_thread = this->previous;
                    }
                } guard{ thread };

                for (const vmhook::hotspot::hooked_method& hook : vmhook::hotspot::g_hooked_methods)
                {
                    if (hook.method == current_method)
                    {
                        hook.detour(frame_pointer, thread, slot);
                        // Ensure the thread state is _thread_in_Java after the detour
                        // so the bytecode dispatcher finds a consistent state.
                        thread->set_thread_state(vmhook::hotspot::java_thread_state::_thread_in_Java);
                        return;
                    }
                }
            }
            catch (const std::exception& exception)
            {
                VMHOOK_LOG("{} common_detour() {}", vmhook::error_tag, exception.what());
            }
        }

        /*
            @brief Bitmask of HotSpot access flags used to disable JIT compilation on hooked methods.
            @details
            OR'd into Method._access_flags to prevent C1, C2, and OSR compilation.
            - JVM_ACC_NOT_C2_COMPILABLE   (0x02000000)
            - JVM_ACC_NOT_C1_COMPILABLE   (0x04000000)
            - JVM_ACC_NOT_C2_OSR_COMPILABLE (0x08000000)
            - JVM_ACC_QUEUED              (0x01000000)
        */
        inline constexpr std::int32_t NO_COMPILE =
            0x02000000 |
            0x04000000 |
            0x08000000 |
            0x01000000;

        /*
            @brief Enables or disables the _dont_inline flag on a HotSpot Method object.
            @details
            The _dont_inline flag is stored in Method._flags at bit position 2.
            When set, it prevents the JIT from inlining this method at any call site.
        */
        static auto set_dont_inline(const vmhook::hotspot::method* const method_pointer, const bool enabled) noexcept
            -> void
        {
            std::uint16_t* const flags{ method_pointer->get_flags() };
            if (!flags)
            {
                return;
            }

            if (enabled)
            {
                *flags |= (1 << 2);
            }
            else
            {
                *flags &= static_cast<std::uint16_t>(~(1 << 2));
            }
        }

        /*
            @brief Reads the c2i (compiled-to-interpreter) adapter entry from an AdapterHandlerEntry.
            @param adapter  AdapterHandlerEntry* stored in Method._adapter.
            @return The c2i entry address, or nullptr if not available.
            @details
            Used when deoptimising a hook to redirect Method._from_compiled_entry to the
            c2i adapter, so compiled callers that miss their inline cache re-enter the
            interpreter and reach our patched i2i stub.
            AdapterHandlerEntry._c2i_entry is exported via gHotSpotVMStructs on all
            supported JDK versions (8 through 26).
        */
        static auto get_c2i_entry_from_adapter(void* const adapter) noexcept
            -> void*
        {
            if (!adapter || !vmhook::hotspot::is_valid_pointer(adapter))
            {
                return nullptr;
            }
            static const vmhook::hotspot::vm_struct_entry_t* const entry{ vmhook::hotspot::iterate_struct_entries("AdapterHandlerEntry", "_c2i_entry") };
            if (!entry)
            {
                return nullptr;
            }

            return *reinterpret_cast<void**>(reinterpret_cast<std::uint8_t*>(adapter) + entry->offset);
        }
    }

    namespace detail
    {
        /*
            @brief Finds a klass via JNI::FindClass using the context class loader.
            @details
            Fallback used by find_class() when the ClassLoaderDataGraph walk fails (e.g.
            when the class is loaded by a non-bootstrap class loader that is not yet
            reachable through the graph).  Calls JNIEnv::FindClass which uses the calling
            thread's context class loader, then resolves the returned jclass handle to the
            underlying Klass* via jni_klass_from_class_mirror.
            Defined after the JNI helper section.

            Complexity: O(lookup in the JVM class loader hierarchy).
            Exception safety: noexcept — returns nullptr on any failure.

            @param class_name  Internal JVM class name with '/' separators.
            @return The matching klass*, or nullptr if not found.
        */
        inline auto jni_find_class_with_context_loader(const std::string_view class_name) noexcept
            -> vmhook::hotspot::klass*;
    }

    // --- Cache and class lookup -----------------------------------------------

    /*
        @brief Cache of klass pointers keyed by their internal class name.
        @details
        Populated by find_class() on first lookup of each class name.
        Subsequent calls return the cached klass* directly without repeating
        the full ClassLoaderDataGraph walk.
    */
    inline std::unordered_map<std::string, vmhook::hotspot::klass*> klass_lookup_cache{};

    /*
        @brief Finds a loaded Java class by its internal name using HotSpot internals only.
        @param class_name The internal JVM class name using '/' separators
                          (e.g. "java/lang/String", "net/minecraft/client/Minecraft").
        @return Pointer to the matching klass if found, nullptr otherwise.
        @details
        Searches all loaded classloaders for a class matching the given name by walking
        the HotSpot ClassLoaderDataGraph entirely via gHotSpotVMStructs, without any
        JNI or JVMTI calls. Results are cached on first lookup.
    */
    static auto find_class(const std::string_view class_name)
        -> vmhook::hotspot::klass*
    {
        const auto cache_entry{ vmhook::klass_lookup_cache.find(std::string{ class_name }) };
        if (cache_entry != vmhook::klass_lookup_cache.end())
        {
            return cache_entry->second;
        }

        try
        {
            const vmhook::hotspot::class_loader_data_graph graph{};
            vmhook::hotspot::klass* found_klass{ graph.find_klass(class_name) };

            if (!found_klass)
            {
                found_klass = vmhook::detail::jni_find_class_with_context_loader(class_name);
                if (!found_klass)
                {
                    return nullptr;
                }
            }

            vmhook::klass_lookup_cache.insert({ std::string{ class_name }, found_klass });
            return found_klass;
        }
        catch (const std::exception& exception)
        {
            VMHOOK_LOG("{} vmhook::find_class() for {}: {}", vmhook::error_tag, class_name, exception.what());
            return nullptr;
        }
    }

    /*
        @brief Associates a C++ type with its corresponding Java class name.
        @tparam T The C++ type to register.
        @param class_name The internal JVM class name using '/' separators.
        @return true if the class was found in the JVM and successfully registered,
                false if the class could not be found.
        @details
        Stores the mapping from the C++ type_index of T to class_name in type_to_class_map,
        then verifies the class exists in the JVM by calling find_class().
        Registration is required before calling hook<T>().
    */
    template<class wrapper_type>
    static auto register_class(const std::string_view class_name) noexcept
        -> bool
    {
        vmhook::hotspot::klass* const verified_klass{ vmhook::find_class(class_name) };

        if (!verified_klass)
        {
            VMHOOK_LOG("{} register_class() for {}: class not found in JVM.", vmhook::error_tag, class_name);
            return false;
        }

        vmhook::type_to_class_map.insert_or_assign(std::type_index{ typeid(wrapper_type) }, std::string{ class_name });

        // Store a factory function so field_proxy::get_as<T>() and frame::get_arguments()
        // can construct C++ wrapper objects from decoded Java object references.
        // The factory returns a raw pointer; consumers immediately wrap it in
        // a unique_ptr at the call site.  See type_factory_function_t for why.
        vmhook::g_type_factory_map.emplace(std::string{ class_name }, +[](void* instance)
            -> object_base*
            {
                return new wrapper_type{ instance };
            }
        );

        return true;
    }

    // -------------------------------------------------------------------------
    // Watchers: long-lived background pollers for field-value changes and
    // for newly-loaded Java classes.  Both are polling-based — the JVM
    // doesn't expose interpreter-level field-write or class-load events
    // through gHotSpotVMStructs alone, so we observe at a configurable
    // cadence.  watch_handle stops the watcher when it goes out of scope.
    // -------------------------------------------------------------------------

    /*
        @brief RAII handle for a background watcher (field-change / class-load).
        @details
        The watcher thread keeps running as long as at least one handle is
        live.  Destroying the last handle signals the thread to exit and
        joins it.  Move-only.
    */
    class watch_handle
    {
    public:
        struct control_block
        {
            std::atomic_bool running{ true };
            std::thread     worker;
        };

        watch_handle() = default;
        explicit watch_handle(std::shared_ptr<control_block> cb) noexcept
            : block{ std::move(cb) }
        {
        }

        watch_handle(const watch_handle&) = delete;
        auto operator=(const watch_handle&) -> watch_handle & = delete;

        watch_handle(watch_handle&& other) noexcept
            : block{ std::move(other.block) }
        {
        }

        auto operator=(watch_handle&& other) noexcept -> watch_handle &
        {
            if (this != &other)
            {
                this->stop();
                this->block = std::move(other.block);
            }
            return *this;
        }

        ~watch_handle()
        {
            this->stop();
        }

        /*
            @brief Stops the watcher synchronously (idempotent).
        */
        auto stop() noexcept -> void
        {
            if (!this->block)
            {
                return;
            }
            this->block->running.store(false, std::memory_order_relaxed);
            if (this->block->worker.joinable())
            {
                this->block->worker.join();
            }
            this->block.reset();
        }

        /*
            @brief Returns true while the background thread is running.
        */
        auto running() const noexcept -> bool
        {
            return this->block && this->block->running.load(std::memory_order_relaxed);
        }

    private:
        std::shared_ptr<control_block> block{};
    };

    // The function templates `watch_static_field` and `on_class_loaded`
    // depend on `vmhook::object_base::get_field` (a static method).  At
    // this point in the header object_base is only forward-declared,
    // which Clang's template-body checker flags eagerly.  The full
    // definitions live further down (search for "watch_static_field").

    // --- Internal helpers for typed hook API ---------------------------------

    namespace detail
    {
        /*
            @brief Type trait that extracts the argument-types tuple from any callable type.
            @details
            Specialisations cover the four common callable forms:
              - Plain function pointers: return_type(*)(args...)
              - std::function instances: std::function<return_type(args...)>
              - Lambdas and functors:    operator() const member pointer
              - Non-const member functions: return_type(class::*)(args...)
            The associated member type args_tuple_t is a std::tuple of the raw argument
            types.  Used by the typed hook<T>() overload to enumerate the Java parameter
            descriptors at compile time so that the interpreter-frame read is fully
            type-safe.

            Exception safety: noexcept — compile-time trait, no runtime cost.
        */
        template<typename function_type, typename = void>
        struct function_traits;

        template<typename return_type, typename... argument_types>
        struct function_traits<return_type(*)(argument_types...)>
        {
            using args_tuple_t = std::tuple<argument_types...>;
        };

        template<typename return_type, typename... argument_types>
        struct function_traits<std::function<return_type(argument_types...)>>
        {
            using args_tuple_t = std::tuple<argument_types...>;
        };

        template<typename function_type>
        struct function_traits<function_type, std::void_t<decltype(&function_type::operator())>>
            : function_traits<decltype(&function_type::operator())>
        {
        };

        template<typename class_type, typename return_type, typename... argument_types>
        struct function_traits<return_type(class_type::*)(argument_types...) const>
        {
            using args_tuple_t = std::tuple<argument_types...>;
        };

        template<typename class_type, typename return_type, typename... argument_types>
        struct function_traits<return_type(class_type::*)(argument_types...)>
        {
            using args_tuple_t = std::tuple<argument_types...>;
        };

        /*
            @brief Removes the first element from a std::tuple type.
            @details
            Used to strip the leading vmhook::return_value& argument from the hook
            callback's args_tuple_t so that only the Java-visible parameters remain.
            The resulting member type_t is the truncated tuple.

            Exception safety: noexcept — compile-time trait, no runtime cost.
        */
        template<typename tuple_type>
        struct tuple_tail;

        template<typename first_type, typename... remaining_types>
        struct tuple_tail<std::tuple<first_type, remaining_types...>>
        {
            using type_t = std::tuple<remaining_types...>;
        };

        /*
            @brief Extracts a single Java method argument from the interpreter frame as value_type.
            @details
            Reads the local-variable slot at position index (negated relative to the locals
            pointer) and converts the raw slot bits to the requested C++ type using a
            compile-time if-constexpr dispatch:
              - std::string               decoded OOP fed into read_java_string()
              - std::unique_ptr<U>        decoded OOP fed into the registered factory for U
              - pointer types             decoded compressed OOP
              - primitive / trivial types raw slot bits via memcpy

            Uses the public get_locals() path so it does not require friendship with frame.

            Complexity: O(1) for primitives; O(S) for strings where S = string length.
            Exception safety: noexcept-boundary — underlying helpers may throw internally
                              but the function returns base_t{} on any error path.

            @param frame  Interpreter frame whose locals to read.
            @param index  Zero-based argument index (0 = this / first parameter).
            @return The decoded argument value, or base_t{} on failure.
        */
        template<typename value_type>
        auto extract_frame_arg(vmhook::hotspot::frame* const frame, const std::int32_t index)
            -> std::remove_cvref_t<value_type>
        {
            using base_t = std::remove_cvref_t<value_type>;

            void** const locals{ frame->get_locals() };
            if (!locals)
            {
                return base_t{};
            }

            void* const raw_value{ locals[-index] };

            // Decode a compressed OOP (32-bit narrow value) to a full 64-bit pointer.
            auto decode_oop = [](void* raw_value)
                -> void*
                {
                    if (!raw_value)
                    {
                        return nullptr;
                    }

                    const std::uintptr_t bits{ reinterpret_cast<std::uintptr_t>(raw_value) };
                    return (bits <= 0xFFFFFFFFull)
                        ? vmhook::hotspot::decode_oop_pointer(static_cast<std::uint32_t>(bits))
                        : raw_value;
                };

            if constexpr (std::is_same_v<base_t, std::string>)
            {
                return vmhook::read_java_string(decode_oop(raw_value));
            }
            else if constexpr (is_unique_ptr_v<base_t>)
            {
                using element_t = typename base_t::element_type;
                void* const oop{ decode_oop(raw_value) };
                if (!oop)
                {
                    return nullptr;
                }
                const auto type_it{ vmhook::type_to_class_map.find(std::type_index{ typeid(element_t) }) };
                if (type_it == vmhook::type_to_class_map.end())
                {
                    return nullptr;
                }
                const auto factory_it{ vmhook::g_type_factory_map.find(type_it->second) };
                if (factory_it == vmhook::g_type_factory_map.end())
                {
                    return nullptr;
                }
                // factory returns object_base*; downcast to element_t* and
                // hand it to the unique_ptr at the call site.
                return base_t{ static_cast<element_t*>(factory_it->second(oop)) };
            }
            else if constexpr (std::is_pointer_v<base_t>)
            {
                return reinterpret_cast<base_t>(decode_oop(raw_value));
            }
            else if constexpr (sizeof(base_t) <= sizeof(void*))
            {
                base_t result{};
                std::memcpy(&result, &raw_value, sizeof(base_t));
                return result;
            }
            else
            {
                return base_t{};
            }
        }

        /*
            @brief Type trait that detects std::unique_ptr<T> where T derives from vmhook::object_base.
            @details
            Used in return_value::set_arg() to identify arguments that are managed C++
            wrappers around Java objects.  The primary template inherits from std::false_type;
            the partial specialisation inherits from std::bool_constant<is_base_of<object_base, T>>.

            Exception safety: noexcept — compile-time trait, no runtime cost.
        */
        template<typename value_type>
        struct is_unique_object_ptr : std::false_type {};

        template<typename value_type, typename deleter_type>
        struct is_unique_object_ptr<std::unique_ptr<value_type, deleter_type>>
            : std::bool_constant<std::is_base_of_v<vmhook::object_base, value_type>> {};

        /*
            @brief Decodes a JNI local reference to a raw heap OOP pointer.
            @details
            JNI object references (jobject, jclass, jstring, etc.) are handles stored in
            the current thread's JNI handle block, not direct heap pointers.  This function
            dereferences the handle to obtain the underlying OOP.
            Defined after the JNI helper section below.

            Complexity: O(1).
            Exception safety: noexcept — returns nullptr on failure.
        */
        inline auto jni_decode_object(void* object_handle) noexcept
            -> void*;

        /*
            @brief Creates a new Java String from a UTF-8 string_view using JNI.
            @details
            Calls JNIEnv::NewStringUTF via the cached current_jni_env.  Returns a JNI
            local reference (jobject handle), not a raw OOP.  Use jni_decode_object() to
            obtain the OOP if raw heap access is needed.
            Defined after the JNI helper section below.

            Complexity: O(N) where N = length of value.
            Exception safety: noexcept — returns nullptr if JNIEnv is unavailable or allocation fails.
        */
        inline auto jni_new_string_utf(std::string_view value) noexcept
            -> void*;
    } // namespace detail

    inline auto return_value::caller() const noexcept -> caller_info
    {
        caller_info empty{};
        if (!this->stack_frame)
        {
            return empty;
        }

        // The HotSpot x64 interpreter writes the caller's rbp at [rbp+0].
        // Saved-rbp chains only work when the caller is *also* an
        // interpreted frame; if the caller is compiled or native, the
        // chain breaks and pointer checks below will reject the read.
        void* const caller_rbp_slot{ this->stack_frame };
        if (!vmhook::hotspot::is_valid_pointer(caller_rbp_slot))
        {
            return empty;
        }
        void* const caller_rbp{ *reinterpret_cast<void* const*>(caller_rbp_slot) };
        if (!vmhook::hotspot::is_valid_pointer(caller_rbp))
        {
            return empty;
        }

        // The Method* lives 3 words (24 bytes) below rbp in the
        // interpreter frame layout (see frame::get_method for the
        // matching read on the current frame).
        void* const caller_method_slot{
            reinterpret_cast<std::uint8_t*>(caller_rbp) - 24 };
        if (!vmhook::hotspot::is_valid_pointer(caller_method_slot))
        {
            return empty;
        }
        auto* const caller_method{
            *reinterpret_cast<vmhook::hotspot::method* const*>(caller_method_slot) };
        if (!caller_method || !vmhook::hotspot::is_valid_pointer(caller_method))
        {
            return empty;
        }

        // Validate by trying to read the method name through the same safe
        // path the rest of vmhook uses.  An empty string signals an
        // unidentifiable frame.
        std::string method_name{ caller_method->get_name() };
        if (method_name.empty())
        {
            return empty;
        }

        caller_info info{};
        info.method      = caller_method;
        info.method_name = std::move(method_name);
        info.signature   = caller_method->get_signature();

        // Best-effort class-name lookup.  Method -> ConstMethod ->
        // ConstantPool holds a back-pointer to the owning Klass via
        // _pool_holder; we read it through the cached VMStruct offset.
        if (const auto* const const_method{ caller_method->get_const_method() })
        {
            if (auto* const cp{ const_method->get_constants() })
            {
                static const auto* const pool_holder_entry{
                    vmhook::hotspot::iterate_struct_entries("ConstantPool", "_pool_holder") };
                if (pool_holder_entry)
                {
                    auto* const klass{ *reinterpret_cast<vmhook::hotspot::klass* const*>(
                        reinterpret_cast<const std::uint8_t*>(cp) + pool_holder_entry->offset) };
                    if (klass && vmhook::hotspot::is_valid_pointer(klass))
                    {
                        if (auto* const name_symbol{ klass->get_name() })
                        {
                            info.class_name = name_symbol->to_string();
                        }
                    }
                }
            }
        }

        return info;
    }

    template<typename value_type>
    auto return_value::set_arg(const std::int32_t index, value_type&& value) noexcept
        -> bool
    {
        if (!this->stack_frame || index < 0)
        {
            return false;
        }

        void** const locals{ this->stack_frame->get_locals() };
        if (!locals)
        {
            return false;
        }

        using clean_value_type = std::remove_cvref_t<value_type>;

        auto store_oop = [&](void* const oop)
            -> bool
        {
            void* const previous_value{ locals[-index] };
            const std::uintptr_t previous_bits{ reinterpret_cast<std::uintptr_t>(previous_value) };

            if (!oop)
            {
                locals[-index] = nullptr;
                return true;
            }

            if (previous_bits > 0xFFFFFFFFull)
            {
                locals[-index] = oop;
                return true;
            }

            const std::uint32_t compressed{ vmhook::hotspot::encode_oop_pointer(oop) };
            locals[-index] = reinterpret_cast<void*>(static_cast<std::uintptr_t>(compressed));
            return true;
        };

        if constexpr (vmhook::detail::is_unique_object_ptr<clean_value_type>::value)
        {
            // Explicit base-class qualification reaches object_base::get_instance()
            // even when the derived wrapper shadows the name (e.g. with a same-named
            // static helper).  Avoids the static_cast<object_base*> which produces an
            // "incomplete type" warning under GCC at template-definition time.
            return store_oop(value ? value->vmhook::object_base::get_instance() : nullptr);
        }
        else if constexpr (std::is_base_of_v<vmhook::object_base, clean_value_type>)
        {
            return store_oop(value.get_instance());
        }
        else if constexpr (std::is_same_v<clean_value_type, std::string> || std::is_same_v<clean_value_type, std::string_view>)
        {
            void* const string_handle{ vmhook::detail::jni_new_string_utf(value) };
            void* const string_oop{ string_handle
                ? vmhook::detail::jni_decode_object(string_handle)
                : vmhook::make_java_string(value) };
            if (!string_oop)
            {
                return false;
            }

            return store_oop(string_oop);
        }
        else if constexpr (std::is_same_v<clean_value_type, const char*> || std::is_same_v<clean_value_type, char*>)
        {
            const std::string_view text{ value ? std::string_view{ value } : std::string_view{} };
            void* const string_handle{ vmhook::detail::jni_new_string_utf(text) };
            void* const string_oop{ string_handle
                ? vmhook::detail::jni_decode_object(string_handle)
                : vmhook::make_java_string(text) };
            if (!string_oop)
            {
                return false;
            }

            return store_oop(string_oop);
        }
        else if constexpr (std::is_trivially_copyable_v<clean_value_type> && sizeof(clean_value_type) <= sizeof(void*))
        {
            void* raw{};
            std::memcpy(&raw, &value, sizeof(clean_value_type));
            locals[-index] = raw;
            return true;
        }
        else
        {
            return false;
        }
    }

    // --- Hooking --------------------------------------------------------------

    /*
        @brief Installs an interpreter hook on a Java method with typed C++ parameters.
        @tparam wrapper_type The C++ wrapper class registered for the Java class that owns the method.
                             Must have been registered via register_class<T>() beforehand.
        @param method_name  The name of the Java method to hook (e.g., "toString", "update").
        @param user_detour  A callable with signature:
                              void(vmhook::return_value& retval, T1 arg1, T2 arg2, ...)
                            where T1, T2, ... correspond to the Java method's explicit parameters:
                              - int / boolean / byte / short / char / float   std::int32_t / bool / ...
                              - long                                          std::int64_t
                              - double                                        double
                              - String                                        std::string (or const std::string&)
                              - Object reference                              std::unique_ptr<WrapperClass>
                                                                               (or const std::unique_ptr<...>&)
                            For instance methods the implicit Java 'this' occupies slot 0, so it must
                            appear as the first argument (typically std::unique_ptr<wrapper_type>).
                            For static methods there is no 'this'; parameters begin at slot 0.
                            Call retval.cancel() to suppress a void method body, or
                            retval.set(value) to suppress the original body and return value to Java.
        @return true if the hook was successfully installed or was already active, false on failure.
        @details
        The hook installation process:
        1. Retrieve the klass for the registered Java class via find_class().
        2. Walk the InstanceKlass::_methods array to locate the target method by name.
        3. Disable JIT compilation by setting NO_COMPILE in Method._access_flags
           and _dont_inline in Method._flags.
        4. Register the method and its detour in g_hooked_methods for dispatch by common_detour.
        5. If the method is already compiled, clear Method._code and restore the
           interpreted entry so future dispatch reaches the interpreter hook.
        6. Check whether the i2i entry point has already been patched; if so, reuse it.
        7. If the i2i entry is new, locate the injection point via find_hook_location(),
           allocate a trampoline via midi2i_hook, and register it in g_hooked_i2i_entries.

        @note Unlike the JNI/JVMTI version, this implementation does not force a class
              retransformation to flush existing inline caches. Hooking early is still best:
              compiled callers that already cached an nmethod can keep bypassing the hook
              until HotSpot repairs that call site at a safepoint.
        @see midi2i_hook, common_detour, set_dont_inline, NO_COMPILE, shutdown_hooks
    */
    template<class wrapper_type>
    static auto hook(const std::string_view method_name,
                     const std::string_view method_signature,
                     auto&& user_detour) -> bool;

    template<class wrapper_type>
    static auto hook(const std::string_view method_name, auto&& user_detour)
        -> bool
    {
        return vmhook::hook<wrapper_type>(method_name, std::string_view{}, std::forward<decltype(user_detour)>(user_detour));
    }

    /*
        @brief Signature-filtered hook overload.
        @details
        Same behaviour as hook<T>(name, callback) but selects the method by
        matching both name and JVM descriptor.  Use when the target class
        has overloaded methods sharing the same name (e.g. ClassLoader's
        five `defineClass` variants).
    */
    template<class wrapper_type>
    static auto hook(const std::string_view method_name,
                     const std::string_view method_signature,
                     auto&& user_detour) -> bool
    {
        try
        {
            using traits_t = vmhook::detail::function_traits<std::remove_cvref_t<decltype(user_detour)>>;
            using all_args_tuple_t = typename traits_t::args_tuple_t;
            using method_arg_tuple_t = typename vmhook::detail::tuple_tail<all_args_tuple_t>::type_t;

            const auto type_map_entry{ vmhook::type_to_class_map.find(std::type_index{ typeid(wrapper_type) }) };
            if (type_map_entry == vmhook::type_to_class_map.end())
            {
                throw vmhook::exception{ std::format("Class not registered for type {}. Did you call register_class<wrapper_type>()?", typeid(wrapper_type).name()) };
            }

            vmhook::hotspot::klass* const target_klass{ vmhook::find_class(type_map_entry->second) };
            if (!target_klass)
            {
                throw vmhook::exception{ std::format("Class '{}' not found in JVM.", type_map_entry->second) };
            }

            const std::int32_t method_count{ target_klass->get_methods_count() };
            vmhook::hotspot::method** const methods_array{ target_klass->get_methods_ptr() };

            if (!methods_array || method_count <= 0)
            {
                throw vmhook::exception{ std::format("No methods found on class '{}'.", type_map_entry->second) };
            }

            vmhook::hotspot::method* found_method{ nullptr };
            for (std::int32_t method_index{ 0 }; method_index < method_count; ++method_index)
            {
                vmhook::hotspot::method* const method_ptr{ methods_array[method_index] };
                if (method_ptr && vmhook::hotspot::is_valid_pointer(method_ptr) && method_ptr->get_name() == method_name)
                {
                    if (method_signature.empty() || method_ptr->get_signature() == method_signature)
                    {
                        found_method = method_ptr;
                        break;
                    }
                }
            }

            if (!found_method)
            {
                throw vmhook::exception{ std::format("Method '{}' not found in class '{}'.", method_name, type_map_entry->second) };
            }

            for (const vmhook::hotspot::hooked_method& hooked_method_entry : vmhook::hotspot::g_hooked_methods)
            {
                if (hooked_method_entry.method == found_method)
                {
                    return true;
                }
            }

            vmhook::hotspot::set_dont_inline(found_method, true);

            std::uint32_t* const flags{ found_method->get_access_flags() };
            if (!flags)
            {
                throw vmhook::exception{ "Failed to retrieve access flags." };
            }
            *flags |= vmhook::hotspot::NO_COMPILE;

            // -- Snapshot original entry points before any modification ----------
            void* const i2i{ found_method->get_i2i_entry() };
            if (!i2i)
            {
                throw vmhook::exception{ "Failed to retrieve i2i entry." };
            }

            void* const original_code{ found_method->get_code() };
            void* const original_from_interpreted{ found_method->get_from_interpreted_entry() };
            void* const original_from_compiled{ found_method->get_from_compiled_entry() };
            const bool was_compiled{ original_code != nullptr && vmhook::hotspot::is_valid_pointer(original_code) };

            if (was_compiled)
            {
                VMHOOK_LOG("{} hook(): '{}' is JIT-compiled (_code=0x{:016X}) - deoptimising.", vmhook::info_tag, method_name, reinterpret_cast<std::uintptr_t>(original_code));
            }
            else
            {
                VMHOOK_LOG("{} hook(): '{}' is interpreted - patching i2i stub.", vmhook::info_tag, method_name);
            }

            // Wrap the user callable: extract typed Java args from the frame and forward them.
            auto wrapper_detour = [detour = std::forward<decltype(user_detour)>(user_detour)]
            (vmhook::hotspot::frame* const frame_pointer, vmhook::hotspot::java_thread*, vmhook::hotspot::return_slot* const slot)
                {
                    vmhook::return_value retval{ slot, frame_pointer };
                    // Slots indexed from 0: instance methods have 'this' at slot 0.
                    auto invoke = [&]<std::size_t... indexes>(std::index_sequence<indexes...>)
                    {
                        detour(retval,
                            vmhook::detail::extract_frame_arg<std::tuple_element_t<indexes, method_arg_tuple_t>>(
                                frame_pointer, static_cast<std::int32_t>(indexes))...);
                    };
                    invoke(std::make_index_sequence<std::tuple_size_v<method_arg_tuple_t>>{});
                };

            vmhook::hotspot::g_hooked_methods.push_back({ found_method, std::move(wrapper_detour), original_code, original_from_interpreted, original_from_compiled, was_compiled });

            // -- Install (or reuse) the i2i stub patch ---------------------------
            bool i2i_already_patched{ false };
            for (const vmhook::hotspot::i2i_hook_data& hook_data_entry : vmhook::hotspot::g_hooked_i2i_entries)
            {
                if (hook_data_entry.i2i_entry == i2i)
                {
                    i2i_already_patched = true;
                    break;
                }
            }

            if (!i2i_already_patched)
            {
                std::uint8_t* const target{ reinterpret_cast<std::uint8_t*>(vmhook::hotspot::find_hook_location(i2i)) };
                if (!target)
                {
                    throw vmhook::exception{ "Failed to find hook location in i2i stub." };
                }

                vmhook::hotspot::midi2i_hook* const hook_instance{ new vmhook::hotspot::midi2i_hook(target, vmhook::hotspot::common_detour) };
                if (hook_instance->has_error())
                {
                    delete hook_instance;
                    throw vmhook::exception{ "midi2i_hook installation failed." };
                }

                vmhook::hotspot::g_hooked_i2i_entries.push_back({ i2i, hook_instance });
            }

            // -- Deoptimise JIT-compiled methods ---------------------------------
            // Problem:  when _code != nullptr, _from_interpreted_entry points to the i2c
            //           adapter (not the i2i stub), so calls bypass our patch entirely.
            // Fix:      null _code and reset the interpreted entry so the JVM dispatches
            //           through the interpreter - and therefore through our patched i2i stub.
            // Limitation: compiled callers with stale monomorphic inline caches still call
            //             the old nmethod directly.  Those caches will be repaired the next
            //             time HotSpot reaches a safe point and re-evaluates the IC.
            if (was_compiled)
            {
                void* const adapter{ found_method->get_adapter() };
                void* const c2i_entry{ vmhook::hotspot::get_c2i_entry_from_adapter(adapter) };
                // 1. Redirect interpreted callers to the (now-patched) i2i stub.
                found_method->set_from_interpreted_entry(i2i);

                // 2. Redirect compiled callers through the c2i adapter  interpreter  i2i stub.
                if (c2i_entry && vmhook::hotspot::is_valid_pointer(c2i_entry))
                {
                    found_method->set_from_compiled_entry(c2i_entry);
                    VMHOOK_LOG("{} hook():   _from_compiled_entry -> c2i @ 0x{:016X}", vmhook::info_tag, reinterpret_cast<std::uintptr_t>(c2i_entry));
                }
                else
                {
                    // Do not point compiled callers directly at i2i: the compiled-call ABI
                    // expects a c2i adapter. Leaving this entry unchanged is safer; once
                    // _code is cleared, normal interpreted dispatch reaches the hook.
                    VMHOOK_LOG("{} hook():   c2i adapter unavailable; leaving _from_compiled_entry unchanged.", vmhook::info_tag);
                }

                // 3. Clear _code last so the above entry-point writes are visible first.
                found_method->set_code(nullptr);
                VMHOOK_LOG("{} hook():   _code cleared - method running via interpreter.", vmhook::info_tag);
            }

            return true;
        }
        catch (const std::exception& exception)
        {
            VMHOOK_LOG("{} vmhook::hook() for {}: {}", vmhook::error_tag, method_name, exception.what());
            return false;
        }
    }

    /*
        @brief Removes all active interpreter hooks and restores the JVM to its original state.
        @details
        Phase 1: Deletes each midi2i_hook instance, restoring original bytes and freeing memory.
        Phase 2: Clears _dont_inline and NO_COMPILE flags.
        Phase 3: For methods that were JIT-compiled at hook-install time, restores the original
                 entry points and re-links _code so the nmethod is active again.
        Order within phase 3:  entry points first, then _code - this ensures callers have a valid
        destination before the JVM re-enables compiled dispatch.
    */
    static auto shutdown_hooks() noexcept
        -> void
    {
        for (const vmhook::hotspot::i2i_hook_data& hook_data_entry : vmhook::hotspot::g_hooked_i2i_entries)
        {
            delete hook_data_entry.hook;
        }

        for (const vmhook::hotspot::hooked_method& hooked_method_entry : vmhook::hotspot::g_hooked_methods)
        {
            vmhook::hotspot::set_dont_inline(hooked_method_entry.method, false);

            std::uint32_t* const flags{ hooked_method_entry.method->get_access_flags() };
            if (flags)
            {
                *flags &= static_cast<std::uint32_t>(~vmhook::hotspot::NO_COMPILE);
            }

            if (hooked_method_entry.was_compiled)
            {
                if (hooked_method_entry.original_from_compiled_entry)
                {
                    hooked_method_entry.method->set_from_compiled_entry(hooked_method_entry.original_from_compiled_entry);
                }
                if (hooked_method_entry.original_from_interpreted_entry)
                {
                    hooked_method_entry.method->set_from_interpreted_entry(hooked_method_entry.original_from_interpreted_entry);
                }
                if (hooked_method_entry.original_code)
                {
                    hooked_method_entry.method->set_code(hooked_method_entry.original_code);
                }
            }
        }

        vmhook::hotspot::g_hooked_methods.clear();
        vmhook::hotspot::g_hooked_i2i_entries.clear();
    }

    // --- JNI helper layer --------------------------------------------------------
    // Low-level wrappers around the JNIEnv function table.  All functions are
    // noexcept and return nullptr / empty on failure so callers never need to
    // catch JNI exceptions explicitly (they call jni_exception_clear() instead).

    namespace detail
    {
        /*
            @brief Tagged union matching the layout of JNI's jvalue type.
            @details
            Used when building argument arrays for CallObjectMethodA and related JNI
            varargs-A variants.  Each field corresponds to a JNI primitive type:
              z = jboolean, b = jbyte, c = jchar, s = jshort,
              i = jint, j = jlong, f = jfloat, d = jdouble, l = jobject.
        */
        union jni_value
        {
            bool z;
            std::int8_t b;
            std::uint16_t c;
            std::int16_t s;
            std::int32_t i;
            std::int64_t j;
            float f;
            double d;
            void* l;
        };

        /*
            @brief Retrieves a JNI function pointer from the JNIEnv function table by index.
            @details
            A JNIEnv* is a pointer to a pointer to an array of function pointers (the
            JNI function table).  Given the zero-based JNI table index (as defined by
            the JNI specification) and the target function_type, this returns a callable
            function pointer cast to function_type, or nullptr if env is invalid.

            Complexity: O(1).
            Exception safety: noexcept — returns nullptr if env is null.

            @tparam index          Zero-based index into the JNIEnv function table.
            @tparam function_type  The expected function pointer type for this slot.
            @param env  The JNIEnv* (current_jni_env).
            @return     Callable function pointer, or nullptr on failure.
        */
        template<std::size_t index, typename function_type>
        inline auto jni_function(void* const env) noexcept
            -> function_type
        {
            if (!env)
            {
                return nullptr;
            }

            void** const table{ *reinterpret_cast<void***>(env) };
            if (!table)
            {
                return nullptr;
            }

            return reinterpret_cast<function_type>(table[index]);
        }

        /*
            @brief Dereferences a JNI local reference handle to obtain the underlying heap OOP.
            @details
            JNI local references are pointers into the current thread's JNI handle block.
            Dereferencing the handle pointer yields the raw Java heap OOP.  The result is
            validated with is_valid_pointer before being returned.

            Complexity: O(1).
            Exception safety: noexcept — returns nullptr if handle is null or OOP is invalid.

            @param object_handle  A JNI local reference (jobject, jclass, jstring, etc.).
            @return  Raw heap OOP, or nullptr on failure.
        */
        inline auto jni_decode_object(void* const object_handle) noexcept
            -> void*
        {
            if (!object_handle)
            {
                return nullptr;
            }

            void* const oop{ *reinterpret_cast<void**>(object_handle) };
            return vmhook::hotspot::is_valid_pointer(oop) ? oop : nullptr;
        }

        /*
            @brief Constructs a synthetic JNI handle for a raw heap OOP without a JNI frame.
            @details
            Stores oop into handle_storage and returns a pointer to handle_storage.
            This creates a stack-allocated "fake" JNI handle that the JNI function table
            will dereference as a normal local reference.  The caller must keep
            handle_storage alive for the duration of any JNI calls that use the handle.

            Complexity: O(1).
            Exception safety: noexcept — plain pointer assignment.

            @param oop             Raw heap OOP to wrap.
            @param handle_storage  Caller-provided storage for the OOP value.
            @return  Pointer to handle_storage, suitable for passing as a JNI jobject.
        */
        inline auto jni_oop_handle(void* const oop, void*& handle_storage) noexcept
            -> void*
        {
            handle_storage = oop;
            return &handle_storage;
        }

        /*
            @brief Calls JNIEnv::FindClass to locate a class by its internal name.
            @details
            Uses JNI table slot 6 (FindClass) with current_jni_env.  The name must use
            '/' separators (e.g. "java/lang/String").  Returns a JNI local reference
            (jclass handle), not a Klass*.  Use jni_klass_from_class_mirror() to get
            the HotSpot Klass* from the returned handle.

            Complexity: O(lookup in the bootstrap class loader).
            Exception safety: noexcept — returns nullptr on any failure.

            @param class_name  Internal JVM class name with '/' separators.
            @return  jclass local reference, or nullptr if not found or JNI is unavailable.
        */
        inline auto jni_find_class(const std::string_view class_name) noexcept
            -> void*
        {
            if (!vmhook::hotspot::ensure_current_java_thread())
            {
                return nullptr;
            }

            using find_class_t = void* (*)(void*, const char*);
            find_class_t const find_class{ vmhook::detail::jni_function<6, find_class_t>(vmhook::hotspot::current_jni_env) };
            if (!find_class)
            {
                return nullptr;
            }

            const std::string name{ class_name };
            return find_class(vmhook::hotspot::current_jni_env, name.c_str());
        }

        /*
            @brief Clears any pending JNI exception on the current thread.
            @details
            Calls JNIEnv::ExceptionCheck (slot 228) to test for a pending exception, then
            JNIEnv::ExceptionClear (slot 17) to dismiss it.  Should be called after any
            JNI call that may have thrown (e.g. FindClass, GetMethodID) to prevent the
            pending exception from poisoning subsequent JNI operations.

            Complexity: O(1).
            Exception safety: noexcept — JNI vtable dispatches only.
        */
        inline auto jni_exception_clear() noexcept
            -> void
        {
            using exception_check_t = bool (*)(void*);
            using exception_clear_t = void (*)(void*);
            exception_check_t const exception_check{ vmhook::detail::jni_function<228, exception_check_t>(vmhook::hotspot::current_jni_env) };
            exception_clear_t const exception_clear{ vmhook::detail::jni_function<17, exception_clear_t>(vmhook::hotspot::current_jni_env) };
            if (exception_check && exception_clear && exception_check(vmhook::hotspot::current_jni_env))
            {
                exception_clear(vmhook::hotspot::current_jni_env);
            }
        }

        /*
            @brief Calls JNIEnv::GetObjectClass to retrieve the jclass of a JNI object handle.
            @details
            Uses JNI table slot 31 (GetObjectClass).  Returns a jclass local reference.

            Complexity: O(1).
            Exception safety: noexcept — returns nullptr if env or handle is null.

            @param object_handle  A JNI local reference to a Java object.
            @return  jclass handle for the object's runtime class, or nullptr on failure.
        */
        inline auto jni_get_object_class(void* const object_handle) noexcept
            -> void*
        {
            using get_object_class_t = void* (*)(void*, void*);
            get_object_class_t const get_object_class{ vmhook::detail::jni_function<31, get_object_class_t>(vmhook::hotspot::current_jni_env) };
            return get_object_class ? get_object_class(vmhook::hotspot::current_jni_env, object_handle) : nullptr;
        }

        /*
            @brief Calls JNIEnv::GetMethodID to look up an instance method by name and descriptor.
            @details
            Uses JNI table slot 33 (GetMethodID).  Clears any pending exception before the
            call and again after if the lookup fails, so callers do not need to handle
            pending exceptions from failed lookups.

            Complexity: O(method count in the class hierarchy).
            Exception safety: noexcept — returns nullptr and clears the exception on failure.

            @param klass      jclass handle for the class to search.
            @param name       Java method name (e.g. "getScore").
            @param signature  JNI descriptor string (e.g. "()I").
            @return  jmethodID, or nullptr if not found.
        */
        inline auto jni_get_method_id(void* const klass, const std::string& name, const std::string& signature) noexcept
            -> void*
        {
            vmhook::detail::jni_exception_clear();
            using get_method_id_t = void* (*)(void*, void*, const char*, const char*);
            get_method_id_t const get_method_id{ vmhook::detail::jni_function<33, get_method_id_t>(vmhook::hotspot::current_jni_env) };
            void* const method_id{ get_method_id ? get_method_id(vmhook::hotspot::current_jni_env, klass, name.c_str(), signature.c_str()) : nullptr };
            if (!method_id)
            {
                vmhook::detail::jni_exception_clear();
            }
            return method_id;
        }

        inline auto jni_new_string_utf(const std::string_view value) noexcept
            -> void*;

        /*
            @brief Calls JNIEnv::GetStaticMethodID to look up a static method by name and descriptor.
            @details
            Uses JNI table slot 113 (GetStaticMethodID).  The return value is a jmethodID
            suitable for jni_call_static_object_method().

            Complexity: O(method count in the class).
            Exception safety: noexcept — returns nullptr on failure.

            @param klass      jclass handle.
            @param name       Java method name.
            @param signature  JNI descriptor string.
            @return  jmethodID, or nullptr if not found.
        */
        inline auto jni_get_static_method_id(void* const klass, const std::string& name, const std::string& signature) noexcept
            -> void*
        {
            using get_static_method_id_t = void* (*)(void*, void*, const char*, const char*);
            get_static_method_id_t const get_static_method_id{ vmhook::detail::jni_function<113, get_static_method_id_t>(vmhook::hotspot::current_jni_env) };
            return get_static_method_id ? get_static_method_id(vmhook::hotspot::current_jni_env, klass, name.c_str(), signature.c_str()) : nullptr;
        }

        /*
            @brief Calls JNIEnv::GetStaticFieldID to look up a static field by name and descriptor.
            @details
            Uses JNI table slot 144 (GetStaticFieldID).  The return value is a jfieldID
            suitable for jni_get_static_object_field().

            Complexity: O(field count in the class).
            Exception safety: noexcept — returns nullptr on failure.

            @param klass      jclass handle.
            @param name       Java field name (e.g. "classLoader").
            @param signature  JNI type descriptor (e.g. "Ljava/lang/ClassLoader;").
            @return  jfieldID, or nullptr if not found.
        */
        inline auto jni_get_static_field_id(void* const klass, const std::string& name, const std::string& signature) noexcept
            -> void*
        {
            using get_static_field_id_t = void* (*)(void*, void*, const char*, const char*);
            get_static_field_id_t const get_static_field_id{ vmhook::detail::jni_function<144, get_static_field_id_t>(vmhook::hotspot::current_jni_env) };
            return get_static_field_id ? get_static_field_id(vmhook::hotspot::current_jni_env, klass, name.c_str(), signature.c_str()) : nullptr;
        }

        /*
            @brief Calls JNIEnv::GetStaticObjectField to read a static object-typed field.
            @details
            Uses JNI table slot 145 (GetStaticObjectField).  Returns a JNI local reference
            (jobject) to the field value.

            Complexity: O(1).
            Exception safety: noexcept — returns nullptr on failure.

            @param klass     jclass handle.
            @param field_id  jfieldID from jni_get_static_field_id().
            @return  jobject local reference, or nullptr on failure.
        */
        inline auto jni_get_static_object_field(void* const klass, void* const field_id) noexcept
            -> void*
        {
            using get_static_object_field_t = void* (*)(void*, void*, void*);
            get_static_object_field_t const get_static_object_field{ vmhook::detail::jni_function<145, get_static_object_field_t>(vmhook::hotspot::current_jni_env) };
            return get_static_object_field ? get_static_object_field(vmhook::hotspot::current_jni_env, klass, field_id) : nullptr;
        }

        /*
            @brief Calls JNIEnv::CallObjectMethodA to invoke an instance method.
            @details
            Uses JNI table slot 36 (CallObjectMethodA).  args may be nullptr for
            zero-argument methods.  Returns a JNI local reference to the result object.

            Complexity: O(method execution time).
            Exception safety: noexcept — returns nullptr on JNI failure.

            @param object     JNI local reference to the receiver object.
            @param method_id  jmethodID from jni_get_method_id().
            @param args       Array of jvalue arguments, or nullptr for no arguments.
            @return  jobject local reference for the return value, or nullptr on failure.
        */
        inline auto jni_call_object_method(void* const object, void* const method_id, const vmhook::detail::jni_value* const args = nullptr) noexcept
            -> void*
        {
            using call_object_method_a_t = void* (*)(void*, void*, void*, const vmhook::detail::jni_value*);
            call_object_method_a_t const call_object_method_a{ vmhook::detail::jni_function<36, call_object_method_a_t>(vmhook::hotspot::current_jni_env) };
            return call_object_method_a ? call_object_method_a(vmhook::hotspot::current_jni_env, object, method_id, args) : nullptr;
        }

        /*
            @brief Calls JNIEnv::CallStaticObjectMethodA to invoke a static method.
            @details
            Uses JNI table slot 116 (CallStaticObjectMethodA).  args may be nullptr.
            Returns a JNI local reference to the result object.

            Complexity: O(method execution time).
            Exception safety: noexcept — returns nullptr on JNI failure.

            @param klass      jclass handle for the class that declares the method.
            @param method_id  jmethodID from jni_get_static_method_id().
            @param args       Array of jvalue arguments, or nullptr for no arguments.
            @return  jobject local reference for the return value, or nullptr on failure.
        */
        inline auto jni_call_static_object_method(void* const klass, void* const method_id, const vmhook::detail::jni_value* const args = nullptr) noexcept
            -> void*
        {
            using call_static_object_method_a_t = void* (*)(void*, void*, void*, const vmhook::detail::jni_value*);
            call_static_object_method_a_t const call_static_object_method_a{ vmhook::detail::jni_function<116, call_static_object_method_a_t>(vmhook::hotspot::current_jni_env) };
            return call_static_object_method_a ? call_static_object_method_a(vmhook::hotspot::current_jni_env, klass, method_id, args) : nullptr;
        }

        /*
            @brief Extracts the HotSpot Klass* from a java.lang.Class JNI handle.
            @details
            A java.lang.Class object (the class mirror) stores a back-pointer to its Klass
            at a fixed offset defined by java_lang_Class._klass_offset, which is exported
            through gHotSpotVMStructs.  This function decodes the JNI handle to a raw OOP,
            reads the klass field, and strips GC tag bits via untag_pointer().

            Complexity: O(1).
            Exception safety: noexcept — returns nullptr on any failure.

            @param class_handle  jclass local reference (e.g. from jni_find_class()).
            @return  The underlying Klass*, or nullptr on failure.
        */
        inline auto jni_klass_from_class_mirror(void* const class_handle) noexcept
            -> vmhook::hotspot::klass*
        {
            void* const class_oop{ vmhook::detail::jni_decode_object(class_handle) };
            if (!class_oop || !vmhook::hotspot::is_valid_pointer(class_oop))
            {
                return nullptr;
            }

            static const vmhook::hotspot::vm_struct_entry_t* const klass_offset{ vmhook::hotspot::iterate_struct_entries("java_lang_Class", "_klass_offset") };
            if (!klass_offset || !klass_offset->address)
            {
                return nullptr;
            }

            const int offset{ *reinterpret_cast<const int*>(klass_offset->address) };
            void* const raw_klass{ const_cast<void*>(vmhook::hotspot::safe_read_pointer(reinterpret_cast<const std::uint8_t*>(class_oop) + offset)) };
            return vmhook::hotspot::is_valid_pointer(raw_klass) ? reinterpret_cast<vmhook::hotspot::klass*>(const_cast<void*>(vmhook::hotspot::untag_pointer(raw_klass))) : nullptr;
        }

        inline auto jni_find_class_with_context_loader(const std::string_view class_name) noexcept
            -> vmhook::hotspot::klass*
        {
            if (!vmhook::hotspot::ensure_current_java_thread())
            {
                return nullptr;
            }

            auto load_with_loader = [&](void* const class_loader) noexcept
                -> vmhook::hotspot::klass*
            {
                if (!class_loader)
                {
                    return nullptr;
                }

                void* const class_loader_class{ vmhook::detail::jni_find_class("java/lang/ClassLoader") };
                if (!class_loader_class)
                {
                    vmhook::detail::jni_exception_clear();
                    return nullptr;
                }

                void* const load_class_id{ vmhook::detail::jni_get_method_id(class_loader_class, "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;") };
                if (!load_class_id)
                {
                    vmhook::detail::jni_exception_clear();
                    return nullptr;
                }

                std::string dotted_name{ class_name };
                std::replace(dotted_name.begin(), dotted_name.end(), '/', '.');
                void* const name_string{ vmhook::detail::jni_new_string_utf(dotted_name) };
                if (!name_string)
                {
                    vmhook::detail::jni_exception_clear();
                    return nullptr;
                }

                vmhook::detail::jni_value args[1]{};
                args[0].l = name_string;

                void* const class_mirror{ vmhook::detail::jni_call_object_method(class_loader, load_class_id, args) };
                vmhook::hotspot::klass* const klass{ vmhook::detail::jni_klass_from_class_mirror(class_mirror) };
                vmhook::detail::jni_exception_clear();
                return klass;
            };

            void* const thread_class{ vmhook::detail::jni_find_class("java/lang/Thread") };
            if (thread_class)
            {
                void* const current_thread_id{ vmhook::detail::jni_get_static_method_id(thread_class, "currentThread", "()Ljava/lang/Thread;") };
                void* const get_context_loader_id{ vmhook::detail::jni_get_method_id(thread_class, "getContextClassLoader", "()Ljava/lang/ClassLoader;") };
                if (current_thread_id && get_context_loader_id)
                {
                    void* const current_thread{ vmhook::detail::jni_call_static_object_method(thread_class, current_thread_id) };
                    void* const context_loader{ current_thread ? vmhook::detail::jni_call_object_method(current_thread, get_context_loader_id) : nullptr };
                    if (vmhook::hotspot::klass* const klass{ load_with_loader(context_loader) })
                    {
                        return klass;
                    }
                }
            }
            vmhook::detail::jni_exception_clear();

            void* const class_loader_class{ vmhook::detail::jni_find_class("java/lang/ClassLoader") };
            if (class_loader_class)
            {
                void* const get_system_loader_id{ vmhook::detail::jni_get_static_method_id(class_loader_class, "getSystemClassLoader", "()Ljava/lang/ClassLoader;") };
                void* const system_loader{ get_system_loader_id ? vmhook::detail::jni_call_static_object_method(class_loader_class, get_system_loader_id) : nullptr };
                if (vmhook::hotspot::klass* const klass{ load_with_loader(system_loader) })
                {
                    return klass;
                }
            }
            vmhook::detail::jni_exception_clear();

            void* const launch_class{ vmhook::detail::jni_find_class("net/minecraft/launchwrapper/Launch") };
            if (!launch_class)
            {
                vmhook::detail::jni_exception_clear();
                return nullptr;
            }

            void* const class_loader_field{ vmhook::detail::jni_get_static_field_id(launch_class, "classLoader", "Lnet/minecraft/launchwrapper/LaunchClassLoader;") };
            void* const launch_loader{ class_loader_field ? vmhook::detail::jni_get_static_object_field(launch_class, class_loader_field) : nullptr };
            if (vmhook::hotspot::klass* const klass{ load_with_loader(launch_loader) })
            {
                return klass;
            }

            vmhook::detail::jni_exception_clear();
            return nullptr;
        }

        /*
            @brief Calls JNIEnv::NewStringUTF to create a Java String from a UTF-8 C string.
            @details
            Uses JNI table slot 167 (NewStringUTF).  Returns a JNI local reference
            (jstring handle), not a raw heap OOP.  Use jni_decode_object() to obtain
            the underlying OOP if needed.

            Complexity: O(N) where N = length of value.
            Exception safety: noexcept — returns nullptr if JNIEnv is unavailable.

            @param value  UTF-8 text to encode as a Java String.
            @return  jstring local reference, or nullptr on failure.
        */
        inline auto jni_new_string_utf(const std::string_view value) noexcept
            -> void*
        {
            using new_string_utf_t = void* (*)(void*, const char*);
            new_string_utf_t const new_string_utf{ vmhook::detail::jni_function<167, new_string_utf_t>(vmhook::hotspot::current_jni_env) };
            if (!new_string_utf)
            {
                return nullptr;
            }

            const std::string text{ value };
            return new_string_utf(vmhook::hotspot::current_jni_env, text.c_str());
        }

        /*
            @brief Reads a Java String's content as a std::string via JNI.
            @details
            Calls JNIEnv::GetStringUTFChars (slot 169) to obtain a UTF-8 C string,
            copies it into a std::string, then calls ReleaseStringUTFChars (slot 170)
            to free the buffer.

            Complexity: O(N) where N = string length.
            Exception safety: noexcept — returns empty string on failure.

            @param string_handle  jstring local reference.
            @return  The string contents, or an empty string on failure.
        */
        inline auto jni_get_string_utf(void* const string_handle) noexcept
            -> std::string
        {
            if (!string_handle)
            {
                return {};
            }

            using get_string_utf_chars_t = const char* (*)(void*, void*, bool*);
            using release_string_utf_chars_t = void (*)(void*, void*, const char*);
            get_string_utf_chars_t const get_string_utf_chars{ vmhook::detail::jni_function<169, get_string_utf_chars_t>(vmhook::hotspot::current_jni_env) };
            release_string_utf_chars_t const release_string_utf_chars{ vmhook::detail::jni_function<170, release_string_utf_chars_t>(vmhook::hotspot::current_jni_env) };
            if (!get_string_utf_chars)
            {
                return {};
            }

            bool is_copy{};
            const char* const chars{ get_string_utf_chars(vmhook::hotspot::current_jni_env, string_handle, &is_copy) };
            if (!chars)
            {
                return {};
            }

            std::string result{ chars };
            if (release_string_utf_chars)
            {
                release_string_utf_chars(vmhook::hotspot::current_jni_env, string_handle, chars);
            }
            return result;
        }

        /*
            @brief Returns the JNI type descriptor character(s) for a C++ argument type.
            @details
            Maps C++ types to their JNI descriptor string at compile time:
              std::string / string_view / const char* -> "Ljava/lang/String;"
              bool        -> "Z"
              int8/uint8  -> "B"
              int16       -> "S"
              uint16      -> "C"
              int64/uint64-> "J"
              float       -> "F"
              double      -> "D"
              other       -> "I" (treated as int)
            Used by method_proxy::call_jni() to build the JNI method descriptor string.

            Exception safety: noexcept — compile-time dispatch only.

            @tparam arg_type  The C++ argument type.
            @return  JNI type descriptor string.
        */
        template<typename arg_type>
        inline auto jni_signature_for_arg() noexcept
            -> std::string
        {
            using clean_t = std::decay_t<arg_type>;

            if constexpr (std::is_same_v<clean_t, std::string> || std::is_same_v<clean_t, std::string_view> || std::is_same_v<clean_t, const char*> || std::is_same_v<clean_t, char*>)
            {
                return "Ljava/lang/String;";
            }
            else if constexpr (std::is_same_v<clean_t, bool>)
            {
                return "Z";
            }
            else if constexpr (std::is_same_v<clean_t, std::int8_t> || std::is_same_v<clean_t, std::uint8_t>)
            {
                return "B";
            }
            else if constexpr (std::is_same_v<clean_t, std::int16_t>)
            {
                return "S";
            }
            else if constexpr (std::is_same_v<clean_t, std::uint16_t>)
            {
                return "C";
            }
            else if constexpr (std::is_same_v<clean_t, std::int64_t> || std::is_same_v<clean_t, std::uint64_t>)
            {
                return "J";
            }
            else if constexpr (std::is_same_v<clean_t, float>)
            {
                return "F";
            }
            else if constexpr (std::is_same_v<clean_t, double>)
            {
                return "D";
            }
            else
            {
                return "I";
            }
        }

        /*
            @brief Converts a single C++ argument to a jni_value and appends it to values.
            @details
            Handles the full range of argument types by compile-time dispatch:
              - std::string / string_view / const char* -> jni_new_string_utf + .l slot
              - unique_ptr<T extends object_base>       -> stores raw OOP in object_handles; .l slot
              - object_base derived by value            -> stores raw OOP in object_handles; .l slot
              - bool                                    -> .z slot
              - integral (<=4 bytes)                    -> .i slot
              - integral (8 bytes)                      -> .j slot
              - float                                   -> .f slot
              - double                                  -> .d slot
            Object handles are stored in the caller-provided object_handles vector to keep
            the OOP alive for the duration of the JNI call.

            Complexity: O(1) for primitives; O(N) for strings.
            Exception safety: noexcept — failures silently append a zero-initialised value.

            @param values          Output jni_value array being built.
            @param object_handles  Storage for OOP pointers wrapped as fake JNI handles.
            @param arg             The C++ argument to convert.
        */
        template<typename arg_type>
        inline auto append_jni_arg(std::vector<vmhook::detail::jni_value>& values, std::vector<void*>& object_handles, arg_type&& arg) noexcept
            -> void
        {
            using clean_t = std::decay_t<arg_type>;
            vmhook::detail::jni_value value{};

            if constexpr (std::is_same_v<clean_t, std::string>)
            {
                value.l = vmhook::detail::jni_new_string_utf(arg);
            }
            else if constexpr (std::is_same_v<clean_t, std::string_view>)
            {
                value.l = vmhook::detail::jni_new_string_utf(arg);
            }
            else if constexpr (std::is_same_v<clean_t, const char*> || std::is_same_v<clean_t, char*>)
            {
                value.l = vmhook::detail::jni_new_string_utf(arg ? std::string_view{ arg } : std::string_view{});
            }
            else if constexpr (vmhook::detail::is_unique_ptr_v<clean_t>)
            {
                using wrapper_type = typename vmhook::detail::is_unique_ptr<clean_t>::value_type_t;
                if constexpr (std::is_base_of_v<vmhook::object_base, wrapper_type>)
                {
                    object_handles.push_back(arg ? arg->get_instance() : nullptr);
                    value.l = object_handles.empty() ? nullptr : &object_handles.back();
                }
            }
            else if constexpr (std::is_base_of_v<vmhook::object_base, clean_t>)
            {
                object_handles.push_back(arg.get_instance());
                value.l = &object_handles.back();
            }
            else if constexpr (std::is_same_v<clean_t, bool>)
            {
                value.z = arg;
            }
            else if constexpr (std::is_integral_v<clean_t> && sizeof(clean_t) <= sizeof(std::int32_t))
            {
                value.i = static_cast<std::int32_t>(arg);
            }
            else if constexpr (std::is_integral_v<clean_t> && sizeof(clean_t) == sizeof(std::int64_t))
            {
                value.j = static_cast<std::int64_t>(arg);
            }
            else if constexpr (std::is_same_v<clean_t, float>)
            {
                value.f = arg;
            }
            else if constexpr (std::is_same_v<clean_t, double>)
            {
                value.d = arg;
            }

            values.push_back(value);
        }

        /*
            @brief Builds a jni_value argument array from a variadic C++ argument pack.
            @details
            Calls append_jni_arg() for each argument in the pack using a fold expression.
            object_handles is pre-reserved to sizeof...(args_t) to avoid reallocations
            that would invalidate the pointers stored in the .l slots.

            Complexity: O(N) where N = number of arguments.
            Exception safety: noexcept — delegates to append_jni_arg which is noexcept.

            @param object_handles  Caller-owned storage for OOP handle pointers.
            @param args            C++ arguments to convert.
            @return  Vector of jni_value ready to pass to CallXMethodA.
        */
        template<typename... args_t>
        inline auto make_jni_args(std::vector<void*>& object_handles, args_t&&... args) noexcept
            -> std::vector<vmhook::detail::jni_value>
        {
            std::vector<vmhook::detail::jni_value> values{};
            values.reserve(sizeof...(args_t));
            object_handles.reserve(sizeof...(args_t));
            (vmhook::detail::append_jni_arg(values, object_handles, std::forward<args_t>(args)), ...);
            return values;
        }

        /*
            @brief Constructs a Java object via JNI and wraps it in a std::unique_ptr<wrapper_type>.
            @details
            Finds the HotSpot klass for class_name, obtains its java.lang.Class mirror,
            builds the JNI argument array, locates the matching constructor via
            jni_get_method_id("<init>", ...), then calls CallObjectMethodA to instantiate
            the object.  The resulting JNI handle is decoded to a raw OOP and passed to
            the wrapper_type constructor.

            This is the JNI-backed implementation of vmhook::make_unique<>(); it is used
            when the TLAB allocation path is unavailable.

            Complexity: O(constructor lookup + constructor execution time).
            Exception safety: noexcept — returns nullptr on any failure; logs via VMHOOK_LOG.

            @tparam wrapper_type  C++ wrapper type deriving from vmhook::object_base.
            @tparam args_t        Constructor argument types.
            @param class_name     Internal JVM class name with '/' separators.
            @param args           Constructor arguments.
            @return  Newly created Java object wrapped in unique_ptr, or nullptr on failure.
        */
        template<typename wrapper_type, typename... args_t>
        inline auto jni_make_unique(const std::string& class_name, args_t&&... args) noexcept
            -> std::unique_ptr<wrapper_type>
        {
            vmhook::hotspot::klass* const hotspot_klass{ vmhook::find_class(class_name) };
            if (!hotspot_klass)
            {
                VMHOOK_LOG("{} jni_make_unique<{}>(): HotSpot class '{}' not found.", vmhook::error_tag, typeid(wrapper_type).name(), class_name);
                return nullptr;
            }

            void* const class_mirror{ hotspot_klass->get_java_mirror() };
            if (!class_mirror || !vmhook::hotspot::is_valid_pointer(class_mirror))
            {
                VMHOOK_LOG("{} jni_make_unique<{}>(): java.lang.Class mirror for '{}' is invalid.", vmhook::error_tag, typeid(wrapper_type).name(), class_name);
                return nullptr;
            }

            void* class_handle_storage{};
            void* const klass{ vmhook::detail::jni_oop_handle(class_mirror, class_handle_storage) };

            std::vector<void*> object_handles{};
            std::vector<vmhook::detail::jni_value> values{ vmhook::detail::make_jni_args(object_handles, std::forward<args_t>(args)...) };

            std::string signature{ "(" };
            ((signature += vmhook::detail::jni_signature_for_arg<std::remove_cvref_t<args_t>>()), ...);
            signature += ")V";

            void* const method_id{ vmhook::detail::jni_get_method_id(klass, "<init>", signature) };
            if (!method_id)
            {
                VMHOOK_LOG("{} jni_make_unique<{}>(): GetMethodID('<init>', '{}') failed.", vmhook::error_tag, typeid(wrapper_type).name(), signature);
                return nullptr;
            }

            using new_object_a_t = void* (*)(void*, void*, void*, const vmhook::detail::jni_value*);
            new_object_a_t const new_object_a{ vmhook::detail::jni_function<30, new_object_a_t>(vmhook::hotspot::current_jni_env) };
            if (!new_object_a)
            {
                return nullptr;
            }

            void* const object_handle{ new_object_a(vmhook::hotspot::current_jni_env, klass, method_id, values.data()) };
            void* const oop{ vmhook::detail::jni_decode_object(object_handle) };
            return oop ? std::make_unique<wrapper_type>(oop) : nullptr;
        }
    }

    /*
        @brief Constructs a new Java object and returns a C++ wrapper.
        @tparam T The C++ wrapper class (must derive from vmhook::object).
        @param args Arguments to pass to the Java constructor.
        @return A std::unique_ptr<T> wrapping the new Java object, or nullptr on failure.
        @details
        Looks up the Java class for type T (via register_class<T>()), allocates a new
        instance, and calls the appropriate constructor with the provided arguments.

        Usage:
            vmhook::register_class<player>("com/example/Player");
            auto p = vmhook::make_unique<player>("Bob", 12);

        @note This is a minimal implementation. Full constructor dispatch requires
              parsing method descriptors and setting up interpreter frames.
    */
    template<typename wrapper_type, typename... args_t>
    static auto make_unique(args_t&&... args)
        -> std::unique_ptr<wrapper_type>
    {
        if (!vmhook::hotspot::ensure_current_java_thread())
        {
            VMHOOK_LOG("{} vmhook::make_unique<{}>(): failed to attach current native thread to the JVM.", vmhook::error_tag, typeid(wrapper_type).name());
            return nullptr;
        }

        auto map_entry{ vmhook::type_to_class_map.find(std::type_index{ typeid(wrapper_type) }) };
        if (map_entry == vmhook::type_to_class_map.end())
        {
            VMHOOK_LOG("{} vmhook::make_unique<{}>(): type not registered.", vmhook::error_tag, typeid(wrapper_type).name());
            return nullptr;
        }

        if (!vmhook::detail::find_call_stub_entry())
        {
            if (std::unique_ptr<wrapper_type> jni_result{ vmhook::detail::jni_make_unique<wrapper_type>(map_entry->second, std::forward<args_t>(args)...) })
            {
                return jni_result;
            }
        }

        vmhook::hotspot::klass* const klass{ vmhook::find_class(map_entry->second) };
        if (!klass)
        {
            return nullptr;
        }

        const std::size_t raw_size{ klass->get_instance_size() };
        const std::size_t object_size{ (raw_size + 7u) & ~static_cast<std::size_t>(7u) };
        if (object_size == 0)
        {
            VMHOOK_LOG("{} vmhook::make_unique<{}>(): failed to read HotSpot instance size.", vmhook::error_tag, typeid(wrapper_type).name());
            return nullptr;
        }

        vmhook::hotspot::java_thread* const thread{ vmhook::hotspot::find_allocation_thread() };
        void* object_pointer{};
        if (thread && vmhook::hotspot::is_valid_pointer(thread))
        {
            object_pointer = thread->allocate_tlab(object_size);
        }

        if (!object_pointer)
        {
            std::int32_t visited_threads{ 0 };
            for (vmhook::hotspot::java_thread* candidate{ vmhook::hotspot::find_any_java_thread() };
                candidate && vmhook::hotspot::is_valid_pointer(candidate) && visited_threads < 256;
                candidate = candidate->get_next(), ++visited_threads)
            {
                object_pointer = candidate->allocate_tlab(object_size);
                if (object_pointer)
                {
                    vmhook::hotspot::last_java_thread.store(candidate, std::memory_order_relaxed);
                    break;
                }
            }
        }

        if (!object_pointer)
        {
            object_pointer = vmhook::hotspot::allocate_from_threads_list(object_size);
        }

        if (!object_pointer)
        {
            VMHOOK_LOG("{} vmhook::make_unique<{}>(): current JavaThread TLAB has no room for {} bytes.", vmhook::warning_tag, typeid(wrapper_type).name(), object_size);
            return nullptr;
        }

        std::memset(object_pointer, 0, object_size);

        static const vmhook::hotspot::vm_struct_entry_t* const mark_entry{ []()
            -> const vmhook::hotspot::vm_struct_entry_t*
            {
                {
                    auto* entry{ vmhook::hotspot::iterate_struct_entries("oopDesc", "_mark") };
                    if (entry)
                    {
                        return entry;
                    }
                }
                return vmhook::hotspot::iterate_struct_entries("oopDesc", "_markWord");
            }()
        };
        static const vmhook::hotspot::vm_struct_entry_t* const compressed_klass_entry{ vmhook::hotspot::iterate_struct_entries("oopDesc", "_metadata._compressed_klass") };
        static const vmhook::hotspot::vm_struct_entry_t* const klass_entry{ vmhook::hotspot::iterate_struct_entries("oopDesc", "_metadata._klass") };

        const std::size_t mark_offset{ mark_entry ? static_cast<std::size_t>(mark_entry->offset) : 0u };
        *reinterpret_cast<std::uintptr_t*>(reinterpret_cast<std::uint8_t*>(object_pointer) + mark_offset) = klass->get_prototype_header();

        if (compressed_klass_entry)
        {
            *reinterpret_cast<std::uint32_t*>(reinterpret_cast<std::uint8_t*>(object_pointer) + compressed_klass_entry->offset) = vmhook::hotspot::encode_klass_pointer(klass);
        }
        else if (klass_entry)
        {
            *reinterpret_cast<vmhook::hotspot::klass**>(reinterpret_cast<std::uint8_t*>(object_pointer) + klass_entry->offset) = klass;
        }
        else
        {
            *reinterpret_cast<std::uint32_t*>(reinterpret_cast<std::uint8_t*>(object_pointer) + 8u) = vmhook::hotspot::encode_klass_pointer(klass);
        }

        auto result{ std::make_unique<wrapper_type>(object_pointer) };

        if constexpr (requires(wrapper_type & wrapper, args_t&&... construct_args)
        {
            wrapper.construct(std::forward<args_t>(construct_args)...);
        })
        {
            result->construct(std::forward<args_t>(args)...);
        }
        else if constexpr (sizeof...(args_t) > 0)
        {
            VMHOOK_LOG("{} vmhook::make_unique<{}>(): object allocated, but wrapper has no matching construct(...) method for the provided arguments.", vmhook::warning_tag, typeid(wrapper_type).name());
        }

        return result;
    }

    // --- Field access ---------------------------------------------------------

    /*
        @brief Cache of field entries keyed by klass* then field name.
        @details
        Populated lazily by find_field() on the first lookup of each (klass, name) pair.
        Raw klass pointers are safe as keys for process-lifetime injection; class unloading
        would invalidate them, but that is not a concern for the typical use case here.
    */
    inline std::unordered_map<vmhook::hotspot::klass*, std::unordered_map<std::string, vmhook::hotspot::field_entry_t>> g_field_cache{};

    /*
        @brief Looks up and caches a field entry for a named field on a klass.
        @param target_klass  The klass that declares the field (obtain via find_class()).
                             Only the declaring class is searched - not superclasses.
        @param name          The exact Java field name.
        @return The cached field_entry_t, or std::nullopt if the field is not found.
        @details
        On the first call for a given (target_klass, name) pair the full InstanceKlass._fields
        array is walked; subsequent calls return the cached result directly.
    */
    static auto find_field(vmhook::hotspot::klass* const target_klass, const std::string_view name)
        -> std::optional<vmhook::hotspot::field_entry_t>
    {
        if (!target_klass || !vmhook::hotspot::is_valid_pointer(target_klass))
        {
            VMHOOK_LOG("{} vmhook::find_field() for '{}': klass pointer is null.", vmhook::error_tag, name);
            return std::nullopt;
        }

        auto& class_fields{ vmhook::g_field_cache[target_klass] };
        const std::string name_str{ name };

        if (const auto field_cache_entry{ class_fields.find(name_str) }; field_cache_entry != class_fields.end())
        {
            return field_cache_entry->second;
        }

        // Walk the superclass chain so that inherited fields are found.
        for (vmhook::hotspot::klass* k{ target_klass }; k != nullptr; k = k->get_super())
        {
            const auto entry{ k->find_field(name) };
            if (entry)
            {
                class_fields.emplace(name_str, *entry);
                return entry;
            }
        }

        VMHOOK_LOG("{} vmhook::find_field() for '{}': field not found in class hierarchy.", vmhook::error_tag, name);
        return std::nullopt;
    }

    /*
        @brief Reads an instance field from a Java object by name.
        @tparam T      The C++ scalar type to read the field as (int, float, bool,
                       double, long long, short, char, std::byte, uint32_t, etc.).
        @param object  Decoded pointer to the Java object (not a compressed OOP).
                       Obtain it from frame->get_arguments<void*>() which already
                       calls decode_oop_pointer() internally.
        @param k       The klass that declares the field (from find_class()).
        @param name    The exact Java field name.
        @return The field value as T, or a default-constructed T on failure.
        @note For reference-type fields specify T = uint32_t to receive the raw
              compressed OOP, then pass it to vmhook::hotspot::decode_oop_pointer() yourself.
    */
    template<typename value_type>
    static auto get_field(void* const object, vmhook::hotspot::klass* const target_klass, const std::string_view name)
        -> value_type
    {
        static_assert(std::is_trivially_copyable_v<value_type>, "get_field<value_type>: value_type must be trivially copyable.");

        try
        {
            const auto entry{ vmhook::find_field(target_klass, name) };

            if (!entry)
            {
                throw vmhook::exception{ std::format("Field '{}' not found.", name) };
            }

            if (entry->is_static)
            {
                void* const mirror{ target_klass->get_java_mirror() };
                if (!mirror || !vmhook::hotspot::is_valid_pointer(mirror))
                {
                    throw vmhook::exception{ "Failed to retrieve java.lang.Class mirror." };
                }

                value_type result{};
                std::memcpy(&result, reinterpret_cast<const std::uint8_t*>(mirror) + entry->offset, sizeof(value_type));
                return result;
            }

            if (!object || !vmhook::hotspot::is_valid_pointer(object))
            {
                throw vmhook::exception{ "Object pointer is null or invalid." };
            }

            value_type result{};
            std::memcpy(&result, reinterpret_cast<const std::uint8_t*>(object) + entry->offset, sizeof(value_type));
            return result;
        }
        catch (const std::exception& exception)
        {
            VMHOOK_LOG("{} vmhook::get_field<{}>('{}') {}", vmhook::error_tag, typeid(value_type).name(), name, exception.what());
            return value_type{};
        }
    }

    /*
        @brief Writes a value to an instance field of a Java object.
        @tparam T      The C++ scalar type to write.
        @param object  Decoded pointer to the Java object instance.
        @param k       The klass that declares the field.
        @param name    The exact Java field name.
        @param value   The value to write.
        @note Writing a reference-type field requires a properly encoded compressed OOP.
              Encoding is: (real_address - narrow_oop_base) >> narrow_oop_shift.
    */
    template<typename value_type>
    static auto set_field(void* const object, vmhook::hotspot::klass* const target_klass, const std::string_view name, const value_type value)
        -> void
    {
        static_assert(std::is_trivially_copyable_v<value_type>, "set_field<value_type>: value_type must be trivially copyable.");

        try
        {
            const auto entry{ vmhook::find_field(target_klass, name) };

            if (!entry)
            {
                throw vmhook::exception{ std::format("Field '{}' not found.", name) };
            }

            if (entry->is_static)
            {
                void* const mirror{ target_klass->get_java_mirror() };
                if (!mirror || !vmhook::hotspot::is_valid_pointer(mirror))
                {
                    throw vmhook::exception{ "Failed to retrieve java.lang.Class mirror." };
                }

                std::memcpy(reinterpret_cast<std::uint8_t*>(mirror) + entry->offset, &value, sizeof(value_type));
                return;
            }

            if (!object || !vmhook::hotspot::is_valid_pointer(object))
            {
                throw vmhook::exception{ "Object pointer is null or invalid." };
            }

            std::memcpy(reinterpret_cast<std::uint8_t*>(object) + entry->offset, &value, sizeof(value_type));
        }
        catch (const std::exception& exception)
        {
            VMHOOK_LOG("{} vmhook::set_field<{}>('{}') {}", vmhook::error_tag, typeid(value_type).name(), name, exception.what());
        }
    }

    // --- Java object / array / string allocation helpers ---------------------

    /*
        @brief Allocates a raw Java object of the given klass directly from a thread TLAB.
        @details
        Performs a zero-initialised TLAB allocation and stamps the HotSpot object header:
          - oopDesc._mark / _markWord  <- klass->get_prototype_header()
          - oopDesc._metadata._compressed_klass (or _klass) <- encoded klass pointer
        Does not call any Java constructor; the resulting object is zeroed beyond the header.
        Used as the low-level allocation primitive by make_java_string() and make_java_array().

        Allocation strategy (in order):
          1. Thread-local current_java_thread TLAB.
          2. Walk up to 256 threads from find_any_java_thread().
          3. allocate_from_threads_list() (SMR list).
        Falls back through all three before returning nullptr.

        Complexity: O(1) on the fast path; O(N) on the fallback walk.
        Exception safety: noexcept — returns nullptr on any failure.

        @param klass           HotSpot klass to stamp into the object header.
        @param requested_size  Minimum allocation size in bytes; rounded up to 8-byte alignment.
        @return  Pointer to zeroed, header-stamped object memory, or nullptr on failure.
    */
    inline auto make_java_object(vmhook::hotspot::klass* const klass, const std::size_t requested_size) noexcept
        -> void*
    {
        if (!vmhook::hotspot::ensure_current_java_thread())
        {
            return nullptr;
        }

        if (!klass || requested_size == 0)
        {
            return nullptr;
        }

        const std::size_t object_size{ (requested_size + 7u) & ~static_cast<std::size_t>(7u) };
        vmhook::hotspot::java_thread* const thread{ vmhook::hotspot::find_allocation_thread() };

        void* object_pointer{};
        if (thread && vmhook::hotspot::is_valid_pointer(thread))
        {
            object_pointer = thread->allocate_tlab(object_size);
        }

        if (!object_pointer)
        {
            std::int32_t visited_threads{ 0 };
            for (vmhook::hotspot::java_thread* candidate{ vmhook::hotspot::find_any_java_thread() };
                candidate && vmhook::hotspot::is_valid_pointer(candidate) && visited_threads < 256;
                candidate = candidate->get_next(), ++visited_threads)
            {
                object_pointer = candidate->allocate_tlab(object_size);
                if (object_pointer)
                {
                    vmhook::hotspot::last_java_thread.store(candidate, std::memory_order_relaxed);
                    break;
                }
            }
        }

        if (!object_pointer)
        {
            object_pointer = vmhook::hotspot::allocate_from_threads_list(object_size);
        }

        if (!object_pointer)
        {
            return nullptr;
        }

        std::memset(object_pointer, 0, object_size);

        static const vmhook::hotspot::vm_struct_entry_t* const mark_entry{ []()
            -> const vmhook::hotspot::vm_struct_entry_t*
            {
                if (auto* const entry{ vmhook::hotspot::iterate_struct_entries("oopDesc", "_mark") })
                {
                    return entry;
                }

                return vmhook::hotspot::iterate_struct_entries("oopDesc", "_markWord");
            }()
        };
        static const vmhook::hotspot::vm_struct_entry_t* const compressed_klass_entry{ vmhook::hotspot::iterate_struct_entries("oopDesc", "_metadata._compressed_klass") };
        static const vmhook::hotspot::vm_struct_entry_t* const klass_entry{ vmhook::hotspot::iterate_struct_entries("oopDesc", "_metadata._klass") };

        const std::size_t mark_offset{ mark_entry ? static_cast<std::size_t>(mark_entry->offset) : 0u };
        *reinterpret_cast<std::uintptr_t*>(reinterpret_cast<std::uint8_t*>(object_pointer) + mark_offset) = klass->get_prototype_header();

        if (compressed_klass_entry)
        {
            *reinterpret_cast<std::uint32_t*>(reinterpret_cast<std::uint8_t*>(object_pointer) + compressed_klass_entry->offset) = vmhook::hotspot::encode_klass_pointer(klass);
        }
        else if (klass_entry)
        {
            *reinterpret_cast<vmhook::hotspot::klass**>(reinterpret_cast<std::uint8_t*>(object_pointer) + klass_entry->offset) = klass;
        }
        else
        {
            *reinterpret_cast<std::uint32_t*>(reinterpret_cast<std::uint8_t*>(object_pointer) + 8u) = vmhook::hotspot::encode_klass_pointer(klass);
        }

        return object_pointer;
    }

    /*
        @brief Allocates a raw Java array of the given element type and length.
        @details
        Finds the array klass by class_name (e.g. "[B" for byte[], "[C" for char[]),
        allocates `array_header_size + length * element_size` bytes via make_java_object(),
        and writes the Java array length into the 32-bit slot at byte offset 12 (the standard
        HotSpot arrayOop _length field layout on 64-bit VMs with UseCompressedOops).

        The caller is responsible for filling in the element data after this call.

        Complexity: O(1) on the fast TLAB path; O(N) on the fallback.
        Exception safety: noexcept — returns nullptr on any failure.

        @param class_name    Internal JVM array type descriptor (e.g. "[B", "[C", "[Ljava/lang/Object;").
        @param length        Number of elements; must be >= 0.
        @param element_size  Size in bytes of each array element.
        @return  Pointer to the raw array OOP with header and length initialised, or nullptr on failure.
    */
    inline auto make_java_array(const std::string_view class_name, const std::int32_t length, const std::size_t element_size) noexcept
        -> void*
    {
        if (length < 0)
        {
            return nullptr;
        }

        vmhook::hotspot::klass* const array_klass{ vmhook::find_class(class_name) };
        if (!array_klass)
        {
            return nullptr;
        }

        constexpr std::size_t array_header_size{ 16u };
        void* const array_oop{ vmhook::make_java_object(array_klass, array_header_size + static_cast<std::size_t>(length) * element_size) };
        if (!array_oop)
        {
            return nullptr;
        }

        *reinterpret_cast<std::int32_t*>(reinterpret_cast<std::uint8_t*>(array_oop) + 12u) = length;
        return array_oop;
    }

    /*
        @brief Allocates a new java.lang.String OOP from a UTF-8 string_view.
        @details
        Allocates a java.lang.String instance via make_java_object(), then detects at
        runtime whether the JVM uses compact strings (JDK 9+, "coder" field present) or
        classic char[] strings (JDK 8):
          - Compact:  allocates a byte[] ("[B"), copies UTF-8 bytes directly, sets coder=0 (LATIN1).
          - Classic:  allocates a char[] ("[C"), widens each byte to uint16.
        Sets the "value" field of the String to the encoded OOP of the backing array.
        Capped at 4096 characters to avoid oversized allocations.

        Complexity: O(N) where N = length of value.
        Exception safety: noexcept — returns nullptr on any allocation failure.

        @param value  UTF-8 text to encode as a Java String (capped at 4096 chars).
        @return  Raw java.lang.String OOP, or nullptr on failure.
    */
    inline auto make_java_string(const std::string_view value) noexcept
        -> void*
    {
        vmhook::hotspot::klass* const string_klass{ vmhook::find_class("java/lang/String") };
        if (!string_klass)
        {
            return nullptr;
        }

        void* const string_oop{ vmhook::make_java_object(string_klass, string_klass->get_instance_size()) };
        if (!string_oop)
        {
            return nullptr;
        }

        const bool compact_string{ string_klass->find_field("coder").has_value() };
        const std::int32_t length{ static_cast<std::int32_t>(std::min<std::size_t>(value.size(), 4096u)) };

        if (compact_string)
        {
            void* const value_array{ vmhook::make_java_array("[B", length, sizeof(std::uint8_t)) };
            if (!value_array)
            {
                return nullptr;
            }

            std::memcpy(reinterpret_cast<std::uint8_t*>(value_array) + 16u, value.data(), static_cast<std::size_t>(length));
            vmhook::set_field(string_oop, string_klass, "value", vmhook::hotspot::encode_oop_pointer(value_array));
            vmhook::set_field<std::uint8_t>(string_oop, string_klass, "coder", 0u);
        }
        else
        {
            void* const value_array{ vmhook::make_java_array("[C", length, sizeof(std::uint16_t)) };
            if (!value_array)
            {
                return nullptr;
            }

            auto* const chars{ reinterpret_cast<std::uint16_t*>(reinterpret_cast<std::uint8_t*>(value_array) + 16u) };
            for (std::int32_t index{ 0 }; index < length; ++index)
            {
                chars[index] = static_cast<std::uint8_t>(value[static_cast<std::size_t>(index)]);
            }

            vmhook::set_field(string_oop, string_klass, "value", vmhook::hotspot::encode_oop_pointer(value_array));

            if (string_klass->find_field("offset").has_value())
            {
                vmhook::set_field<std::int32_t>(string_oop, string_klass, "offset", 0);
            }

            if (string_klass->find_field("count").has_value())
            {
                vmhook::set_field<std::int32_t>(string_oop, string_klass, "count", length);
            }
        }

        if (string_klass->find_field("hash").has_value())
        {
            vmhook::set_field<std::int32_t>(string_oop, string_klass, "hash", 0);
        }

        return string_oop;
    }

    // --- Array element access -------------------------------------------------

    /*
        @brief Reads the length of a Java primitive array.
        @param array_oop Decoded pointer to the Java array object (not compressed).
        @return Element count, or 0 if the pointer is invalid.
        @details
        HotSpot Array object layout (x64, compressed OOPs):
          +0  mark word (8 B)
          +8  klass pointer (4 B)
          +12 _length   (int)
          +16 _data[0]
    */
    inline static auto array_length(void* const array_oop) noexcept
        -> std::int32_t
    {
        if (!array_oop || !vmhook::hotspot::is_valid_pointer(array_oop))
        {
            return 0;
        }

        return *reinterpret_cast<const std::int32_t*>(reinterpret_cast<const std::uint8_t*>(array_oop) + 12);
    }

    /*
        @brief Reads the element at the given index from a Java primitive array.
        @tparam T    The C++ element type (int32_t, double, int8_t, bool, etc.).
        @param array_oop Decoded pointer to the Java array object.
        @param index Zero-based element index.
        @return The element value, or T{} if the pointer/index is invalid.
        @details
        Data starts at offset +16 from the array oop.  Element stride is sizeof(T).
        Bounds checking is performed against the array length.
    */
    template<typename element_type>
    static auto get_array_element(void* const array_oop, const std::int32_t index)
        -> element_type
    {
        static_assert(std::is_trivially_copyable_v<element_type>, "get_array_element<element_type>: element_type must be trivially copyable.");
        if (!array_oop || !vmhook::hotspot::is_valid_pointer(array_oop))
        {
            return element_type{};
        }
        const std::int32_t length{ array_length(array_oop) };
        if (index < 0 || index >= length)
        {
            return element_type{};
        }

        element_type result{};
        std::memcpy(&result, reinterpret_cast<const std::uint8_t*>(array_oop) + 16 + index * static_cast<std::int32_t>(sizeof(element_type)), sizeof(element_type));
        return result;
    }

    /*
        @brief Writes a value into a Java primitive array at the given index.
        @tparam T    The C++ element type.
        @param array_oop Decoded pointer to the Java array object.
        @param index Zero-based element index.
        @param value The value to write.
    */
    template<typename element_type>
    static auto set_array_element(void* const array_oop, const std::int32_t index, const element_type value)
        -> void
    {
        static_assert(std::is_trivially_copyable_v<element_type>, "set_array_element<element_type>: element_type must be trivially copyable.");
        if (!array_oop || !vmhook::hotspot::is_valid_pointer(array_oop))
        {
            return;
        }
        const std::int32_t length{ array_length(array_oop) };
        if (index < 0 || index >= length)
        {
            return;
        }
        std::memcpy(reinterpret_cast<std::uint8_t*>(array_oop) + 16 + index * static_cast<std::int32_t>(sizeof(element_type)), &value, sizeof(element_type));
    }

    // --- Field proxy ----------------------------------------------------------

        /*
            @brief Lightweight proxy to a single Java field value in memory.
            @details
            Returned by vmhook::object::get_field().  Reads the field value on demand,
            dispatches the correct C++ type from the JVM type descriptor (signature),
            and returns a typed copy - not a raw-pointer alias.

            Usage inside a wrapper class:

                // No trailing return type needed - auto deduces field_proxy::value_t,
                // which converts implicitly to the target type at the call site.
                 auto is_connected() -> bool { return get_field("isConnected")->get(); }
                 auto get_health()   -> int { return get_field("health")->get(); }

                 // If you want a concrete type inside the method, cast before returning:
                 auto get_max_hp()   -> int { return static_cast<int>(get_field("maxHp")->get()); }

            Usage at the call site:
                bool ok  = client.is_connected();   // operator bool()  fires
                int  hp  = client.get_health();     // operator int()   fires
                if  (client.is_connected()) { ... } // contextual bool conversion

            Writing:
                obj.get_field("health")->set(100);  // T = int, deduced
                obj.get_field("flag"  )->set(true); // T = bool, deduced
        */
    class field_proxy final
    {
    public:
        /*
            @brief A typed copy of the field's value.
            @details
            Holds one alternative from a variant whose alternatives cover every JVM
            primitive type and compressed-OOP references.  get() selects the
            alternative that matches the field's JVM type descriptor before returning,
            so the value is already correctly cast.

            Implicitly converts to any type T via std::visit + static_cast, which
            means you can write:

                bool b  = proxy->get();   // contextual / assignment conversion
                int  i  = proxy->get();
                float f = proxy->get();
                auto  v = proxy->get();   // type is field_proxy::value_t; converts lazily

            For reference-type fields (signature starts with 'L' or '[') the stored
            alternative is uint32_t (the raw compressed OOP).  Pass it to
            vmhook::hotspot::decode_oop_pointer() to recover the real 64-bit address.
        */
        struct value_t
        {
            std::variant<
                bool,
                std::int8_t,
                std::int16_t,
                std::int32_t,
                std::int64_t,
                float,
                double,
                std::uint16_t,
                std::uint32_t   // reference / array (compressed OOP)
            > data;
            std::string signature{};

            /*
                @brief Appends one boolean element from a Java boolean[] to result.
                @details
                Reads the element as uint8_t and converts non-zero to true.
                Complexity: O(1). Exception safety: noexcept.
            */
            static auto append_array_value(std::vector<bool>& result, void* const array_oop, const std::int32_t index, std::string_view) noexcept
                -> void
            {
                result.push_back(vmhook::get_array_element<std::uint8_t>(array_oop, index) != 0);
            }

            /*
                @brief Appends one Java String element from a String[] to result.
                @details
                Reads the compressed OOP at index, decodes it, then calls read_java_string().
                Complexity: O(S) where S = string length. Exception safety: noexcept.
            */
            static auto append_array_value(std::vector<std::string>& result, void* const array_oop, const std::int32_t index, std::string_view) noexcept
                -> void
            {
                const std::uint32_t element_compressed{ vmhook::get_array_element<std::uint32_t>(array_oop, index) };
                result.push_back(vmhook::read_java_string(vmhook::hotspot::decode_oop_pointer(element_compressed)));
            }

            /*
                @brief Appends one element from a Java char[] or byte[] to a std::vector<char>.
                @details
                For "[C" arrays reads a uint16 and narrows to char; for all other arrays
                reads directly as char.
                Complexity: O(1). Exception safety: noexcept.
            */
            static auto append_array_value(std::vector<char>& result, void* const array_oop, const std::int32_t index, const std::string_view signature) noexcept
                -> void
            {
                if (signature == "[C")
                {
                    result.push_back(static_cast<char>(vmhook::get_array_element<std::uint16_t>(array_oop, index)));
                }
                else
                {
                    result.push_back(vmhook::get_array_element<char>(array_oop, index));
                }
            }

            /*
                @brief Generic overload: appends one element_type element from a Java array to result.
                @details
                Used for numeric types (int, float, double, etc.) and any other trivially
                copyable element_type.
                Complexity: O(1). Exception safety: noexcept.
            */
            template<typename element_type>
            static auto append_array_value(std::vector<element_type>& result, void* const array_oop, const std::int32_t index, std::string_view) noexcept
                -> void
            {
                result.push_back(vmhook::get_array_element<element_type>(array_oop, index));
            }

            /*
                @brief Decodes a compressed array OOP and reads all elements into a std::vector<target_type>.
                @details
                Decodes compressed (the compressed array OOP) via decode_array_oop(), reads
                the Java array length, reserves the result vector, then calls append_array_value()
                for each element.  The correct append_array_value overload is selected based on
                target_type's element type at compile time.

                Complexity: O(N) where N = array length.
                Exception safety: noexcept — returns an empty vector on any failure.

                @param compressed  Compressed OOP of the Java array.
                @param signature   JVM array type descriptor (e.g. "[I", "[Ljava/lang/String;").
                @return  Vector containing all decoded elements, or an empty vector on failure.
            */
            template<typename target_type>
            static auto read_array_value(const std::uint32_t compressed, const std::string_view signature) noexcept
                -> target_type
            {
                target_type result{};
                void* const array_oop{ vmhook::decode_array_oop(compressed) };
                if (!array_oop)
                {
                    return result;
                }

                const std::int32_t length{ vmhook::array_length(array_oop) };
                if (length <= 0)
                {
                    return result;
                }

                result.reserve(static_cast<std::size_t>(length));
                for (std::int32_t index{ 0 }; index < length; ++index)
                {
                    append_array_value(result, array_oop, index, signature);
                }

                return result;
            }

            /*
                @brief Converts a variant alternative source_type to target_type with semantic dispatch.
                @details
                Called by operator target_type() via std::visit.  Applies the correct
                conversion strategy based on the target:
                  - std::string          <- decodes compressed OOP via read_java_string()
                  - std::vector<T>       <- decodes array OOP and reads all elements
                  - std::unique_ptr<T>   <- decodes compressed OOP and constructs wrapper
                  - void*                <- decodes compressed OOP to raw pointer
                  - numeric / bool types <- static_cast from the variant alternative

                Complexity: O(1) for scalars; O(N) for strings/arrays.
                Exception safety: noexcept — returns a default-constructed target_type on failure.

                @tparam target_type  Desired output type.
                @tparam source_type  The variant alternative actually stored.
                @param value         The stored alternative value.
                @return              Converted value, or default-constructed target_type on failure.
            */
            template<typename target_type, typename source_type>
            auto cast_for_variant(source_type value) const noexcept
                -> target_type
            {
                using clean_target_type = std::remove_cvref_t<target_type>;
                using clean_source_type = std::remove_cvref_t<source_type>;

                if constexpr (std::is_same_v<clean_target_type, std::string>)
                {
                    if constexpr (std::is_same_v<clean_source_type, std::uint32_t>)
                    {
                        return vmhook::read_java_string(vmhook::hotspot::decode_oop_pointer(value));
                    }
                    else
                    {
                        return {};
                    }
                }
                else if constexpr (vmhook::detail::is_vector_v<clean_target_type>)
                {
                    if constexpr (std::is_same_v<clean_source_type, std::uint32_t>)
                    {
                        return read_array_value<clean_target_type>(value, this->signature);
                    }
                    else
                    {
                        return {};
                    }
                }
                else if constexpr (vmhook::detail::is_unique_ptr_v<clean_target_type>)
                {
                    if constexpr (std::is_same_v<clean_source_type, std::uint32_t>)
                    {
                        using wrapper_type = typename clean_target_type::element_type;
                        void* const decoded{ vmhook::hotspot::decode_oop_pointer(value) };
                        if (!decoded || !vmhook::hotspot::is_valid_pointer(decoded))
                        {
                            return nullptr;
                        }
                        return clean_target_type{ new wrapper_type{ decoded } };
                    }
                    else
                    {
                        return nullptr;
                    }
                }
                else if constexpr (std::is_same_v<target_type, void*>)
                {
                    // For void*, only convert from uint32_t (compressed OOP)
                    if constexpr (std::is_same_v<clean_source_type, std::uint32_t>)
                    {
                        return vmhook::hotspot::decode_oop_pointer(value);
                    }
                    else
                    {
                        return static_cast<target_type>(nullptr);
                    }
                }
                else if constexpr (requires { static_cast<target_type>(value); })
                {
                    return static_cast<target_type>(value);
                }
                else
                {
                    return target_type{};
                }
            }

            /*
                @brief Implicit conversion operator from value_t to any supported target type.
                @details
                Delegates to std::visit + cast_for_variant<target_type> so any assignment
                or contextual conversion triggers the correct semantic:
                  bool b  = proxy->get();
                  int  i  = proxy->get();
                  auto s  = static_cast<std::string>(proxy->get());

                Complexity: O(1) for scalars; O(N) for strings/arrays.
                Exception safety: noexcept.

                @tparam target_type  The type the value is being converted to.
            */
            template<typename target_type>
            operator target_type() const noexcept
            {
                return std::visit([this](auto value) noexcept
                    -> target_type
                    {
                        return this->cast_for_variant<target_type>(value);
                    }, data);
            }

            /*
                @brief Converts a reference-type field into a vector of unique_ptr wrappers.
                @details
                Interprets the stored compressed OOP as a Java object array, decodes each
                element, and constructs a std::unique_ptr<element_type> wrapper for each.
                Defined out-of-line after array helpers are available.

                Complexity: O(N) where N = array length.
            */
            template<typename element_type>
            auto to_vector() const
                -> std::vector<std::unique_ptr<element_type>>;
        };

        /*
            @param field_pointer Direct pointer to the field's bytes in JVM memory
                                 (decoded object address + offset for instance fields;
                                  java.lang.Class mirror + offset for static fields).
            @param signature     JVM type descriptor, e.g. "I", "Z", "Ljava/lang/String;"
            @param is_static     true when JVM_ACC_STATIC is set on the field.
        */
        field_proxy(void* field_pointer, std::string sig, const bool is_static_flag) noexcept
            : field_pointer{ field_pointer }
            , signature_text{ std::move(sig) }
            , static_field{ is_static_flag }
        {
        }

        /*
            @brief Reads the field and returns a typed copy.
            @details
            Dispatches on the JVM type descriptor to determine how many bytes to
            read and which variant alternative to populate:
              "Z"  bool       "B"  int8_t    "S"  int16_t   "I"  int32_t
              "J"  int64_t    "F"  float     "D"  double    "C"  uint16_t
              "L"/"["  uint32_t (compressed OOP)
            The returned value is a copy - safe to store and return from methods.
        */
        auto get() const noexcept
            -> value_t
        {
            if (!this->field_pointer)
            {
                return value_t{ std::int32_t{}, this->signature_text };
            }

            if (this->signature_text == "Z")
            {
                bool value{};
                std::memcpy(&value, this->field_pointer, sizeof(value));
                return value_t{ value, this->signature_text };
            }
            if (this->signature_text == "B")
            {
                std::int8_t value{};
                std::memcpy(&value, this->field_pointer, sizeof(value));
                return value_t{ value, this->signature_text };
            }
            if (this->signature_text == "S")
            {
                std::int16_t value{};
                std::memcpy(&value, this->field_pointer, sizeof(value));
                return value_t{ value, this->signature_text };
            }
            if (this->signature_text == "I")
            {
                std::int32_t value{};
                std::memcpy(&value, this->field_pointer, sizeof(value));
                return value_t{ value, this->signature_text };
            }
            if (this->signature_text == "J")
            {
                std::int64_t value{};
                std::memcpy(&value, this->field_pointer, sizeof(value));
                return value_t{ value, this->signature_text };
            }
            if (this->signature_text == "F")
            {
                float value{};
                std::memcpy(&value, this->field_pointer, sizeof(value));
                return value_t{ value, this->signature_text };
            }
            if (this->signature_text == "D")
            {
                double value{};
                std::memcpy(&value, this->field_pointer, sizeof(value));
                return value_t{ value, this->signature_text };
            }
            if (this->signature_text == "C")
            {
                std::uint16_t value{};
                std::memcpy(&value, this->field_pointer, sizeof(value));
                return value_t{ value, this->signature_text };
            }

            // Reference or array type - store compressed OOP
            std::uint32_t value{};
            std::memcpy(&value, this->field_pointer, sizeof(value));
            return value_t{ value, this->signature_text };
        }

        /*
            @brief Writes value into the field's storage.
            @details
            This is the only field setter exposed by field_proxy. It accepts JVM
            primitives, std::string for java.lang.String, and std::vector<T> for
            Java arrays. String and array writes update the existing Java object
            in place because this zero-JNI layer cannot resize Java heap objects.
        */
        template<typename value_type>
        auto set(const value_type& value) const noexcept
            -> void
        {
            using clean_value_type = std::remove_cvref_t<value_type>;

            if constexpr (std::is_same_v<clean_value_type, std::string>)
            {
                vmhook::set_str_field(*this, value);
            }
            else if constexpr (std::is_convertible_v<value_type, std::string_view> && !std::is_same_v<clean_value_type, std::string>)
            {
                vmhook::set_str_field(*this, std::string_view{ value });
            }
            else if constexpr (vmhook::detail::is_vector_v<clean_value_type>)
            {
                if constexpr (std::is_same_v<clean_value_type, std::vector<bool>>)
                {
                    vmhook::set_bool_array(*this, value);
                }
                else if constexpr (std::is_same_v<clean_value_type, std::vector<std::string>>)
                {
                    vmhook::set_str_array(*this, value);
                }
                else
                {
                    vmhook::set_prim_array(*this, value);
                }
            }
            else if constexpr (vmhook::detail::is_unique_ptr_v<clean_value_type>)
            {
                if (this->field_pointer)
                {
                    // Explicit base-class qualification reaches object_base::get_instance()
                    // even when the derived wrapper shadows the name with a same-named
                    // static helper (e.g. example_class::get_instance() returning a
                    // unique_ptr).  Resolved at instantiation; never requires object_base
                    // to be complete at template-definition time.
                    void* oop_pointer{ nullptr };
                    if (value)
                    {
                        oop_pointer = value->vmhook::object_base::get_instance();
                    }
                    const std::uint32_t compressed{ oop_pointer
                        ? vmhook::hotspot::encode_oop_pointer(oop_pointer)
                        : std::uint32_t{ 0 } };
                    std::memcpy(this->field_pointer, &compressed, sizeof(compressed));
                }
            }
            else if constexpr (std::is_trivially_copyable_v<clean_value_type>)
            {
                if (this->field_pointer)
                {
                    if (this->signature_text == "C" && sizeof(clean_value_type) == sizeof(char))
                    {
                        const std::uint16_t wide_value{ static_cast<std::uint16_t>(static_cast<unsigned char>(value)) };
                        std::memcpy(this->field_pointer, &wide_value, sizeof(wide_value));
                    }
                    else
                    {
                        std::memcpy(this->field_pointer, &value, sizeof(clean_value_type));
                    }
                }
            }
        }

        /*
            @brief Returns the JVM type descriptor of this field (e.g. "I", "Z", "J").
        */
        auto signature() const noexcept
            -> std::string_view
        {
            return this->signature_text;
        }

        /*
            @brief Returns the raw memory address backing this field.
            @details
            For instance fields this is `decoded_object + field_offset`; for
            static fields it is `java.lang.Class mirror + field_offset`.
            Exposed so that watch_static_field can install a hardware
            breakpoint on the slot; most code should prefer get()/set().
        */
        auto raw_address() const noexcept -> void*
        {
            return this->field_pointer;
        }

        /*
            @brief Returns true if this field has JVM_ACC_STATIC set.
            @details
            Static fields are read from / written to the java.lang.Class mirror
            of the declaring class rather than from an object instance.

            Complexity: O(1).
            Exception safety: noexcept.
        */
        auto is_static() const noexcept
            -> bool
        {
            return this->static_field;
        }

        /*
            @brief Returns the compressed OOP stored in this field (for reference/array types).
            @return The compressed OOP as uint32_t, or 0 if field_pointer is null.
        */
        auto get_compressed_oop() const noexcept
            -> std::uint32_t
        {
            if (!this->field_pointer)
            {
                return 0;
            }
            std::uint32_t value{};
            std::memcpy(&value, this->field_pointer, sizeof(value));
            return value;
        }

    private:
        void* field_pointer;
        std::string signature_text;
        bool        static_field;
    };

    // --- detail helpers that depend on hotspot types -------------------------
    // (reopened here because find_call_stub_entry references hotspot types
    //  that are not yet defined at the earlier detail block position)
    namespace detail
    {
        /*
            @brief Locates StubRoutines::_call_stub_entry via VMStructs.

            HotSpot's official C++-to-Java call gate. Creates a properly-typed
            "entry frame" so the GC, exception handler, and frame-walker all
            recognise the native-to-Java boundary. Unlike jumping directly to a
            c2i adapter, this prevents JVM fatal errors when the frame-walker
            encounters an unexpected native frame above the interpreter frame.

            Stub signature (Windows x64):
              rcx  = link (1 sentinel)
              rdx  = intptr_t* result holder
              r8   = BasicType (T_INT=10, T_VOID=14, )
              r9   = Method*
              stk  = entry_point, parameters*, param_count, JavaThread*
        */
        inline auto find_call_stub_entry() noexcept
            -> void*
        {
            static const vmhook::hotspot::vm_struct_entry_t* const entry{
                vmhook::hotspot::iterate_struct_entries("StubRoutines", "_call_stub_entry") };
            if (!entry || !entry->address)
            {
                return nullptr;
            }

            void* const stub{ *reinterpret_cast<void**>(entry->address) };
            return vmhook::hotspot::is_valid_pointer(stub) ? stub : nullptr;
        }

        /*
            @brief Maps a JVM type-descriptor character to a HotSpot BasicType int.
            Values are stable across all supported JDK versions.
        */
        inline auto sig_char_to_basic_type(const char c) noexcept -> int
        {
            switch (c)
            {
            case 'Z': return 4;   // T_BOOLEAN
            case 'C': return 5;   // T_CHAR
            case 'F': return 6;   // T_FLOAT
            case 'D': return 7;   // T_DOUBLE
            case 'B': return 8;   // T_BYTE
            case 'S': return 9;   // T_SHORT
            case 'I': return 10;  // T_INT
            case 'J': return 11;  // T_LONG
            case 'L': return 12;  // T_OBJECT
            case '[': return 13;  // T_ARRAY
            case 'V': return 14;  // T_VOID
            default:  return 12;  // T_OBJECT (fallback)
            }
        }
    }

    // --- Method proxy ------------------------------------------------------

    /*
        @brief Lightweight proxy to a Java method, supporting typed argument calls.
        @details
        Returned by vmhook::object::get_method().  Handles argument conversion
        and dispatches to the JVM method via the interpreter entry point.

        Usage inside a wrapper class:
             auto say_hi() -> void { get_method("sayHi")->call(); }

             auto say_string(const std::string& value) -> void {
                get_method("sayString")->call(value);
            }

        Returns a value_t that implicitly converts to the expected C++ type.
    */
    class method_proxy final
    {
    public:
        /*
            @brief Return type from method_proxy::call().
            @details
            Holds the result value as a variant, supporting implicit conversion
            to common C++ types via the conversion operator.
        */
        struct value_t
        {
            std::variant<
                std::monostate,
                bool,
                std::int8_t,
                std::int16_t,
                std::int32_t,
                std::int64_t,
                float,
                double,
                std::uint16_t,
                std::uint32_t,   // reference / array (compressed OOP)
                std::string      // for String objects
            > data;

            /*
                @brief Converts the stored value to T via static_cast.
                Falls back to a default-constructed T for variant alternatives
                that cannot be cast to the target type (std::monostate, std::string).
            */
            template<typename target_type>
            operator target_type() const noexcept
            {
                return std::visit([](auto v) noexcept
                    -> target_type
                    {
                        if constexpr (requires { static_cast<target_type>(v); })
                        {
                            return static_cast<target_type>(v);
                        }
                        else
                        {
                            return target_type{};
                        }
                    }
                , data);
            }
        };

        /*
            @param owning_object The object whose method will be called (may be null for static).
            @param method_pointer Pointer to the HotSpot Method object.
            @param signature     JVM method descriptor, e.g. "(I)Ljava/lang/String;"
        */
        method_proxy(void* owning_object, vmhook::hotspot::method* method_ptr, std::string sig) noexcept
            : object{ owning_object }
            , method{ method_ptr }
            , signature_text{ std::move(sig) }
            , static_field{ false }
        {
        }

        /*
            @brief Invokes the Java method via JNI as a fallback when the call stub is unavailable.
            @details
            Builds a jni_value argument array from args, resolves the jmethodID via
            JNIEnv::GetMethodID, then dispatches to either CallObjectMethodA (for methods
            returning java.lang.String) or CallVoidMethodA (for all other return types).
            Used when detail::find_call_stub_entry() returns nullptr (call stub not yet
            resolved or JDK version incompatibility).

            Complexity: O(method lookup + method execution time).
            Exception safety: noexcept — returns monostate value_t on any failure.

            @param args  C++ arguments to pass; converted via make_jni_args().
            @return  value_t containing the result, or monostate for void/failure.
        */
        template<typename... args_t>
        auto call_jni(args_t&&... args) const noexcept
            -> value_t
        {
            if (!this->object)
            {
                return value_t{ std::monostate{} };
            }

            void* object_handle_storage{};
            void* const object_handle{ vmhook::detail::jni_oop_handle(this->object, object_handle_storage) };
            void* const klass{ vmhook::detail::jni_get_object_class(object_handle) };
            if (!klass)
            {
                VMHOOK_LOG("{} method_proxy::call_jni('{}{}'): GetObjectClass failed.", vmhook::error_tag, this->name(), this->signature_text);
                return value_t{ std::monostate{} };
            }

            void* const method_id{ vmhook::detail::jni_get_method_id(klass, this->name(), this->signature_text) };
            if (!method_id)
            {
                vmhook::detail::jni_exception_clear();

                if (!this->method || !vmhook::hotspot::is_valid_pointer(this->method))
                {
                    VMHOOK_LOG("{} method_proxy::call_jni('{}{}'): GetMethodID failed.", vmhook::error_tag, this->name(), this->signature_text);
                    return value_t{ std::monostate{} };
                }
            }
            void* const resolved_method_id{ method_id ? method_id : reinterpret_cast<void*>(this->method) };

            std::vector<void*> object_handles{};
            std::vector<vmhook::detail::jni_value> values{ vmhook::detail::make_jni_args(object_handles, std::forward<args_t>(args)...) };

            const std::size_t rparen{ this->signature_text.rfind(')') };
            const std::string_view return_signature{ rparen != std::string::npos ? std::string_view{ this->signature_text }.substr(rparen + 1) : std::string_view{ "V" } };

            if (return_signature == "Ljava/lang/String;")
            {
                using call_object_method_a_t = void* (*)(void*, void*, void*, const vmhook::detail::jni_value*);
                call_object_method_a_t const call_object_method_a{ vmhook::detail::jni_function<36, call_object_method_a_t>(vmhook::hotspot::current_jni_env) };
                if (!call_object_method_a)
                {
                    VMHOOK_LOG("{} method_proxy::call_jni('{}{}'): CallObjectMethodA unavailable.", vmhook::error_tag, this->name(), this->signature_text);
                    return value_t{ std::monostate{} };
                }

                void* const result_handle{ call_object_method_a(vmhook::hotspot::current_jni_env, object_handle, resolved_method_id, values.data()) };
                return value_t{ vmhook::detail::jni_get_string_utf(result_handle) };
            }

            using call_void_method_a_t = void (*)(void*, void*, void*, const vmhook::detail::jni_value*);
            call_void_method_a_t const call_void_method_a{ vmhook::detail::jni_function<63, call_void_method_a_t>(vmhook::hotspot::current_jni_env) };
            if (!call_void_method_a)
            {
                VMHOOK_LOG("{} method_proxy::call_jni('{}{}'): CallVoidMethodA unavailable.", vmhook::error_tag, this->name(), this->signature_text);
                return value_t{ std::monostate{} };
            }

            call_void_method_a(vmhook::hotspot::current_jni_env, object_handle, resolved_method_id, values.data());
            return value_t{ std::monostate{} };
        }

        /*
            @brief Calls the method with the given arguments.
            @param args Arguments to pass (C++ types: int, std::string, etc.).
            @return value_t containing the result, or empty value_t for void methods.
            @details
            Converts C++ arguments to JVM types and invokes the method through
            the interpreter entry point. Handles both instance and static methods.

            For reference-type arguments (strings, objects), pass std::string for class name
            or use an object wrapper.
        */
        /*
            @brief Invokes the Java method and returns its result.
            @param args  C++ values forwarded as the method's Java arguments.
                         For instance methods the receiver is added automatically
                         from the proxy's stored object pointer.
            @return value_t holding the Java return value, or monostate if the
                    call gate is unavailable.

            @note  Calling Java methods from native C++ requires HotSpot's
                   StubRoutines::_call_stub_entry, which sets up a properly-typed
                   "entry frame" that the GC and frame-walker understand.
                   This address is located via VMStructs at runtime; if the
                   VMStruct entry is absent (removed in some JDK releases), call()
                   falls back to returning monostate without invoking the method.

                   call() must be invoked from inside a vmhook::hook() detour
                   where vmhook::hotspot::current_java_thread is set.
        */
        template<typename... args_t>
        auto call(args_t&&... args) const noexcept
            -> value_t
        {
            if (!this->method || !vmhook::hotspot::is_valid_pointer(this->method))
            {
                VMHOOK_LOG("{} method_proxy::call(): method pointer is null or invalid.", vmhook::error_tag);
                return value_t{ std::monostate{} };
            }

            vmhook::hotspot::method* const selected_method{ this->resolve_compatible_method<std::remove_cvref_t<args_t>...>() };
            const std::string selected_signature{ selected_method ? selected_method->get_signature() : this->signature_text };

            if (!vmhook::hotspot::ensure_current_java_thread())
            {
                VMHOOK_LOG("{} method_proxy::call('{}{}'): no current JavaThread.", vmhook::error_tag, this->name(), selected_signature);
                return value_t{ std::monostate{} };
            }

            auto* const thread{ vmhook::hotspot::current_java_thread };
            if (!thread)
            {
                VMHOOK_LOG("{} method_proxy::call('{}{}'): current JavaThread is null after attach.", vmhook::error_tag, this->name(), selected_signature);
                return value_t{ std::monostate{} };
            }

            // Locate HotSpot's C++-to-Java call gate via VMStructs.
            // StubRoutines::_call_stub_entry was removed from the VMStruct
            // export table in some JDK releases; check availability at runtime.
            void* const call_stub{ vmhook::detail::find_call_stub_entry() };
            if (!call_stub)
            {
                return method_proxy{ this->object, selected_method, selected_signature }.call_jni(std::forward<args_t>(args)...);
            }

            void* const entry{ selected_method->get_from_interpreted_entry() };
            if (!entry || !vmhook::hotspot::is_valid_pointer(entry))
            {
                VMHOOK_LOG("{} method_proxy::call('{}{}'): interpreted entry is null or invalid.", vmhook::error_tag, this->name(), selected_signature);
                return value_t{ std::monostate{} };
            }

            //  Return type
            const std::string_view sig{ selected_signature };
            const std::size_t rparen{ sig.rfind(')') };
            const char ret_char{
                rparen != std::string_view::npos ? sig[rparen + 1] : 'V' };
            const int result_type{ vmhook::detail::sig_char_to_basic_type(ret_char) };

            //  Parameter slot array
            // The call_stub passes parameters[] to the interpreter as locals[].
            // Each slot is an intptr_t: primitives are zero-extended, object
            // references are uncompressed decoded OOP pointers.
            std::intptr_t params[8]{};
            std::size_t   param_idx{ 0 };

            if (this->object && !this->static_field)
            {
                params[param_idx++] = reinterpret_cast<std::intptr_t>(this->object);
            }

            auto pack = [&](auto&& a) noexcept
                {
                    if (param_idx >= 8)
                    {
                        return;
                    }
                    using clean_t = std::remove_cvref_t<decltype(a)>;
                    if constexpr (std::is_same_v<clean_t, std::string>)
                    {
                        void* const string_oop{ vmhook::make_java_string(a) };
                        if (!string_oop)
                        {
                            VMHOOK_LOG("{} method_proxy::call('{}{}'): failed to allocate Java String argument.", vmhook::error_tag, this->name(), selected_signature);
                        }
                        params[param_idx++] = reinterpret_cast<std::intptr_t>(string_oop);
                    }
                    else if constexpr (std::is_same_v<clean_t, std::string_view>)
                    {
                        void* const string_oop{ vmhook::make_java_string(a) };
                        if (!string_oop)
                        {
                            VMHOOK_LOG("{} method_proxy::call('{}{}'): failed to allocate Java String argument.", vmhook::error_tag, this->name(), selected_signature);
                        }
                        params[param_idx++] = reinterpret_cast<std::intptr_t>(string_oop);
                    }
                    else if constexpr (std::is_same_v<clean_t, const char*> || std::is_same_v<clean_t, char*>)
                    {
                        void* const string_oop{ vmhook::make_java_string(a ? std::string_view{ a } : std::string_view{}) };
                        if (!string_oop)
                        {
                            VMHOOK_LOG("{} method_proxy::call('{}{}'): failed to allocate Java String argument.", vmhook::error_tag, this->name(), selected_signature);
                        }
                        params[param_idx++] = reinterpret_cast<std::intptr_t>(string_oop);
                    }
                    else if constexpr (vmhook::detail::is_unique_ptr_v<clean_t>)
                    {
                        using wrapper_type = typename vmhook::detail::is_unique_ptr<clean_t>::value_type_t;
                        if constexpr (std::is_base_of_v<vmhook::object_base, wrapper_type>)
                        {
                            params[param_idx++] = reinterpret_cast<std::intptr_t>(a ? a->get_instance() : nullptr);
                        }
                    }
                    else if constexpr (std::is_base_of_v<vmhook::object_base, clean_t>)
                    {
                        params[param_idx++] = reinterpret_cast<std::intptr_t>(a.get_instance());
                    }
                    else
                    {
                        static_assert(sizeof(clean_t) <= 8);
                        std::intptr_t v{};
                        std::memcpy(&v, &a, sizeof(clean_t));
                        params[param_idx++] = v;
                    }
                };
            (pack(std::forward<args_t>(args)), ...);

            //  Call the stub
            // Windows x64 calling convention  8 arguments:
            //   rcx = link  (return address for stub frame, -1 sentinel)
            //   rdx = result* (where the Java return value is written)
            //   r8  = result_type (BasicType enum)
            //   r9  = Method*
            //   stk = entry_point, parameters*, param_count, JavaThread*
            using call_stub_fn_t = void (*)(
                void*,          // link
                std::intptr_t*, // result
                int,            // result_type
                void*,          // method
                void*,          // entry_point
                std::intptr_t*, // parameters
                int,            // size_of_parameters
                void*           // thread
                );

            std::intptr_t result_holder{ 0 };
            const vmhook::hotspot::java_thread_state previous_state{ thread->get_thread_state() };
            thread->set_thread_state(vmhook::hotspot::java_thread_state::_thread_in_Java);

            reinterpret_cast<call_stub_fn_t>(call_stub)(
                reinterpret_cast<void*>(static_cast<std::intptr_t>(-1)),
                &result_holder,
                result_type,
                reinterpret_cast<void*>(selected_method),
                entry,
                params,
                static_cast<int>(param_idx),
                reinterpret_cast<void*>(thread)
                );

            //  Decode result
            thread->set_thread_state(previous_state);

            switch (ret_char)
            {
            case 'Z': return value_t{ (result_holder & 1) != 0 };
            case 'B': return value_t{ static_cast<std::int8_t> (result_holder) };
            case 'S': return value_t{ static_cast<std::int16_t>(result_holder) };
            case 'I': return value_t{ static_cast<std::int32_t>(result_holder) };
            case 'J': return value_t{ static_cast<std::int64_t>(result_holder) };
            case 'C': return value_t{ static_cast<std::uint16_t>(result_holder) };
            case 'F':
            {
                float f{};
                const std::int32_t bits{ static_cast<std::int32_t>(result_holder) };
                std::memcpy(&f, &bits, sizeof(f));
                return value_t{ f };
            }
            case 'D':
            {
                double d{};
                std::memcpy(&d, &result_holder, sizeof(d));
                return value_t{ d };
            }
            case 'V': return value_t{ std::monostate{} };
            default:  return value_t{ static_cast<std::uint32_t>(result_holder) };
            }
        }

        /*
            @brief Returns the method name.
        */
        auto name() const noexcept
            -> std::string
        {
            if (!this->method)
            {
                return {};
            }
            return this->method->get_name();
        }

        /*
            @brief Returns the JVM method signature.
        */
        auto signature() const noexcept
            -> std::string_view
        {
            return this->signature_text;
        }

        /*
            @brief Returns true if this method is a static Java method.
            @details
            Static methods do not receive a 'this' pointer as the first argument.
            The object field of this proxy is null when the method is static.

            Complexity: O(1).
            Exception safety: noexcept.
        */
        auto is_static() const noexcept
            -> bool
        {
            return this->static_field;
        }

        /*
            @brief Returns the compressed OOP of the receiver object.
            @details
            Reads the first 4 bytes of the object pointer as a compressed OOP.
            Used internally when the proxy needs to pass the receiver to call().

            Complexity: O(1).
            Exception safety: noexcept — returns 0 if object is null.
        */
        auto get_compressed_oop() const noexcept
            -> std::uint32_t
        {
            if (!this->object)
            {
                return 0;
            }
            std::uint32_t value{};
            std::memcpy(&value, this->object, sizeof(value));
            return value;
        }

    private:
        /*
            @brief Reads the compressed Klass* from byte offset 8 of a HotSpot OOP header.
            @details
            On 64-bit HotSpot with UseCompressedClassPointers, oopDesc._metadata._compressed_klass
            is stored at a fixed offset of 8 bytes from the object start.  This function reads
            that slot and decodes it with decode_klass_pointer().

            Complexity: O(1).
            Exception safety: noexcept — returns nullptr on failure.

            @param oop  Decoded (full 64-bit) heap pointer to the Java object.
            @return  klass* for the runtime type of oop, or nullptr on failure.
        */
        static auto klass_from_object_header(void* const oop) noexcept
            -> vmhook::hotspot::klass*
        {
            if (!oop || !vmhook::hotspot::is_valid_pointer(oop))
            {
                return nullptr;
            }

            const std::uint32_t narrow_klass{ *reinterpret_cast<const std::uint32_t*>(
                reinterpret_cast<const std::uint8_t*>(oop) + 8) };
            void* const decoded{ vmhook::hotspot::decode_klass_pointer(narrow_klass) };
            if (!decoded || !vmhook::hotspot::is_valid_pointer(decoded))
            {
                return nullptr;
            }

            return reinterpret_cast<vmhook::hotspot::klass*>(decoded);
        }

        /*
            @brief Tests whether a C++ argument type is compatible with a JVM parameter descriptor.
            @details
            Compile-time dispatch based on argument_type:
              - string types         -> descriptor == "Ljava/lang/String;"
              - unique_ptr<T extends object_base> -> 'L' class descriptor matching T's registered name
              - object_base derived  -> same as unique_ptr case
              - bool                 -> "Z"
              - integral 1-byte      -> "B" or "Z"
              - integral 2-byte      -> "S" or "C"
              - integral 4-byte      -> "I"
              - integral 8-byte      -> "J"
              - float                -> "F"
              - double               -> "D"
              - other                -> false

            Used by signature_matches_arguments() to select the correct overloaded method.

            Complexity: O(1) — compile-time trait dispatch.
            Exception safety: noexcept.

            @tparam argument_type  The C++ argument type to test.
            @param descriptor      One JVM type descriptor token from a method signature.
            @return  true if argument_type is compatible with descriptor.
        */
        template<typename argument_type>
        static auto argument_matches_descriptor(const std::string_view descriptor) noexcept
            -> bool
        {
            using clean_type = std::remove_cvref_t<argument_type>;

            if constexpr (std::is_same_v<clean_type, std::string> || std::is_same_v<clean_type, std::string_view> || std::is_same_v<clean_type, const char*> || std::is_same_v<clean_type, char*>)
            {
                return descriptor == "Ljava/lang/String;";
            }
            else if constexpr (vmhook::detail::is_unique_ptr_v<clean_type>)
            {
                using wrapper_type = typename vmhook::detail::is_unique_ptr<clean_type>::value_type_t;
                if constexpr (std::is_base_of_v<vmhook::object_base, wrapper_type>)
                {
                    const auto type_map_entry{ vmhook::type_to_class_map.find(std::type_index{ typeid(wrapper_type) }) };
                    return descriptor.size() >= 3
                        && descriptor.front() == 'L'
                        && descriptor.back() == ';'
                        && (type_map_entry == vmhook::type_to_class_map.end() || descriptor.substr(1, descriptor.size() - 2) == type_map_entry->second);
                }
                else
                {
                    return false;
                }
            }
            else if constexpr (std::is_base_of_v<vmhook::object_base, clean_type>)
            {
                const auto type_map_entry{ vmhook::type_to_class_map.find(std::type_index{ typeid(clean_type) }) };
                return descriptor.size() >= 3
                    && descriptor.front() == 'L'
                    && descriptor.back() == ';'
                    && (type_map_entry == vmhook::type_to_class_map.end() || descriptor.substr(1, descriptor.size() - 2) == type_map_entry->second);
            }
            else if constexpr (std::is_same_v<clean_type, bool>)
            {
                return descriptor == "Z";
            }
            else if constexpr (std::is_integral_v<clean_type> && sizeof(clean_type) == 1)
            {
                return descriptor == "B" || descriptor == "Z";
            }
            else if constexpr (std::is_integral_v<clean_type> && sizeof(clean_type) == 2)
            {
                return descriptor == "S" || descriptor == "C";
            }
            else if constexpr (std::is_integral_v<clean_type> && sizeof(clean_type) == 4)
            {
                return descriptor == "I";
            }
            else if constexpr (std::is_integral_v<clean_type> && sizeof(clean_type) == 8)
            {
                return descriptor == "J";
            }
            else if constexpr (std::is_same_v<clean_type, float>)
            {
                return descriptor == "F";
            }
            else if constexpr (std::is_same_v<clean_type, double>)
            {
                return descriptor == "D";
            }
            else
            {
                return false;
            }
        }

        /*
            @brief Extracts the next descriptor token from a JVM method signature.
            @details
            Advances position past any leading array brackets, then past the full
            type descriptor (a single letter for primitives, or 'L...;' for objects).
            Returns the full descriptor including leading '[' array dimensions.
            Returns an empty string_view when the parse is complete or position reaches
            close_paren.

            Complexity: O(D) where D = descriptor token length.
            Exception safety: noexcept.

            @param signature    Full JVM method signature (e.g. "(ILjava/lang/String;)V").
            @param position     Current parse position; advanced in-place past the token.
            @param close_paren  Index of the closing ')' character.
            @return  The next descriptor token, or empty on end-of-parameters.
        */
        static auto next_argument_descriptor(const std::string_view signature, std::size_t& position, const std::size_t close_paren) noexcept
            -> std::string_view
        {
            const std::size_t start{ position };
            while (position < close_paren && signature[position] == '[')
            {
                ++position;
            }

            if (position >= close_paren)
            {
                return {};
            }

            if (signature[position] == 'L')
            {
                const std::size_t semicolon{ signature.find(';', position) };
                if (semicolon == std::string_view::npos || semicolon > close_paren)
                {
                    return {};
                }
                position = semicolon + 1;
                return signature.substr(start, position - start);
            }

            ++position;
            return signature.substr(start, position - start);
        }

        /*
            @brief Returns true if the parameter list of signature is compatible with args_t.
            @details
            Parses the JVM method signature from the opening '(' to the closing ')',
            calling next_argument_descriptor() + argument_matches_descriptor<arg>() for
            each argument in the pack.  Returns true only if all arguments match and there
            are no leftover descriptor tokens.

            Complexity: O(P) where P = number of parameters.
            Exception safety: noexcept.

            @tparam args_t  C++ argument types to test against the signature.
            @param signature  Full JVM method signature.
            @return  true if args_t are all compatible with signature's parameter list.
        */
        template<typename... args_t>
        static auto signature_matches_arguments(const std::string_view signature) noexcept
            -> bool
        {
            const std::size_t open_paren{ signature.find('(') };
            const std::size_t close_paren{ signature.find(')') };
            if (open_paren == std::string_view::npos || close_paren == std::string_view::npos || close_paren < open_paren)
            {
                return false;
            }

            std::size_t position{ open_paren + 1 };
            bool matches{ true };
            ([&]
                {
                    if (!matches)
                    {
                        return;
                    }
                    const std::string_view descriptor{ next_argument_descriptor(signature, position, close_paren) };
                    matches = !descriptor.empty() && argument_matches_descriptor<args_t>(descriptor);
                }(), ...);

            return matches && position == close_paren;
        }

        /*
            @brief Finds the Method* whose signature best matches the given C++ argument types.
            @details
            First tests whether this->signature_text already matches args_t using
            signature_matches_arguments().  If so, returns this->method directly.
            Otherwise walks the runtime klass hierarchy (obtained from the object header),
            and checks every method with the same name for a matching signature.  Returns
            the first match, or this->method as a fallback if nothing better is found.

            This allows a single get_method("foo") result to handle overloaded Java methods
            when the call site provides typed arguments.

            Complexity: O(N * M) where N = number of methods in the class hierarchy,
                        M = signature parse cost per method.
            Exception safety: noexcept — returns this->method on any failure.

            @tparam args_t  C++ argument types from the call<>() invocation.
            @return  The best-matching Method*, or this->method as fallback.
        */
        template<typename... args_t>
        auto resolve_compatible_method() const noexcept
            -> vmhook::hotspot::method*
        {
            if (signature_matches_arguments<args_t...>(this->signature_text))
            {
                return this->method;
            }

            vmhook::hotspot::klass* const resolved_klass{ this->object ? klass_from_object_header(this->object) : nullptr };
            if (!resolved_klass)
            {
                return this->method;
            }

            const std::string method_name{ this->name() };
            for (vmhook::hotspot::klass* k{ resolved_klass }; k != nullptr; k = k->get_super())
            {
                const std::int32_t method_count{ k->get_methods_count() };
                vmhook::hotspot::method** const methods_array{ k->get_methods_ptr() };
                if (!methods_array || method_count <= 0)
                {
                    continue;
                }

                for (std::int32_t method_index{ 0 }; method_index < method_count; ++method_index)
                {
                    vmhook::hotspot::method* const current_method{ methods_array[method_index] };
                    if (!current_method || !vmhook::hotspot::is_valid_pointer(current_method) || current_method->get_name() != method_name)
                    {
                        continue;
                    }

                    const std::string current_signature{ current_method->get_signature() };
                    if (signature_matches_arguments<args_t...>(current_signature))
                    {
                        return current_method;
                    }
                }
            }

            return this->method;
        }

        void* object;
        vmhook::hotspot::method* method;
        std::string signature_text;
        bool        static_field;
    };

    // --- Object base class ----------------------------------------------------

    /*
        @brief Alias for a decoded (uncompressed) Java object pointer.
        @details
        This is the type passed to vmhook::object constructors. Obtain it from a
        hook frame via frame->get_arguments<vmhook::oop_type_t>(), which internally calls
        vmhook::hotspot::decode_oop_pointer() to convert the 32-bit compressed OOP to a full
        64-bit address.  It is NOT a JNI global reference - no GC handles are
        created and the pointer remains valid only for the duration of the hook.
    */
    using oop_type_t = void*;

    /*
        @brief Short alias for oop_type_t; both names refer to a decoded Java heap pointer.
        @details
        Provided for brevity in user code:
            vmhook::hook<my_class>("tick",
                [](vmhook::return_value& ret, const std::unique_ptr<my_class>& self, vmhook::oop_t other) { ... });
    */
    using oop_t = oop_type_t;

    /*
        @brief Base class for C++ wrappers around live Java objects.
        @details
        Derive from this class to create a typed faade for a Java class.
        get_field() handles both instance and static fields automatically -
        the library reads JVM_ACC_STATIC from the InstanceKlass._fields array
        so there is no separate static-field accessor.

        Example:

            class http_client : public vmhook::object
            {
            public:
                explicit http_client(vmhook::oop_type_t instance)
                    : vmhook::object{ instance }
                {}

                // auto return - field_proxy::value_t converts implicitly at the call site
                auto is_connected() -> bool { return get_field("isConnected")->get(); }
                auto get_health()   -> int { return get_field("health")->get(); }

                // Cast inside the method if you want a concrete return type
                auto get_timeout()  -> int { return static_cast<int>(get_field("timeout")->get()); }

                // Static field - get_field resolves through the registered class.
                static auto get_version() -> std::string { return get_field("VERSION")->get(); }

                // Writing a field
                auto set_health(int hp) -> void { get_field("health")->set(hp); }
            };

            vmhook::register_class<http_client>("com/example/HttpClient");

            // Inside a hook detour:
            auto [self] = frame->get_arguments<vmhook::oop_type_t>();
            http_client client{ self };
            bool ok  = client.is_connected(); // operator bool()  fires
            int  hp  = client.get_health();   // operator int()   fires
            client.set_health(100);

        @note The wrapped pointer is a raw decoded OOP, not a JNI global reference.
              It is valid for the duration of the hook invocation only.
    */
    class object_base
    {
    public:
        /*
            @brief Wraps a decoded Java object pointer.
            @param instance Decoded OOP from frame->get_arguments<vmhook::oop_type_t>().
                            May be nullptr if the Java reference is null.
        */
        explicit object_base(oop_type_t instance = nullptr) noexcept
            : instance{ instance }
        {
        }

        virtual ~object_base() = default;

        /*
            @brief Copy constructor — copies the raw OOP pointer.
            @details
            Both the source and the copy point to the same Java object.
            The pointer is not a GC handle so no reference counting occurs.
        */
        object_base(const object_base&) = default;

        /*
            @brief Copy assignment — copies the raw OOP pointer.
        */
        auto operator=(const object_base&)
            -> object_base & = default;

        /*
            @brief Move constructor — transfers the OOP pointer and nulls the source.
            @details
            After the move, the source instance is set to nullptr so that
            the moved-from object is safely destructible.
        */
        object_base(object_base&& other) noexcept
            : instance{ other.instance }
        {
            other.instance = nullptr;
        }

        auto operator=(object_base&& other) noexcept
            -> object_base&
        {
            if (this != &other)
            {
                this->instance = other.instance;
                other.instance = nullptr;
            }
            return *this;
        }

        /*
            @brief Returns the raw decoded OOP pointer held by this wrapper.
        */
        auto get_instance() const noexcept
            -> oop_type_t
        {
            return this->instance;
        }

        /*
            @brief Returns a field proxy for any field declared on this class.
            @param name Exact Java field name.
            @return Optional holding the field proxy, or nullopt on failure.
            @details
            Works for both instance and static fields.  The JVM_ACC_STATIC flag is
            read from InstanceKlass._fields so no static/instance distinction is
            needed at the call site.

            Instance fields:  value lives at decoded_object_ptr + field_offset.
            Static fields:    value lives at java.lang.Class mirror + field_offset.

            The klass is resolved from type_to_class_map via typeid(*this) (dynamic type),
            so the derived C++ class must have been registered with register_class<T>()
            before this is called.  Field entries are cached after the first lookup.

            @note Dereferencing a nullopt with -> is undefined behaviour.
                  In production code always check the optional, or assert field names
                  are correct at development time.
        */
        auto get_field(const std::string_view name) const
            -> std::optional<vmhook::field_proxy>
        {
            vmhook::hotspot::klass* const resolved_klass{ this->resolve_klass() };
            if (!resolved_klass)
            {
                return std::nullopt;
            }

            const auto entry{ vmhook::find_field(resolved_klass, name) };
            if (!entry)
            {
                return std::nullopt;
            }

            if (entry->is_static)
            {
                void* const mirror{ resolved_klass->get_java_mirror() };
                if (!mirror || !vmhook::hotspot::is_valid_pointer(mirror))
                {
                    VMHOOK_LOG("{} object::get_field('{}') failed to get java.lang.Class mirror.", vmhook::error_tag, name);
                    return std::nullopt;
                }
                void* const field_pointer{ reinterpret_cast<std::uint8_t*>(mirror) + entry->offset };
                return vmhook::field_proxy{ field_pointer, entry->signature, true };
            }

            if (!this->instance)
            {
                VMHOOK_LOG("{} object::get_field('{}') instance pointer is null.", vmhook::error_tag, name);
                return std::nullopt;
            }

            void* const field_pointer{ reinterpret_cast<std::uint8_t*>(this->instance) + entry->offset };
            return vmhook::field_proxy{ field_pointer, entry->signature, false };
        }

        /*
            @brief Returns a field proxy for a static field identified by wrapper_type and name.
            @details
            Unlike the instance overload, this variant does not need a live OOP because
            static fields live on the java.lang.Class mirror rather than on an object.
            Fails with nullopt if the field is not static (caller should use the instance
            overload in that case).

            Complexity: O(F) on first call; O(1) after caching.
            Exception safety: may throw vmhook::exception internally but returns nullopt on failure.

            @param wrapper_type  std::type_index of the registered C++ wrapper class.
            @param name          Exact Java field name.
            @return  Optional field_proxy, or nullopt on failure.
        */
        static auto get_field(const std::type_index wrapper_type, const std::string_view name)
            -> std::optional<vmhook::field_proxy>
        {
            vmhook::hotspot::klass* const resolved_klass{ resolve_klass(wrapper_type) };
            if (!resolved_klass)
            {
                return std::nullopt;
            }

            const auto entry{ vmhook::find_field(resolved_klass, name) };
            if (!entry)
            {
                return std::nullopt;
            }

            if (!entry->is_static)
            {
                VMHOOK_LOG("{} object::get_field('{}') needs an object instance.", vmhook::error_tag, name);
                return std::nullopt;
            }

            void* const mirror{ resolved_klass->get_java_mirror() };
            if (!mirror || !vmhook::hotspot::is_valid_pointer(mirror))
            {
                VMHOOK_LOG("{} object::get_field('{}') failed to get java.lang.Class mirror.", vmhook::error_tag, name);
                return std::nullopt;
            }

            void* const field_pointer{ reinterpret_cast<std::uint8_t*>(mirror) + entry->offset };
            return vmhook::field_proxy{ field_pointer, entry->signature, true };
        }

        /*
            @brief Returns a method proxy for a method declared on this class.
            @param method_name Exact Java method name.
            @return Optional holding the method proxy, or nullopt on failure.
            @details
            Looks up the method in the InstanceKlass._methods array by name.
            The returned proxy can be used to call the method with arguments.

            Usage:
                auto say_hi() -> void { return get_method("sayHi")->call(); }
                auto take_values(int a, const std::string& b) -> void {
                    return get_method("takeValues")->call(a, b);
                }
        */
        auto get_method(const std::string_view method_name) const
            -> std::optional<vmhook::method_proxy>
        {
            vmhook::hotspot::klass* const resolved_klass{ this->resolve_klass() };
            if (!resolved_klass)
            {
                return std::nullopt;
            }

            // Walk the superclass chain so inherited methods are found.
            for (vmhook::hotspot::klass* k{ resolved_klass }; k != nullptr; k = k->get_super())
            {
                const std::int32_t method_count{ k->get_methods_count() };
                vmhook::hotspot::method** const methods_array{ k->get_methods_ptr() };

                if (!methods_array || method_count <= 0)
                {
                    continue;
                }

                for (std::int32_t method_index{ 0 }; method_index < method_count; ++method_index)
                {
                    vmhook::hotspot::method* const current_method{ methods_array[method_index] };
                    if (current_method && vmhook::hotspot::is_valid_pointer(current_method)
                        && current_method->get_name() == method_name)
                    {
                        return vmhook::method_proxy{ this->instance, current_method, current_method->get_signature() };
                    }
                }
            }

            return std::nullopt;
        }

        /*
            @brief Returns a method proxy for a method that matches both name and JVM signature.
            @details
            Walks the class hierarchy searching for a method whose name equals method_name
            and whose JVM descriptor equals method_signature.  Use this overload when the
            class has overloaded Java methods with the same name but different signatures.

            Complexity: O(N * M) where N = methods in the hierarchy, M = name comparison cost.
            Exception safety: does not throw; returns nullopt on failure.

            @param method_name       Exact Java method name.
            @param method_signature  JVM descriptor, e.g. "(I)Ljava/lang/String;".
            @return  Optional method_proxy, or nullopt if not found.
        */
        auto get_method(const std::string_view method_name, const std::string_view method_signature) const
            -> std::optional<vmhook::method_proxy>
        {
            vmhook::hotspot::klass* const resolved_klass{ this->resolve_klass() };
            if (!resolved_klass)
            {
                return std::nullopt;
            }

            // Walk the superclass chain so inherited methods are found.
            for (vmhook::hotspot::klass* k{ resolved_klass }; k != nullptr; k = k->get_super())
            {
                const std::int32_t method_count{ k->get_methods_count() };
                vmhook::hotspot::method** const methods_array{ k->get_methods_ptr() };

                if (!methods_array || method_count <= 0)
                {
                    continue;
                }

                for (std::int32_t method_index{ 0 }; method_index < method_count; ++method_index)
                {
                    vmhook::hotspot::method* const current_method{ methods_array[method_index] };
                    if (!current_method || !vmhook::hotspot::is_valid_pointer(current_method))
                    {
                        continue;
                    }

                    const std::string current_signature{ current_method->get_signature() };
                    if (current_method->get_name() == method_name && current_signature == method_signature)
                    {
                        return vmhook::method_proxy{ this->instance, current_method, current_signature };
                    }
                }
            }

            return std::nullopt;
        }

        /*
            @brief Returns a method proxy for a static method identified by wrapper_type and name.
            @details
            Like the static get_field() overload but for methods.  The returned proxy has
            a null object pointer so it cannot be used to call instance methods.

            Complexity: O(N * M) where N = methods in the hierarchy, M = name comparison cost.
            Exception safety: does not throw; returns nullopt on failure.

            @param wrapper_type  std::type_index of the registered C++ wrapper class.
            @param method_name   Exact Java method name.
            @return  Optional method_proxy, or nullopt if not found.
        */
        static auto get_method(const std::type_index wrapper_type, const std::string_view method_name)
            -> std::optional<vmhook::method_proxy>
        {
            vmhook::hotspot::klass* const resolved_klass{ resolve_klass(wrapper_type) };
            if (!resolved_klass)
            {
                return std::nullopt;
            }

            // Walk the superclass chain so inherited methods are found.
            for (vmhook::hotspot::klass* k{ resolved_klass }; k != nullptr; k = k->get_super())
            {
                const std::int32_t method_count{ k->get_methods_count() };
                vmhook::hotspot::method** const methods_array{ k->get_methods_ptr() };

                if (!methods_array || method_count <= 0)
                {
                    continue;
                }

                for (std::int32_t method_index{ 0 }; method_index < method_count; ++method_index)
                {
                    vmhook::hotspot::method* const current_method{ methods_array[method_index] };
                    if (current_method && vmhook::hotspot::is_valid_pointer(current_method)
                        && current_method->get_name() == method_name)
                    {
                        return vmhook::method_proxy{ nullptr, current_method, current_method->get_signature() };
                    }
                }
            }

            return std::nullopt;
        }

        /*
            @brief Returns a method proxy for a static method matching both name and JVM signature.
            @details
            Combines the type_index-based lookup from the static name-only overload with the
            signature-matching logic from the instance name+signature overload.  Use when the
            Java class has overloaded static methods.

            Complexity: O(N * M).
            Exception safety: does not throw; returns nullopt on failure.

            @param wrapper_type      std::type_index of the registered C++ wrapper class.
            @param method_name       Exact Java method name.
            @param method_signature  JVM descriptor, e.g. "(I)V".
            @return  Optional method_proxy, or nullopt if not found.
        */
        static auto get_method(const std::type_index wrapper_type, const std::string_view method_name, const std::string_view method_signature)
            -> std::optional<vmhook::method_proxy>
        {
            vmhook::hotspot::klass* const resolved_klass{ resolve_klass(wrapper_type) };
            if (!resolved_klass)
            {
                return std::nullopt;
            }

            // Walk the superclass chain so inherited methods are found.
            for (vmhook::hotspot::klass* k{ resolved_klass }; k != nullptr; k = k->get_super())
            {
                const std::int32_t method_count{ k->get_methods_count() };
                vmhook::hotspot::method** const methods_array{ k->get_methods_ptr() };

                if (!methods_array || method_count <= 0)
                {
                    continue;
                }

                for (std::int32_t method_index{ 0 }; method_index < method_count; ++method_index)
                {
                    vmhook::hotspot::method* const current_method{ methods_array[method_index] };
                    if (!current_method || !vmhook::hotspot::is_valid_pointer(current_method))
                    {
                        continue;
                    }

                    const std::string current_signature{ current_method->get_signature() };
                    if (current_method->get_name() == method_name && current_signature == method_signature)
                    {
                        return vmhook::method_proxy{ nullptr, current_method, current_signature };
                    }
                }
            }

            return std::nullopt;
        }

    protected:
        /*
            @brief The raw decoded OOP pointer to the wrapped Java object.
        */
        oop_type_t instance{ nullptr };

    private:
        /*
            @brief Resolves the HotSpot klass for the dynamic type of this object.
            @details
            Uses typeid(*this) to look up the class name in type_to_class_map, then
            calls find_class() which walks the ClassLoaderDataGraph (cached after
            the first call).
            @return Pointer to the klass, or nullptr if the type is not registered.
        */
        auto resolve_klass() const
            -> vmhook::hotspot::klass*
        {
            return resolve_klass(std::type_index{ typeid(*this) });
        }

        /*
            @brief Resolves the HotSpot klass for an arbitrary registered C++ wrapper type.
            @details
            Looks up wrapper_type in type_to_class_map and delegates to vmhook::find_class()
            which searches the ClassLoaderDataGraph (cached after the first call).
            Used by the static overloads of get_field() and get_method() that cannot use
            typeid(*this) because there is no object instance.

            Complexity: O(1) after the first call per type (cached in klass_lookup_cache).
            Exception safety: does not throw; returns nullptr and logs on failure.

            @param wrapper_type  std::type_index of the C++ wrapper class.
            @return  klass*, or nullptr if the type is not registered or the class is not found.
        */
        static auto resolve_klass(const std::type_index wrapper_type)
            -> vmhook::hotspot::klass*
        {
            const auto type_map_entry{ vmhook::type_to_class_map.find(wrapper_type) };
            if (type_map_entry == vmhook::type_to_class_map.end())
            {
                VMHOOK_LOG("{} object::resolve_klass() type '{}' not registered via register_class<T>().", vmhook::error_tag, wrapper_type.name());
                return nullptr;
            }

            vmhook::hotspot::klass* const found_klass{ vmhook::find_class(type_map_entry->second) };
            if (!found_klass)
            {
                VMHOOK_LOG("{} object::resolve_klass() class '{}' not found in JVM.", vmhook::error_tag, type_map_entry->second);
            }

            return found_klass;
        }
    };

    /*
        @brief CRTP base class for typed Java-object wrappers.
        @details
        Derive from vmhook::object<YourClass> (not object_base directly).

        Field / method access syntax:

          - Inside an instance C++ method, `get_field("name")` /
            `get_method("name")` route through the implicit `this` and
            resolve via the live OOP.  These are inherited from
            object_base.

          - Inside a static C++ method, the same `get_field("name")` /
            `get_method("name")` call resolves through the C++23
            deducing-this overloads below on MSVC and Clang.  These
            compilers correctly exclude deducing-this overloads from
            overload resolution when no implicit object is available,
            so the static fallbacks defined here win automatically.

          - On GCC the deducing-this overloads are still selected from
            a static-call context (the compiler picks the better
            argument-type match before checking object availability,
            then errors), so static methods on GCC must call
            `static_field("name")` / `static_method("name")` instead.
            The same names are available on every compiler for
            authors who want a uniformly portable call site.

        Usage:
            class my_entity : public vmhook::object<my_entity>
            {
            public:
                explicit my_entity(vmhook::oop_t oop) : vmhook::object<my_entity>{ oop } {}

                auto get_health()       -> int { return get_field("health")->get(); }

                // Portable static accessor (works on MSVC, Clang, GCC):
                static auto get_count() -> int { return static_field("entityCount")->get(); }

                // MSVC/Clang only — deducing-this resolves to the static fallback:
                // static auto get_count() -> int { return get_field("entityCount")->get(); }
            };
    */
    template<typename derived>
    class object : public object_base
    {
    public:
        using object_base::object_base;

        // -------------------------------------------------------------------
        // Deducing-this overloads (compiled only on toolchains that support
        // C++23 explicit-object parameters: MSVC 19.32+, GCC 14+, Clang 18+).
        //
        // These exist so that a call like `get_field("staticName")` made
        // from a static C++ method *of the wrapper class* dispatches to the
        // static fallback below:  the deducing-this overloads are non-viable
        // in a static-call context (no implicit object), so on MSVC and
        // Clang overload resolution falls through to the string_view static
        // `get_field`.  From an instance method they are an exact match for
        // string literals and outrank the string_view static.
        //
        // On GCC the deducing-this overloads are still considered for
        // static-call overload resolution and produce a compile error.  We
        // still emit them because instance-context calls need a non-static
        // overload (otherwise the same-named static in this class would
        // hide the inherited `object_base::get_field`).  Users targeting
        // GCC should call `static_field("name")` explicitly from static
        // methods; that name is always available below.
        // -------------------------------------------------------------------
#if VMHOOK_HAS_DEDUCING_THIS
        auto get_field(this object_base const& self, char const* name)
            -> std::optional<vmhook::field_proxy>
        {
            return self.object_base::get_field(name);
        }

        auto get_method(this object_base const& self, char const* name)
            -> std::optional<vmhook::method_proxy>
        {
            return self.object_base::get_method(name);
        }

        auto get_method(this object_base const& self, char const* name, char const* signature)
            -> std::optional<vmhook::method_proxy>
        {
            return self.object_base::get_method(name, signature);
        }
#else
        // Pre-C++23 fallback: forward via the inherited non-static overloads.
        // Brought in with using-declarations.  These cover instance-context
        // get_field("name") / get_method("name") on compilers that don't
        // support deducing-this.  The same-name static overloads further
        // below add the type_index-based static-field lookup; both are in
        // scope, and overload resolution distinguishes them via the
        // implicit object parameter.
        using object_base::get_field;
        using object_base::get_method;
#endif

        // -------------------------------------------------------------------
        // Static-context fallbacks for `get_field` / `get_method`.
        //
        // Only emitted when deducing-this is available; on older toolchains
        // a same-name same-signature static overload would hide the inherited
        // non-static brought in via the `using` directives above, which would
        // break instance-context access.  On those compilers users still
        // call `static_field` / `static_method` (always available below).
        // -------------------------------------------------------------------
#if VMHOOK_HAS_DEDUCING_THIS
        static auto get_field(std::string_view name)
            -> std::optional<vmhook::field_proxy>
        {
            return object_base::get_field(std::type_index{ typeid(derived) }, name);
        }

        static auto get_method(std::string_view name)
            -> std::optional<vmhook::method_proxy>
        {
            return object_base::get_method(std::type_index{ typeid(derived) }, name);
        }

        static auto get_method(std::string_view name, std::string_view signature)
            -> std::optional<vmhook::method_proxy>
        {
            return object_base::get_method(std::type_index{ typeid(derived) }, name, signature);
        }
#endif

        /*
            @brief Explicit static field accessor.  Portable across compilers.
        */
        static auto static_field(std::string_view name)
            -> std::optional<vmhook::field_proxy>
        {
            return object_base::get_field(std::type_index{ typeid(derived) }, name);
        }

        /*
            @brief Explicit static method accessor.  Portable across compilers.
        */
        static auto static_method(std::string_view name)
            -> std::optional<vmhook::method_proxy>
        {
            return object_base::get_method(std::type_index{ typeid(derived) }, name);
        }

        /*
            @brief Explicit static method accessor with JVM descriptor.
        */
        static auto static_method(std::string_view name, std::string_view signature)
            -> std::optional<vmhook::method_proxy>
        {
            return object_base::get_method(std::type_index{ typeid(derived) }, name, signature);
        }
    };

    // --- Built-in Java collection wrappers ------------------------------------

    /*
        @brief Resolves the HotSpot klass* directly from a decoded OOP header.

        On x64 HotSpot (compressed class pointers enabled, the default):
          offset 0  : mark word (8 bytes)
          offset 8  : narrow klass (uint32_t, 4 bytes)
        The narrow klass is decoded the same way as regular narrow oops but using
        the klass base/shift instead of the oop base/shift.

        Returns nullptr if the pointer is invalid or decoding fails.
    */
    inline auto klass_from_oop(void* const oop) noexcept -> vmhook::hotspot::klass*
    {
        if (!oop || !vmhook::hotspot::is_valid_pointer(oop))
        {
            return nullptr;
        }
        const std::uint32_t narrow{ *reinterpret_cast<const std::uint32_t*>(
            reinterpret_cast<const std::uint8_t*>(oop) + 8) };
        void* const decoded{ vmhook::hotspot::decode_klass_pointer(narrow) };
        if (!decoded || !vmhook::hotspot::is_valid_pointer(decoded))
        {
            return nullptr;
        }
        return reinterpret_cast<vmhook::hotspot::klass*>(decoded);
    }

    /*
        @brief C++ wrapper for java.util.Collection objects.

        Uses the live OOP's klass pointer (read from its object header) to
        resolve fields and methods, so no register_class<T>() call is needed
        and it works with any concrete Collection implementation
        (ArrayList, LinkedList, HashSet, etc.).

        Typical usage (from a hook detour where you received a List OOP):

            collection col{ list_oop };
            std::int32_t n = col.size();
    */
    class collection : public vmhook::object_base
    {
    public:
        /*
            @brief Wraps a decoded OOP that refers to any java.util.Collection implementation.
            @details
            The oop must be a fully decoded 64-bit heap pointer (not a compressed OOP).
            The constructor does not validate the pointer; pass nullptr for a null-safe
            collection that returns empty/zero for all operations.

            Exception safety: noexcept.
            @param oop  Decoded OOP of the Java Collection object, or nullptr.
        */
        explicit collection(vmhook::oop_t oop) noexcept
            : vmhook::object_base{ oop }
        {
        }

    protected:
        /*
            @brief Returns the klass by reading the narrow klass slot in the OOP header.
            Falls back to nullptr if the OOP is invalid.
        */
        auto oop_klass() const noexcept -> vmhook::hotspot::klass*
        {
            return vmhook::klass_from_oop(this->instance);
        }

        /*
            @brief get_field variant that uses the live OOP's klass, not the C++ type registry.
        */
        auto get_field_by_oop_klass(const std::string_view name) const
            -> std::optional<vmhook::field_proxy>
        {
            vmhook::hotspot::klass* const k{ oop_klass() };
            if (!k)
            {
                return std::nullopt;
            }
            const auto entry{ vmhook::find_field(k, name) };
            if (!entry)
            {
                return std::nullopt;
            }
            if (entry->is_static)
            {
                void* const mirror{ k->get_java_mirror() };
                if (!mirror || !vmhook::hotspot::is_valid_pointer(mirror))
                {
                    return std::nullopt;
                }
                void* const field_pointer{ reinterpret_cast<std::uint8_t*>(mirror) + entry->offset };
                return vmhook::field_proxy{ field_pointer, entry->signature, true };
            }
            if (!this->instance)
            {
                return std::nullopt;
            }
            void* const field_pointer{ reinterpret_cast<std::uint8_t*>(this->instance) + entry->offset };
            return vmhook::field_proxy{ field_pointer, entry->signature, false };
        }

        /*
            @brief get_method variant that searches the live OOP's klass hierarchy.
        */
        auto get_method_by_oop_klass(const std::string_view method_name) const
            -> std::optional<vmhook::method_proxy>
        {
            for (vmhook::hotspot::klass* k{ oop_klass() }; k != nullptr; k = k->get_super())
            {
                const std::int32_t method_count{ k->get_methods_count() };
                vmhook::hotspot::method** const methods_array{ k->get_methods_ptr() };
                if (!methods_array || method_count <= 0)
                {
                    continue;
                }
                for (std::int32_t i{ 0 }; i < method_count; ++i)
                {
                    vmhook::hotspot::method* const m{ methods_array[i] };
                    if (m && vmhook::hotspot::is_valid_pointer(m) && m->get_name() == method_name)
                    {
                        return vmhook::method_proxy{ this->instance, m, m->get_signature() };
                    }
                }
            }
            return std::nullopt;
        }

    public:
        /*
            @brief Returns the number of elements via the Java size() method.
            Resolves through the concrete class's virtual dispatch table,
            so it works for ArrayList, LinkedList, HashSet, etc.
        */
        auto size() const noexcept -> std::int32_t
        {
            const auto proxy{ get_method_by_oop_klass("size") };
            if (!proxy)
            {
                return 0;
            }
            return proxy->call();
        }

        /*
            @brief Returns true if the collection contains no elements.
            @details
            Delegates to size(); returns true when size() == 0.

            Complexity: O(1) if size() is O(1) for the underlying implementation.
            Exception safety: noexcept.
        */
        auto is_empty() const noexcept -> bool
        {
            return size() == 0;
        }

        /*
            @brief Converts this java.util.Collection to a std::vector<std::unique_ptr<element_type>>.
            @details
            Attempts the ArrayList fast path first: reads the "size" and "elementData" fields
            directly from heap memory and decodes each compressed OOP element.
            Falls back to calling the Java get(int) method for other implementations.
            Null Java elements become nullptr entries in the returned vector.

            Complexity: O(N) where N = collection size.
            Exception safety: does not throw; returns an empty vector on any failure.

            @tparam element_type  C++ wrapper whose constructor accepts vmhook::oop_t.
            @return  Vector of unique_ptr<element_type>; nullptr slots for null elements.
        */
        template<typename element_type>
        auto to_vector() const -> std::vector<std::unique_ptr<element_type>>
        {
            std::vector<std::unique_ptr<element_type>> result;

            if (!instance || !vmhook::hotspot::is_valid_pointer(instance))
            {
                return result;
            }

            const auto size_opt{ get_field_by_oop_klass("size") };
            const auto data_opt{ get_field_by_oop_klass("elementData") };

            if (size_opt && data_opt)
            {
                const std::int32_t n{ size_opt->get() };
                if (n <= 0)
                {
                    return result;
                }

                const std::uint32_t compressed_array{ static_cast<std::uint32_t>(data_opt->get()) };
                void* const array_oop{ vmhook::decode_array_oop(compressed_array) };
                if (array_oop && vmhook::hotspot::is_valid_pointer(array_oop))
                {
                    result.reserve(static_cast<std::size_t>(n));
                    for (std::int32_t index{ 0 }; index < n; ++index)
                    {
                        const std::uint32_t compressed_element{ vmhook::get_array_element<std::uint32_t>(array_oop, index) };
                        void* const element_oop{ vmhook::hotspot::decode_oop_pointer(compressed_element) };
                        if (element_oop && vmhook::hotspot::is_valid_pointer(element_oop))
                        {
                            result.push_back(std::make_unique<element_type>(static_cast<vmhook::oop_t>(element_oop)));
                        }
                        else
                        {
                            result.push_back(nullptr);
                        }
                    }
                    return result;
                }
            }

            const std::int32_t n{ size() };
            if (n <= 0)
            {
                return result;
            }

            const auto get_method_opt{ get_method_by_oop_klass("get") };
            if (!get_method_opt)
            {
                return result;
            }

            result.reserve(static_cast<std::size_t>(n));
            for (std::int32_t index{ 0 }; index < n; ++index)
            {
                const auto element_value{ get_method_opt->call<std::uint32_t>(index) };
                void* const element_oop{ vmhook::hotspot::decode_oop_pointer(element_value) };
                if (element_oop && vmhook::hotspot::is_valid_pointer(element_oop))
                {
                    result.push_back(std::make_unique<element_type>(static_cast<vmhook::oop_t>(element_oop)));
                }
                else
                {
                    result.push_back(nullptr);
                }
            }

            return result;
        }
    };

    /*
        @brief C++ wrapper for java.util.List objects.

        Provides to_vector<T>() which reads an ArrayList's backing array
        directly from the JVM heap and returns a std::vector of unique_ptr<T>.

        Usage:

            // Java: public List<A> listOfAs;
            auto vec = get_field("listOfAs")->get<std::unique_ptr<vmhook::list>>()
                           ->to_vector<a_class>();
            // vec is std::vector<std::unique_ptr<a_class>>
    */
    class list : public vmhook::collection
    {
    public:
        /*
            @brief Wraps a decoded OOP that refers to a java.util.List implementation.
            @details
            Inherits the collection(oop) constructor.  Adds a List-specific to_vector<T>()
            that uses the ArrayList backing array fast path when available.

            Exception safety: noexcept.
            @param oop  Decoded OOP of the Java List object, or nullptr.
        */
        explicit list(vmhook::oop_t oop) noexcept
            : vmhook::collection{ oop }
        {
        }

        /*
            @brief Converts this Java List to a std::vector<std::unique_ptr<T>>.
            @tparam element_type  C++ wrapper class whose constructor accepts a
                                  vmhook::oop_t (decoded Java object pointer).

            Reads the ArrayList backing array directly from the JVM heap.
            Null Java elements become nullptr entries in the returned vector.
            Works on all Java versions supported by vmhook (824+).

            For non-ArrayList implementations the method falls back to calling
            the Java get(int) method for each index.
        */
        template<typename element_type>
        auto to_vector() const -> std::vector<std::unique_ptr<element_type>>
        {
            std::vector<std::unique_ptr<element_type>> result;

            if (!instance || !vmhook::hotspot::is_valid_pointer(instance))
            {
                return result;
            }

            //  ArrayList fast path
            // ArrayList stores its live element count in a field named "size"
            // and its backing Object[] in a field named "elementData".
            const auto size_opt{ get_field_by_oop_klass("size") };
            const auto data_opt{ get_field_by_oop_klass("elementData") };

            if (size_opt && data_opt)
            {
                const std::int32_t n{ size_opt->get() };
                if (n > 0)
                {
                    // elementData is stored as a compressed OOP (uint32_t)
                    const std::uint32_t compressed_array{
                        static_cast<std::uint32_t>(data_opt->get()) };
                    void* const array_oop{ vmhook::decode_array_oop(compressed_array) };

                    if (array_oop && vmhook::hotspot::is_valid_pointer(array_oop))
                    {
                        result.reserve(static_cast<std::size_t>(n));
                        for (std::int32_t i{ 0 }; i < n; ++i)
                        {
                            const std::uint32_t compressed_elem{
                                vmhook::get_array_element<std::uint32_t>(array_oop, i) };
                            void* const elem_oop{
                                vmhook::hotspot::decode_oop_pointer(compressed_elem) };

                            if (elem_oop && vmhook::hotspot::is_valid_pointer(elem_oop))
                            {
                                result.push_back(std::make_unique<element_type>(
                                    static_cast<vmhook::oop_t>(elem_oop)));
                            }
                            else
                            {
                                result.push_back(nullptr);
                            }
                        }
                        return result;
                    }
                }
                else
                {
                    return result;  // empty list
                }
            }

            //  Generic fallback: call Java List.get(int)
            const std::int32_t n{ size() };
            if (n <= 0)
            {
                return result;
            }
            result.reserve(static_cast<std::size_t>(n));

            const auto get_method_opt{ get_method_by_oop_klass("get") };
            if (!get_method_opt)
            {
                return result;
            }

            for (std::int32_t i{ 0 }; i < n; ++i)
            {
                const auto elem_val{ get_method_opt->call<std::uint32_t>(i) };
                void* const elem_oop{ vmhook::hotspot::decode_oop_pointer(elem_val) };
                if (elem_oop && vmhook::hotspot::is_valid_pointer(elem_oop))
                {
                    result.push_back(std::make_unique<element_type>(
                        static_cast<vmhook::oop_t>(elem_oop)));
                }
                else
                {
                    result.push_back(nullptr);
                }
            }
            return result;
        }
    };

    /*
        @brief Out-of-line definition of field_proxy::value_t::to_vector<element_type>().
        @details
        Reads the stored compressed OOP, decodes it with decode_oop_pointer(), wraps the
        result in a vmhook::collection, and delegates to collection::to_vector<element_type>().
        Defined here (after collection is complete) because collection is an incomplete type
        at the point where value_t is declared.

        Complexity: O(N) where N = collection size.
        Exception safety: does not throw; returns an empty vector on failure.

        @tparam element_type  C++ wrapper whose constructor accepts vmhook::oop_t.
        @return  Vector of unique_ptr<element_type>.
    */
    template<typename element_type>
    auto field_proxy::value_t::to_vector() const
        -> std::vector<std::unique_ptr<element_type>>
    {
        const std::uint32_t compressed_collection{ static_cast<std::uint32_t>(*this) };
        void* const collection_oop{ vmhook::hotspot::decode_oop_pointer(compressed_collection) };
        if (!collection_oop || !vmhook::hotspot::is_valid_pointer(collection_oop))
        {
            return {};
        }

        return vmhook::collection{ collection_oop }.to_vector<element_type>();
    }

    // --- Helper: read a Java String OOP to std::string ------------------------

    /*
        @brief Decodes a Java String object into a std::string.
        @param string_oop  A decoded OOP pointing to a java.lang.String instance.
        @return A std::string containing the string contents, or empty on failure.
        @details
        Handles both pre-Java-9 (char[] value) and Java-9+ (byte[] value + coder) layouts.
        Truncates strings longer than 4096 characters as a sanity check.
    */
    inline auto read_java_string(void* const string_oop)
        -> std::string
    {
        if (!string_oop || !vmhook::hotspot::is_valid_pointer(string_oop))
        {
            return {};
        }

        vmhook::hotspot::klass* const string_klass{ vmhook::find_class("java/lang/String") };
        if (!string_klass)
        {
            return {};
        }

        const std::uint32_t arr_compressed{ vmhook::get_field<std::uint32_t>(string_oop, string_klass, "value") };
        if (!arr_compressed)
        {
            return {};
        }

        void* const arr_oop{ vmhook::hotspot::decode_oop_pointer(arr_compressed) };
        if (!arr_oop || !vmhook::hotspot::is_valid_pointer(arr_oop))
        {
            return {};
        }

        const auto* const arr{ reinterpret_cast<const std::uint8_t*>(arr_oop) };
        const std::int32_t length{ *reinterpret_cast<const std::int32_t*>(arr + 12) };
        if (length <= 0 || length > 4096)
        {
            return {};
        }

        const std::uint8_t* const data{ arr + 16 };
        const bool has_coder{ string_klass->find_field("coder").has_value() };

        std::string result;
        if (!has_coder)
        {
            const auto* const chars{ reinterpret_cast<const std::uint16_t*>(data) };
            result.reserve(static_cast<std::size_t>(length));
            for (std::int32_t i{ 0 }; i < length; ++i)
            {
                result += (chars[i] < 0x80) ? static_cast<char>(chars[i]) : '?';
            }
        }
        else
        {
            const std::uint8_t coder{ vmhook::get_field<std::uint8_t>(string_oop, string_klass, "coder") };
            if (coder == 0)
            {
                result.assign(reinterpret_cast<const char*>(data), static_cast<std::size_t>(length));
            }
            else
            {
                const auto* const chars{ reinterpret_cast<const std::uint16_t*>(data) };
                const std::int32_t char_count{ length / 2 };
                result.reserve(static_cast<std::size_t>(char_count));
                for (std::int32_t i{ 0 }; i < char_count; ++i)
                {
                    result += (chars[i] < 0x80) ? static_cast<char>(chars[i]) : '?';
                }
            }
        }
        return result;
    }

    /*
        @brief Returns the decoded array or object OOP from a field_proxy.
        @details
        Reads the compressed OOP stored in field via get_compressed_oop() and decodes
        it using decode_array_oop().  Convenience wrapper used by set_str_field(),
        set_bool_array(), set_prim_array(), and set_str_array().

        Complexity: O(1).
        Exception safety: noexcept — returns nullptr if the field OOP is null or invalid.

        @param field  A field_proxy whose underlying field holds a reference or array OOP.
        @return  Decoded 64-bit heap pointer to the array/object, or nullptr on failure.
    */
    inline auto field_oop(const vmhook::field_proxy& field) noexcept
        -> void*
    {
        return vmhook::decode_array_oop(field.get_compressed_oop());
    }

    /*
        @brief Overwrites the contents of an existing java.lang.String in-place.
        @details
        Reads the backing array OOP from the String's "value" field, then writes
        value's bytes into the backing array element-by-element:
          - char[] (JDK 8):  writes uint16 per character (narrow to 0x00FF).
          - byte[] (JDK 9+): writes uint8 per character (LATIN1 coder assumed).
        Writes only up to min(existing_length, value.length()) characters.
        Does NOT change the array length; value is silently truncated if longer
        than the existing backing array.

        Complexity: O(N) where N = written character count.
        Exception safety: noexcept — returns on any failure.

        @param string_oop  Decoded OOP of a live java.lang.String instance.
        @param value       New content to write; truncated to the existing array length.
    */
    inline auto write_java_string(void* const string_oop, const std::string_view value) noexcept
        -> void
    {
        if (!string_oop || !vmhook::hotspot::is_valid_pointer(string_oop))
        {
            return;
        }

        vmhook::hotspot::klass* const string_klass{ vmhook::find_class("java/lang/String") };
        if (!string_klass)
        {
            return;
        }

        const auto value_field{ vmhook::find_field(string_klass, "value") };
        if (!value_field)
        {
            return;
        }

        std::uint32_t compressed{};
        std::memcpy(&compressed, reinterpret_cast<const std::uint8_t*>(string_oop) + value_field->offset, sizeof(compressed));
        void* const array_oop{ vmhook::decode_array_oop(compressed) };
        if (!array_oop)
        {
            return;
        }

        const std::int32_t length{ vmhook::array_length(array_oop) };
        const std::int32_t writable_length{ (std::min)(length, static_cast<std::int32_t>(value.size())) };
        if (writable_length <= 0)
        {
            return;
        }

        if (value_field->signature == "[C")
        {
            for (std::int32_t index{ 0 }; index < writable_length; ++index)
            {
                vmhook::set_array_element<std::uint16_t>(array_oop, index, static_cast<std::uint16_t>(static_cast<unsigned char>(value[static_cast<std::size_t>(index)])));
            }
        }
        else
        {
            for (std::int32_t index{ 0 }; index < writable_length; ++index)
            {
                vmhook::set_array_element<std::uint8_t>(array_oop, index, static_cast<std::uint8_t>(value[static_cast<std::size_t>(index)]));
            }
        }
    }

    /*
        @brief Overwrites a String field's backing array content in-place.
        @details
        Decodes the field's OOP to obtain the backing java.lang.String object, then
        delegates to write_java_string().  Convenience wrapper for the common pattern
        of mutating a String-typed instance field without replacing the String reference.

        Complexity: O(N) where N = written character count.
        Exception safety: noexcept — returns silently on failure.

        @param field  A field_proxy whose underlying field is a java.lang.String reference.
        @param value  New string content; truncated to the existing array length.
    */
    inline auto set_str_field(const vmhook::field_proxy& field, const std::string_view value) noexcept
        -> void
    {
        vmhook::write_java_string(vmhook::field_oop(field), value);
    }

    /*
        @brief Writes a std::vector<bool> into a Java boolean[] field.
        @details
        Decodes the field's array OOP via field_oop(), then writes each element
        as uint8 (0 = false, 1 = true) into the backing boolean[] array.
        Writes only min(array_length, values.size()) elements.

        Complexity: O(N) where N = written element count.
        Exception safety: noexcept — returns silently if the OOP is null.

        @param field   A field_proxy whose field holds a boolean[] reference.
        @param values  Boolean values to write into the array.
    */
    inline auto set_bool_array(const vmhook::field_proxy& field, const std::vector<bool>& values) noexcept
        -> void
    {
        void* const array_oop{ vmhook::field_oop(field) };
        if (!array_oop)
        {
            return;
        }

        const std::int32_t length{ (std::min)(vmhook::array_length(array_oop), static_cast<std::int32_t>(values.size())) };
        for (std::int32_t index{ 0 }; index < length; ++index)
        {
            vmhook::set_array_element<std::uint8_t>(array_oop, index, values[static_cast<std::size_t>(index)] ? 1u : 0u);
        }
    }

    /*
        @brief Writes a std::vector<element_type> into a Java primitive array field.
        @details
        Decodes the field's array OOP, then writes each element into the backing Java
        primitive array.  Special-cases char/uint16 conversion for "[C" (Java char[])
        fields: narrows each char to uint16 to avoid sign-extension issues.
        Writes only min(array_length, values.size()) elements.

        Complexity: O(N) where N = written element count.
        Exception safety: noexcept — returns silently if the OOP is null.

        @tparam element_type  Trivially copyable C++ element type (int, float, etc.).
        @param field   A field_proxy whose field holds a primitive array reference.
        @param values  Values to write into the array.
    */
    template<typename element_type>
    inline auto set_prim_array(const vmhook::field_proxy& field, const std::vector<element_type>& values) noexcept
        -> void
    {
        void* const array_oop{ vmhook::field_oop(field) };
        if (!array_oop)
        {
            return;
        }

        const std::int32_t length{ (std::min)(vmhook::array_length(array_oop), static_cast<std::int32_t>(values.size())) };
        for (std::int32_t index{ 0 }; index < length; ++index)
        {
            const element_type& value{ values[static_cast<std::size_t>(index)] };
            if constexpr (std::is_same_v<element_type, char>)
            {
                if (field.signature() == "[C")
                {
                    vmhook::set_array_element<std::uint16_t>(array_oop, index, static_cast<std::uint16_t>(static_cast<unsigned char>(value)));
                }
                else
                {
                    vmhook::set_array_element<char>(array_oop, index, value);
                }
            }
            else
            {
                vmhook::set_array_element<element_type>(array_oop, index, value);
            }
        }
    }

    /*
        @brief Writes a std::vector<std::string> into a Java String[] field in-place.
        @details
        Decodes the field's array OOP, then for each slot reads the existing compressed
        OOP (the java.lang.String* in the array), decodes it, and overwrites the backing
        char[]/byte[] via write_java_string().  Does NOT replace the String references in
        the array; it mutates the content of each pre-existing String object.
        Writes only min(array_length, values.size()) elements.

        Complexity: O(N * S) where N = element count, S = average string length.
        Exception safety: noexcept — returns silently if the OOP is null.

        @param field   A field_proxy whose field holds a String[] reference.
        @param values  New string contents for each slot.
    */
    inline auto set_str_array(const vmhook::field_proxy& field, const std::vector<std::string>& values) noexcept
        -> void
    {
        void* const array_oop{ vmhook::field_oop(field) };
        if (!array_oop)
        {
            return;
        }

        const std::int32_t length{ (std::min)(vmhook::array_length(array_oop), static_cast<std::int32_t>(values.size())) };
        for (std::int32_t index{ 0 }; index < length; ++index)
        {
            const std::uint32_t compressed{ vmhook::get_array_element<std::uint32_t>(array_oop, index) };
            vmhook::write_java_string(vmhook::hotspot::decode_oop_pointer(compressed), values[static_cast<std::size_t>(index)]);
        }
    }

    // --- Helper: decode compressed array OOP ---------------------------------

    /*
        @brief Decodes a 32-bit compressed array OOP into a raw pointer.
        @param compressed  A 32-bit compressed OOP value.
        @return A valid pointer if decoding succeeded, nullptr otherwise.
    */
    auto decode_array_oop(const std::uint32_t compressed)
        -> void*
    {
        if (!compressed)
        {
            return nullptr;
        }
        void* const decoded_pointer{ vmhook::hotspot::decode_oop_pointer(compressed) };
        return (decoded_pointer && vmhook::hotspot::is_valid_pointer(decoded_pointer)) ? decoded_pointer : nullptr;
    }

    // -------------------------------------------------------------------------
    // Background watchers (definitions).  Declared above as part of the
    // public API; defined here so the lambda bodies see the complete
    // `vmhook::object_base` type.
    // -------------------------------------------------------------------------

    /*
        @brief Registry that owns hardware-data-breakpoint state for the
               trap-based watch_static_field path.  At most four
               simultaneous watches per process (the CPU exposes DR0-DR3).
    */
#if VMHOOK_HAS_HW_DATA_BREAKPOINTS
    namespace detail
    {
        struct dr_slot
        {
            void*                                                    address{ nullptr };
            std::function<void(const void*)>                         callback{};
            std::uint64_t                                            dr7_bits{ 0 };
            std::atomic_bool                                         in_use{ false };
        };

        inline std::mutex   dr_mutex{};
        inline dr_slot      dr_slots[4]{};
        inline PVOID        dr_veh_handle{ nullptr };

        inline auto find_free_slot() -> int
        {
            for (int i{ 0 }; i < 4; ++i)
            {
                if (!dr_slots[i].in_use.load(std::memory_order_acquire))
                {
                    return i;
                }
            }
            return -1;
        }

        inline auto refresh_thread_drs(int slot, std::uint64_t address, std::uint64_t dr7_bits) -> void
        {
            vmhook::os::detail_dr::for_each_thread([&](HANDLE thread)
            {
                CONTEXT ctx{};
                ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
                if (::GetThreadContext(thread, &ctx))
                {
                    switch (slot)
                    {
                        case 0: ctx.Dr0 = address; break;
                        case 1: ctx.Dr1 = address; break;
                        case 2: ctx.Dr2 = address; break;
                        case 3: ctx.Dr3 = address; break;
                        default: return;
                    }
                    // Set Dr7 with our slot's bits or'd in (don't clobber
                    // other slots that may be configured).
                    const std::uint64_t slot_mask_local{ std::uint64_t{ 0b11 } << (slot * 2) };
                    const std::uint64_t slot_mask_rwlen{ std::uint64_t{ 0xF }  << (16 + slot * 4) };
                    const std::uint64_t mask{ slot_mask_local | slot_mask_rwlen };
                    ctx.Dr7 = (ctx.Dr7 & ~mask) | (dr7_bits & mask);
                    ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
                    ::SetThreadContext(thread, &ctx);
                }
            });
        }

        inline auto clear_thread_drs(int slot) -> void
        {
            vmhook::os::detail_dr::for_each_thread([&](HANDLE thread)
            {
                CONTEXT ctx{};
                ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
                if (::GetThreadContext(thread, &ctx))
                {
                    switch (slot)
                    {
                        case 0: ctx.Dr0 = 0; break;
                        case 1: ctx.Dr1 = 0; break;
                        case 2: ctx.Dr2 = 0; break;
                        case 3: ctx.Dr3 = 0; break;
                        default: return;
                    }
                    const std::uint64_t slot_mask_local{ std::uint64_t{ 0b11 } << (slot * 2) };
                    const std::uint64_t slot_mask_rwlen{ std::uint64_t{ 0xF }  << (16 + slot * 4) };
                    ctx.Dr7 &= ~(slot_mask_local | slot_mask_rwlen);
                    ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
                    ::SetThreadContext(thread, &ctx);
                }
            });
        }

        inline auto WINAPI dr_exception_handler(EXCEPTION_POINTERS* eptrs) -> LONG
        {
            if (eptrs->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP)
            {
                return EXCEPTION_CONTINUE_SEARCH;
            }
            const std::uint64_t dr6{ eptrs->ContextRecord->Dr6 };
            int slot{ -1 };
            if      (dr6 & 0x1) { slot = 0; }
            else if (dr6 & 0x2) { slot = 1; }
            else if (dr6 & 0x4) { slot = 2; }
            else if (dr6 & 0x8) { slot = 3; }
            if (slot < 0)
            {
                return EXCEPTION_CONTINUE_SEARCH;
            }
            dr_slot* slot_ptr{ nullptr };
            {
                std::lock_guard<std::mutex> guard{ dr_mutex };
                if (dr_slots[slot].in_use.load(std::memory_order_relaxed))
                {
                    slot_ptr = &dr_slots[slot];
                }
            }
            if (slot_ptr && slot_ptr->callback)
            {
                try
                {
                    slot_ptr->callback(slot_ptr->address);
                }
                catch (...)
                {
                    // never let user exceptions escape into the kernel.
                }
            }
            // Clear DR6 status and set RF so we don't re-trigger on the
            // very instruction that just fired.
            eptrs->ContextRecord->Dr6   = 0;
            eptrs->ContextRecord->EFlags |= 0x10000;
            return EXCEPTION_CONTINUE_EXECUTION;
        }

        inline auto ensure_dr_handler_installed() -> void
        {
            if (dr_veh_handle != nullptr)
            {
                return;
            }
            dr_veh_handle = ::AddVectoredExceptionHandler(1, &dr_exception_handler);
        }
    } // namespace detail
#endif // VMHOOK_HAS_HW_DATA_BREAKPOINTS

    /*
        @brief Watches a Java static field and invokes a callback when it changes.
        @details
        On Windows x86_64 this installs a hardware data breakpoint (one of
        DR0-DR3) on the field's address.  The trap fires *instantly* on
        every write — no polling, no idle CPU.  The callback runs inside
        a vectored exception handler on whichever thread issued the write,
        so it must not allocate Java objects or call back into the JVM
        (those operations require a JavaThread and a safe-point window).

        On platforms without hardware data-breakpoint support, the
        implementation falls back to a polling background thread that
        reads the field every `poll_interval` and fires the callback on
        observed changes.  Pass a reasonable poll_interval (e.g.
        50 ms) — it's only used on the fallback path.

        Limits (trap path):
          - At most 4 simultaneous trap-based watches per process.
          - Threads that exist when the watch is installed get the trap
            armed; threads created later do NOT.  Hooking thread creation
            so they are caught too is a known improvement.
          - The callback receives the field's address; if the value-type
            is known statically, the caller can read it directly with
            `*reinterpret_cast<const field_type*>(addr)`.

        @tparam wrapper_type  Registered C++ wrapper for the Java class.
        @tparam field_type    Expected value type for the polling path.
        @tparam callback_type Callable invoked with (field_type, field_type)
                              on the polling path, or (field_type)
                              on the trap path (no "old" value cached).
    */
    template<class wrapper_type, typename field_type, typename callback_type>
    inline auto watch_static_field(
        std::string_view                 field_name,
        std::chrono::milliseconds        poll_interval,
        callback_type                    on_change) -> watch_handle
    {
#if VMHOOK_HAS_HW_DATA_BREAKPOINTS
        // Resolve the field's address through the field-proxy machinery.
        const auto proxy{ vmhook::object_base::get_field(
            std::type_index{ typeid(wrapper_type) }, field_name) };
        if (!proxy.has_value())
        {
            VMHOOK_LOG("{} watch_static_field<{}>('{}'): field not found",
                       vmhook::error_tag, typeid(wrapper_type).name(), field_name);
            return watch_handle{};
        }
        void* const address{ proxy->raw_address() };
        if (!address)
        {
            VMHOOK_LOG("{} watch_static_field<{}>('{}'): null address",
                       vmhook::error_tag, typeid(wrapper_type).name(), field_name);
            return watch_handle{};
        }

        // Choose a DR slot.
        std::lock_guard<std::mutex> guard{ detail::dr_mutex };
        const int slot{ detail::find_free_slot() };
        if (slot < 0)
        {
            VMHOOK_LOG("{} watch_static_field: all 4 hardware breakpoint slots in use",
                       vmhook::error_tag);
            return watch_handle{};
        }
        detail::ensure_dr_handler_installed();

        // The trap fires on every write of any size that touches the
        // field's bytes; we use the field type's size to decide LEN.
        constexpr auto length{
            sizeof(field_type) == 1 ? vmhook::os::data_breakpoint_length::one_byte    :
            sizeof(field_type) == 2 ? vmhook::os::data_breakpoint_length::two_bytes   :
            sizeof(field_type) == 4 ? vmhook::os::data_breakpoint_length::four_bytes  :
                                       vmhook::os::data_breakpoint_length::eight_bytes };
        const std::uint64_t dr7_bits{ vmhook::os::detail_dr::build_dr7(
            slot, vmhook::os::data_breakpoint_kind::write, length) };

        detail::dr_slots[slot].address  = address;
        detail::dr_slots[slot].dr7_bits = dr7_bits;
        detail::dr_slots[slot].callback = [on_change = std::move(on_change), address](const void*) mutable
        {
            // The trap arrives *during* the write instruction.  The
            // memory at `address` may or may not already hold the new
            // value depending on how the CPU pipelines the access; we
            // call the user callback with what we observe now.
            try
            {
                field_type current{};
                std::memcpy(&current, address, sizeof(field_type));
                on_change(field_type{}, current);
            }
            catch (...) { /* swallowed */ }
        };
        detail::dr_slots[slot].in_use.store(true, std::memory_order_release);

        detail::refresh_thread_drs(slot, reinterpret_cast<std::uint64_t>(address), dr7_bits);

        // Return a handle whose worker thread just blocks until stop()
        // is invoked, then disarms the DR slot.
        auto block{ std::make_shared<watch_handle::control_block>() };
        block->worker = std::thread{ [block, slot]()
        {
            while (block->running.load(std::memory_order_relaxed))
            {
                std::this_thread::sleep_for(std::chrono::milliseconds{ 50 });
            }
            std::lock_guard<std::mutex> guard{ detail::dr_mutex };
            detail::clear_thread_drs(slot);
            detail::dr_slots[slot].in_use.store(false, std::memory_order_release);
            detail::dr_slots[slot].address = nullptr;
            detail::dr_slots[slot].callback = nullptr;
            detail::dr_slots[slot].dr7_bits = 0;
        } };
        (void)poll_interval;
        return watch_handle{ std::move(block) };
#else
        // Polling fallback on platforms without hardware data breakpoints.
        auto block{ std::make_shared<watch_handle::control_block>() };
        std::string                 captured_field{ field_name };
        std::chrono::milliseconds   captured_interval{ poll_interval };

        block->worker = std::thread{ [block,
                                       captured_field    = std::move(captured_field),
                                       captured_interval,
                                       on_change         = std::move(on_change)]()
            {
                std::optional<field_type> last_value{};
                while (block->running.load(std::memory_order_relaxed))
                {
                    try
                    {
                        const auto proxy{ vmhook::object_base::get_field(
                            std::type_index{ typeid(wrapper_type) }, captured_field) };
                        if (proxy.has_value())
                        {
                            field_type current{ proxy->get() };
                            if (last_value.has_value())
                            {
                                if (!(*last_value == current))
                                {
                                    field_type prev{ std::move(*last_value) };
                                    last_value = current;
                                    try { on_change(std::move(prev), current); }
                                    catch (const std::exception& ex) {
                                        VMHOOK_LOG("{} watch_static_field callback: {}",
                                                   vmhook::error_tag, ex.what());
                                    }
                                }
                            }
                            else { last_value = std::move(current); }
                        }
                    }
                    catch (const std::exception& ex)
                    {
                        VMHOOK_LOG("{} watch_static_field poll: {}",
                                   vmhook::error_tag, ex.what());
                    }
                    std::this_thread::sleep_for(captured_interval);
                }
            } };
        return watch_handle{ std::move(block) };
#endif
    }

    /*
        @brief Registry that owns the class-load callbacks installed by
               on_class_loaded.  Lives inside namespace detail because it
               is implementation-only; users get the class name through
               the on_class_loaded callback.
    */
    namespace detail
    {
        using class_load_callback_t = std::function<void(const std::string&)>;
        inline std::mutex                                          class_load_mutex{};
        inline std::vector<std::shared_ptr<class_load_callback_t>> class_load_callbacks{};
        inline bool                                                class_load_hook_installed{ false };

        // Internal C++ wrapper for java.lang.ClassLoader used only by the
        // class-load hook.  Not exposed publicly; users see the class
        // name string in the callback.
        class class_loader_wrapper : public vmhook::object<class_loader_wrapper>
        {
        public:
            explicit class_loader_wrapper(vmhook::oop_t oop) noexcept
                : vmhook::object<class_loader_wrapper>{ oop }
            {
            }
        };
    }

    /*
        @brief Registers a callback fired whenever java.lang.ClassLoader
               defines a new class.
        @details
        Installs an interpreter hook on the Java method
        `java.lang.ClassLoader::defineClass(String, byte[], int, int,
        ProtectionDomain)` and dispatches the callback with the internal
        class name read from the call's first argument.  This is
        event-driven: zero latency, zero idle cost — no polling.

        Limitations:
          - Only catches classes defined through ClassLoader.defineClass.
            Bootstrap-loaded classes (java.*, javax.*, sun.*) are not
            reported because they bypass the Java-side defineClass entry.
          - The (String, ByteBuffer, ProtectionDomain) overload of
            defineClass is not yet hooked.

        Exception safety: noexcept boundary — callback exceptions are caught
            and logged through VMHOOK_LOG.
        Thread safety: the callback runs on the Java thread that triggered
            the class definition.

        Example:
            auto handle{ vmhook::on_class_loaded(
                [](const std::string& internal_name)
                {
                    std::println("loaded: {}", internal_name);
                }) };

        @tparam callback_type Callable invoked as `void(const std::string&)`.
                              Argument is the JVM-style `/`-separated name.
    */
    template<typename callback_type>
    inline auto on_class_loaded(callback_type on_load) -> watch_handle
    {
        auto cb{ std::make_shared<detail::class_load_callback_t>(std::move(on_load)) };

        {
            std::lock_guard<std::mutex> guard{ detail::class_load_mutex };
            detail::class_load_callbacks.push_back(cb);

            if (!detail::class_load_hook_installed)
            {
                if (vmhook::type_to_class_map.find(std::type_index{ typeid(detail::class_loader_wrapper) })
                    == vmhook::type_to_class_map.end())
                {
                    vmhook::register_class<detail::class_loader_wrapper>("java/lang/ClassLoader");
                }

                auto define_class_detour = [](
                    vmhook::return_value& /*ret*/,
                    const std::unique_ptr<detail::class_loader_wrapper>& /*self*/,
                    const std::string& name,
                    vmhook::oop_t /*bytes*/,
                    std::int32_t /*offset*/,
                    std::int32_t /*length*/,
                    vmhook::oop_t /*protection_domain*/)
                {
                    std::vector<std::shared_ptr<detail::class_load_callback_t>> snapshot;
                    {
                        std::lock_guard<std::mutex> guard2{ detail::class_load_mutex };
                        snapshot = detail::class_load_callbacks;
                    }
                    std::string internal_name{ name };
                    std::replace(internal_name.begin(), internal_name.end(), '.', '/');
                    for (auto& callback : snapshot)
                    {
                        if (!callback) { continue; }
                        try
                        {
                            (*callback)(internal_name);
                        }
                        catch (const std::exception& ex)
                        {
                            VMHOOK_LOG("{} on_class_loaded callback: {}",
                                       vmhook::error_tag, ex.what());
                        }
                    }
                };
                const bool installed = vmhook::hook<detail::class_loader_wrapper>(
                    "defineClass",
                    "(Ljava/lang/String;[BIILjava/security/ProtectionDomain;)Ljava/lang/Class;",
                    define_class_detour);

                if (installed)
                {
                    detail::class_load_hook_installed = true;
                }
                else
                {
                    VMHOOK_LOG("{} on_class_loaded: ClassLoader.defineClass hook installation failed",
                               vmhook::error_tag);
                }
            }
        }

        auto block{ std::make_shared<watch_handle::control_block>() };
        block->worker = std::thread{ [block, cb]()
            {
                while (block->running.load(std::memory_order_relaxed))
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds{ 50 });
                }
                std::lock_guard<std::mutex> guard{ detail::class_load_mutex };
                detail::class_load_callbacks.erase(
                    std::remove(detail::class_load_callbacks.begin(),
                                detail::class_load_callbacks.end(), cb),
                    detail::class_load_callbacks.end());
            } };

        return watch_handle{ std::move(block) };
    }
}
