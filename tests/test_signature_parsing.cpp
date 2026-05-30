// Standalone (no-JVM) unit test for vmhook's JVM-descriptor parsing helpers:
// detail::sig_char_to_basic_type, detail::jvm_primitive_byte_width,
// detail::jni_signature_for_arg<T>, and the inline return-descriptor extraction
// (the char after the close-paren) that method_proxy::call feeds into
// sig_char_to_basic_type.  These are all pure compile-time / table-lookup
// helpers: they touch no oop and no running JVM, so they are exhaustively
// testable here.  Anything that needs a live oop or interpreter (the actual
// resolve_compatible_method hierarchy walk, get_method(name, sig) against a
// real klass, method_proxy::call dispatch) is OUT OF SCOPE for this file and is
// covered by JVM integration in example.cpp.
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

// Mirror of the inline return-type extraction in method_proxy::call
// (vmhook.hpp ~12241-12245): find the last ')', the return descriptor is the
// single char immediately after it; when there is no ')' the contract is to
// treat the return type as void ('V').  The library has no *named* helper for
// this step — the BasicType lookup it performs IS detail::sig_char_to_basic_type
// — so we reproduce the one-line extraction and assert the composed result,
// exercising sig_char_to_basic_type through the exact code path call() uses.
static auto return_basic_type_of(std::string_view signature) -> int
{
    const std::size_t rparen{ signature.rfind(')') };
    const char        ret_char{ rparen != std::string_view::npos ? signature[rparen + 1] : 'V' };
    return vmhook::detail::sig_char_to_basic_type(ret_char);
}

int main()
{
    // ---- detail::sig_char_to_basic_type: HotSpot BasicType ints --------------
    // Values are the stable HotSpot BasicType enum integers and must not drift.
    check("sig_char_basic_type_boolean_Z_is_4", vmhook::detail::sig_char_to_basic_type('Z') == 4);
    check("sig_char_basic_type_char_C_is_5", vmhook::detail::sig_char_to_basic_type('C') == 5);
    check("sig_char_basic_type_float_F_is_6", vmhook::detail::sig_char_to_basic_type('F') == 6);
    check("sig_char_basic_type_double_D_is_7", vmhook::detail::sig_char_to_basic_type('D') == 7);
    check("sig_char_basic_type_byte_B_is_8", vmhook::detail::sig_char_to_basic_type('B') == 8);
    check("sig_char_basic_type_short_S_is_9", vmhook::detail::sig_char_to_basic_type('S') == 9);
    check("sig_char_basic_type_int_I_is_10", vmhook::detail::sig_char_to_basic_type('I') == 10);
    check("sig_char_basic_type_long_J_is_11", vmhook::detail::sig_char_to_basic_type('J') == 11);
    check("sig_char_basic_type_object_L_is_12", vmhook::detail::sig_char_to_basic_type('L') == 12);
    check("sig_char_basic_type_array_bracket_is_13", vmhook::detail::sig_char_to_basic_type('[') == 13);
    check("sig_char_basic_type_void_V_is_14", vmhook::detail::sig_char_to_basic_type('V') == 14);
    // Unknown / unexpected chars fall back to T_OBJECT (12) by documented design,
    // so a malformed return descriptor degrades to "treat as object" rather than
    // tripping an assert.
    check("sig_char_basic_type_unknown_falls_back_to_object_12", vmhook::detail::sig_char_to_basic_type('Q') == 12);
    check("sig_char_basic_type_nul_falls_back_to_object_12", vmhook::detail::sig_char_to_basic_type('\0') == 12);
    check("sig_char_basic_type_lowercase_is_not_primitive_12", vmhook::detail::sig_char_to_basic_type('i') == 12);

    // ---- detail::jvm_primitive_byte_width: in-heap primitive widths ----------
    // Single-char primitive descriptors map to their JVM spec widths.
    check("byte_width_boolean_Z_is_1", vmhook::detail::jvm_primitive_byte_width("Z") == 1);
    check("byte_width_byte_B_is_1", vmhook::detail::jvm_primitive_byte_width("B") == 1);
    check("byte_width_short_S_is_2", vmhook::detail::jvm_primitive_byte_width("S") == 2);
    check("byte_width_char_C_is_2", vmhook::detail::jvm_primitive_byte_width("C") == 2);
    check("byte_width_int_I_is_4", vmhook::detail::jvm_primitive_byte_width("I") == 4);
    check("byte_width_float_F_is_4", vmhook::detail::jvm_primitive_byte_width("F") == 4);
    check("byte_width_long_J_is_8", vmhook::detail::jvm_primitive_byte_width("J") == 8);
    check("byte_width_double_D_is_8", vmhook::detail::jvm_primitive_byte_width("D") == 8);
    // Reference / array / void / unknown / non-single-char all return 0 so the
    // caller (field_proxy::set) skips its width validation rather than rejecting.
    check("byte_width_void_V_is_0", vmhook::detail::jvm_primitive_byte_width("V") == 0);
    check("byte_width_object_descriptor_is_0", vmhook::detail::jvm_primitive_byte_width("Ljava/lang/String;") == 0);
    check("byte_width_array_descriptor_is_0", vmhook::detail::jvm_primitive_byte_width("[I") == 0);
    check("byte_width_bare_L_single_char_is_0", vmhook::detail::jvm_primitive_byte_width("L") == 0);
    check("byte_width_empty_string_is_0", vmhook::detail::jvm_primitive_byte_width("") == 0);
    check("byte_width_unknown_single_char_is_0", vmhook::detail::jvm_primitive_byte_width("Q") == 0);
    // The size==1 guard means even a valid primitive letter padded to length>1
    // is rejected (no leading/trailing-byte tolerance).
    check("byte_width_multichar_primitive_is_0", vmhook::detail::jvm_primitive_byte_width("II") == 0);

    // ---- detail::jni_signature_for_arg<T>: C++ type -> JNI descriptor --------
    // Only the supported types are instantiated here; the unsupported-type branch
    // is a hard static_assert(dependent_false_v) by design, so e.g.
    // jni_signature_for_arg<void*> / <char> would fail to COMPILE — that compile
    // -time rejection is the contract and cannot be probed at runtime.
    check("jni_sig_std_string_is_String", vmhook::detail::jni_signature_for_arg<std::string>() == "Ljava/lang/String;");
    check("jni_sig_string_view_is_String", vmhook::detail::jni_signature_for_arg<std::string_view>() == "Ljava/lang/String;");
    check("jni_sig_const_char_ptr_is_String", vmhook::detail::jni_signature_for_arg<const char*>() == "Ljava/lang/String;");
    check("jni_sig_char_ptr_is_String", vmhook::detail::jni_signature_for_arg<char*>() == "Ljava/lang/String;");
    check("jni_sig_bool_is_Z", vmhook::detail::jni_signature_for_arg<bool>() == "Z");
    check("jni_sig_int8_is_B", vmhook::detail::jni_signature_for_arg<std::int8_t>() == "B");
    check("jni_sig_uint8_is_B", vmhook::detail::jni_signature_for_arg<std::uint8_t>() == "B");
    check("jni_sig_int16_is_S", vmhook::detail::jni_signature_for_arg<std::int16_t>() == "S");
    // uint16_t maps to Java char ('C'), NOT short — this is the unsigned-16 split.
    check("jni_sig_uint16_is_C", vmhook::detail::jni_signature_for_arg<std::uint16_t>() == "C");
    check("jni_sig_int32_is_I", vmhook::detail::jni_signature_for_arg<std::int32_t>() == "I");
    check("jni_sig_uint32_is_I", vmhook::detail::jni_signature_for_arg<std::uint32_t>() == "I");
    check("jni_sig_int64_is_J", vmhook::detail::jni_signature_for_arg<std::int64_t>() == "J");
    check("jni_sig_uint64_is_J", vmhook::detail::jni_signature_for_arg<std::uint64_t>() == "J");
    check("jni_sig_float_is_F", vmhook::detail::jni_signature_for_arg<float>() == "F");
    check("jni_sig_double_is_D", vmhook::detail::jni_signature_for_arg<double>() == "D");
    // Plain `int` is a 4-byte integral on every supported target, so it routes
    // through the generic sizeof==int32 branch to "I".
    check("jni_sig_plain_int_is_I", vmhook::detail::jni_signature_for_arg<int>() == "I");
    // cv / ref qualifiers are stripped via std::decay_t before dispatch.
    check("jni_sig_strips_const_ref_on_string", vmhook::detail::jni_signature_for_arg<const std::string&>() == "Ljava/lang/String;");
    check("jni_sig_strips_const_ref_on_double", vmhook::detail::jni_signature_for_arg<const double&>() == "D");

    // ---- return-descriptor extraction (char after the close paren) ----------
    // Reproduces method_proxy::call's inline rfind(')')+1 lookup feeding
    // sig_char_to_basic_type, asserting the composed BasicType for each return.
    check("return_type_void_method_is_14", return_basic_type_of("()V") == 14);
    check("return_type_int_method_is_10", return_basic_type_of("(I)I") == 10);
    check("return_type_long_method_is_11", return_basic_type_of("(II)J") == 11);
    check("return_type_object_method_is_12", return_basic_type_of("(I)Ljava/lang/String;") == 12);
    check("return_type_array_method_is_13", return_basic_type_of("(I)[B") == 13);
    check("return_type_boolean_method_is_4", return_basic_type_of("(Ljava/lang/Object;)Z") == 4);
    // rfind(')') correctly picks the LAST paren even when a nested object
    // descriptor in the param list contains characters — here a method whose
    // single param is itself a method-typed... (synthetic) — we only assert that
    // the final ')' governs: trailing 'D' after the last ')' is the return.
    check("return_type_uses_last_paren_double_is_7", return_basic_type_of("(Ljava/lang/Object;)D") == 7);
    // Degenerate / malformed signature with no ')' is treated as void ('V' -> 14)
    // by the call-site fallback, never an out-of-bounds read.
    check("return_type_no_paren_defaults_void_14", return_basic_type_of("garbage") == 14);
    check("return_type_empty_signature_defaults_void_14", return_basic_type_of("") == 14);

    return failures == 0 ? 0 : 1;
}
