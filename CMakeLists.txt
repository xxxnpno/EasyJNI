cmake_minimum_required(VERSION 3.20)

project(vmhook
    VERSION 0.5.0
    DESCRIPTION "Single-header HotSpot JVM hooking library"
    LANGUAGES CXX)

# ── Detect target platform ───────────────────────────────────────────────────
# The library compiles on Windows / Linux / macOS / iOS / Android.  Runtime
# functionality (HotSpot interpreter hooking) needs a HotSpot JVM in-process,
# which is only available on the first three.  iOS and Android ship their own
# VMs (no HotSpot) so vmhook is build-only on those platforms.

set(VMHOOK_PLATFORM_WINDOWS FALSE)
set(VMHOOK_PLATFORM_LINUX   FALSE)
set(VMHOOK_PLATFORM_MACOS   FALSE)
set(VMHOOK_PLATFORM_IOS     FALSE)
set(VMHOOK_PLATFORM_ANDROID FALSE)

if(WIN32)
    set(VMHOOK_PLATFORM_WINDOWS TRUE)
elseif(ANDROID OR CMAKE_SYSTEM_NAME STREQUAL "Android")
    set(VMHOOK_PLATFORM_ANDROID TRUE)
elseif(IOS OR CMAKE_SYSTEM_NAME STREQUAL "iOS")
    set(VMHOOK_PLATFORM_IOS TRUE)
elseif(APPLE)
    set(VMHOOK_PLATFORM_MACOS TRUE)
elseif(UNIX)
    set(VMHOOK_PLATFORM_LINUX TRUE)
endif()

# Runtime hooking (the trampoline) is x86_64-only and excludes iOS where
# Apple disallows runtime code generation in user processes.
set(VMHOOK_HAS_RUNTIME_HOOKING TRUE)
if(NOT (CMAKE_SIZEOF_VOID_P EQUAL 8))
    message(STATUS "vmhook: 32-bit build detected — runtime hooking disabled, headers still compile")
    set(VMHOOK_HAS_RUNTIME_HOOKING FALSE)
endif()
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm|aarch).*" OR
   CMAKE_OSX_ARCHITECTURES MATCHES "arm" OR
   ANDROID_ABI MATCHES "arm" OR
   IOS)
    set(VMHOOK_HAS_RUNTIME_HOOKING FALSE)
endif()
if(VMHOOK_PLATFORM_IOS)
    set(VMHOOK_HAS_RUNTIME_HOOKING FALSE)
endif()

set(_vmhook_platform_label "unknown")
if(VMHOOK_PLATFORM_WINDOWS)
    set(_vmhook_platform_label "Windows")
elseif(VMHOOK_PLATFORM_LINUX)
    set(_vmhook_platform_label "Linux")
elseif(VMHOOK_PLATFORM_MACOS)
    set(_vmhook_platform_label "macOS")
elseif(VMHOOK_PLATFORM_IOS)
    set(_vmhook_platform_label "iOS")
elseif(VMHOOK_PLATFORM_ANDROID)
    set(_vmhook_platform_label "Android")
endif()
message(STATUS "vmhook platform = ${_vmhook_platform_label}"
              "; runtime hooking = ${VMHOOK_HAS_RUNTIME_HOOKING}")

# ── Project-wide options ─────────────────────────────────────────────────────
# Defaults are chosen per platform so a plain `cmake -S . -B build` works
# everywhere: Windows / Linux / macOS get the full build; iOS / Android get
# the header + tests but skip the JVM-injection-only example DLL and the
# Windows-only injector.

# Determine the default for VMHOOK_BUILD_EXAMPLE.  The example DLL is
# useful as a self-test even on platforms where runtime hooking is not
# functional (the field-access, klass-walking, and JNI-OnLoad paths still
# work; only the interpreter-stub trampoline degrades).  We only disable
# the example by default on cross-compile targets that have no JVM at
# all: iOS and Android.
set(_vmhook_example_default TRUE)
if(VMHOOK_PLATFORM_IOS OR VMHOOK_PLATFORM_ANDROID)
    set(_vmhook_example_default FALSE)
endif()

option(VMHOOK_BUILD_EXAMPLE  "Build the example test DLL/SO that runs the hook suite"
       ${_vmhook_example_default})
option(VMHOOK_BUILD_INJECTOR "Build the Windows-only injector executable"
       ${VMHOOK_PLATFORM_WINDOWS})
option(VMHOOK_BUILD_TESTS    "Build the standalone unit-test suite (no JVM needed)"
       ON)
option(VMHOOK_WARNINGS_AS_ERRORS "Treat warnings as errors for first-party code"
       OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT GENERATOR_IS_MULTI_CONFIG)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build configuration" FORCE)
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ── Interface target for the single-header library ──────────────────────────
add_library(vmhook INTERFACE)
add_library(vmhook::vmhook ALIAS vmhook)
target_include_directories(vmhook INTERFACE
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/vmhook/ext>"
    "$<INSTALL_INTERFACE:include>")

# POSIX dependencies: dl / pthread / mach (only the libs that are actually
# available on each variant).
if(VMHOOK_PLATFORM_LINUX OR VMHOOK_PLATFORM_ANDROID)
    target_link_libraries(vmhook INTERFACE dl)
    if(VMHOOK_PLATFORM_LINUX)
        target_link_libraries(vmhook INTERFACE pthread)
    endif()
elseif(VMHOOK_PLATFORM_MACOS OR VMHOOK_PLATFORM_IOS)
    # libdl and pthread are part of libSystem on Apple platforms — no
    # explicit link required.  We use mach VM APIs through libSystem too.
endif()

# ── Helper: apply our standard warning set to a target ──────────────────────
function(vmhook_apply_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE
            /W4 /permissive- /Zc:__cplusplus /utf-8
            /wd4505     # unreferenced static function (header carries multiple by design)
            /wd4101)    # unreferenced local (release builds compile out VMHOOK_LOG)
        if(VMHOOK_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE /WX)
        endif()
        target_compile_definitions(${target} PRIVATE
            WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE
            _WIN32_WINNT=0x0A00 WINVER=0x0A00)
    else()
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wpedantic
            -Wno-unused-function
            -Wno-unused-local-typedefs
            -Wno-unused-but-set-variable
            -Wno-cast-function-type)
        if(VMHOOK_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()
endfunction()

# ── Example DLL / shared library that drives the JVM tests ──────────────────
if(VMHOOK_BUILD_EXAMPLE)
    set(_vmhook_example_sources vmhook/src/example.cpp)

    # Modular JVM test harness: the registry implementation plus every
    # self-registering per-feature module under tests/jvm/modules/.  Each module
    # is one feature's exhaustive JVM tests; CONFIGURE_DEPENDS re-globs when a
    # new module file is added so authors never edit this list.
    list(APPEND _vmhook_example_sources tests/jvm/harness.cpp)
    file(GLOB _vmhook_jvm_modules CONFIGURE_DEPENDS
         "${CMAKE_CURRENT_SOURCE_DIR}/tests/jvm/modules/*.cpp")
    list(APPEND _vmhook_example_sources ${_vmhook_jvm_modules})

    # Optional JNI-side speed test.  Only built when the JDK include
    # path is locatable, since pure-JNI calls require <jni.h> — vmhook
    # itself stays JNI-header-free, the bench is in a separate TU.
    find_package(JNI QUIET COMPONENTS JVM)
    if(NOT JNI_FOUND)
        find_package(JNI QUIET)
    endif()
    if(JNI_FOUND)
        message(STATUS "vmhook bench: JNI found at ${JNI_INCLUDE_DIRS}")
        list(APPEND _vmhook_example_sources vmhook/src/speedtest.cpp)
    else()
        message(STATUS "vmhook bench: <jni.h> not found, speedtest will be a no-op")
    endif()

    add_library(vmhook_example MODULE ${_vmhook_example_sources})
    set_target_properties(vmhook_example PROPERTIES
        OUTPUT_NAME "vmhook"
        PREFIX     ""
        SUFFIX     "${CMAKE_SHARED_MODULE_SUFFIX}")
    if(VMHOOK_PLATFORM_LINUX OR VMHOOK_PLATFORM_ANDROID
       OR VMHOOK_PLATFORM_MACOS OR VMHOOK_PLATFORM_IOS)
        # Java's System.loadLibrary("vmhook") expects "libvmhook" on POSIX.
        set_target_properties(vmhook_example PROPERTIES PREFIX "lib")
    endif()
    target_link_libraries(vmhook_example PRIVATE vmhook::vmhook)
    if(JNI_FOUND)
        target_include_directories(vmhook_example PRIVATE ${JNI_INCLUDE_DIRS})
        target_compile_definitions(vmhook_example PRIVATE VMHOOK_BENCH_USE_JNI=1)
    endif()
    vmhook_apply_warnings(vmhook_example)
    set_target_properties(vmhook_example PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
endif()

# ── Windows-only injector (CreateRemoteThread + LoadLibraryW) ───────────────
if(VMHOOK_BUILD_INJECTOR AND VMHOOK_PLATFORM_WINDOWS)
    add_executable(vmhook_injector injector/src/main.cpp)
    set_target_properties(vmhook_injector PROPERTIES OUTPUT_NAME "injector")
    target_link_libraries(vmhook_injector PRIVATE kernel32)
    vmhook_apply_warnings(vmhook_injector)
    set_target_properties(vmhook_injector PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
endif()

# ── Standalone tests (header compiles + smoke-level pure-logic tests) ───────
if(VMHOOK_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# ── Install support ─────────────────────────────────────────────────────────
include(GNUInstallDirs)
install(DIRECTORY vmhook/ext/vmhook
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(TARGETS vmhook EXPORT vmhookTargets)
install(EXPORT vmhookTargets
        NAMESPACE vmhook::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/vmhook
        FILE vmhookConfig.cmake)
