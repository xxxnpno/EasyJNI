cmake_minimum_required(VERSION 3.20)

project(vmhook
    VERSION 0.2.0
    DESCRIPTION "Single-header HotSpot JVM hooking library"
    LANGUAGES CXX)

# ── Project-wide options ─────────────────────────────────────────────────────
option(VMHOOK_BUILD_EXAMPLE  "Build the example test DLL/SO that runs the hook suite" ON)
option(VMHOOK_BUILD_INJECTOR "Build the Windows-only injector executable"             ON)
option(VMHOOK_BUILD_TESTS    "Build the standalone unit-test suite (no JVM needed)"   ON)
option(VMHOOK_WARNINGS_AS_ERRORS "Treat warnings as errors for first-party code"      OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT GENERATOR_IS_MULTI_CONFIG)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build configuration" FORCE)
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT (CMAKE_SIZEOF_VOID_P EQUAL 8))
    message(FATAL_ERROR "vmhook targets x86_64 only (CMAKE_SIZEOF_VOID_P=${CMAKE_SIZEOF_VOID_P})")
endif()

# ── Interface target for the single-header library ──────────────────────────
add_library(vmhook INTERFACE)
add_library(vmhook::vmhook ALIAS vmhook)
target_include_directories(vmhook INTERFACE
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/vmhook/ext>"
    "$<INSTALL_INTERFACE:include>")

if(UNIX)
    target_link_libraries(vmhook INTERFACE dl pthread)
endif()

# ── Helper: apply our standard warning set to a target ──────────────────────
function(vmhook_apply_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE
            /W4 /permissive- /Zc:__cplusplus /utf-8
            /wd4505)  # unreferenced static function (header carries multiple by design)
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
    add_library(vmhook_example MODULE vmhook/src/example.cpp)
    set_target_properties(vmhook_example PROPERTIES
        OUTPUT_NAME "vmhook"
        PREFIX     ""               # no "lib" prefix on Linux: libvmhook.so → vmhook.so for symmetry
        SUFFIX     "${CMAKE_SHARED_MODULE_SUFFIX}")
    if(UNIX)
        # On Linux JDK loads "libvmhook.so" via System.loadLibrary("vmhook"),
        # so we DO want the lib prefix on Linux.
        set_target_properties(vmhook_example PROPERTIES PREFIX "lib")
    endif()
    target_link_libraries(vmhook_example PRIVATE vmhook::vmhook)
    vmhook_apply_warnings(vmhook_example)
    set_target_properties(vmhook_example PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
endif()

# ── Windows-only injector (CreateRemoteThread + LoadLibraryW) ───────────────
if(VMHOOK_BUILD_INJECTOR AND WIN32)
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
