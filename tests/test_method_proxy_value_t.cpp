// Standalone (no-JVM) unit tests for method_proxy::value_t variant conversions
// and method_proxy API-surface helpers (is_reference / is_static / raw_method /
// name / signature on a proxy built with a null Method*).
//
// SCOPE NOTE: This file runs with NO JVM in-process. It therefore only exercises
//   * value_t variant -> C++ type conversion (pure logic, the std::visit operator)
//   * method_proxy accessor helpers that do not touch the JVM
//   * the void*/compressed-OOP decode seam (decode_oop_pointer is null-safe with
//     no VMStructs: it returns nullptr instead of crashing, see vmhook.hpp:4229
//     and 4280-4283).
// Anything that needs a live oop or a running JVM (method_proxy::call(),
// call_jni(), real String/object/array decode, slot dispatch) is OUT OF SCOPE
// here and is covered by JVM integration in example.cpp.
//
// value_t alternatives (vmhook.hpp:11519-11531):
//   monostate, bool, int8_t, int16_t, int32_t, int64_t, float, double,
//   uint16_t, uint32_t (compressed OOP), std::string.
// value_t is an aggregate with a single std::variant member, so value_t{ X }
// aggregate-initialises the variant from X; the templated conversion operator
// is a member function and does not suppress aggregate init.

#include <vmhook/vmhook.hpp>
#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>

static int failures{ 0 };
static auto check(const char* name, bool ok) -> void
{
    std::printf("%s %s\n", ok ? "[PASS]" : "[FAIL]", name);
    if (!ok) { ++failures; }
}

using value_t = vmhook::method_proxy::value_t;

int main()
{
    // -------------------------------------------------------------------------
    // value_t: monostate -> default-constructed target (the void-return / failure
    // contract). monostate cannot be static_cast to an arithmetic/pointer type,
    // so the operator's `else` arm returns target_type{}.
    // -------------------------------------------------------------------------
    check("monostate_to_int_is_zero",
          static_cast<std::int32_t>(value_t{ std::monostate{} }) == 0);
    check("monostate_to_int64_is_zero",
          static_cast<std::int64_t>(value_t{ std::monostate{} }) == 0);
    check("monostate_to_float_is_zero",
          static_cast<float>(value_t{ std::monostate{} }) == 0.0f);
    check("monostate_to_double_is_zero",
          static_cast<double>(value_t{ std::monostate{} }) == 0.0);
    check("monostate_to_bool_is_false",
          static_cast<bool>(value_t{ std::monostate{} }) == false);
    check("monostate_to_voidptr_is_null",
          static_cast<void*>(value_t{ std::monostate{} }) == nullptr);
    {
        // monostate -> std::string falls into the default arm (empty string).
        // Use static_cast (not brace-init): with a *templated* conversion
        // operator, `std::string s{ value_t }` runs overload resolution over
        // every conversion the operator could produce (const char*, string_view,
        // ...) and picks a surprising one — a known C++ gotcha unrelated to the
        // operator's logic.  static_cast<std::string> names the target exactly.
        const auto s = value_t{ std::monostate{} }.as_string();
        check("monostate_to_string_is_empty", s.empty());
    }

    // -------------------------------------------------------------------------
    // value_t: primitive round-trip when target type matches the stored type.
    // -------------------------------------------------------------------------
    check("bool_true_round_trips",
          static_cast<bool>(value_t{ true }) == true);
    check("bool_false_round_trips",
          static_cast<bool>(value_t{ false }) == false);
    check("int32_round_trips",
          static_cast<std::int32_t>(value_t{ std::int32_t{ 123456 } }) == 123456);
    check("int64_round_trips",
          static_cast<std::int64_t>(value_t{ std::int64_t{ 9000000000LL } }) == 9000000000LL);
    check("uint16_round_trips",
          static_cast<std::uint16_t>(value_t{ std::uint16_t{ 0xBEEF } }) == 0xBEEF);
    check("float_round_trips",
          static_cast<float>(value_t{ 1.5f }) == 1.5f);
    check("double_round_trips",
          static_cast<double>(value_t{ 2.25 }) == 2.25);

    // -------------------------------------------------------------------------
    // value_t: signed narrow alternatives keep their sign through int conversion
    // (int8_t / int16_t hold -1, NOT 255 / 65535).
    // -------------------------------------------------------------------------
    check("int8_negative_one_sign_preserved",
          static_cast<std::int32_t>(value_t{ std::int8_t{ -1 } }) == -1);
    check("int16_negative_one_sign_preserved",
          static_cast<std::int32_t>(value_t{ std::int16_t{ -1 } }) == -1);
    check("int8_to_self_is_negative_one",
          static_cast<std::int8_t>(value_t{ std::int8_t{ -1 } }) == std::int8_t{ -1 });

    // -------------------------------------------------------------------------
    // value_t: cross-arithmetic static_cast path (stored float -> double, etc.).
    // -------------------------------------------------------------------------
    check("float_widens_to_double",
          static_cast<double>(value_t{ 1.5f }) == 1.5);
    check("int32_narrows_to_int8_wraps",
          static_cast<std::int8_t>(value_t{ std::int32_t{ 257 } }) == std::int8_t{ 1 });
    check("int64_truncates_to_int32",
          static_cast<std::int32_t>(value_t{ std::int64_t{ 0x1'0000'0001LL } }) == 1);

    // -------------------------------------------------------------------------
    // value_t: std::string alternative. The call_jni String path stores a
    // std::string directly; verify it converts back out unchanged, and that a
    // std::string stored value cannot be coerced to int (default 0 via the else
    // arm, since static_cast<int>(std::string) is ill-formed).
    // -------------------------------------------------------------------------
    {
        const auto s = value_t{ std::string{ "hello" } }.as_string();
        check("string_alternative_round_trips", s == "hello");
    }
    {
        const auto s = value_t{ std::string{ "" } }.as_string();
        check("empty_string_alternative_round_trips", s.empty());
    }
    check("string_alternative_to_int_is_zero",
          static_cast<std::int32_t>(value_t{ std::string{ "123" } }) == 0);
    check("string_alternative_to_voidptr_is_null",
          static_cast<void*>(value_t{ std::string{ "x" } }) == nullptr);

    // -------------------------------------------------------------------------
    // value_t: uint32_t (compressed-OOP) -> void* is routed through
    // decode_oop_pointer, NOT a plain static_cast.
    //
    // Proof that the special-case fires WITHOUT a JVM:
    //   * a plain static_cast<void*>(uint32_t{42}) would yield (void*)42,
    //   * decode_oop_pointer(42) with no VMStructs yields nullptr
    //     (vmhook.hpp:4280-4283), so the result is nullptr != (void*)42.
    // For the zero case decode_oop_pointer short-circuits to nullptr
    // (vmhook.hpp:4229).
    // -------------------------------------------------------------------------
    check("uint32_zero_to_voidptr_is_null",
          static_cast<void*>(value_t{ std::uint32_t{ 0 } }) == nullptr);
    check("uint32_nonzero_to_voidptr_uses_decode_not_truncation",
          static_cast<void*>(value_t{ std::uint32_t{ 42 } })
              != reinterpret_cast<void*>(static_cast<std::uintptr_t>(42)));
    check("uint32_nonzero_to_voidptr_matches_decode_oop_pointer",
          static_cast<void*>(value_t{ std::uint32_t{ 0xDEADBEEF } })
              == vmhook::hotspot::decode_oop_pointer(0xDEADBEEFu));
    // The uint32_t alternative still static_casts to integer targets normally
    // (only the void* target gets the decode treatment).
    check("uint32_to_int_is_plain_static_cast",
          static_cast<std::int64_t>(value_t{ std::uint32_t{ 42 } }) == 42);
    check("uint32_to_uint32_round_trips",
          static_cast<std::uint32_t>(value_t{ std::uint32_t{ 0xCAFEBABE } }) == 0xCAFEBABEu);

    // -------------------------------------------------------------------------
    // value_t: compile-time conversion-operator surface. These pin the
    // convertible target set so a future reshuffle of the variant alternatives
    // is caught at build time.
    // -------------------------------------------------------------------------
    static_assert(std::is_convertible_v<value_t, bool>,           "value_t -> bool");
    static_assert(std::is_convertible_v<value_t, std::int32_t>,   "value_t -> int32");
    static_assert(std::is_convertible_v<value_t, std::int64_t>,   "value_t -> int64");
    static_assert(std::is_convertible_v<value_t, float>,          "value_t -> float");
    static_assert(std::is_convertible_v<value_t, double>,         "value_t -> double");
    static_assert(std::is_convertible_v<value_t, void*>,          "value_t -> void*");
    static_assert(std::is_convertible_v<value_t, std::string>,    "value_t -> string");
    static_assert(std::is_convertible_v<value_t, std::uint16_t>,  "value_t -> uint16");
    check("value_t_compile_time_conversions_present", true);

    // -------------------------------------------------------------------------
    // method_proxy built with a NULL Method* (no JVM): accessor round-trips.
    // The (object, method*, signature) constructor just stores fields; it does
    // not touch the JVM, so this is safe with no VMStructs present.
    // -------------------------------------------------------------------------
    {
        // Reference (object) return type: "(I)Ljava/lang/String;"
        vmhook::method_proxy proxy{ nullptr, nullptr, std::string{ "(I)Ljava/lang/String;" } };

        check("null_method_name_is_empty", proxy.name().empty());
        check("null_method_signature_round_trips",
              proxy.signature() == std::string_view{ "(I)Ljava/lang/String;" });
        check("null_method_raw_method_is_null", proxy.raw_method() == nullptr);

        // is_static() reflects the constructor's hardcoded static_field=false,
        // independent of the null object pointer (documented contract).
        check("constructed_proxy_is_static_false", proxy.is_static() == false);

        // is_reference(): char after ')' is 'L' -> true.
        check("is_reference_true_for_L_return", proxy.is_reference() == true);
    }

    // is_reference(): array return "[" -> true.
    {
        vmhook::method_proxy proxy{ nullptr, nullptr, std::string{ "()[I" } };
        check("is_reference_true_for_array_return", proxy.is_reference() == true);
        check("array_proxy_signature_round_trips",
              proxy.signature() == std::string_view{ "()[I" });
    }

    // is_reference(): primitive return 'I' -> false.
    {
        vmhook::method_proxy proxy{ nullptr, nullptr, std::string{ "(I)I" } };
        check("is_reference_false_for_int_return", proxy.is_reference() == false);
    }

    // is_reference(): void return 'V' -> false.
    {
        vmhook::method_proxy proxy{ nullptr, nullptr, std::string{ "(I)V" } };
        check("is_reference_false_for_void_return", proxy.is_reference() == false);
    }

    // is_reference(): malformed signature (no ')') -> false, never throws.
    {
        vmhook::method_proxy proxy{ nullptr, nullptr, std::string{ "garbage" } };
        check("is_reference_false_for_malformed_signature", proxy.is_reference() == false);
        check("malformed_signature_round_trips",
              proxy.signature() == std::string_view{ "garbage" });
    }

    // is_reference(): empty signature -> false (find(')') == npos branch).
    {
        vmhook::method_proxy proxy{ nullptr, nullptr, std::string{ "" } };
        check("is_reference_false_for_empty_signature", proxy.is_reference() == false);
        check("empty_signature_name_still_empty", proxy.name().empty());
    }

    // is_reference(): trailing ')' with nothing after it -> false
    // (close + 1 >= size() guard).
    {
        vmhook::method_proxy proxy{ nullptr, nullptr, std::string{ "(I)" } };
        check("is_reference_false_for_truncated_after_paren", proxy.is_reference() == false);
    }

    return failures == 0 ? 0 : 1;
}
