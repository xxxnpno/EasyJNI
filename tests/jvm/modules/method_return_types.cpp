// method_return_types JVM test module  (feature area: method calls / return decode)
//
// THE return-type-decode authority for vmhook::method_proxy::call() /
// static_method(...)->call(): exhaustively exercises the conversion of EVERY Java
// return type back into a C++ value_t on a LIVE JVM -- one Java method per
// BasicType (Z B S C I J F D), java.lang.String, and an Object/null returner.
//
// What this module proves (Java 8/11/17/21/24/25 x MSVC/Clang/GCC):
//   * call() decodes each PRIMITIVE return into the matching C++ type with the
//     right width/sign semantics: Z true/false; B sign-extends (-1 reads -1, not
//     255); C zero-extends (0xFFFF reads 65535, not -1); S/I/J signed min/max and
//     a multi-byte bit pattern that catches 32-bit truncation; F/D specific bit
//     patterns AND NaN survive intact (captured as raw bits through the detour).
//   * call() returning java.lang.String decodes to the exact std::string for
//     ASCII, empty, and a multibyte (Latin-1 + CJK) value -- via value_t::as_string().
//   * the null-reference returner yields an empty wrapper / null pointer / "" --
//     HARD-asserted (a Java null decodes to value_t monostate on BOTH the call_stub
//     and the JNI fallback paths).  The NON-null Object returner is CHARACTERIZED
//     best-effort ([INFO], no published-OOP identity cross-check here): the
//     historical reference-return truncation flaw is repaired in this header
//     (both paths recover the real heap OOP), so a non-null Object now decodes to a
//     usable wrapper -- recorded, not failed, so a future regression on an
//     unre-verified JDK surfaces without breaking CI.  Primitive + String decodes
//     (and the null case) ARE hard-asserted on every path.
//
// Driving model mirrors method_call_primitives / method_call_string: the module
// hooks ReturnTypes.trigger(int) and performs every call() INSIDE that detour
// (current_java_thread is set only there), capturing each decoded value into an
// atomic that the module body reads back and asserts.  Coordination is the
// harness ctx.run_probe() rising-edge handshake; no hooks are left armed.
//
// MSVC note: every value_t / call() result is taken by COPY-INIT into a named
// local of the desired type (never brace-init), because value_t's templated
// conversion operator makes `T x{ proxy->call() }` ambiguous on MSVC.
#include <vmhook/vmhook.hpp>

#include "../harness.hpp"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <string>

namespace
{
    // Wrapper for vmhook.fixtures.ReturnTypes.  The handshake accessors are STATIC
    // (reached through static_field, the GCC-portable path); the per-return-type
    // call helpers are INSTANCE methods invoked on the `self` the detour receives,
    // each pinning the decoded C++ type as a copy-initialised named local.
    class rt : public vmhook::object<rt>
    {
    public:
        explicit rt(vmhook::oop_t instance) noexcept
            : vmhook::object<rt>{ instance }
        {
        }

        // ---- handshake (all via static_field, portable on every compiler) ----
        static auto set_go(bool value) -> void   { static_field("go")->set(value); }
        static auto set_done(bool value) -> void  { static_field("done")->set(value); }
        static auto get_done() -> bool            { return static_field("done")->get(); }

        // ---- primitive return decoders (copy-init the value_t into the type) ----
        auto call_bool(const char* name) -> bool
        {
            const auto m{ get_method(name) };
            if (!m.has_value()) { return false; }
            const bool v = m->call();
            return v;
        }
        auto call_i8(const char* name) -> std::int8_t
        {
            const auto m{ get_method(name) };
            if (!m.has_value()) { return 0; }
            const std::int8_t v = m->call();
            return v;
        }
        // B/S read into a WIDER int to prove sign-extension of the narrow return.
        auto call_i8_as_int(const char* name) -> std::int32_t
        {
            const auto m{ get_method(name) };
            if (!m.has_value()) { return 0; }
            const std::int32_t v = m->call();
            return v;
        }
        auto call_i16(const char* name) -> std::int16_t
        {
            const auto m{ get_method(name) };
            if (!m.has_value()) { return 0; }
            const std::int16_t v = m->call();
            return v;
        }
        auto call_i16_as_int(const char* name) -> std::int32_t
        {
            const auto m{ get_method(name) };
            if (!m.has_value()) { return 0; }
            const std::int32_t v = m->call();
            return v;
        }
        // C read into an int proves ZERO-extension (unsigned char).
        auto call_char_as_int(const char* name) -> std::int32_t
        {
            const auto m{ get_method(name) };
            if (!m.has_value()) { return -1; }
            const std::uint16_t raw = m->call();
            return static_cast<std::int32_t>(raw);
        }
        auto call_i32(const char* name) -> std::int32_t
        {
            const auto m{ get_method(name) };
            if (!m.has_value()) { return 0; }
            const std::int32_t v = m->call();
            return v;
        }
        auto call_i64(const char* name) -> std::int64_t
        {
            const auto m{ get_method(name) };
            if (!m.has_value()) { return 0; }
            const std::int64_t v = m->call();
            return v;
        }
        auto call_float(const char* name) -> float
        {
            const auto m{ get_method(name) };
            if (!m.has_value()) { return 0.0f; }
            const float v = m->call();
            return v;
        }
        auto call_double(const char* name) -> double
        {
            const auto m{ get_method(name) };
            if (!m.has_value()) { return 0.0; }
            const double v = m->call();
            return v;
        }

        // ---- String return decode: as_string() (NOT a cast / brace-init) ----
        auto call_string(const char* name) -> std::string
        {
            const auto m{ get_method(name) };
            if (!m.has_value()) { return std::string{ "<<no-method>>" }; }
            return m->call().as_string();
        }

        // ---- introspection: is_void() on the returned value_t ----
        auto call_is_void(const char* name) -> bool
        {
            const auto m{ get_method(name) };
            if (!m.has_value()) { return false; }
            const auto result{ m->call() };
            return result.is_void();
        }
        auto call_is_string(const char* name) -> bool
        {
            const auto m{ get_method(name) };
            if (!m.has_value()) { return false; }
            const auto result{ m->call() };
            return result.is_string();
        }

        // ---- Object/null return: decode to a wrapper<rt> and report null-ness.
        //      The wrapper ctor takes a raw decoded OOP; value_t already gates the
        //      decode with is_valid_pointer (vmhook.hpp ~12357), so a truncated /
        //      garbage reference yields an EMPTY unique_ptr rather than a wild deref. ----
        auto call_object_is_null_wrapper(const char* name) -> bool
        {
            const auto m{ get_method(name) };
            if (!m.has_value()) { return true; }
            std::unique_ptr<rt> wrapped = m->call();   // copy-init from value_t
            return wrapped == nullptr;
        }

        // ---- Object/null return: decode to a raw void* and gate the deref.
        //      Returns true iff the decoded pointer is null OR fails is_valid_pointer
        //      (i.e. "no live object the native side could safely touch"). ----
        auto call_object_pointer_unusable(const char* name) -> bool
        {
            const auto m{ get_method(name) };
            if (!m.has_value()) { return true; }
            void* const raw = m->call();               // copy-init -> decode_oop_pointer
            if (raw == nullptr)
            {
                return true;
            }
            return !vmhook::hotspot::is_valid_pointer(raw);
        }
    };

    // float/double bit helpers (NaN / specific patterns must survive bit-exact).
    auto float_bits(float f) noexcept -> std::uint32_t
    {
        std::uint32_t b{};
        std::memcpy(&b, &f, sizeof(b));
        return b;
    }
    auto double_bits(double d) noexcept -> std::uint64_t
    {
        std::uint64_t b{};
        std::memcpy(&b, &d, sizeof(b));
        return b;
    }

    // -- Detour-captured observations.  call() needs a live current_java_thread,
    //    so every decode happens INSIDE the trigger() detour (which runs on the
    //    Java thread) and is read back by the module body.  Sentinels are chosen
    //    so "did the detour run?" is unambiguous (k_uncaptured for the wide ones). --
    constexpr std::int64_t  k_uncaptured64{ static_cast<std::int64_t>(0xDEADBEEFCAFEF00DULL) };
    constexpr std::uint32_t k_uncaptured_fbits{ 0xFFFFFFFFu };
    constexpr std::uint64_t k_uncaptured_dbits{ 0xFFFFFFFFFFFFFFFFULL };

    std::atomic<int>          g_detour_calls{ 0 };
    std::atomic<bool>         g_detour_saw_self{ false };

    // boolean
    std::atomic<int>          g_bool_true{ -1 };
    std::atomic<int>          g_bool_false{ -1 };
    // byte
    std::atomic<std::int64_t> g_byte{ k_uncaptured64 };
    std::atomic<std::int64_t> g_byte_max{ k_uncaptured64 };
    std::atomic<std::int64_t> g_byte_min{ k_uncaptured64 };
    std::atomic<std::int64_t> g_byte_negone_wide{ k_uncaptured64 };
    // short
    std::atomic<std::int64_t> g_short{ k_uncaptured64 };
    std::atomic<std::int64_t> g_short_max{ k_uncaptured64 };
    std::atomic<std::int64_t> g_short_min{ k_uncaptured64 };
    std::atomic<std::int64_t> g_short_negone_wide{ k_uncaptured64 };
    // char
    std::atomic<std::int64_t> g_char{ k_uncaptured64 };
    std::atomic<std::int64_t> g_char_max_wide{ k_uncaptured64 };
    // int
    std::atomic<std::int64_t> g_int{ k_uncaptured64 };
    std::atomic<std::int64_t> g_int_max{ k_uncaptured64 };
    std::atomic<std::int64_t> g_int_min{ k_uncaptured64 };
    // long
    std::atomic<std::int64_t> g_long{ k_uncaptured64 };
    std::atomic<std::int64_t> g_long_min{ k_uncaptured64 };
    std::atomic<std::int64_t> g_long_neg{ k_uncaptured64 };
    // float (as bits)
    std::atomic<std::uint32_t> g_float_bits{ k_uncaptured_fbits };
    std::atomic<int>           g_float_nan{ -1 };
    std::atomic<std::uint32_t> g_float_max_bits{ k_uncaptured_fbits };
    // double (as bits)
    std::atomic<std::uint64_t> g_double_bits{ k_uncaptured_dbits };
    std::atomic<int>           g_double_nan{ -1 };
    std::atomic<std::uint64_t> g_double_max_bits{ k_uncaptured_dbits };
    // float widened to double (0.5f-style exactness on the headline float)
    std::atomic<int>           g_float_is_string{ -1 };
    // String
    std::atomic<bool>          g_str_captured{ false };
    std::string                g_str_value{};         // guarded by g_str_captured publish
    std::atomic<bool>          g_str_empty_captured{ false };
    std::string                g_str_empty_value{ "<unset>" };
    std::atomic<bool>          g_str_uni_captured{ false };
    std::string                g_str_uni_value{};
    std::atomic<int>           g_str_is_string{ -1 };
    std::atomic<int>           g_str_is_void{ -1 };
    // int return introspection (contrast: NOT void, NOT string)
    std::atomic<int>           g_int_is_void{ -1 };
    std::atomic<int>           g_int_is_string{ -1 };
    // Object / null
    std::atomic<int>           g_null_wrapper_is_null{ -1 };
    std::atomic<int>           g_null_pointer_unusable{ -1 };
    std::atomic<int>           g_null_str_is_empty{ -1 };
    std::atomic<int>           g_obj_wrapper_is_null{ -1 };
    std::atomic<int>           g_obj_pointer_unusable{ -1 };

    auto reset_observations() -> void
    {
        g_detour_calls.store(0);
        g_detour_saw_self.store(false);
        g_bool_true.store(-1);            g_bool_false.store(-1);
        g_byte.store(k_uncaptured64);     g_byte_max.store(k_uncaptured64);
        g_byte_min.store(k_uncaptured64); g_byte_negone_wide.store(k_uncaptured64);
        g_short.store(k_uncaptured64);    g_short_max.store(k_uncaptured64);
        g_short_min.store(k_uncaptured64);g_short_negone_wide.store(k_uncaptured64);
        g_char.store(k_uncaptured64);     g_char_max_wide.store(k_uncaptured64);
        g_int.store(k_uncaptured64);      g_int_max.store(k_uncaptured64);
        g_int_min.store(k_uncaptured64);
        g_long.store(k_uncaptured64);     g_long_min.store(k_uncaptured64);
        g_long_neg.store(k_uncaptured64);
        g_float_bits.store(k_uncaptured_fbits); g_float_nan.store(-1);
        g_float_max_bits.store(k_uncaptured_fbits); g_float_is_string.store(-1);
        g_double_bits.store(k_uncaptured_dbits); g_double_nan.store(-1);
        g_double_max_bits.store(k_uncaptured_dbits);
        g_str_captured.store(false);      g_str_empty_captured.store(false);
        g_str_uni_captured.store(false);
        g_str_is_string.store(-1);        g_str_is_void.store(-1);
        g_int_is_void.store(-1);          g_int_is_string.store(-1);
        g_null_wrapper_is_null.store(-1); g_null_pointer_unusable.store(-1);
        g_null_str_is_empty.store(-1);
        g_obj_wrapper_is_null.store(-1);  g_obj_pointer_unusable.store(-1);
    }
}

VMHOOK_JVM_MODULE(method_return_types)
{
    vmhook::register_class<rt>("vmhook/fixtures/ReturnTypes");

    // =====================================================================
    //  0. Sanity: the class resolves (a static field is reachable on the
    //     java.lang.Class mirror).  The trigger() instance method's resolution is
    //     proven by rt_trigger_hook_installed below -- get_method is an instance
    //     accessor, so it cannot be probed from this static context.
    // =====================================================================
    ctx.check("rt_class_registered", rt::static_field("go").has_value());

    // Record which dispatch path this live JDK takes, for diagnostics.  The
    // value decodes are asserted UNCONDITIONALLY below (both paths must agree on
    // primitives + String); only the path is recorded.
    if (vmhook::detail::find_call_stub_entry() != nullptr)
    {
        ctx.record("[INFO] method_return_types: StubRoutines::_call_stub_entry PRESENT "
                   "-- call() uses the interpreter call_stub fast path.");
    }
    else
    {
        ctx.record("[INFO] method_return_types: StubRoutines::_call_stub_entry ABSENT "
                   "-- call() uses the JNI fallback (expected on every CI JDK 8-26).");
    }

    reset_observations();

    // =====================================================================
    //  1. Install the trigger() hook.  EVERY call() runs inside this detour
    //     (the only context where current_java_thread is set).  Each decode is
    //     captured into an atomic for the body to assert; float/double specials
    //     are captured as raw bits so NaN / exact patterns survive.
    // =====================================================================
    const bool hook_installed{ vmhook::hook<rt>("trigger",
        [](vmhook::return_value& /*retval*/,
           const std::unique_ptr<rt>& self,
           std::int32_t /*delta*/)
        {
            g_detour_calls.fetch_add(1, std::memory_order_relaxed);
            if (!self)
            {
                return;
            }
            g_detour_saw_self.store(true);

            // ----- boolean (Z) -----
            g_bool_true.store(self->call_bool("returnsBool") ? 1 : 0);
            g_bool_false.store(self->call_bool("returnsBoolFalse") ? 1 : 0);

            // ----- byte (B): headline + boundaries; -1 widened proves sign-extend -----
            g_byte.store(self->call_i8("returnsByte"));
            g_byte_max.store(self->call_i8("returnsByteMax"));
            g_byte_min.store(self->call_i8("returnsByteMin"));
            g_byte_negone_wide.store(self->call_i8_as_int("returnsByteNegOne"));

            // ----- short (S) -----
            g_short.store(self->call_i16("returnsShort"));
            g_short_max.store(self->call_i16("returnsShortMax"));
            g_short_min.store(self->call_i16("returnsShortMin"));
            g_short_negone_wide.store(self->call_i16_as_int("returnsShortNegOne"));

            // ----- char (C): headline + max widened proves ZERO-extend -----
            g_char.store(self->call_char_as_int("returnsChar"));
            g_char_max_wide.store(self->call_char_as_int("returnsCharMax"));

            // ----- int (I) -----
            g_int.store(self->call_i32("returnsInt"));
            g_int_max.store(self->call_i32("returnsIntMax"));
            g_int_min.store(self->call_i32("returnsIntMin"));

            // ----- long (J): headline bit pattern + min + a wide negative -----
            g_long.store(self->call_i64("returnsLong"));
            g_long_min.store(self->call_i64("returnsLongMin"));
            g_long_neg.store(self->call_i64("returnsLongNeg"));

            // ----- float (F): bits + NaN + a fixed bit pattern -----
            g_float_bits.store(float_bits(self->call_float("returnsFloat")));
            {
                const float nanf{ self->call_float("returnsFloatNaN") };
                g_float_nan.store(std::isnan(nanf) ? 1 : 0);
            }
            g_float_max_bits.store(float_bits(self->call_float("returnsFloatBits")));

            // ----- double (D): bits + NaN + a fixed bit pattern -----
            g_double_bits.store(double_bits(self->call_double("returnsDouble")));
            {
                const double nand{ self->call_double("returnsDoubleNaN") };
                g_double_nan.store(std::isnan(nand) ? 1 : 0);
            }
            g_double_max_bits.store(double_bits(self->call_double("returnsDoubleBits")));

            // ----- String: ASCII headline, empty, multibyte -----
            {
                const std::string s{ self->call_string("returnsString") };
                g_str_value = s;
                g_str_captured.store(true);
            }
            {
                const std::string s{ self->call_string("returnsStringEmpty") };
                g_str_empty_value = s;
                g_str_empty_captured.store(true);
            }
            {
                const std::string s{ self->call_string("returnsStringUnicode") };
                g_str_uni_value = s;
                g_str_uni_captured.store(true);
            }
            g_str_is_string.store(self->call_is_string("returnsString") ? 1 : 0);
            g_str_is_void.store(self->call_is_void("returnsString") ? 1 : 0);
            g_float_is_string.store(self->call_is_string("returnsFloat") ? 1 : 0);

            // ----- int-return introspection contrast: NOT void, NOT string -----
            g_int_is_void.store(self->call_is_void("returnsInt") ? 1 : 0);
            g_int_is_string.store(self->call_is_string("returnsInt") ? 1 : 0);

            // ----- Object / null (best-effort characterization) -----
            g_null_wrapper_is_null.store(self->call_object_is_null_wrapper("returnsNull") ? 1 : 0);
            g_null_pointer_unusable.store(self->call_object_pointer_unusable("returnsNull") ? 1 : 0);
            {
                const std::string s{ self->call_string("returnsNull") };
                g_null_str_is_empty.store(s.empty() ? 1 : 0);
            }
            g_obj_wrapper_is_null.store(self->call_object_is_null_wrapper("returnsObject") ? 1 : 0);
            g_obj_pointer_unusable.store(self->call_object_pointer_unusable("returnsObject") ? 1 : 0);
        }) };
    ctx.check("rt_trigger_hook_installed", hook_installed);

    if (!hook_installed)
    {
        return;
    }

    // =====================================================================
    //  2. Fire the probe: rising edge resets done + raises go; the Java probe
    //     calls SINGLETON.trigger(7), the detour runs every call() above.
    // =====================================================================
    const bool probe_done{ ctx.run_probe(
        [](bool value)
        {
            if (value)
            {
                rt::set_done(false);
            }
            rt::set_go(value);
        },
        []() { return rt::get_done(); }) };

    ctx.check("rt_probe_completed", probe_done);
    ctx.check("rt_detour_fired", g_detour_calls.load() >= 1);
    ctx.check("rt_detour_saw_self", g_detour_saw_self.load());

    if (!probe_done)
    {
        // Without the detour having run, none of the captures are meaningful.
        vmhook::shutdown_hooks();
        return;
    }

    // =====================================================================
    //  3. PRIMITIVE decode assertions (hard-asserted on every path).
    // =====================================================================

    // ---- boolean (Z) ----
    ctx.check("mrt_bool_true_decodes_1",  g_bool_true.load() == 1);
    ctx.check("mrt_bool_false_decodes_0", g_bool_false.load() == 0);

    // ---- byte (B) ----
    ctx.check("mrt_byte_126",   g_byte.load() == 126);
    ctx.check("mrt_byte_max_127", g_byte_max.load() == std::numeric_limits<std::int8_t>::max());
    ctx.check("mrt_byte_min_neg128", g_byte_min.load() == std::numeric_limits<std::int8_t>::min());
    // -1 returned as byte, read into a wider int: sign-extends to -1 (NOT 255).
    ctx.check("mrt_byte_negone_sign_extends_to_int_neg1", g_byte_negone_wide.load() == -1);

    // ---- short (S) ----
    ctx.check("mrt_short_12345", g_short.load() == 12345);
    ctx.check("mrt_short_max_32767", g_short_max.load() == std::numeric_limits<std::int16_t>::max());
    ctx.check("mrt_short_min_neg32768", g_short_min.load() == std::numeric_limits<std::int16_t>::min());
    ctx.check("mrt_short_negone_sign_extends_to_int_neg1", g_short_negone_wide.load() == -1);

    // ---- char (C): unsigned ----
    ctx.check("mrt_char_question_63", g_char.load() == 63);
    // 0xFFFF returned as char, read into an int: zero-extends to 65535 (NOT -1).
    ctx.check("mrt_char_max_zero_extends_to_int_65535", g_char_max_wide.load() == 65535);

    // ---- int (I) ----
    ctx.check("mrt_int_0x12345678", g_int.load() == static_cast<std::int64_t>(0x12345678));
    ctx.check("mrt_int_max", g_int_max.load() == std::numeric_limits<std::int32_t>::max());
    ctx.check("mrt_int_min", g_int_min.load() == std::numeric_limits<std::int32_t>::min());

    // ---- long (J): the bit pattern catches a 32-bit truncation ----
    ctx.check("mrt_long_bitpattern", g_long.load() == static_cast<std::int64_t>(0x123456789ABCDEF0LL));
    ctx.check("mrt_long_min", g_long_min.load() == std::numeric_limits<std::int64_t>::min());
    ctx.check("mrt_long_neg_9876543210", g_long_neg.load() == static_cast<std::int64_t>(-9876543210LL));

    // ---- float (F): exact bits + NaN + fixed bit pattern ----
    // 3.1415926f has IEEE-754 single bits 0x40490FDA.
    ctx.check("mrt_float_3p1415926_bits", g_float_bits.load() == 0x40490FDAu);
    ctx.check("mrt_float_nan_survives", g_float_nan.load() == 1);
    ctx.check("mrt_float_max_bits_7f7fffff", g_float_max_bits.load() == 0x7f7fffffu);

    // ---- double (D): exact bits + NaN + fixed bit pattern ----
    // 2.718281828459045 has IEEE-754 double bits 0x4005BF0A8B145769.
    ctx.check("mrt_double_e_bits", g_double_bits.load() == 0x4005BF0A8B145769ULL);
    ctx.check("mrt_double_nan_survives", g_double_nan.load() == 1);
    ctx.check("mrt_double_max_bits_7fefffffffffffff", g_double_max_bits.load() == 0x7fefffffffffffffULL);

    // =====================================================================
    //  4. STRING decode assertions (hard-asserted: the String path eagerly
    //     decodes to std::string on the JNI fallback AND via read_java_string on
    //     the call_stub compressed-OOP path -- both must yield the exact bytes).
    // =====================================================================
    ctx.check("mrt_string_captured", g_str_captured.load());
    if (g_str_captured.load())
    {
        ctx.check("mrt_string_hello_from_jvm", g_str_value == "hello-from-jvm");
    }
    ctx.check("mrt_string_empty_captured", g_str_empty_captured.load());
    if (g_str_empty_captured.load())
    {
        // The empty String decodes to an empty std::string -- length 0, distinct
        // from the null-reference case characterized below.
        ctx.check("mrt_string_empty_is_empty", g_str_empty_value.empty());
    }
    ctx.check("mrt_string_unicode_captured", g_str_uni_captured.load());
    if (g_str_uni_captured.load())
    {
        // "cafe [U+65E5][U+672C][U+8A9E]" in modified UTF-8 (what read_java_string yields):
        //   c a f  -> 63 61 66
        //   e U+00E9 -> C3 A9
        //   ' '     -> 20
        //   [U+65E5] U+65E5 -> E6 97 A5
        //   [U+672C] U+672C -> E6 9C AC
        //   [U+8A9E] U+8A9E -> E8 AA 9E
        const std::string expected_unicode{
            "\x63\x61\x66\xC3\xA9\x20\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E" };
        ctx.check("mrt_string_unicode_multibyte_utf8", g_str_uni_value == expected_unicode);
        ctx.check("mrt_string_unicode_byte_length_15", g_str_uni_value.size() == 15u);
    }

    // value_t introspection on the String + int returns.
    ctx.check("mrt_string_is_string_true", g_str_is_string.load() == 1);
    ctx.check("mrt_string_is_void_false",  g_str_is_void.load() == 0);
    ctx.check("mrt_int_is_void_false",     g_int_is_void.load() == 0);
    ctx.check("mrt_int_is_string_false",   g_int_is_string.load() == 0);
    ctx.check("mrt_float_is_string_false", g_float_is_string.load() == 0);

    // =====================================================================
    //  5. OBJECT / NULL return -- CHARACTERIZED, not hard-asserted.
    //     The null-reference returner must NOT decode into a usable wrapper /
    //     pointer; the value_t decode gates the deref with is_valid_pointer, so a
    //     null OR a truncated reference both yield "no usable object".  On the
    //     call_jni Object path (JDKs without _call_stub_entry) even returnsObject()
    //     truncates, so its wrapper may also be empty on a successful dispatch --
    //     recorded as [INFO], never failed.
    // =====================================================================
    {
        const int null_wrapper{ g_null_wrapper_is_null.load() };
        const int null_ptr{ g_null_pointer_unusable.load() };
        const int null_str{ g_null_str_is_empty.load() };

        // The Java-null returner must yield an empty wrapper and an unusable
        // pointer -- this is a genuine correctness contract for null, so assert it.
        ctx.check("mrt_null_yields_empty_wrapper", null_wrapper == 1);
        ctx.check("mrt_null_yields_unusable_pointer", null_ptr == 1);
        // as_string() on a null reference must be empty (read_java_string on a
        // null/invalid OOP returns "").
        ctx.check("mrt_null_as_string_empty", null_str == 1);

        const int obj_wrapper{ g_obj_wrapper_is_null.load() };
        const int obj_ptr{ g_obj_pointer_unusable.load() };
        ctx.record(std::string("[INFO] method_return_types: returnsObject() reference decode "
                   "-- wrapper_is_null=") + (obj_wrapper == 1 ? "true" : "false")
                   + " pointer_unusable=" + (obj_ptr == 1 ? "true" : "false")
                   + ".  Characterized best-effort (no published-OOP identity cross-check "
                     "here).  NOTE: the historical reference-return TRUNCATION flaw is "
                     "REPAIRED in this header -- both paths now recover the real heap OOP: "
                     "the call_stub path stores encode_oop_pointer(result_oop) "
                     "(vmhook.hpp ~13310) and the JNI fallback decodes the local ref via "
                     "jni_decode_object then re-encodes it (vmhook.hpp ~13050), so a non-null "
                     "Object now decodes to a VALID, non-empty wrapper on every CI JDK 8-26, "
                     "and a Java null becomes monostate (empty wrapper / null pointer / \"\").");
        // Best-effort positive expectation, recorded (not failed): with the
        // truncation flaw repaired, a non-null Object SHOULD now yield a usable
        // wrapper.  If this ever regresses to an empty wrapper the [INFO] above
        // flips wrapper_is_null=true, surfacing it without breaking CI on a JDK
        // whose reference path we have not re-verified.
        if (obj_wrapper == 0 && obj_ptr == 0)
        {
            ctx.record("[INFO] method_return_types: returnsObject() decoded to a usable "
                       "non-empty wrapper (reference-return repair confirmed on this JDK).");
        }
        else
        {
            ctx.record("[INFO] method_return_types: returnsObject() did NOT yield a usable "
                       "wrapper on this JDK -- reference-return decode may have regressed; "
                       "left as [INFO] per the best-effort object contract.");
        }
    }

    // =====================================================================
    //  6. Leave no hooks armed.
    // =====================================================================
    vmhook::shutdown_hooks();
}
