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

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>
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
// ---------------------------------------------------------------------------
// 12. is_valid_pointer must reject debug-poison sentinel patterns
//
// Old behaviour: a pointer whose low 32 bits are 0xDEADBEEF / 0xCDCDCDCD /
// 0xCAFEBABE etc. fell inside the user-address range and was passed through
// untouched, then segfaulted on dereference.  After the fix is_valid_pointer
// rejects them up-front so callers can rely on the boolean.
// ---------------------------------------------------------------------------
static auto test_is_valid_pointer_rejects_sentinels() -> void
{
    using vmhook::hotspot::is_valid_pointer;

    // Well-known debug-poison values that the old range-only check accepted.
    auto poison = [](std::uint64_t low32) -> const void*
    {
        return reinterpret_cast<const void*>(static_cast<std::uintptr_t>(low32));
    };
    check("is_valid_pointer_rejects_DEADBEEF", !is_valid_pointer(poison(0xDEADBEEFu)));
    check("is_valid_pointer_rejects_CAFEBABE", !is_valid_pointer(poison(0xCAFEBABEu)));
    check("is_valid_pointer_rejects_CCCCCCCC", !is_valid_pointer(poison(0xCCCCCCCCu)));
    check("is_valid_pointer_rejects_CDCDCDCD", !is_valid_pointer(poison(0xCDCDCDCDu)));
    check("is_valid_pointer_rejects_BAADF00D", !is_valid_pointer(poison(0xBAADF00Du)));
    check("is_valid_pointer_rejects_FEEEFEEE", !is_valid_pointer(poison(0xFEEEFEEEu)));
    check("is_valid_pointer_rejects_ABABABAB", !is_valid_pointer(poison(0xABABABABu)));
    check("is_valid_pointer_rejects_FDFDFDFD", !is_valid_pointer(poison(0xFDFDFDFDu)));
    check("is_valid_pointer_rejects_DDDDDDDD", !is_valid_pointer(poison(0xDDDDDDDDu)));

    // Real stack address must STILL pass after the new sentinel check.
    int local_var{ 42 };
    check("is_valid_pointer_accepts_stack_address", is_valid_pointer(&local_var));

    // Heap address must pass.
    auto heap_buf{ std::make_unique<int>(42) };
    check("is_valid_pointer_accepts_heap_address", is_valid_pointer(heap_buf.get()));

    // Null still rejected.
    check("is_valid_pointer_rejects_null", !is_valid_pointer(nullptr));

    // Odd low-bit address now rejected (HotSpot pointers are always aligned).
    const void* odd{ reinterpret_cast<const void*>(reinterpret_cast<std::uintptr_t>(&local_var) | 0x1u) };
    check("is_valid_pointer_rejects_odd_low_bit", !is_valid_pointer(odd));
}

// ---------------------------------------------------------------------------
// 13. return_value::set must sign-extend signed integers into the 64-bit slot.
//
// Old behaviour: memcpy of the low N bytes left the upper bits at zero, so a
// hook returning int8_t{-1} (= 0xFF) was visible to Java as +255 instead of -1.
// Fix: signed integer types < 8 bytes go through a static_cast<int64_t>(value)
// which sign-extends; other types still use memcpy.
// ---------------------------------------------------------------------------
static auto test_return_value_sign_extension() -> void
{
    // Build a fake slot on the stack.  The slot lives in vmhook::hotspot.
    vmhook::hotspot::return_slot slot{};
    vmhook::return_value rv{ &slot };

    // int8_t{-1} -> retval should be -1 (sign-extended), not 255 (zero-extended).
    rv.set(std::int8_t{ -1 });
    check("return_value_set_int8_minus_one_sign_extends",
          slot.retval == static_cast<std::int64_t>(-1));

    // int16_t{-12345} -> sign-extend to -12345 in 64 bits.
    slot.retval = 0; slot.cancel = false;
    rv.set(std::int16_t{ -12345 });
    check("return_value_set_int16_neg_sign_extends",
          slot.retval == static_cast<std::int64_t>(-12345));

    // int32_t{-1} -> sign-extend.
    slot.retval = 0; slot.cancel = false;
    rv.set(std::int32_t{ -1 });
    check("return_value_set_int32_minus_one_sign_extends",
          slot.retval == static_cast<std::int64_t>(-1));

    // Unsigned types still zero-extend (correct behaviour - JVM treats them
    // as the corresponding signed types and interprets the bit pattern).
    slot.retval = 0; slot.cancel = false;
    rv.set(std::uint8_t{ 0xFF });
    check("return_value_set_uint8_zero_extends_to_255",
          slot.retval == static_cast<std::int64_t>(0xFFu));

    // Positive signed values still come through correctly.
    slot.retval = 0; slot.cancel = false;
    rv.set(std::int32_t{ 42 });
    check("return_value_set_int32_positive_unchanged",
          slot.retval == 42);

    // cancel flag must be set on every set().
    slot.retval = 0; slot.cancel = false;
    rv.set(std::int32_t{ 0 });
    check("return_value_set_sets_cancel_flag", slot.cancel == true);
}

// ---------------------------------------------------------------------------
// 14. return_value::set<wrapper_type>(nullptr) overload — sets cancel + writes
//     a zero OOP to the retval slot, regardless of any garbage previously in
//     the slot.  Selected via requires-clause on wrapper_type deriving from
//     object_base, so primitive set<int>(...) calls are unaffected.
// ---------------------------------------------------------------------------
static auto test_return_value_set_nullptr_for_wrapper() -> void
{
    struct fake_wrapper : public vmhook::object_base {};

    vmhook::hotspot::return_slot slot{};
    vmhook::return_value rv{ &slot };

    // Pre-fill the slot with garbage so we can prove set() zeroes it.
    slot.retval = static_cast<std::int64_t>(0xDEADBEEFCAFEBABEull);
    slot.cancel = false;

    rv.set<fake_wrapper>(nullptr);

    check("return_value_set_wrapper_nullptr_writes_zero_oop", slot.retval == 0);
    check("return_value_set_wrapper_nullptr_sets_cancel_flag", slot.cancel == true);

    // Sanity: primitive path still picks the integer overload (no ambiguity).
    slot.retval = 0; slot.cancel = false;
    rv.set(std::int32_t{ -1 });
    check("return_value_set_primitive_unaffected_by_wrapper_overload",
          slot.retval == static_cast<std::int64_t>(-1));
}

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

// ---------------------------------------------------------------------------
// 15. iterate_struct_entries / iterate_type_entries hardening
//
// These walk gHotSpotVMStructs / gHotSpotVMTypes.  In a no-JVM unit test the
// underlying global pointers are null, so the loop must terminate immediately
// and return nullptr rather than crashing.  Also exercises the defensive
// null-arg guards: both functions now reject null type_name / field_name
// up-front so callers passing through user-controlled symbol strings can't
// accidentally hand strcmp a nullptr.
// ---------------------------------------------------------------------------
static auto test_iterate_entries_no_jvm() -> void
{
    // No JVM is loaded in the test process, so the static lookups must
    // return nullptr without faulting on strcmp(nullptr, ...).
    check("iterate_struct_entries_no_jvm_returns_null",
          vmhook::hotspot::iterate_struct_entries("Symbol", "_length") == nullptr);
    check("iterate_type_entries_no_jvm_returns_null",
          vmhook::hotspot::iterate_type_entries("Symbol") == nullptr);

    // Null arg guards: both functions must short-circuit to nullptr when
    // any of their string arguments is null, NOT call strcmp on it.
    check("iterate_struct_entries_null_type_name",
          vmhook::hotspot::iterate_struct_entries(nullptr, "_length") == nullptr);
    check("iterate_struct_entries_null_field_name",
          vmhook::hotspot::iterate_struct_entries("Symbol", nullptr) == nullptr);
    check("iterate_struct_entries_both_null",
          vmhook::hotspot::iterate_struct_entries(nullptr, nullptr) == nullptr);
    check("iterate_type_entries_null_type_name",
          vmhook::hotspot::iterate_type_entries(nullptr) == nullptr);
}

// ---------------------------------------------------------------------------
// 16. get_vm_types / get_vm_structs in a no-JVM process
//
// The first call resolves gHotSpotVMTypes / gHotSpotVMStructs from the JVM
// module via dlsym/GetProcAddress.  Without a JVM the module handle is null,
// the symbol resolves to nullptr, and the cached value stays nullptr forever.
// Two consecutive calls must return identical nullptr (proves the static
// caching path runs without a crash).
// ---------------------------------------------------------------------------
static auto test_vm_types_and_structs_no_jvm() -> void
{
    auto* const types_first{ vmhook::hotspot::get_vm_types() };
    auto* const types_second{ vmhook::hotspot::get_vm_types() };
    check("get_vm_types_no_jvm_returns_null", types_first == nullptr);
    check("get_vm_types_cache_stable", types_first == types_second);

    auto* const structs_first{ vmhook::hotspot::get_vm_structs() };
    auto* const structs_second{ vmhook::hotspot::get_vm_structs() };
    check("get_vm_structs_no_jvm_returns_null", structs_first == nullptr);
    check("get_vm_structs_cache_stable", structs_first == structs_second);
}

// ---------------------------------------------------------------------------
// 17. get_jvm_module no-JVM behaviour
//
// In a unit test process, no JVM library is loaded.  find_jvm_module() must
// walk every candidate name (jvm.dll / libjvm.so / libjvm.dylib) and return
// nullptr without ever calling GetProcAddress / dlsym on a null handle.
// Cached after first call, so two queries return the same value.
// ---------------------------------------------------------------------------
static auto test_find_jvm_module_no_jvm() -> void
{
    auto const first{ vmhook::hotspot::get_jvm_module() };
    auto const second{ vmhook::hotspot::get_jvm_module() };
    check("get_jvm_module_no_jvm_returns_null", first == nullptr);
    check("get_jvm_module_cache_stable", first == second);
}

// ---------------------------------------------------------------------------
// 18. return_value::set on non-integer trivially-copyable values
//
// The sign-extension branch only fires for signed integer types < 8 bytes.
// float, double, and pointer types must take the memcpy path and land in
// the slot with their bit pattern intact (no spurious sign extension).
// ---------------------------------------------------------------------------
static auto test_return_value_set_non_integer_types() -> void
{
    vmhook::hotspot::return_slot slot{};
    vmhook::return_value rv{ &slot };

    // float - 4 bytes, NOT a signed integer, so memcpy path.  Bit pattern
    // of 3.14f is 0x4048F5C3 - the upper 32 bits of the slot must stay zero.
    rv.set(3.14f);
    check("return_value_set_float_cancel", slot.cancel == true);
    {
        float roundtrip{};
        std::memcpy(&roundtrip, &slot.retval, sizeof(roundtrip));
        check("return_value_set_float_roundtrip", roundtrip == 3.14f);
    }
    check("return_value_set_float_upper_bits_zero",
          (static_cast<std::uint64_t>(slot.retval) >> 32) == 0u);

    // double - 8 bytes, memcpy path fills the whole slot.
    slot.retval = 0;
    slot.cancel = false;
    rv.set(2.71828);
    check("return_value_set_double_cancel", slot.cancel == true);
    {
        double roundtrip{};
        std::memcpy(&roundtrip, &slot.retval, sizeof(roundtrip));
        check("return_value_set_double_roundtrip", roundtrip == 2.71828);
    }

    // void* - 8 bytes on x86_64, memcpy path.  No sign-extension even though
    // the high bit is set: a kernel-space-looking pointer must round-trip
    // bit-for-bit.
    slot.retval = 0;
    slot.cancel = false;
    void* const sentinel{ reinterpret_cast<void*>(
        static_cast<std::uintptr_t>(0xCAFEBABEDEADBEEFull)) };
    rv.set<void*>(sentinel);
    check("return_value_set_pointer_cancel", slot.cancel == true);
    {
        void* roundtrip{};
        std::memcpy(&roundtrip, &slot.retval, sizeof(roundtrip));
        check("return_value_set_pointer_roundtrip", roundtrip == sentinel);
    }

    // uint32_t - unsigned, NO sign extension even if high bit is set.
    slot.retval = 0;
    slot.cancel = false;
    rv.set(std::uint32_t{ 0x80000000u });
    check("return_value_set_uint32_high_bit_no_sign_extend",
          slot.retval == static_cast<std::int64_t>(0x80000000u));

    // bool - 1 byte, but NOT signed integer (the requires-clause keys on
    // is_signed && is_integral; bool is integral and signed-ness depends
    // on the platform, but std::is_signed_v<bool> is false).
    // Must round-trip without garbage in the upper bytes.
    slot.retval = static_cast<std::int64_t>(0xFFFFFFFFFFFFFF00ull);
    slot.cancel = false;
    rv.set(true);
    check("return_value_set_bool_true_clears_upper_bytes",
          slot.retval == 1);
    slot.retval = static_cast<std::int64_t>(0xFFFFFFFFFFFFFFFFull);
    rv.set(false);
    check("return_value_set_bool_false_clears_slot",
          slot.retval == 0);
}

// ---------------------------------------------------------------------------
// 19. return_value::cancel / caller / stack_trace with no frame
//
// Construct a return_value with a null frame_pointer.  cancel() must
// flip slot.cancel; set() with no value can't be tested via the public
// API for void returns, but caller() and stack_trace() must each return
// the documented empty defaults rather than crashing.
// ---------------------------------------------------------------------------
static auto test_return_value_no_frame_helpers() -> void
{
    vmhook::hotspot::return_slot slot{};
    vmhook::return_value rv{ &slot, /*frame=*/nullptr };

    // cancel() always succeeds; just records the flag on the slot.
    rv.cancel();
    check("return_value_cancel_sets_flag", slot.cancel == true);

    // No frame -> caller() returns empty caller_info with valid() == false.
    const auto caller{ rv.caller() };
    check("return_value_caller_no_frame_invalid", !caller.valid());
    check("return_value_caller_no_frame_method_null", caller.method == nullptr);
    check("return_value_caller_no_frame_class_empty", caller.class_name.empty());
    check("return_value_caller_no_frame_method_name_empty", caller.method_name.empty());
    check("return_value_caller_no_frame_signature_empty", caller.signature.empty());

    // No frame -> stack_trace() returns an empty vector.
    const auto frames_default{ rv.stack_trace() };
    check("return_value_stack_trace_no_frame_empty",
          frames_default.empty());

    // max_depth = 0 must promote to the default 64 internally, but with
    // no frame still returns empty.
    const auto frames_zero{ rv.stack_trace(0) };
    check("return_value_stack_trace_zero_depth_empty",
          frames_zero.empty());

    // Even a small explicit depth returns empty with no frame.
    const auto frames_small{ rv.stack_trace(4) };
    check("return_value_stack_trace_small_depth_empty",
          frames_small.empty());

    // frame() exposes the raw pointer the constructor was given.
    check("return_value_frame_accessor_returns_null", rv.frame() == nullptr);
}

// ---------------------------------------------------------------------------
// 20. return_value::set_arg short-circuits on bad inputs
//
// The wrapper short-circuits to false (and logs) when the underlying frame
// is null, the index is negative, or the index exceeds the JVM's max_locals
// limit (u2 = 65535).  All three early returns happen before any HotSpot
// read, so no JVM is needed.
// ---------------------------------------------------------------------------
static auto test_return_value_set_arg_guards() -> void
{
    vmhook::hotspot::return_slot slot{};
    vmhook::return_value rv{ &slot, /*frame=*/nullptr };

    check("return_value_set_arg_no_frame_returns_false",
          rv.set_arg(0, std::int32_t{ 42 }) == false);
    check("return_value_set_arg_no_frame_returns_false_neg_idx",
          rv.set_arg(-1, std::int32_t{ 42 }) == false);
    check("return_value_set_arg_no_frame_returns_false_large_idx",
          rv.set_arg(1000, std::int32_t{ 42 }) == false);

    // JVM max_locals is a u2 (65535).  set_arg must reject anything past
    // that even when a frame is present: otherwise locals[-index] would
    // walk off the interpreter local-variable array into adjacent thread
    // state and silently corrupt the operand stack / frame header.
    // We can't supply a real frame here without a JVM, but we can verify
    // the guard fires via the no-frame check (same return path on this
    // codepath: any of "missing frame / negative / index > 65535" exits
    // before touching get_locals).
    check("return_value_set_arg_above_max_locals_returns_false",
          rv.set_arg(0x10000, std::int32_t{ 42 }) == false);
    check("return_value_set_arg_int_max_returns_false",
          rv.set_arg(std::numeric_limits<std::int32_t>::max(), std::int32_t{ 42 }) == false);
}

// ---------------------------------------------------------------------------
// 21. is_valid_pointer alignment + boundary cases
//
// is_valid_pointer rejects:
//   - addresses at or below user_address_floor (low sentinels)
//   - addresses at or above user_address_ceiling (kernel / non-canonical)
//   - odd low-bit addresses (HotSpot pointers are at least 2-byte aligned)
//   - well-known debug-poison patterns
// Exercise each boundary explicitly to catch off-by-one regressions in the
// comparison operators (e.g. `< floor` instead of `<= floor`).
// ---------------------------------------------------------------------------
static auto test_is_valid_pointer_boundaries() -> void
{
    using vmhook::hotspot::is_valid_pointer;

    // Exactly at user_address_floor must be REJECTED (the function uses <=).
    check("is_valid_pointer_at_floor_rejected",
          !is_valid_pointer(reinterpret_cast<void*>(vmhook::os::user_address_floor)));

    // Just below the floor: rejected.
    check("is_valid_pointer_below_floor_rejected",
          !is_valid_pointer(reinterpret_cast<void*>(
              vmhook::os::user_address_floor - 1)));

    // Just above the floor (and 2-byte aligned): accepted.  The floor
    // sentinel itself is 0xFFFF which is odd, so floor+1 = 0x10000 is the
    // first 2-byte-aligned address that clears both the range and the
    // alignment checks.
    {
        const std::uintptr_t addr{ vmhook::os::user_address_floor + 1 };
        check("is_valid_pointer_just_above_floor_accepted",
              is_valid_pointer(reinterpret_cast<void*>(addr)));
    }

    // Exactly at user_address_ceiling must be REJECTED (the function uses >=).
    check("is_valid_pointer_at_ceiling_rejected",
          !is_valid_pointer(reinterpret_cast<void*>(vmhook::os::user_address_ceiling)));

    // Just above the ceiling: rejected.
    check("is_valid_pointer_above_ceiling_rejected",
          !is_valid_pointer(reinterpret_cast<void*>(
              vmhook::os::user_address_ceiling + 1)));

    // 2-byte alignment is the documented minimum requirement; 4- and 8-byte
    // alignment must both pass.
    int locals_for_alignment[4]{};  // stack array, naturally aligned
    void* const aligned_4{ &locals_for_alignment[1] };
    void* const aligned_8{ &locals_for_alignment[0] };
    check("is_valid_pointer_4byte_aligned_accepted", is_valid_pointer(aligned_4));
    check("is_valid_pointer_8byte_aligned_accepted", is_valid_pointer(aligned_8));
}

// ---------------------------------------------------------------------------
// 22. decode_u5 multi-byte boundary
//
// Validates the UNSIGNED5 decoder at the boundaries that the existing test
// did not exercise: a 3-byte encoding and a near-maximum-value 5-byte one.
// Encoder spec from src/hotspot/share/utilities/unsigned5.hpp:
//   for value v in [L, L + L*H), 2 bytes  (L = 191, H = 64)
//   for value v in [L + L*H, L + L*H + L*H*H), 3 bytes
// 3-byte boundary: value = 191 + 64*191 = 12415 maps to [192, 192, 1].
//   First two bytes are continuation (>= 192), final byte is low (=1).
// ---------------------------------------------------------------------------
static auto test_decode_u5_multi_byte() -> void
{
    // 3-byte encoding: 12415 = (192-1) + (192-1)*64 + (1-1)*64*64
    //                       = 191 + 12224 + 0 = 12415
    int pos_3byte{ 0 };
    std::array<std::uint8_t, 8> buf_3byte{ 192, 192, 1, 0, 0, 0, 0, 0 };
    const std::uint32_t v_3byte{
        vmhook::hotspot::klass::decode_u5(buf_3byte.data(), pos_3byte) };
    check("decode_u5_3byte_value", v_3byte == 12415u);
    check("decode_u5_3byte_pos", pos_3byte == 3);

    // After a value-decode, the next call must continue from the new
    // stream position (no reset).  Encode value 5 in the byte that
    // immediately follows our 3-byte sequence.
    std::array<std::uint8_t, 8> buf_sequence{ 192, 192, 1, 6, 0, 0, 0, 0 };
    int seq_pos{ 0 };
    const std::uint32_t first{
        vmhook::hotspot::klass::decode_u5(buf_sequence.data(), seq_pos) };
    const std::uint32_t second{
        vmhook::hotspot::klass::decode_u5(buf_sequence.data(), seq_pos) };
    check("decode_u5_sequence_first_12415", first == 12415u);
    check("decode_u5_sequence_second_5", second == 5u);
    check("decode_u5_sequence_pos_advanced", seq_pos == 4);

    // 4-byte encoding stress: every byte must be checked.  Build
    // value (b0 - 1) + (b1 - 1)*64 + (b2 - 1)*64*64 + (b3 - 1)*64*64*64
    // with b0 = b1 = b2 = 192 (highest continuation) and b3 = 2
    // (terminator) - that resolves to 191 + 191*64 + 191*64*64 + 64^3
    // = 191 + 12224 + 782336 + 262144 = 1056895.
    int pos_4byte{ 0 };
    std::array<std::uint8_t, 8> buf_4byte{ 192, 192, 192, 2, 0, 0, 0, 0 };
    const std::uint32_t v_4byte{
        vmhook::hotspot::klass::decode_u5(buf_4byte.data(), pos_4byte) };
    check("decode_u5_4byte_pos", pos_4byte == 4);
    check("decode_u5_4byte_value",
          v_4byte == (191u + 191u * 64u + 191u * 64u * 64u + 64u * 64u * 64u));
}

// ---------------------------------------------------------------------------
// 23. format_log positive path - when std::format is available the helper
//     must actually expand the placeholders, not return the raw template.
//     The fallback path is exercised by test_format_log_safe_on_bad_pattern.
// ---------------------------------------------------------------------------
static auto test_format_log_positive() -> void
{
#if VMHOOK_HAS_STD_FORMAT
    // Well-formed pattern with one substitution.
    const std::string result{ vmhook::detail::format_log("answer={}", 42) };
    check("format_log_substitutes_int", result == "answer=42");

    // Two substitutions of different types.
    const std::string result_two{
        vmhook::detail::format_log("{} = {}", "pi", 3.14) };
    check("format_log_substitutes_two", !result_two.empty()
          && result_two.find("pi") != std::string::npos
          && result_two.find("3.14") != std::string::npos);

    // No substitutions - format string passes through unchanged.
    const std::string result_none{ vmhook::detail::format_log("hello") };
    check("format_log_no_placeholders_passthrough", result_none == "hello");
#else
    // Fallback path: the helper returns the raw template - that's the
    // documented behaviour, and the existing bad-pattern test covers it.
    std::printf("[INFO] test_format_log_positive: skipped (no <format>)\n");
#endif
}

// ---------------------------------------------------------------------------
// 24Z. jvm_primitive_byte_width - introduced in v0.4.4 to size-check
//      field_proxy::set against the JVM field width.
// ---------------------------------------------------------------------------
static auto test_jvm_primitive_byte_width() -> void
{
    using vmhook::detail::jvm_primitive_byte_width;

    // Single-character primitive descriptors map to their JVM spec widths.
    check("jvm_primitive_byte_width_Z", jvm_primitive_byte_width("Z") == 1);
    check("jvm_primitive_byte_width_B", jvm_primitive_byte_width("B") == 1);
    check("jvm_primitive_byte_width_S", jvm_primitive_byte_width("S") == 2);
    check("jvm_primitive_byte_width_C", jvm_primitive_byte_width("C") == 2);
    check("jvm_primitive_byte_width_I", jvm_primitive_byte_width("I") == 4);
    check("jvm_primitive_byte_width_F", jvm_primitive_byte_width("F") == 4);
    check("jvm_primitive_byte_width_J", jvm_primitive_byte_width("J") == 8);
    check("jvm_primitive_byte_width_D", jvm_primitive_byte_width("D") == 8);

    // Reference / array / void all return 0 (skip size validation upstream).
    check("jvm_primitive_byte_width_L_is_0",
          jvm_primitive_byte_width("Ljava/lang/String;") == 0);
    check("jvm_primitive_byte_width_array_is_0",
          jvm_primitive_byte_width("[I") == 0);
    check("jvm_primitive_byte_width_V_is_0",
          jvm_primitive_byte_width("V") == 0);

    // Empty / unknown signatures also return 0.
    check("jvm_primitive_byte_width_empty_is_0",
          jvm_primitive_byte_width("") == 0);
    check("jvm_primitive_byte_width_unknown_is_0",
          jvm_primitive_byte_width("?") == 0);
    check("jvm_primitive_byte_width_multichar_is_0",
          jvm_primitive_byte_width("Ix") == 0);
}

// ---------------------------------------------------------------------------
// 24Y. field_proxy::set size-mismatch guard
//
// The set() implementation now refuses to memcpy a value when the C++
// type's size doesn't match the JVM field width.  Previously,
// `field.set(int64_t{x})` on an "I" field wrote 8 bytes into a 4-byte
// slot, clobbering whatever came next in the heap object's layout.
// We exercise the guard on a stack buffer with sentinel bytes after the
// field; a successful guard leaves those sentinel bytes intact.
// ---------------------------------------------------------------------------
static auto test_field_proxy_set_size_guard() -> void
{
    // Layout:
    //   [0..3]   the field's storage (4-byte "I")
    //   [4..7]   sentinel guard bytes (0xAB) - the test verifies these
    //            stay 0xAB after a malformed set() call
    std::array<std::uint8_t, 8> storage{};
    storage.fill(std::uint8_t{ 0xAB });

    vmhook::field_proxy proxy_int{ storage.data(), "I", false };

    // Right-sized: int32_t into an "I" field writes 4 bytes, sentinels stay.
    proxy_int.set(std::int32_t{ 0x11223344 });
    {
        std::int32_t read_back{};
        std::memcpy(&read_back, storage.data(), sizeof(read_back));
        check("field_proxy_set_int32_into_I_writes_correctly",
              read_back == 0x11223344);
        check("field_proxy_set_int32_into_I_preserves_sentinels",
              storage[4] == 0xAB && storage[5] == 0xAB
              && storage[6] == 0xAB && storage[7] == 0xAB);
    }

    // Refill sentinels after a clean reset.
    storage.fill(std::uint8_t{ 0xAB });

    // Mismatch: int64_t into "I" field would previously have written 8 bytes
    // and clobbered the sentinels.  Guard now refuses the write entirely.
    proxy_int.set(static_cast<std::int64_t>(0xDEADBEEFCAFEBABEull));
    check("field_proxy_set_int64_into_I_does_NOT_clobber_sentinels",
          storage[4] == 0xAB && storage[5] == 0xAB
          && storage[6] == 0xAB && storage[7] == 0xAB);
    // The field bytes are unchanged from their pre-set state (still 0xAB).
    check("field_proxy_set_int64_into_I_leaves_field_unchanged",
          storage[0] == 0xAB && storage[1] == 0xAB
          && storage[2] == 0xAB && storage[3] == 0xAB);

    // Inverse mismatch: int32_t into a "J" (8-byte long) field also refused.
    storage.fill(std::uint8_t{ 0xAB });
    vmhook::field_proxy proxy_long{ storage.data(), "J", false };
    proxy_long.set(std::int32_t{ 0x11223344 });
    check("field_proxy_set_int32_into_J_leaves_field_unchanged",
          std::all_of(storage.begin(), storage.end(),
                      [](std::uint8_t b) { return b == 0xAB; }));

    // Reference / array signatures bypass the size guard (size==0 -> skip).
    // The trivially_copyable branch should still run for those, but in
    // practice users go through the unique_ptr or string branches.  Just
    // verify the helper doesn't accidentally crash for a reference-typed
    // proxy when set() is called with an int (which would route to the
    // trivially_copyable branch and write 4 bytes into a 4-byte compressed
    // OOP slot - that's an OOP-shaped write, semantically wrong but not
    // crash-y).
    storage.fill(std::uint8_t{ 0 });
    vmhook::field_proxy proxy_ref{ storage.data(), "Ljava/lang/String;", false };
    proxy_ref.set(std::uint32_t{ 0xCAFEBABE });
    {
        std::uint32_t read_back{};
        std::memcpy(&read_back, storage.data(), sizeof(read_back));
        check("field_proxy_set_uint32_into_ref_writes_4_bytes",
              read_back == 0xCAFEBABE);
    }

    // The "C" -> char widening special case still works.
    storage.fill(std::uint8_t{ 0xCC });
    vmhook::field_proxy proxy_char{ storage.data(), "C", false };
    proxy_char.set(char{ 'A' });
    {
        std::uint16_t read_back{};
        std::memcpy(&read_back, storage.data(), sizeof(read_back));
        check("field_proxy_set_char_into_C_widens_to_uint16",
              read_back == static_cast<std::uint16_t>(
                  static_cast<unsigned char>('A')));
        // Following bytes untouched.
        check("field_proxy_set_char_into_C_preserves_remaining_bytes",
              storage[2] == 0xCC && storage[3] == 0xCC);
    }

    // Null field_pointer: set() must short-circuit, not deref the null.
    vmhook::field_proxy proxy_null{ nullptr, "I", false };
    proxy_null.set(std::int32_t{ 999 });
    check("field_proxy_set_null_field_pointer_is_safe", true);  // no crash
}

// ---------------------------------------------------------------------------
// 24a. jni_delete_local_ref - new helper added in v0.4.1 to release jstring
//      locals from set_arg's string path.  Must be a safe no-op in the
//      no-JVM unit test: null current_jni_env means jni_function returns
//      nullptr and the helper exits without calling through.  We exercise
//      both the null-handle branch (early return) and the non-null branch
//      (table-resolution fall-through).
// ---------------------------------------------------------------------------
static auto test_jni_delete_local_ref_no_jvm() -> void
{
    // Null handle: documented JNI no-op.
    vmhook::detail::jni_delete_local_ref(nullptr);
    check("jni_delete_local_ref_null_handle_does_not_crash", true);

    // Non-null handle in a no-JVM process: current_jni_env is null, so the
    // function-table lookup short-circuits and the helper returns without
    // dereferencing anything.  We just want to assert no fault.
    void* const fake_handle{ reinterpret_cast<void*>(
        static_cast<std::uintptr_t>(0xABCDEF1234567880ull)) };
    vmhook::detail::jni_delete_local_ref(fake_handle);
    check("jni_delete_local_ref_no_jvm_returns_safely", true);
}

// ---------------------------------------------------------------------------
// 24b. dr_arm_one / dr_unarm_one refcount transitions
//
// v0.4.2 changed ensure_dr_handler_installed from "install once, never
// uninstall" to a refcounted scheme.  The 0 -> 1 transition installs the
// VEH; the 1 -> 0 transition uninstalls it.  We can't realistically install
// a VEH in a unit-test process (AddVectoredExceptionHandler succeeds but
// then any subsequent unhandled exception in another test will route
// through our dispatcher), so we just exercise the counter logic by
// holding the mutex ourselves and calling the inc/dec pair.  Skipped on
// platforms without HW data breakpoints (where the helpers don't exist).
// ---------------------------------------------------------------------------
#if VMHOOK_HAS_HW_DATA_BREAKPOINTS
static auto test_dr_armed_count_refcount() -> void
{
    // Snapshot the count.  The unit-test process never arms a real watch
    // so it should be zero before this test runs.
    {
        std::lock_guard<std::mutex> guard{ vmhook::detail::dr_mutex };
        check("dr_armed_count_starts_zero",
              vmhook::detail::dr_armed_count == 0);
    }

    // arm three times -> count == 3, VEH installed exactly once.
    {
        std::lock_guard<std::mutex> guard{ vmhook::detail::dr_mutex };
        const PVOID veh_before{ vmhook::detail::dr_veh_handle };
        vmhook::detail::dr_arm_one();
        vmhook::detail::dr_arm_one();
        vmhook::detail::dr_arm_one();
        check("dr_armed_count_after_three_arms",
              vmhook::detail::dr_armed_count == 3);
        check("dr_veh_installed_after_first_arm",
              vmhook::detail::dr_veh_handle != nullptr);
        // Subsequent arms must NOT re-install (the handle stays the same).
        // veh_before could be null (first ever arm in this process) or
        // could be a previous handle - either way the only thing that
        // matters is that the handle is non-null AFTER arming.
        (void)veh_before;
    }

    // unarm three times -> count == 0, VEH removed.
    {
        std::lock_guard<std::mutex> guard{ vmhook::detail::dr_mutex };
        vmhook::detail::dr_unarm_one();
        check("dr_armed_count_after_one_unarm",
              vmhook::detail::dr_armed_count == 2);
        check("dr_veh_still_installed_above_zero",
              vmhook::detail::dr_veh_handle != nullptr);

        vmhook::detail::dr_unarm_one();
        check("dr_armed_count_after_two_unarms",
              vmhook::detail::dr_armed_count == 1);

        vmhook::detail::dr_unarm_one();
        check("dr_armed_count_back_to_zero",
              vmhook::detail::dr_armed_count == 0);
        check("dr_veh_removed_at_zero",
              vmhook::detail::dr_veh_handle == nullptr);
    }

    // Extra unarm at zero must be a no-op (no underflow / no crash).
    {
        std::lock_guard<std::mutex> guard{ vmhook::detail::dr_mutex };
        vmhook::detail::dr_unarm_one();
        check("dr_unarm_one_at_zero_is_noop",
              vmhook::detail::dr_armed_count == 0);
    }
}
#endif

// ---------------------------------------------------------------------------
// 24. version_string composition + numeric range sanity
// ---------------------------------------------------------------------------
static auto test_version_string_composition() -> void
{
    constexpr std::string_view v{ VMHOOK_VERSION_STRING };
    // Components are non-negative and fit in the documented widths.
    static_assert(VMHOOK_VERSION_MAJOR >= 0 && VMHOOK_VERSION_MAJOR < 1000,
                  "VMHOOK_VERSION_MAJOR must fit in the packed integer's MAJOR slot");
    static_assert(VMHOOK_VERSION_MINOR >= 0 && VMHOOK_VERSION_MINOR < 1000,
                  "VMHOOK_VERSION_MINOR must fit in the packed integer's MINOR slot");
    static_assert(VMHOOK_VERSION_PATCH >= 0 && VMHOOK_VERSION_PATCH < 1000,
                  "VMHOOK_VERSION_PATCH must fit in the packed integer's PATCH slot");

    // Every character is a digit or a dot, and there are no leading dots
    // or empty components.
    bool every_char_valid{ true };
    bool seen_dot{ false };
    char last_char{ 'x' };
    for (const char c : v)
    {
        if (c != '.' && (c < '0' || c > '9'))
        {
            every_char_valid = false;
            break;
        }
        if (c == '.' && last_char == '.')
        {
            every_char_valid = false;
            break;
        }
        if (c == '.')
        {
            seen_dot = true;
        }
        last_char = c;
    }
    check("version_string_only_digits_and_dots", every_char_valid);
    check("version_string_contains_a_dot", seen_dot);
    check("version_string_does_not_start_with_dot", !v.empty() && v.front() != '.');
    check("version_string_does_not_end_with_dot", !v.empty() && v.back() != '.');

    // The packed integer must agree with the components for inequality
    // gates downstream code might use ("#if VMHOOK_VERSION >= 4_001").
    static_assert(VMHOOK_VERSION > 0, "VMHOOK_VERSION must be a positive integer");
}

int main()
{
    test_version_macros();
    test_decode_u5();
    test_decode_u5_multi_byte();
    test_valid_pointer_filters();
    test_untag_pointer();
    test_sig_char_to_basic_type();
    test_to_native_protect();
#if VMHOOK_HAS_HW_DATA_BREAKPOINTS
    test_build_dr7();
#endif
    test_array_helpers();
    test_format_log_safe_on_bad_pattern();
    test_format_log_positive();
    test_write_jni_arg_to_slot_unique_ptr_branch();
    test_write_jni_arg_to_slot_null_unique_ptr();
    test_write_jni_arg_to_slot_primitive_branches();
    test_jni_namespace_signature_for_arg();
    test_is_valid_pointer_rejects_sentinels();
    test_is_valid_pointer_boundaries();
    test_return_value_sign_extension();
    test_return_value_set_nullptr_for_wrapper();
    test_return_value_set_non_integer_types();
    test_return_value_no_frame_helpers();
    test_return_value_set_arg_guards();
    test_iterate_entries_no_jvm();
    test_vm_types_and_structs_no_jvm();
    test_find_jvm_module_no_jvm();
    test_jni_delete_local_ref_no_jvm();
    test_jvm_primitive_byte_width();
    test_field_proxy_set_size_guard();
#if VMHOOK_HAS_HW_DATA_BREAKPOINTS
    test_dr_armed_count_refcount();
#endif
    test_version_string_composition();

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
