// Tests for self-contained helpers that don't require a live JVM:
//   * VMHOOK_VERSION / VMHOOK_VERSION_STRING macros
//   * vmhook::hotspot::klass::decode_u5  (UNSIGNED5 decoder used for JDK 21+ FieldInfoStream)
//   * vmhook::hotspot::is_valid_pointer  (canonical-address & sentinel filter)
//   * vmhook::hotspot::untag_pointer     (GC-tag-bit strip)
//   * vmhook::detail::sig_char_to_basic_type  (JVM type descriptor -> HotSpot BasicType)
//   * vmhook::os::to_native_protect      (memory_protection -> native flags roundtrip)
//   * vmhook::os::detail_dr::build_dr7   (Windows + x86_64 only)
//   * vmhook::array_length / get_array_element / set_array_element on a fake buffer
//
// All cases run without a JVM in-process.

#include <vmhook/vmhook.hpp>

#include <array>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>

static int failures{ 0 };

static auto check(const char* name, bool ok) -> void
{
    std::printf("%s %s\n", ok ? "[PASS]" : "[FAIL]", name);
    if (!ok)
    {
        ++failures;
    }
}

// ---------------------------------------------------------------------------
// 1. Version macros
// ---------------------------------------------------------------------------
static auto test_version_macros() -> void
{
    // VMHOOK_MAKE_VERSION should pack semver in the documented way.
    static_assert(VMHOOK_MAKE_VERSION(0, 0, 0) == 0,
                  "VMHOOK_MAKE_VERSION(0,0,0) should be 0");
    static_assert(VMHOOK_MAKE_VERSION(1, 0, 0) == 1000000,
                  "VMHOOK_MAKE_VERSION(1,0,0) should be 1_000_000");
    static_assert(VMHOOK_MAKE_VERSION(0, 4, 0) == 4000,
                  "VMHOOK_MAKE_VERSION(0,4,0) should be 4000");
    static_assert(VMHOOK_MAKE_VERSION(0, 4, 1) == 4001,
                  "VMHOOK_MAKE_VERSION(0,4,1) should be 4001");

    static_assert(VMHOOK_VERSION == VMHOOK_MAKE_VERSION(VMHOOK_VERSION_MAJOR,
                                                       VMHOOK_VERSION_MINOR,
                                                       VMHOOK_VERSION_PATCH),
                  "VMHOOK_VERSION should be the packed form of its parts");

    // VMHOOK_VERSION_STRING must compose as "MAJOR.MINOR.PATCH" with no
    // accidental whitespace or stringification artefacts.
    constexpr std::string_view version_text{ VMHOOK_VERSION_STRING };
    bool has_three_dot_components{ false };
    {
        std::size_t dots{ 0 };
        for (const char c : version_text)
        {
            if (c == '.')
            {
                ++dots;
            }
        }
        has_three_dot_components = (dots == 2);
    }
    check("version_string_has_two_dots", has_three_dot_components);
    check("version_string_starts_with_major",
          version_text.size() >= 3 && version_text[0] >= '0' && version_text[0] <= '9');

    // VMHOOK_VERSION must round-trip through MAKE_VERSION.
    constexpr int reconstructed{ VMHOOK_MAKE_VERSION(VMHOOK_VERSION_MAJOR,
                                                    VMHOOK_VERSION_MINOR,
                                                    VMHOOK_VERSION_PATCH) };
    check("version_roundtrip", reconstructed == VMHOOK_VERSION);

    // Compare against the CMake-defined project version when available.
    // CMakeLists.txt should populate these matching the header.
#if defined(VMHOOK_CMAKE_VERSION_MAJOR) && defined(VMHOOK_CMAKE_VERSION_MINOR) \
    && defined(VMHOOK_CMAKE_VERSION_PATCH)
    check("cmake_version_matches_header_major",
          VMHOOK_CMAKE_VERSION_MAJOR == VMHOOK_VERSION_MAJOR);
    check("cmake_version_matches_header_minor",
          VMHOOK_CMAKE_VERSION_MINOR == VMHOOK_VERSION_MINOR);
    check("cmake_version_matches_header_patch",
          VMHOOK_CMAKE_VERSION_PATCH == VMHOOK_VERSION_PATCH);
#endif
}

// ---------------------------------------------------------------------------
// 2. decode_u5 (HotSpot UNSIGNED5 decoder)
// ---------------------------------------------------------------------------
static auto decode_one(std::initializer_list<std::uint8_t> bytes) -> std::uint32_t
{
    // Copy into a heap-allocated buffer so the function reads past the literal
    // safely (it never reads more than 5 bytes from the decoded stream).
    std::array<std::uint8_t, 16> buffer{};
    std::size_t i{ 0 };
    for (auto b : bytes)
    {
        buffer[i++] = b;
    }
    int pos{ 0 };
    return vmhook::hotspot::klass::decode_u5(buffer.data(), pos);
}

static auto decode_with_pos(std::initializer_list<std::uint8_t> bytes, int& pos_out)
    -> std::uint32_t
{
    std::array<std::uint8_t, 16> buffer{};
    std::size_t i{ 0 };
    for (auto b : bytes)
    {
        buffer[i++] = b;
    }
    return vmhook::hotspot::klass::decode_u5(buffer.data(), pos_out);
}

static auto test_decode_u5() -> void
{
    // Reference encodings from HotSpot UNSIGNED5 (src/hotspot/share/utilities/unsigned5.hpp)
    //   - X = 1 ("excluded" byte 0 -> end marker)
    //   - L = 191 (count of "low" / terminator bytes)
    //   - H = 64  (count of "high" / continuation bytes)
    //   value = sum_i (b_i - X) * 64^i, stopping at first b_i in [1, 191].
    //   Encoder writes b_0 = X+value when value < L, else writes X+L+(remainder mod H)
    //   for continuation bytes and a final low byte for the remainder/L.

    // 1-byte encodings: value < L (=191) maps to byte (value + 1).
    check("decode_u5_zero",  decode_one({ 1 })   == 0u);
    check("decode_u5_one",   decode_one({ 2 })   == 1u);
    check("decode_u5_64",    decode_one({ 65 })  == 64u);
    check("decode_u5_190",   decode_one({ 191 }) == 190u);

    // 2-byte encodings begin at value 191.
    //   191 = (192 - 1) * 1 + (1 - 1) * 64   =>  [192, 1]
    //   255 = (192 - 1) * 1 + (2 - 1) * 64   =>  [192, 2]
    //   4096 = (193 - 1) * 1 + (62 - 1) * 64 =>  [193, 62]
    check("decode_u5_191",   decode_one({ 192, 1 })   == 191u);
    check("decode_u5_255",   decode_one({ 192, 2 })   == 255u);
    check("decode_u5_4096",  decode_one({ 193, 62 })  == 4096u);

    // End-of-stream marker: byte 0 returns ~0u and rewinds stream_pos.
    {
        int pos{ 0 };
        const std::uint32_t result{ decode_with_pos({ 0 }, pos) };
        check("decode_u5_end_marker_returns_ones", result == ~0u);
        check("decode_u5_end_marker_rewinds_pos",  pos == 0);
    }

    // stream_pos advances by the consumed byte count.
    {
        int pos{ 0 };
        (void)decode_with_pos({ 65 }, pos);
        check("decode_u5_advance_one", pos == 1);
    }
    {
        int pos{ 0 };
        (void)decode_with_pos({ 192, 1 }, pos);
        check("decode_u5_advance_two", pos == 2);
    }

    // Decoding two consecutive values continues from the current stream_pos.
    {
        std::array<std::uint8_t, 4> stream{ 65, 192, 1, 3 }; // 64, 191, 2
        int pos{ 0 };
        const auto v0{ vmhook::hotspot::klass::decode_u5(stream.data(), pos) };
        const auto v1{ vmhook::hotspot::klass::decode_u5(stream.data(), pos) };
        const auto v2{ vmhook::hotspot::klass::decode_u5(stream.data(), pos) };
        check("decode_u5_sequence_v0", v0 == 64u);
        check("decode_u5_sequence_v1", v1 == 191u);
        check("decode_u5_sequence_v2", v2 == 2u);
        check("decode_u5_sequence_pos", pos == 4);
    }
}

// ---------------------------------------------------------------------------
// 3. is_valid_pointer & untag_pointer
// ---------------------------------------------------------------------------
static auto test_valid_pointer_filters() -> void
{
    // nullptr and small sentinels are rejected.
    check("is_valid_pointer_null", !vmhook::hotspot::is_valid_pointer(nullptr));
    check("is_valid_pointer_small_sentinel",
          !vmhook::hotspot::is_valid_pointer(reinterpret_cast<void*>(0x100ull)));
    check("is_valid_pointer_floor",
          !vmhook::hotspot::is_valid_pointer(reinterpret_cast<void*>(
              vmhook::os::user_address_floor)));

    // A canonical heap address (anything above the floor sentinel, below the
    // ceiling) is accepted.  Use a stack-local; its address is always canonical.
    int   stack_local{ 0 };
    void* canonical{ &stack_local };
    check("is_valid_pointer_stack_local", vmhook::hotspot::is_valid_pointer(canonical));

    // Kernel / non-canonical pointers are rejected.
    check("is_valid_pointer_kernel",
          !vmhook::hotspot::is_valid_pointer(reinterpret_cast<void*>(
              0xFFFFFFFFFFFFFFFFull)));
}

static auto test_untag_pointer() -> void
{
    // Stripping tag bits from a clean canonical address must leave it untouched.
    int   stack_local{ 0 };
    void* canonical{ &stack_local };
    check("untag_pointer_preserves_canonical",
          vmhook::hotspot::untag_pointer(canonical) == canonical);

    // Setting a high "GC tag" bit (e.g. bit 60) and stripping should recover
    // the original canonical value.
    const std::uintptr_t base_addr{ reinterpret_cast<std::uintptr_t>(canonical) };
    const std::uintptr_t tagged_addr{ base_addr | (std::uintptr_t{ 1 } << 60) };
    const void* const stripped{
        vmhook::hotspot::untag_pointer(reinterpret_cast<void*>(tagged_addr)) };
    check("untag_pointer_strips_high_bit",
          reinterpret_cast<std::uintptr_t>(stripped) == base_addr);

    // nullptr round-trips to nullptr.
    check("untag_pointer_null",
          vmhook::hotspot::untag_pointer(nullptr) == nullptr);
}

// ---------------------------------------------------------------------------
// 4. sig_char_to_basic_type — JVM type descriptor -> HotSpot BasicType integer
// ---------------------------------------------------------------------------
static auto test_sig_char_to_basic_type() -> void
{
    // Values from HotSpot's BasicType enum (src/hotspot/share/utilities/globalDefinitions.hpp).
    check("sig_char_Z_T_BOOLEAN", vmhook::detail::sig_char_to_basic_type('Z') == 4);
    check("sig_char_C_T_CHAR",    vmhook::detail::sig_char_to_basic_type('C') == 5);
    check("sig_char_F_T_FLOAT",   vmhook::detail::sig_char_to_basic_type('F') == 6);
    check("sig_char_D_T_DOUBLE",  vmhook::detail::sig_char_to_basic_type('D') == 7);
    check("sig_char_B_T_BYTE",    vmhook::detail::sig_char_to_basic_type('B') == 8);
    check("sig_char_S_T_SHORT",   vmhook::detail::sig_char_to_basic_type('S') == 9);
    check("sig_char_I_T_INT",     vmhook::detail::sig_char_to_basic_type('I') == 10);
    check("sig_char_J_T_LONG",    vmhook::detail::sig_char_to_basic_type('J') == 11);
    check("sig_char_L_T_OBJECT",  vmhook::detail::sig_char_to_basic_type('L') == 12);
    check("sig_char_array_T_ARRAY", vmhook::detail::sig_char_to_basic_type('[') == 13);
    check("sig_char_V_T_VOID",    vmhook::detail::sig_char_to_basic_type('V') == 14);

    // Unknown characters default to T_OBJECT (12) as a safe fallback.
    check("sig_char_unknown_defaults_to_T_OBJECT",
          vmhook::detail::sig_char_to_basic_type('?') == 12);
    check("sig_char_lower_i_defaults_to_T_OBJECT",
          vmhook::detail::sig_char_to_basic_type('i') == 12);
}

// ---------------------------------------------------------------------------
// 5. to_native_protect — every enum value maps to a distinct non-zero flag set
// ---------------------------------------------------------------------------
static auto test_to_native_protect() -> void
{
    using namespace vmhook::os;

    // no_access must map to PROT_NONE / PAGE_NOACCESS = 0 on POSIX,
    // PAGE_NOACCESS (= 0x01) on Windows.  Each platform's "no access"
    // value is the default fallback, so we just check that the function
    // returns it consistently.
    const auto noaccess{ to_native_protect(memory_protection::no_access) };
    const auto read{ to_native_protect(memory_protection::read) };
    const auto read_write{ to_native_protect(memory_protection::read_write) };
    const auto execute_read{ to_native_protect(memory_protection::execute_read) };
    const auto execute_rw{ to_native_protect(memory_protection::execute_rw) };

    // The four "real" protections must be mutually distinct, otherwise the
    // protect() function silently treats two different intents the same way.
    check("to_native_protect_read_vs_no_access", read != noaccess);
    check("to_native_protect_rw_vs_read", read_write != read);
    check("to_native_protect_exec_read_vs_read", execute_read != read);
    check("to_native_protect_exec_rw_vs_rw", execute_rw != read_write);
    check("to_native_protect_exec_rw_vs_exec_read", execute_rw != execute_read);

    // A garbage enum value (cast from an unknown int) must fall back to
    // no_access rather than returning an uninitialised DWORD.  Catches
    // the case where someone added a new enum entry without updating the
    // switch and forgot the default branch.
    const auto bogus{ to_native_protect(static_cast<memory_protection>(255)) };
    check("to_native_protect_unknown_falls_back_to_no_access", bogus == noaccess);
}

// ---------------------------------------------------------------------------
// 6. build_dr7 — DR7 control-mask construction (Windows + x86_64 only)
// ---------------------------------------------------------------------------
#if VMHOOK_HAS_HW_DATA_BREAKPOINTS
static auto test_build_dr7() -> void
{
    using namespace vmhook::os;
    using namespace vmhook::os::detail_dr;

    // Slot 0, write-only, 4-byte window:
    //   L0 = bit 0 -> 0x1
    //   R/W0 (bits 16-17) = 01 -> 0x10000
    //   LEN0 (bits 18-19) = 11 -> 0xC0000
    //   total = 0xD0001
    check("build_dr7_slot0_write_4bytes",
          build_dr7(0, data_breakpoint_kind::write,
                    data_breakpoint_length::four_bytes) == 0xD0001ull);

    // Slot 1, read/write, 8-byte window:
    //   L1 = bit 2 -> 0x4
    //   R/W1 (bits 20-21) = 11 -> 0x300000
    //   LEN1 (bits 22-23) = 10 -> 0x800000
    //   total = 0xB00004
    check("build_dr7_slot1_rw_8bytes",
          build_dr7(1, data_breakpoint_kind::read_write,
                    data_breakpoint_length::eight_bytes) == 0xB00004ull);

    // Slot 3, write-only, 1-byte window:
    //   L3 = bit 6 -> 0x40
    //   R/W3 (bits 28-29) = 01 -> 0x10000000
    //   LEN3 (bits 30-31) = 00 -> 0x0
    //   total = 0x10000040
    check("build_dr7_slot3_write_1byte",
          build_dr7(3, data_breakpoint_kind::write,
                    data_breakpoint_length::one_byte) == 0x10000040ull);

    // The local-enable bit must always land at the documented position;
    // a typo in the slot * 2 shift would make this fail.
    for (int slot{ 0 }; slot < 4; ++slot)
    {
        const std::uint64_t dr7{
            build_dr7(slot, data_breakpoint_kind::write,
                      data_breakpoint_length::one_byte) };
        const std::uint64_t expected_local{ std::uint64_t{ 1 } << (slot * 2) };
        char tag[64];
        std::snprintf(tag, sizeof(tag), "build_dr7_local_enable_slot%d", slot);
        check(tag, (dr7 & expected_local) == expected_local);
    }
}
#endif

// ---------------------------------------------------------------------------
// 7. Array helpers — array_length / get_array_element / set_array_element
//    Exercises real header code against a fake heap-style buffer.
// ---------------------------------------------------------------------------
static auto test_array_helpers() -> void
{
    // Layout (HotSpot, compressed OOPs, x64):
    //   +0  mark word (8 B)
    //   +8  klass narrow ptr (4 B)
    //   +12 _length        (int)
    //   +16 _data[0]
    //
    // Buffer is heap-allocated so GCC's -Warray-bounds does not constant-fold
    // the indices the OOB tests pass into get/set_array_element; we want to
    // verify the runtime guard, not satisfy the static analyser.

    constexpr std::int32_t expected_length{ 5 };
    const std::size_t buffer_bytes{ 16u + 5u * sizeof(std::int32_t) };

    std::vector<std::uint8_t> buffer(buffer_bytes, std::uint8_t{ 0 });
    std::memcpy(buffer.data() + 12, &expected_length, sizeof(expected_length));

    const std::int32_t values[5]{ 100, 200, 300, 400, 500 };
    for (std::int32_t i{ 0 }; i < 5; ++i)
    {
        std::memcpy(buffer.data() + 16 + static_cast<std::size_t>(i) * sizeof(std::int32_t),
                    &values[i], sizeof(std::int32_t));
    }

    void* const array_oop{ buffer.data() };

    check("array_length_reads_offset_12",
          vmhook::array_length(array_oop) == expected_length);

    for (std::int32_t i{ 0 }; i < 5; ++i)
    {
        const std::int32_t v{ vmhook::get_array_element<std::int32_t>(array_oop, i) };
        char tag[64];
        std::snprintf(tag, sizeof(tag), "get_array_element_int32_index_%d", i);
        check(tag, v == values[i]);
    }

    // Bounds checks: negative index and out-of-range index should return T{}.
    // The indices are routed through a volatile read so GCC's -Warray-bounds
    // does not statically prove they're OOB and refuse to compile the call.
    auto opaque_index{ [](std::int32_t i) noexcept
        {
            volatile std::int32_t v{ i };
            return v;
        } };

    check("get_array_element_negative_index_returns_default",
          vmhook::get_array_element<std::int32_t>(array_oop, opaque_index(-1)) == 0);
    check("get_array_element_oob_index_returns_default",
          vmhook::get_array_element<std::int32_t>(array_oop, opaque_index(expected_length)) == 0);
    check("get_array_element_far_oob_index_returns_default",
          vmhook::get_array_element<std::int32_t>(array_oop, opaque_index(1000)) == 0);

    // set_array_element with a valid index updates the buffer in place;
    // set with an out-of-range index is a no-op.
    vmhook::set_array_element<std::int32_t>(array_oop, opaque_index(2), 9999);
    check("set_array_element_writes_value",
          vmhook::get_array_element<std::int32_t>(array_oop, opaque_index(2)) == 9999);

    vmhook::set_array_element<std::int32_t>(array_oop, opaque_index(-1), 1234);
    vmhook::set_array_element<std::int32_t>(array_oop, opaque_index(100), 1234);
    // Confirm the neighbouring elements were not corrupted.
    check("set_array_element_oob_is_noop_neighbour_below",
          vmhook::get_array_element<std::int32_t>(array_oop, opaque_index(0)) == values[0]);
    check("set_array_element_oob_is_noop_neighbour_above",
          vmhook::get_array_element<std::int32_t>(array_oop, opaque_index(4)) == values[4]);

    // Null array_oop short-circuits without faulting.
    check("array_length_null_returns_zero", vmhook::array_length(nullptr) == 0);
    check("get_array_element_null_returns_default",
          vmhook::get_array_element<std::int32_t>(nullptr, 0) == 0);
}

// ---------------------------------------------------------------------------
// 8. format_log — fallback must not throw or crash on misformatted patterns.
// ---------------------------------------------------------------------------
static auto test_format_log_safe_on_bad_pattern() -> void
{
    // The release-build VMHOOK_LOG expands to (void)sizeof(format_log(...)),
    // but the function itself is callable directly.  std::vformat will throw
    // a std::format_error on a malformed pattern; the helper must catch it
    // and return the literal format string instead of propagating.
    const std::string result{ vmhook::detail::format_log("{") };
    check("format_log_handles_bad_pattern", !result.empty());
}

// ---------------------------------------------------------------------------
// 10. write_jni_arg_to_slot for unique_ptr<wrapper> — regression test for the
//     value_type-shadowing bug that silently dropped every IChatComponent arg
//     into Lunar / Forge / vanilla addChatMessage calls.
//
//     The bug:
//       template<typename value_type, typename deleter_type>
//       struct is_unique_ptr<std::unique_ptr<value_type, deleter_type>>
//           : std::true_type
//       { using value_type_t = value_type; };
//
//     The std::true_type base inherits `using value_type = bool` from
//     std::integral_constant<bool, true>; inside the class body unqualified
//     name lookup found the inherited typedef first, so value_type_t became
//     bool, then `is_base_of_v<object_base, value_type_t>` evaluated to
//     `is_base_of_v<object_base, bool>` -> false, the unique_ptr branch in
//     write_jni_arg_to_slot was silently skipped, and the JVM received
//     values[0].l == nullptr for the IChatComponent arg.
//
//     This test wraps a sentinel oop in a test_wrapper, runs the arg slot
//     packer, and asserts that value.l points back at the storage cell
//     containing our sentinel.  Re-introducing the trait bug would set
//     value.l to nullptr and this would fail loudly.
// ---------------------------------------------------------------------------
namespace {
    struct test_wrapper_helpers : public vmhook::object<test_wrapper_helpers> {
        using vmhook::object<test_wrapper_helpers>::object;
    };
}

static auto test_write_jni_arg_to_slot_unique_ptr_branch() -> void
{
    // Sentinel OOP value - just an opaque pointer.  We never deref it; the
    // arg-packer only stores it.
    auto* const sentinel_oop{ reinterpret_cast<void*>(static_cast<std::uintptr_t>(0xDEADBEEFCAFE0000ull)) };
    auto wrapper{ std::make_unique<test_wrapper_helpers>(sentinel_oop) };

    vmhook::detail::jni_value value{};
    void* storage{ nullptr };
    vmhook::detail::write_jni_arg_to_slot(value, storage, wrapper);

    // value.l must be a NON-NULL pointer (specifically, &storage).
    check("write_jni_arg_to_slot_unique_ptr_value_l_non_null",
          value.l != nullptr);

    // value.l must point at our local `storage` slot - that is the
    // indirect-handle pattern the JVM expects (jobject = jobject*).
    check("write_jni_arg_to_slot_unique_ptr_value_l_points_at_storage",
          value.l == static_cast<void*>(&storage));

    // The storage slot must hold our sentinel.  Re-introducing the trait
    // shadow bug (value_type_t = bool) would skip both writes and leave
    // storage == nullptr.
    check("write_jni_arg_to_slot_unique_ptr_storage_holds_oop",
          storage == sentinel_oop);

    // Dereferencing value.l (the JVM-internal JNIHandles::resolve operation)
    // must yield the sentinel - this is what call_jni's diagnostic dump
    // does, and is also exactly what the JVM does inside CallVoidMethodA.
    check("write_jni_arg_to_slot_unique_ptr_deref_yields_oop",
          *static_cast<void**>(value.l) == sentinel_oop);
}

static auto test_write_jni_arg_to_slot_null_unique_ptr() -> void
{
    // A null unique_ptr arg should result in storage == nullptr but
    // value.l still pointing at &storage (so the JVM receives a NULL jobject,
    // not garbage).
    std::unique_ptr<test_wrapper_helpers> wrapper{};   // empty

    vmhook::detail::jni_value value{};
    void* storage{ reinterpret_cast<void*>(static_cast<std::uintptr_t>(0xDEADull)) };  // pre-fill to detect overwrite
    vmhook::detail::write_jni_arg_to_slot(value, storage, wrapper);

    check("write_jni_arg_to_slot_null_unique_ptr_value_l_still_points_at_storage",
          value.l == static_cast<void*>(&storage));
    check("write_jni_arg_to_slot_null_unique_ptr_storage_cleared",
          storage == nullptr);
}

// ---------------------------------------------------------------------------
// 11. vmhook::jni wrappers - verify they delegate to the underlying detail
//     functions with no signature drift.  signature_for_arg<T> returns a
//     std::string (non-constexpr) so we cross-check at runtime instead of
//     via static_assert.
// ---------------------------------------------------------------------------
static auto test_jni_namespace_signature_for_arg() -> void
{
    check("jni::signature_for_arg<bool> == 'Z'",
          vmhook::jni::signature_for_arg<bool>() == "Z");
    check("jni::signature_for_arg<int8_t> == 'B'",
          vmhook::jni::signature_for_arg<std::int8_t>() == "B");
    check("jni::signature_for_arg<int16_t> == 'S'",
          vmhook::jni::signature_for_arg<std::int16_t>() == "S");
    check("jni::signature_for_arg<uint16_t> == 'C'",
          vmhook::jni::signature_for_arg<std::uint16_t>() == "C");
    check("jni::signature_for_arg<int32_t> == 'I'",
          vmhook::jni::signature_for_arg<std::int32_t>() == "I");
    check("jni::signature_for_arg<int64_t> == 'J'",
          vmhook::jni::signature_for_arg<std::int64_t>() == "J");
    check("jni::signature_for_arg<float> == 'F'",
          vmhook::jni::signature_for_arg<float>() == "F");
    check("jni::signature_for_arg<double> == 'D'",
          vmhook::jni::signature_for_arg<double>() == "D");
    check("jni::signature_for_arg<string> == 'Ljava/lang/String;'",
          vmhook::jni::signature_for_arg<std::string>() == "Ljava/lang/String;");
    check("jni::signature_for_arg<string_view> == 'Ljava/lang/String;'",
          vmhook::jni::signature_for_arg<std::string_view>() == "Ljava/lang/String;");
    check("jni::signature_for_arg<const char*> == 'Ljava/lang/String;'",
          vmhook::jni::signature_for_arg<const char*>() == "Ljava/lang/String;");

    // Cross-check that the wrapper returns the same string as the underlying
    // implementation it delegates to.  Catches accidental drift between the
    // two if someone adds a new type to detail::jni_signature_for_arg but
    // forgets the corresponding wrapper instantiation (templates compile
    // lazily, so without this check the wrapper would just silently fall to
    // the default 'I' branch).
    check("jni::signature_for_arg<bool> matches detail::jni_signature_for_arg<bool>",
          vmhook::jni::signature_for_arg<bool>()
          == vmhook::detail::jni_signature_for_arg<bool>());
    check("jni::signature_for_arg<string> matches detail::jni_signature_for_arg<string>",
          vmhook::jni::signature_for_arg<std::string>()
          == vmhook::detail::jni_signature_for_arg<std::string>());
    check("jni::signature_for_arg<int64_t> matches detail::jni_signature_for_arg<int64_t>",
          vmhook::jni::signature_for_arg<std::int64_t>()
          == vmhook::detail::jni_signature_for_arg<std::int64_t>());
}

static auto test_write_jni_arg_to_slot_primitive_branches() -> void
{
    // Sanity that the primitive branches still hit, after the static_assert
    // guard was added in the trailing else.
    {
        vmhook::detail::jni_value value{};
        void* storage{};
        vmhook::detail::write_jni_arg_to_slot(value, storage, true);
        check("write_jni_arg_to_slot_bool", value.z == true);
    }
    {
        vmhook::detail::jni_value value{};
        void* storage{};
        vmhook::detail::write_jni_arg_to_slot(value, storage, std::int32_t{ 42 });
        check("write_jni_arg_to_slot_int", value.i == 42);
    }
    {
        vmhook::detail::jni_value value{};
        void* storage{};
        vmhook::detail::write_jni_arg_to_slot(value, storage, std::int64_t{ 0x1122334455667788ll });
        check("write_jni_arg_to_slot_long", value.j == 0x1122334455667788ll);
    }
    {
        vmhook::detail::jni_value value{};
        void* storage{};
        vmhook::detail::write_jni_arg_to_slot(value, storage, 3.14f);
        check("write_jni_arg_to_slot_float", value.f == 3.14f);
    }
    {
        vmhook::detail::jni_value value{};
        void* storage{};
        vmhook::detail::write_jni_arg_to_slot(value, storage, 2.71828);
        check("write_jni_arg_to_slot_double", value.d == 2.71828);
    }
}

int main()
{
    test_version_macros();
    test_decode_u5();
    test_valid_pointer_filters();
    test_untag_pointer();
    test_sig_char_to_basic_type();
    test_to_native_protect();
#if VMHOOK_HAS_HW_DATA_BREAKPOINTS
    test_build_dr7();
#endif
    test_array_helpers();
    test_format_log_safe_on_bad_pattern();
    test_write_jni_arg_to_slot_unique_ptr_branch();
    test_write_jni_arg_to_slot_null_unique_ptr();
    test_write_jni_arg_to_slot_primitive_branches();
    test_jni_namespace_signature_for_arg();

    if (failures == 0)
    {
        std::printf("vmhook helpers: OK\n");
    }
    else
    {
        std::printf("vmhook helpers: %d FAILURE(S)\n", failures);
    }
    return failures == 0 ? 0 : 1;
}
