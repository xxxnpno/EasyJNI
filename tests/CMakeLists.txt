cmake_minimum_required(VERSION 3.20)

# Each test below either compiles the header in isolation (catches compile-time
# regressions for each compiler/platform combo) or exercises pieces of the API
# that work without a live HotSpot JVM (the os::* shims, type traits, etc.).

# Helper to register a test executable with the common configuration.
function(vmhook_add_test name)
    set(options)
    set(oneValueArgs)
    set(multiValueArgs SOURCES)
    cmake_parse_arguments(_t "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    add_executable("vmhook_test_${name}" ${_t_SOURCES})
    target_link_libraries("vmhook_test_${name}" PRIVATE vmhook::vmhook)
    vmhook_apply_warnings("vmhook_test_${name}")

    # Apple Xcode-generated builds need a bundle identifier on every
    # binary they produce; CMake doesn't fill one in by default.  iOS
    # builds further require code-signing identities (which the
    # GitHub-hosted runners don't have) — we disable signing for the
    # test targets since we never deploy or run them on a device.
    if(VMHOOK_PLATFORM_IOS OR APPLE)
        set_target_properties("vmhook_test_${name}" PROPERTIES
            XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "com.vmhook.test.${name}"
            XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED          "NO"
            XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY            ""
            XCODE_ATTRIBUTE_CODE_SIGNING_REQUIRED         "NO"
            XCODE_ATTRIBUTE_ENABLE_BITCODE                "NO"
            XCODE_ATTRIBUTE_EXPANDED_CODE_SIGN_IDENTITY   "-")
    endif()

    add_test(NAME "${name}" COMMAND "vmhook_test_${name}")
endfunction()

# 1) Smoke-compile the header on its own (no JVM, no other includes).
vmhook_add_test(header_compile SOURCES test_header_compile.cpp)

# 2) Header included from two TUs to catch ODR / inline issues.
vmhook_add_test(odr_two_translation_units
    SOURCES test_odr_unit_a.cpp test_odr_unit_b.cpp)

# 3) Type-trait + concept compile-time checks (no JVM required).
vmhook_add_test(traits_static_assertions SOURCES test_traits.cpp)

# 4) OS abstraction layer — page/protect/allocate round-trip (no JVM required).
vmhook_add_test(os_layer_roundtrip SOURCES test_os_layer.cpp)

# 5) Static API surface — make sure register_class, hook<>, etc. are callable
#    with the right signatures even when no JVM is present (they no-op).
vmhook_add_test(api_surface_no_jvm SOURCES test_api_surface.cpp)

# 6) Unified `get_field` / `get_method` call syntax across static + instance.
#    Toolchains with C++23 deducing-this resolve the overloads correctly in
#    static context; the file is guarded by VMHOOK_HAS_DEDUCING_THIS so older
#    Clang / GCC compile a reduced subset.
vmhook_add_test(unified_call_syntax SOURCES test_unified_call_syntax.cpp)
