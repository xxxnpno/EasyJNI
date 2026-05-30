// Standalone unit test: field_proxy::set size-mismatch + non-primitive guards (no JVM).
//
// Covers the pure-logic / null-safety half of three audit findings:
//   audit/findings/field_proxy_set_size_guard.md
//   audit/findings/field_proxy_string_set.md
//   audit/findings/field_proxy_array_primitives.md
//
// field_proxy::set's primitive arm is a plain memcpy gated by
// vmhook::detail::jvm_primitive_byte_width, and the non-primitive arms are
// guarded by the same width oracle (returning early for genuine primitive
// signatures Z/B/C/S/I/J/F/D).  Both guards run entirely on a caller-supplied
// raw pointer, so we exercise them over a stack buffer with sentinel bytes and
// never touch a live oop or a running JVM.
//
// OUT OF SCOPE for this file (needs a live oop / running JVM, covered by JVM
// integration in vmhook/src/example.cpp):
//   * actual primitive write landing in a real Java field,
//   * set_str_field / set_prim_array success paths (they decode a compressed
//     OOP and mutate a real Java backing array),
//   * the value_t -> std::vector<T> read path and its width/length guards,
//   * unique_ptr<wrapper> success path (encodes a real OOP).
// Here we only assert that the guards reject mistyped writes and leave the
// caller's buffer (and its sentinels) untouched.

#include <vmhook/vmhook.hpp>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <array>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

static int failures{ 0 };
static auto check(const char* name, bool ok) -> void
{
    std::printf("%s %s\n", ok ? "[PASS]" : "[FAIL]", name);
    if (!ok) { ++failures; }
}

// A minimal wrapper so std::unique_ptr<test_wrapper> satisfies
// detail::is_unique_ptr_v AND so field_proxy::set's unique_ptr branch (which
// calls value->object_base::get_instance()) instantiates.  The guard short-
// circuits before that branch runs for a primitive signature, so a null
// unique_ptr never gets dereferenced.
struct test_wrapper : vmhook::object_base
{
    using vmhook::object_base::object_base;
};

// A 32-byte stack canvas: 8 leading sentinel bytes, an 8-byte field slot, then
// 16 trailing sentinel bytes.  Wider-than-field writes that slipped past the
// guard would smash the trailing sentinels; the guard must keep them intact.
namespace
{
    constexpr std::size_t k_lead{ 8 };
    constexpr std::size_t k_slot{ 8 };
    constexpr std::size_t k_trail{ 16 };
    constexpr std::uint8_t k_sentinel{ 0xCD };

    struct canvas
    {
        std::array<std::uint8_t, k_lead + k_slot + k_trail> bytes{};

        canvas() { bytes.fill(k_sentinel); }

        auto field_ptr() -> void* { return bytes.data() + k_lead; }

        // True if every byte outside the [k_lead, k_lead + k_slot) field slot
        // still holds the sentinel value.
        auto sentinels_intact() const -> bool
        {
            for (std::size_t i{ 0 }; i < bytes.size(); ++i)
            {
                if (i >= k_lead && i < k_lead + k_slot) { continue; }
                if (bytes[i] != k_sentinel) { return false; }
            }
            return true;
        }

        // True if the field slot still holds all-sentinel bytes (i.e. the write
        // was rejected and nothing landed in the slot either).
        auto slot_intact() const -> bool
        {
            for (std::size_t i{ k_lead }; i < k_lead + k_slot; ++i)
            {
                if (bytes[i] != k_sentinel) { return false; }
            }
            return true;
        }
    };
}

int main()
{
    using vmhook::detail::jvm_primitive_byte_width;

    // ----------------------------------------------------------------------
    // jvm_primitive_byte_width: the width oracle both guards consult.
    // Z/B == 1, S/C == 2, I/F == 4, J/D == 8; everything else == 0.
    // ----------------------------------------------------------------------
    check("width_boolean_Z_is_1", jvm_primitive_byte_width("Z") == 1);
    check("width_byte_B_is_1", jvm_primitive_byte_width("B") == 1);
    check("width_short_S_is_2", jvm_primitive_byte_width("S") == 2);
    check("width_char_C_is_2", jvm_primitive_byte_width("C") == 2);
    check("width_int_I_is_4", jvm_primitive_byte_width("I") == 4);
    check("width_float_F_is_4", jvm_primitive_byte_width("F") == 4);
    check("width_long_J_is_8", jvm_primitive_byte_width("J") == 8);
    check("width_double_D_is_8", jvm_primitive_byte_width("D") == 8);
    check("width_reference_L_is_0", jvm_primitive_byte_width("Ljava/lang/String;") == 0);
    check("width_array_bracket_is_0", jvm_primitive_byte_width("[I") == 0);
    check("width_void_V_is_0", jvm_primitive_byte_width("V") == 0);
    check("width_empty_is_0", jvm_primitive_byte_width("") == 0);
    check("width_unknown_X_is_0", jvm_primitive_byte_width("X") == 0);
    check("width_multichar_II_is_0", jvm_primitive_byte_width("II") == 0);

    // ----------------------------------------------------------------------
    // Right-sized primitive writes succeed: the exact-width C++ value lands in
    // the field slot and never disturbs the surrounding sentinels.
    // ----------------------------------------------------------------------
    {
        canvas c;
        vmhook::field_proxy proxy{ c.field_ptr(), "Z", false };
        proxy.set(std::uint8_t{ 0x01 });
        check("set_Z_right_size_writes_low_byte", c.bytes[k_lead] == 0x01);
        check("set_Z_right_size_keeps_sentinels", c.sentinels_intact());
    }
    {
        canvas c;
        vmhook::field_proxy proxy{ c.field_ptr(), "B", false };
        proxy.set(std::int8_t{ -2 });
        check("set_B_right_size_writes_byte", c.bytes[k_lead] == 0xFE);
        check("set_B_right_size_keeps_sentinels", c.sentinels_intact());
    }
    {
        canvas c;
        vmhook::field_proxy proxy{ c.field_ptr(), "S", false };
        proxy.set(std::int16_t{ 0x1234 });
        std::int16_t read{};
        std::memcpy(&read, c.field_ptr(), sizeof(read));
        check("set_S_right_size_writes_2_bytes", read == std::int16_t{ 0x1234 });
        check("set_S_right_size_keeps_sentinels", c.sentinels_intact());
    }
    {
        canvas c;
        vmhook::field_proxy proxy{ c.field_ptr(), "I", false };
        proxy.set(std::int32_t{ 0x0BADF00D });
        std::int32_t read{};
        std::memcpy(&read, c.field_ptr(), sizeof(read));
        check("set_I_right_size_writes_4_bytes", read == std::int32_t{ 0x0BADF00D });
        check("set_I_right_size_keeps_sentinels", c.sentinels_intact());
    }
    {
        canvas c;
        vmhook::field_proxy proxy{ c.field_ptr(), "J", false };
        proxy.set(std::int64_t{ 0x0123456789ABCDEF });
        std::int64_t read{};
        std::memcpy(&read, c.field_ptr(), sizeof(read));
        check("set_J_right_size_writes_8_bytes", read == std::int64_t{ 0x0123456789ABCDEF });
        check("set_J_right_size_keeps_sentinels", c.sentinels_intact());
    }
    {
        canvas c;
        vmhook::field_proxy proxy{ c.field_ptr(), "F", false };
        proxy.set(float{ 3.5F });
        float read{};
        std::memcpy(&read, c.field_ptr(), sizeof(read));
        check("set_F_right_size_writes_4_bytes", read == 3.5F);
        check("set_F_right_size_keeps_sentinels", c.sentinels_intact());
    }
    {
        canvas c;
        vmhook::field_proxy proxy{ c.field_ptr(), "D", false };
        proxy.set(double{ 2.25 });
        double read{};
        std::memcpy(&read, c.field_ptr(), sizeof(read));
        check("set_D_right_size_writes_8_bytes", read == 2.25);
        check("set_D_right_size_keeps_sentinels", c.sentinels_intact());
    }

    // ----------------------------------------------------------------------
    // Wrong-size writes are refused and leave BOTH the slot and the sentinels
    // untouched.  Covers too-wide (would clobber adjacent fields) and too-narrow
    // (would leave a stale tail) mismatches across the width classes.
    // ----------------------------------------------------------------------
    {
        // int64 -> "I": 8 bytes into a 4-byte field. The headline too-wide case.
        canvas c;
        vmhook::field_proxy proxy{ c.field_ptr(), "I", false };
        proxy.set(std::int64_t{ 0x1122334455667788 });
        check("set_I_rejects_int64_slot_intact", c.slot_intact());
        check("set_I_rejects_int64_sentinels_intact", c.sentinels_intact());
    }
    {
        // int32 -> "J": 4 bytes into an 8-byte field (too narrow).
        canvas c;
        vmhook::field_proxy proxy{ c.field_ptr(), "J", false };
        proxy.set(std::int32_t{ 0x12345678 });
        check("set_J_rejects_int32_slot_intact", c.slot_intact());
        check("set_J_rejects_int32_sentinels_intact", c.sentinels_intact());
    }
    {
        // int32 -> "Z": 4 bytes into a 1-byte boolean field.
        canvas c;
        vmhook::field_proxy proxy{ c.field_ptr(), "Z", false };
        proxy.set(std::int32_t{ 0x7FFFFFFF });
        check("set_Z_rejects_int32_slot_intact", c.slot_intact());
        check("set_Z_rejects_int32_sentinels_intact", c.sentinels_intact());
    }
    {
        // int32 -> "B": 4 bytes into a 1-byte byte field.
        canvas c;
        vmhook::field_proxy proxy{ c.field_ptr(), "B", false };
        proxy.set(std::int32_t{ 0x44332211 });
        check("set_B_rejects_int32_slot_intact", c.slot_intact());
        check("set_B_rejects_int32_sentinels_intact", c.sentinels_intact());
    }
    {
        // int32 -> "S": 4 bytes into a 2-byte short field.
        canvas c;
        vmhook::field_proxy proxy{ c.field_ptr(), "S", false };
        proxy.set(std::int32_t{ 0x0000BEEF });
        check("set_S_rejects_int32_slot_intact", c.slot_intact());
        check("set_S_rejects_int32_sentinels_intact", c.sentinels_intact());
    }
    {
        // double -> "F": 8 bytes into a 4-byte float field.
        canvas c;
        vmhook::field_proxy proxy{ c.field_ptr(), "F", false };
        proxy.set(double{ 1.0 });
        check("set_F_rejects_double_slot_intact", c.slot_intact());
        check("set_F_rejects_double_sentinels_intact", c.sentinels_intact());
    }
    {
        // float -> "D": 4 bytes into an 8-byte double field.
        canvas c;
        vmhook::field_proxy proxy{ c.field_ptr(), "D", false };
        proxy.set(float{ 1.0F });
        check("set_D_rejects_float_slot_intact", c.slot_intact());
        check("set_D_rejects_float_sentinels_intact", c.sentinels_intact());
    }

    // ----------------------------------------------------------------------
    // "C" + 1-byte value widening shortcut: a 1-byte trivially-copyable value
    // passed to a 2-byte char field is zero-extended to 16 bits (high byte 0),
    // for ANY 1-byte arithmetic type — not just char.  Pins down the behaviour
    // so a future change can't tighten it to is_same_v<char> and break callers
    // passing signed/unsigned char.
    // ----------------------------------------------------------------------
    {
        canvas c;
        vmhook::field_proxy proxy{ c.field_ptr(), "C", false };
        proxy.set(char{ 'A' });
        std::uint16_t read{};
        std::memcpy(&read, c.field_ptr(), sizeof(read));
        check("set_C_char_widens_to_u16", read == std::uint16_t{ 0x0041 });
        check("set_C_char_high_byte_zero", c.bytes[k_lead + 1] == 0x00);
        check("set_C_char_keeps_sentinels", c.sentinels_intact());
    }
    {
        // int8_t{-1}: zero-extended via (unsigned char) cast -> 0x00FF, NOT 0xFFFF.
        canvas c;
        vmhook::field_proxy proxy{ c.field_ptr(), "C", false };
        proxy.set(std::int8_t{ -1 });
        std::uint16_t read{};
        std::memcpy(&read, c.field_ptr(), sizeof(read));
        check("set_C_int8_minus1_widens_to_00FF", read == std::uint16_t{ 0x00FF });
        check("set_C_int8_keeps_sentinels", c.sentinels_intact());
    }
    {
        // uint8_t{0xFF}: same 0x00FF result, confirming unsigned char works too.
        canvas c;
        vmhook::field_proxy proxy{ c.field_ptr(), "C", false };
        proxy.set(std::uint8_t{ 0xFF });
        std::uint16_t read{};
        std::memcpy(&read, c.field_ptr(), sizeof(read));
        check("set_C_uint8_FF_widens_to_00FF", read == std::uint16_t{ 0x00FF });
        check("set_C_uint8_keeps_sentinels", c.sentinels_intact());
    }
    {
        // A right-sized 2-byte value to "C" goes through the normal memcpy path
        // (not the widening shortcut) and lands verbatim.
        canvas c;
        vmhook::field_proxy proxy{ c.field_ptr(), "C", false };
        proxy.set(std::uint16_t{ 0x20AC });
        std::uint16_t read{};
        std::memcpy(&read, c.field_ptr(), sizeof(read));
        check("set_C_u16_right_size_writes_verbatim", read == std::uint16_t{ 0x20AC });
        check("set_C_u16_keeps_sentinels", c.sentinels_intact());
    }

    // ----------------------------------------------------------------------
    // Null field_pointer is a no-op in the trivially-copyable arm: the early
    // `if (!field_pointer) return;` guard means no write and no crash.
    // ----------------------------------------------------------------------
    {
        vmhook::field_proxy proxy{ nullptr, "I", false };
        proxy.set(std::int32_t{ 0x12345678 });   // must not crash / deref null
        check("set_null_field_pointer_no_op", true);
    }

    // ----------------------------------------------------------------------
    // NEW guard: string / string_view / const char* / vector / unique_ptr
    // writes into a PRIMITIVE field are refused before any OOP reinterpretation.
    // Each must leave the slot AND the sentinels untouched.  (On a real Java
    // String/array field the success path is exercised by JVM integration in
    // example.cpp; here the field is primitive so the guard fires.)
    // ----------------------------------------------------------------------
    {
        // std::string -> "I": must NOT reach set_str_field/field_oop/decode.
        canvas c;
        vmhook::field_proxy proxy{ c.field_ptr(), "I", false };
        proxy.set(std::string{ "42" });
        check("set_I_refuses_string_slot_intact", c.slot_intact());
        check("set_I_refuses_string_sentinels_intact", c.sentinels_intact());
    }
    {
        // const char* (string_view-convertible) -> "J".
        canvas c;
        vmhook::field_proxy proxy{ c.field_ptr(), "J", false };
        proxy.set("hello");
        check("set_J_refuses_cstr_slot_intact", c.slot_intact());
        check("set_J_refuses_cstr_sentinels_intact", c.sentinels_intact());
    }
    {
        // std::string_view -> "D".
        canvas c;
        vmhook::field_proxy proxy{ c.field_ptr(), "D", false };
        proxy.set(std::string_view{ "pi" });
        check("set_D_refuses_string_view_slot_intact", c.slot_intact());
        check("set_D_refuses_string_view_sentinels_intact", c.sentinels_intact());
    }
    {
        // std::vector<int> -> "I": the array branch (set_prim_array) must be skipped.
        canvas c;
        vmhook::field_proxy proxy{ c.field_ptr(), "I", false };
        proxy.set(std::vector<int>{ 1, 2, 3 });
        check("set_I_refuses_vector_int_slot_intact", c.slot_intact());
        check("set_I_refuses_vector_int_sentinels_intact", c.sentinels_intact());
    }
    {
        // std::vector<bool> -> "Z": the set_bool_array branch must be skipped.
        canvas c;
        vmhook::field_proxy proxy{ c.field_ptr(), "Z", false };
        proxy.set(std::vector<bool>{ true, false, true });
        check("set_Z_refuses_vector_bool_slot_intact", c.slot_intact());
        check("set_Z_refuses_vector_bool_sentinels_intact", c.sentinels_intact());
    }
    {
        // std::vector<std::string> -> "S": the set_str_array branch must be skipped.
        canvas c;
        vmhook::field_proxy proxy{ c.field_ptr(), "S", false };
        proxy.set(std::vector<std::string>{ "a", "b" });
        check("set_S_refuses_vector_string_slot_intact", c.slot_intact());
        check("set_S_refuses_vector_string_sentinels_intact", c.sentinels_intact());
    }
    {
        // std::unique_ptr<wrapper> -> "F": the compressed-OOP write must be skipped.
        // A null unique_ptr is enough — the guard fires before the branch runs,
        // so get_instance() / encode_oop_pointer are never reached.
        canvas c;
        vmhook::field_proxy proxy{ c.field_ptr(), "F", false };
        proxy.set(std::unique_ptr<test_wrapper>{});
        check("set_F_refuses_unique_ptr_slot_intact", c.slot_intact());
        check("set_F_refuses_unique_ptr_sentinels_intact", c.sentinels_intact());
    }

    return failures == 0 ? 0 : 1;
}
