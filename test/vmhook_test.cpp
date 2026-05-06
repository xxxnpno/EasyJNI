#include <vmhook/vmhook.hpp>

#include <bit>
#include <cstdint>
#include <format>
#include <fstream>
#include <limits>
#include <print>
#include <string>
#include <vector>

// ─── Java class wrappers ────────────────────────────────────────────────────
//
// These mirror the wrappers in vmhook/src/example.cpp.
// Differences from example.cpp:
//   - char fields return std::uint16_t (Java char is 16-bit unsigned; the C++
//     char return in example.cpp silently truncates to 8 bits).
//   - Array getters use get_as_vector / get_as_vector_bool (the get() overload
//     returns an empty container for reference/array fields in example.cpp).
//   - String getters are omitted: vmhook has no built-in API to decode a Java
//     String OOP into std::string.

class main_class : public vmhook::object<main_class>
{
public:
    explicit main_class(vmhook::oop_t instance)
        : vmhook::object<main_class>{ instance }
    {
    }

    auto set_stop_jvm(bool value)
        -> void
    {
        this->get_field("stopJVM")->set(value);
    }
};

class example_class : public vmhook::object<example_class>
{
public:
    explicit example_class(vmhook::oop_t instance)
        : vmhook::object<example_class>{ instance }
    {
    }

    // ── Static scalar getters ────────────────────────────────────────────
    auto get_static_bool()   -> bool           { return this->get_field("staticBool")->get();   }
    auto get_static_byte()   -> std::int8_t    { return this->get_field("staticByte")->get();   }
    auto get_static_short()  -> std::int16_t   { return this->get_field("staticShort")->get();  }
    auto get_static_int()    -> std::int32_t   { return this->get_field("staticInt")->get();    }
    auto get_static_long()   -> std::int64_t   { return this->get_field("staticLong")->get();   }
    auto get_static_float()  -> float          { return this->get_field("staticFloat")->get();  }
    auto get_static_double() -> double         { return this->get_field("staticDouble")->get(); }
    auto get_static_char()   -> std::uint16_t  { return this->get_field("staticChar")->get();   }

    // ── Non-static scalar getters ────────────────────────────────────────
    auto get_not_static_bool()   -> bool          { return this->get_field("notStaticBool")->get();   }
    auto get_not_static_byte()   -> std::int8_t   { return this->get_field("notStaticByte")->get();   }
    auto get_not_static_short()  -> std::int16_t  { return this->get_field("notStaticShort")->get();  }
    auto get_not_static_int()    -> std::int32_t  { return this->get_field("notStaticInt")->get();    }
    auto get_not_static_long()   -> std::int64_t  { return this->get_field("notStaticLong")->get();   }
    auto get_not_static_float()  -> float         { return this->get_field("notStaticFloat")->get();  }
    auto get_not_static_double() -> double        { return this->get_field("notStaticDouble")->get(); }
    auto get_not_static_char()   -> std::uint16_t { return this->get_field("notStaticChar")->get();   }

    // ── Static array getters ─────────────────────────────────────────────
    auto get_static_bool_array()
        -> std::vector<bool>
    {
        return this->get_field("staticBoolArray")->get_as_vector_bool();
    }
    auto get_static_byte_array()
        -> std::vector<std::int8_t>
    {
        return this->get_field("staticByteArray")->get_as_vector<std::int8_t>();
    }
    auto get_static_short_array()
        -> std::vector<std::int16_t>
    {
        return this->get_field("staticShortArray")->get_as_vector<std::int16_t>();
    }
    auto get_static_int_array()
        -> std::vector<std::int32_t>
    {
        return this->get_field("staticIntArray")->get_as_vector<std::int32_t>();
    }
    auto get_static_long_array()
        -> std::vector<std::int64_t>
    {
        return this->get_field("staticLongArray")->get_as_vector<std::int64_t>();
    }
    auto get_static_float_array()
        -> std::vector<float>
    {
        return this->get_field("staticFloatArray")->get_as_vector<float>();
    }
    auto get_static_double_array()
        -> std::vector<double>
    {
        return this->get_field("staticDoubleArray")->get_as_vector<double>();
    }
    auto get_static_char_array()
        -> std::vector<std::uint16_t>
    {
        return this->get_field("staticCharArray")->get_as_vector<std::uint16_t>();
    }

    // ── Non-static array getters ─────────────────────────────────────────
    auto get_not_static_bool_array()
        -> std::vector<bool>
    {
        return this->get_field("notStaticBoolArray")->get_as_vector_bool();
    }
    auto get_not_static_byte_array()
        -> std::vector<std::int8_t>
    {
        return this->get_field("notStaticByteArray")->get_as_vector<std::int8_t>();
    }
    auto get_not_static_short_array()
        -> std::vector<std::int16_t>
    {
        return this->get_field("notStaticShortArray")->get_as_vector<std::int16_t>();
    }
    auto get_not_static_int_array()
        -> std::vector<std::int32_t>
    {
        return this->get_field("notStaticIntArray")->get_as_vector<std::int32_t>();
    }
    auto get_not_static_long_array()
        -> std::vector<std::int64_t>
    {
        return this->get_field("notStaticLongArray")->get_as_vector<std::int64_t>();
    }
    auto get_not_static_float_array()
        -> std::vector<float>
    {
        return this->get_field("notStaticFloatArray")->get_as_vector<float>();
    }
    auto get_not_static_double_array()
        -> std::vector<double>
    {
        return this->get_field("notStaticDoubleArray")->get_as_vector<double>();
    }
    auto get_not_static_char_array()
        -> std::vector<std::uint16_t>
    {
        return this->get_field("notStaticCharArray")->get_as_vector<std::uint16_t>();
    }

    // ── Counter getters ──────────────────────────────────────────────────
    auto get_static_called()     -> std::int32_t { return this->get_field("staticCalled")->get();    }
    auto get_non_static_called() -> std::int32_t { return this->get_field("nonStaticCalled")->get(); }

    // ── Object getter ────────────────────────────────────────────────────
    auto get_instance()
        -> std::unique_ptr<example_class>
    {
        return this->get_field("instance")->get_as<example_class>();
    }
};

class a_class : public vmhook::object<a_class>
{
public:
    explicit a_class(vmhook::oop_t instance)
        : vmhook::object<a_class>{ instance }
    {
    }

    auto get_counter() -> std::int32_t { return this->get_field("counter")->get(); }
    auto get_val()     -> std::int32_t { return this->get_field("val")->get();     }
};

// ─── Test framework ──────────────────────────────────────────────────────────

static std::int32_t g_passed{ 0 };
static std::int32_t g_failed{ 0 };
static std::ofstream g_log;

static auto emit(bool passed, const std::string& name, const std::string& actual, const std::string& expected)
    -> void
{
    std::string line;
    if (passed)
    {
        line = std::format("[PASS] {} = {}", name, actual);
        ++g_passed;
    }
    else
    {
        line = std::format("[FAIL] {} : expected={} got={}", name, expected, actual);
        ++g_failed;
    }
    std::println("{}", line);
    g_log << line << '\n';
}

// Integer check – upcasts to int64 to avoid int8_t being formatted as char.
static auto check_int(const std::string& name, std::int64_t actual, std::int64_t expected)
    -> void
{
    emit(actual == expected, name, std::format("{}", actual), std::format("{}", expected));
}

static auto check_uint(const std::string& name, std::uint64_t actual, std::uint64_t expected)
    -> void
{
    emit(actual == expected, name, std::format("{}", actual), std::format("{}", expected));
}

static auto check_float(const std::string& name, float actual, float expected)
    -> void
{
    const bool passed{ std::bit_cast<std::uint32_t>(actual) == std::bit_cast<std::uint32_t>(expected) };
    emit(passed, name, std::format("{}", actual), std::format("{}", expected));
}

static auto check_double(const std::string& name, double actual, double expected)
    -> void
{
    const bool passed{ std::bit_cast<std::uint64_t>(actual) == std::bit_cast<std::uint64_t>(expected) };
    emit(passed, name, std::format("{}", actual), std::format("{}", expected));
}

// Generic vector check; element_type must support std::format.
template<typename element_type>
static auto check_vec(const std::string& name,
                      const std::vector<element_type>& actual,
                      const std::vector<element_type>& expected)
    -> void
{
    auto to_str{ [](const std::vector<element_type>& v)
    {
        std::string s{ "[" };
        for (std::size_t i{}; i < v.size(); ++i)
        {
            if (i) s += ", ";
            // Cast small int types to int so they print as numbers, not chars.
            if constexpr (std::is_same_v<element_type, std::int8_t> ||
                          std::is_same_v<element_type, std::uint8_t>)
                s += std::format("{}", static_cast<int>(v[i]));
            else
                s += std::format("{}", v[i]);
        }
        s += "]";
        return s;
    }};
    emit(actual == expected, name, to_str(actual), to_str(expected));
}

static auto check_bool_vec(const std::string& name,
                           const std::vector<bool>& actual,
                           const std::vector<bool>& expected)
    -> void
{
    auto to_str{ [](const std::vector<bool>& v)
    {
        std::string s{ "[" };
        for (std::size_t i{}; i < v.size(); ++i)
        {
            if (i) s += ", ";
            s += v[i] ? "true" : "false";
        }
        s += "]";
        return s;
    }};
    emit(actual == expected, name, to_str(actual), to_str(expected));
}

static auto check_float_vec(const std::string& name,
                            const std::vector<float>& actual,
                            const std::vector<float>& expected)
    -> void
{
    bool passed{ actual.size() == expected.size() };
    if (passed)
    {
        for (std::size_t i{}; i < actual.size(); ++i)
        {
            if (std::bit_cast<std::uint32_t>(actual[i]) != std::bit_cast<std::uint32_t>(expected[i]))
            {
                passed = false;
                break;
            }
        }
    }
    auto to_str{ [](const std::vector<float>& v)
    {
        std::string s{ "[" };
        for (std::size_t i{}; i < v.size(); ++i) { if (i) s += ", "; s += std::format("{}", v[i]); }
        s += "]";
        return s;
    }};
    emit(passed, name, to_str(actual), to_str(expected));
}

static auto check_double_vec(const std::string& name,
                             const std::vector<double>& actual,
                             const std::vector<double>& expected)
    -> void
{
    bool passed{ actual.size() == expected.size() };
    if (passed)
    {
        for (std::size_t i{}; i < actual.size(); ++i)
        {
            if (std::bit_cast<std::uint64_t>(actual[i]) != std::bit_cast<std::uint64_t>(expected[i]))
            {
                passed = false;
                break;
            }
        }
    }
    auto to_str{ [](const std::vector<double>& v)
    {
        std::string s{ "[" };
        for (std::size_t i{}; i < v.size(); ++i) { if (i) s += ", "; s += std::format("{}", v[i]); }
        s += "]";
        return s;
    }};
    emit(passed, name, to_str(actual), to_str(expected));
}

// ─── Thread entry point ──────────────────────────────────────────────────────

static auto WINAPI thread_entry(HMODULE module)
    -> DWORD
{
    // Give the JVM time to finish loading all classes (including vmhook/Example
    // via Class.forName in Main.java) before we start walking HotSpot internals.
    Sleep(2000);

    g_log.open("test_results.txt", std::ios::out | std::ios::trunc);

    std::println("[VMHook Test] Starting unit tests...");
    g_log << "[VMHook Test] Starting unit tests...\n";

    vmhook::register_class<main_class>("vmhook/Main");
    vmhook::register_class<example_class>("vmhook/Example");
    vmhook::register_class<a_class>("vmhook/A");

    // Expected values are derived directly from Example.java.
    // Float/double bit patterns match Java's Float/Double.intBitsToFloat / longBitsToDouble.
    static constexpr float  expected_float  = std::numeric_limits<float>::max();   // 0x7f7fffff
    static constexpr double expected_double = std::numeric_limits<double>::max();  // 0x7fefffffffffffff

    // ── Static scalar fields ──────────────────────────────────────────────────
    example_class ex{ nullptr };

    check_int ("staticBool",   ex.get_static_bool()   ? 1 : 0, 1);
    check_int ("staticByte",   ex.get_static_byte(),            0xf);
    check_int ("staticShort",  ex.get_static_short(),           0xff);
    check_int ("staticInt",    ex.get_static_int(),             0xffff);
    check_int ("staticLong",   ex.get_static_long(),            0xffffffffLL);
    check_float ("staticFloat",  ex.get_static_float(),  expected_float);
    check_double("staticDouble", ex.get_static_double(), expected_double);
    check_uint("staticChar",   ex.get_static_char(),            0xffffu);

    check_int("staticCalled",  ex.get_static_called(), 0);

    // ── Non-static scalar fields (accessed via Example.instance) ─────────────
    auto inst{ ex.get_instance() };

    if (!inst)
    {
        const std::string msg{ "[FAIL] Example.instance : expected=non-null got=null" };
        std::println("{}", msg);
        g_log << msg << '\n';
        ++g_failed;
    }
    else
    {
        check_int ("notStaticBool",   inst->get_not_static_bool()   ? 1 : 0, 1);
        check_int ("notStaticByte",   inst->get_not_static_byte(),            0xf);
        check_int ("notStaticShort",  inst->get_not_static_short(),           0xff);
        check_int ("notStaticInt",    inst->get_not_static_int(),             0xffff);
        check_int ("notStaticLong",   inst->get_not_static_long(),            0xffffffffLL);
        check_float ("notStaticFloat",  inst->get_not_static_float(),  expected_float);
        check_double("notStaticDouble", inst->get_not_static_double(), expected_double);
        check_uint("notStaticChar",   inst->get_not_static_char(),            0xffffu);

        check_int("nonStaticCalled", inst->get_non_static_called(), 0);
    }

    // ── Static array fields ───────────────────────────────────────────────────
    check_bool_vec  ("staticBoolArray",   ex.get_static_bool_array(),   { true, false, true });
    check_vec<std::int8_t> ("staticByteArray",   ex.get_static_byte_array(),   { 0x1, 0x2, 0x3 });
    check_vec<std::int16_t>("staticShortArray",  ex.get_static_short_array(),  { 0x10, 0x20, 0x30 });
    check_vec<std::int32_t>("staticIntArray",    ex.get_static_int_array(),    { 0x100, 0x200, 0x300 });
    check_vec<std::int64_t>("staticLongArray",   ex.get_static_long_array(),   { 0x1000LL, 0x2000LL, 0x3000LL });
    check_float_vec ("staticFloatArray",  ex.get_static_float_array(),  { 1.0f, 2.0f, 3.0f });
    check_double_vec("staticDoubleArray", ex.get_static_double_array(), { 1.0, 2.0, 3.0 });
    check_vec<std::uint16_t>("staticCharArray",  ex.get_static_char_array(),
        { std::uint16_t{ 'A' }, std::uint16_t{ 'B' }, std::uint16_t{ 'C' } });

    // ── Non-static array fields ───────────────────────────────────────────────
    if (inst)
    {
        check_bool_vec  ("notStaticBoolArray",   inst->get_not_static_bool_array(),   { true, false, true });
        check_vec<std::int8_t> ("notStaticByteArray",   inst->get_not_static_byte_array(),   { 0x1, 0x2, 0x3 });
        check_vec<std::int16_t>("notStaticShortArray",  inst->get_not_static_short_array(),  { 0x10, 0x20, 0x30 });
        check_vec<std::int32_t>("notStaticIntArray",    inst->get_not_static_int_array(),    { 0x100, 0x200, 0x300 });
        check_vec<std::int64_t>("notStaticLongArray",   inst->get_not_static_long_array(),   { 0x1000LL, 0x2000LL, 0x3000LL });
        check_float_vec ("notStaticFloatArray",  inst->get_not_static_float_array(),  { 1.0f, 2.0f, 3.0f });
        check_double_vec("notStaticDoubleArray", inst->get_not_static_double_array(), { 1.0, 2.0, 3.0 });
        check_vec<std::uint16_t>("notStaticCharArray",  inst->get_not_static_char_array(),
            { std::uint16_t{ 'X' }, std::uint16_t{ 'Y' }, std::uint16_t{ 'Z' } });
    }

    // ── Summary ───────────────────────────────────────────────────────────────
    const std::string summary{
        std::format("TOTAL: {}/{} PASSED", g_passed, g_passed + g_failed)
    };
    std::println("[VMHook Test] {}", summary);
    g_log << summary << '\n';
    g_log.close();

    // Signal Main.java to exit.
    main_class main_obj{ nullptr };
    main_obj.set_stop_jvm(true);

    FreeLibraryAndExitThread(module, 0);
    return 0;
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(module);

        HANDLE worker_thread{ CreateThread(nullptr, 0,
            reinterpret_cast<LPTHREAD_START_ROUTINE>(thread_entry), module, 0, nullptr) };

        if (worker_thread)
        {
            CloseHandle(worker_thread);
        }
    }

    return TRUE;
}
