cmake_minimum_required(VERSION 3.20)

# Each test below either compiles the header in isolation (catches compile-time
# regressions for each compiler/platform combo) or exercises pieces of the API
# that work without a live HotSpot JVM (the os::* shims, type traits, etc.).

# 1) Smoke-compile the header on its own (no JVM, no other includes).
add_executable(vmhook_test_header_compile test_header_compile.cpp)
target_link_libraries(vmhook_test_header_compile PRIVATE vmhook::vmhook)
vmhook_apply_warnings(vmhook_test_header_compile)
add_test(NAME header_compile COMMAND vmhook_test_header_compile)

# 2) Header included from two TUs to catch ODR / inline issues.
add_executable(vmhook_test_odr test_odr_unit_a.cpp test_odr_unit_b.cpp)
target_link_libraries(vmhook_test_odr PRIVATE vmhook::vmhook)
vmhook_apply_warnings(vmhook_test_odr)
add_test(NAME odr_two_translation_units COMMAND vmhook_test_odr)

# 3) Type-trait + concept compile-time checks (no JVM required).
add_executable(vmhook_test_traits test_traits.cpp)
target_link_libraries(vmhook_test_traits PRIVATE vmhook::vmhook)
vmhook_apply_warnings(vmhook_test_traits)
add_test(NAME traits_static_assertions COMMAND vmhook_test_traits)

# 4) OS abstraction layer — page/protect/allocate round-trip (no JVM required).
add_executable(vmhook_test_os_layer test_os_layer.cpp)
target_link_libraries(vmhook_test_os_layer PRIVATE vmhook::vmhook)
vmhook_apply_warnings(vmhook_test_os_layer)
add_test(NAME os_layer_roundtrip COMMAND vmhook_test_os_layer)

# 5) Static API surface — make sure register_class, hook<>, etc. are callable
#    with the right signatures even when no JVM is present (they no-op).
add_executable(vmhook_test_api_surface test_api_surface.cpp)
target_link_libraries(vmhook_test_api_surface PRIVATE vmhook::vmhook)
vmhook_apply_warnings(vmhook_test_api_surface)
add_test(NAME api_surface_no_jvm COMMAND vmhook_test_api_surface)
