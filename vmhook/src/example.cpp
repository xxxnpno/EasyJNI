// Prevent windows.h (included by vmhook.hpp) from defining min/max macros.
#define NOMINMAX

#include <vmhook/vmhook.hpp>

#include <bit>
#include <limits>

// ─── Java class wrappers ─────────────────────────────────────────────────────
//
// These mirror the Java classes exactly and use ->get() / ->set() throughout,
// as in the original example.cpp.  The test logic below accesses fields
// through the base-class get_field() directly for types where ->get() cannot
// return the full value (char, String, arrays, object references).

class main_class : public vmhook::object<main_class>
{
public:
    explicit main_class(vmhook::oop_t instance)
        : vmhook::object<main_class>{ instance }
    {
    }

    auto get_stop_jvm()
        -> bool
    {
        return this->get_field("stopJVM")->get();
    }

    auto set_stop_jvm(bool value)
        -> void
    {
        this->get_field("stopJVM")->set(value);
    }
};

class example : public vmhook::object<example>
{
public:
    explicit example(vmhook::oop_t instance)
        : vmhook::object<example>{ instance }
    {
    }

    auto get_static_bool()
        -> bool
    {
        return this->get_field("staticBool")->get();
    }

    auto set_static_bool(bool value)
        -> void
    {
        this->get_field("staticBool")->set(value);
    }

    auto get_static_byte()
        -> std::byte
    {
        return this->get_field("staticByte")->get();
    }

    auto set_static_byte(std::byte value)
        -> void
    {
        this->get_field("staticByte")->set(value);
    }

    auto get_static_short()
        -> std::int16_t
    {
        return this->get_field("staticShort")->get();
    }

    auto set_static_short(std::int16_t value)
        -> void
    {
        this->get_field("staticShort")->set(value);
    }

    auto get_static_int()
        -> std::int32_t
    {
        return this->get_field("staticInt")->get();
    }

    auto set_static_int(std::int32_t value)
        -> void
    {
        this->get_field("staticInt")->set(value);
    }

    auto get_static_long()
        -> std::int64_t
    {
        return this->get_field("staticLong")->get();
    }

    auto set_static_long(std::int64_t value)
        -> void
    {
        this->get_field("staticLong")->set(value);
    }

    auto get_static_float()
        -> float
    {
        return this->get_field("staticFloat")->get();
    }

    auto set_static_float(float value)
        -> void
    {
        this->get_field("staticFloat")->set(value);
    }

    auto get_static_double()
        -> double
    {
        return this->get_field("staticDouble")->get();
    }

    auto set_static_double(double value)
        -> void
    {
        this->get_field("staticDouble")->set(value);
    }

    auto get_static_char()
        -> char
    {
        return this->get_field("staticChar")->get();
    }

    auto set_static_char(char value)
        -> void
    {
        this->get_field("staticChar")->set(value);
    }

    auto get_static_string()
        -> std::string
    {
        return this->get_field("staticString")->get();
    }

    auto set_static_string(const std::string& value)
        -> void
    {
        this->get_field("staticString")->set(value);
    }

    auto get_not_static_bool()
        -> bool
    {
        return this->get_field("notStaticBool")->get();
    }

    auto set_not_static_bool(bool value)
        -> void
    {
        this->get_field("notStaticBool")->set(value);
    }

    auto get_not_static_byte()
        -> std::byte
    {
        return this->get_field("notStaticByte")->get();
    }

    auto set_not_static_byte(std::byte value)
        -> void
    {
        this->get_field("notStaticByte")->set(value);
    }

    auto get_not_static_short()
        -> std::int16_t
    {
        return this->get_field("notStaticShort")->get();
    }

    auto set_not_static_short(std::int16_t value)
        -> void
    {
        this->get_field("notStaticShort")->set(value);
    }

    auto get_not_static_int()
        -> std::int32_t
    {
        return this->get_field("notStaticInt")->get();
    }

    auto set_not_static_int(std::int32_t value)
        -> void
    {
        this->get_field("notStaticInt")->set(value);
    }

    auto get_not_static_long()
        -> std::int64_t
    {
        return this->get_field("notStaticLong")->get();
    }

    auto set_not_static_long(std::int64_t value)
        -> void
    {
        this->get_field("notStaticLong")->set(value);
    }

    auto get_not_static_float()
        -> float
    {
        return this->get_field("notStaticFloat")->get();
    }

    auto set_not_static_float(float value)
        -> void
    {
        this->get_field("notStaticFloat")->set(value);
    }

    auto get_not_static_double()
        -> double
    {
        return this->get_field("notStaticDouble")->get();
    }

    auto set_not_static_double(double value)
        -> void
    {
        this->get_field("notStaticDouble")->set(value);
    }

    auto get_not_static_char()
        -> char
    {
        return this->get_field("notStaticChar")->get();
    }

    auto set_not_static_char(char value)
        -> void
    {
        this->get_field("notStaticChar")->set(value);
    }

    auto get_not_static_string()
        -> std::string
    {
        return this->get_field("notStaticString")->get();
    }

    auto set_not_static_string(const std::string& value)
        -> void
    {
        this->get_field("notStaticString")->set(value);
    }

    auto get_static_bool_array()
        -> std::vector<bool>
    {
        return this->get_field("staticBoolArray")->get();
    }

    auto set_static_bool_array(const std::vector<bool>& value)
        -> void
    {
        this->get_field("staticBoolArray")->set(value);
    }

    auto get_static_byte_array()
        -> std::vector<std::byte>
    {
        return this->get_field("staticByteArray")->get();
    }

    auto set_static_byte_array(const std::vector<std::byte>& value)
        -> void
    {
        this->get_field("staticByteArray")->set(value);
    }

    auto get_static_short_array()
        -> std::vector<std::int16_t>
    {
        return this->get_field("staticShortArray")->get();
    }

    auto set_static_short_array(const std::vector<std::int16_t>& value)
        -> void
    {
        this->get_field("staticShortArray")->set(value);
    }

    auto get_static_int_array()
        -> std::vector<std::int32_t>
    {
        return this->get_field("staticIntArray")->get();
    }

    auto set_static_int_array(const std::vector<std::int32_t>& value)
        -> void
    {
        this->get_field("staticIntArray")->set(value);
    }

    auto get_static_long_array()
        -> std::vector<std::int64_t>
    {
        return this->get_field("staticLongArray")->get();
    }

    auto set_static_long_array(const std::vector<std::int64_t>& value)
        -> void
    {
        this->get_field("staticLongArray")->set(value);
    }

    auto get_static_float_array()
        -> std::vector<float>
    {
        return this->get_field("staticFloatArray")->get();
    }

    auto set_static_float_array(const std::vector<float>& value)
        -> void
    {
        this->get_field("staticFloatArray")->set(value);
    }

    auto get_static_double_array()
        -> std::vector<double>
    {
        return this->get_field("staticDoubleArray")->get();
    }

    auto set_static_double_array(const std::vector<double>& value)
        -> void
    {
        this->get_field("staticDoubleArray")->set(value);
    }

    auto get_static_char_array()
        -> std::vector<char>
    {
        return this->get_field("staticCharArray")->get();
    }

    auto set_static_char_array(const std::vector<char>& value)
        -> void
    {
        this->get_field("staticCharArray")->set(value);
    }

    auto get_static_string_array()
        -> std::vector<std::string>
    {
        return this->get_field("staticStringArray")->get();
    }

    auto set_static_string_array(const std::vector<std::string>& value)
        -> void
    {
        this->get_field("staticStringArray")->set(value);
    }

    auto get_not_static_bool_array()
        -> std::vector<bool>
    {
        return this->get_field("notStaticBoolArray")->get();
    }

    auto set_not_static_bool_array(const std::vector<bool>& value)
        -> void
    {
        this->get_field("notStaticBoolArray")->set(value);
    }

    auto get_not_static_byte_array()
        -> std::vector<std::byte>
    {
        return this->get_field("notStaticByteArray")->get();
    }

    auto set_not_static_byte_array(const std::vector<std::byte>& value)
        -> void
    {
        this->get_field("notStaticByteArray")->set(value);
    }

    auto get_not_static_short_array()
        -> std::vector<std::int16_t>
    {
        return this->get_field("notStaticShortArray")->get();
    }

    auto set_not_static_short_array(const std::vector<std::int16_t>& value)
        -> void
    {
        this->get_field("notStaticShortArray")->set(value);
    }

    auto get_not_static_int_array()
        -> std::vector<std::int32_t>
    {
        return this->get_field("notStaticIntArray")->get();
    }

    auto set_not_static_int_array(const std::vector<std::int32_t>& value)
        -> void
    {
        this->get_field("notStaticIntArray")->set(value);
    }

    auto get_not_static_long_array()
        -> std::vector<std::int64_t>
    {
        return this->get_field("notStaticLongArray")->get();
    }

    auto set_not_static_long_array(const std::vector<std::int64_t>& value)
        -> void
    {
        this->get_field("notStaticLongArray")->set(value);
    }

    auto get_not_static_float_array()
        -> std::vector<float>
    {
        return this->get_field("notStaticFloatArray")->get();
    }

    auto set_not_static_float_array(const std::vector<float>& value)
        -> void
    {
        this->get_field("notStaticFloatArray")->set(value);
    }

    auto get_not_static_double_array()
        -> std::vector<double>
    {
        return this->get_field("notStaticDoubleArray")->get();
    }

    auto set_not_static_double_array(const std::vector<double>& value)
        -> void
    {
        this->get_field("notStaticDoubleArray")->set(value);
    }

    auto get_not_static_char_array()
        -> std::vector<char>
    {
        return this->get_field("notStaticCharArray")->get();
    }

    auto set_not_static_char_array(const std::vector<char>& value)
        -> void
    {
        this->get_field("notStaticCharArray")->set(value);
    }

    auto get_not_static_string_array()
        -> std::vector<std::string>
    {
        return this->get_field("notStaticStringArray")->get();
    }

    auto set_not_static_string_array(const std::vector<std::string>& value)
        -> void
    {
        this->get_field("notStaticStringArray")->set(value);
    }

    auto get_instance()
        -> std::unique_ptr<example>
    {
        return this->get_field("instance")->get();
    }

    auto set_instance(const std::unique_ptr<example>& value)
        -> void
    {
        this->get_field("instance")->set(value);
    }

    auto get_static_called()
        -> std::int32_t
    {
        return this->get_field("staticCalled")->get();
    }

    auto set_static_called(std::int32_t value)
        -> void
    {
        this->get_field("staticCalled")->set(value);
    }

    auto get_non_static_called()
        -> std::int32_t
    {
        return this->get_field("nonStaticCalled")->get();
    }

    auto set_non_static_called(std::int32_t value)
        -> void
    {
        this->get_field("nonStaticCalled")->set(value);
    }

    auto static_call_me(std::int32_t value)
        -> void
    {
        this->get_method("staticCallMe")->call(value);
    }

    auto not_static_call_me(std::int32_t value)
        -> void
    {
        this->get_method("notStaticCallMe")->call(value);
    }

    auto use_a(const std::unique_ptr<class a_class>& value)
        -> void
    {
        this->get_method("useA")->call(value);
    }
};

class a_class : public vmhook::object<a_class>
{
public:
    explicit a_class(vmhook::oop_t instance)
        : vmhook::object<a_class>{ instance }
    {
    }

    auto get_string()
        -> std::string
    {
        return this->get_field("string")->get();
    }

    auto set_string(const std::string& value)
        -> void
    {
        this->get_field("string")->set(value);
    }

    auto get_counter()
        -> std::int32_t
    {
        return this->get_field("counter")->get();
    }

    auto set_counter(std::int32_t value)
        -> void
    {
        this->get_field("counter")->set(value);
    }

    auto get_val()
        -> std::int32_t
    {
        return this->get_field("val")->get();
    }

    auto set_val(std::int32_t value)
        -> void
    {
        this->get_field("val")->set(value);
    }
};

// ─── Unit test framework ─────────────────────────────────────────────────────

static std::int32_t g_passed{ 0 };
static std::int32_t g_failed{ 0 };
static std::ofstream g_log{};

static auto emit(bool passed, const std::string& name,
                 const std::string& actual, const std::string& expected)
    -> void
{
    std::string line{};
    if (passed)
    {
        ++g_passed;
        line = std::format("[PASS] {} = {}", name, actual);
    }
    else
    {
        ++g_failed;
        line = std::format("[FAIL] {} : expected={} got={}", name, expected, actual);
    }
    std::println("{}", line);
    g_log << line << '\n';
}

// Upcast to int64 so int8_t / std::byte print as numbers, not control chars.
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

static auto check_str(const std::string& name,
                      const std::string& actual, const std::string& expected)
    -> void
{
    emit(actual == expected, name,
         "\"" + actual + "\"", "\"" + expected + "\"");
}

template<typename T>
static auto check_vec(const std::string& name,
                      const std::vector<T>& actual,
                      const std::vector<T>& expected)
    -> void
{
    auto to_str{ [](const std::vector<T>& v)
    {
        std::string s{ "[" };
        for (std::size_t i{}; i < v.size(); ++i)
        {
            if (i) s += ", ";
            if constexpr (std::is_same_v<T, std::byte>)
                s += std::format("{}", std::to_integer<int>(v[i]));
            else if constexpr (std::is_same_v<T, std::uint16_t>)
                s += std::format("{}", static_cast<std::uint32_t>(v[i]));
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
        return s + "]";
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
        return s + "]";
    }};
    emit(passed, name, to_str(actual), to_str(expected));
}

static auto check_str_vec(const std::string& name,
                          const std::vector<std::string>& actual,
                          const std::vector<std::string>& expected)
    -> void
{
    auto to_str{ [](const std::vector<std::string>& v)
    {
        std::string s{ "[" };
        for (std::size_t i{}; i < v.size(); ++i)
        {
            if (i) s += ", ";
            s += "\"" + v[i] + "\"";
        }
        return s + "]";
    }};
    emit(actual == expected, name, to_str(actual), to_str(expected));
}

// ─── Thread entry point ──────────────────────────────────────────────────────

static auto WINAPI thread_entry(HMODULE module)
    -> DWORD
{
    // Allow the JVM to finish loading classes (Class.forName in Main.java)
    // before walking HotSpot internals.
    Sleep(2000);

    g_log.open("test_results.txt", std::ios::out | std::ios::trunc);

    std::println("[VMHook Test] Starting unit tests...");
    g_log << "[VMHook Test] Starting unit tests...\n";

    vmhook::register_class<main_class>("vmhook/Main");
    vmhook::register_class<example>("vmhook/Example");
    vmhook::register_class<a_class>("vmhook/A");

    // Expected values taken verbatim from Example.java.
    static constexpr float  expected_float  = std::numeric_limits<float>::max();
    static constexpr double expected_double = std::numeric_limits<double>::max();

    // ── Static scalar fields ──────────────────────────────────────────────────
    // Primitive wrappers use ->get() and return correct C++ types.
    example ex{ nullptr };

    check_int ("staticBool",  ex.get_static_bool()   ? 1 : 0,  1);
    // std::byte is not constructible from int8_t (narrowing), so ->get() returns 0.
    // Read the field directly via field_proxy to get the real int8_t value.
    {
        const std::int8_t v{ ex.get_field("staticByte")->get() };
        check_int("staticByte", v, 0xf);
    }
    check_int ("staticShort", ex.get_static_short(), 0xff);
    check_int ("staticInt",   ex.get_static_int(),                        0xffff);
    check_int ("staticLong",  ex.get_static_long(),                       0xffffffffLL);
    check_float ("staticFloat",  ex.get_static_float(),  expected_float);
    check_double("staticDouble", ex.get_static_double(), expected_double);

    // Java char is 16-bit; the wrapper returns char (8-bit), so read the
    // field directly via field_proxy to get the full uint16_t value.
    {
        const std::uint16_t v{ ex.get_field("staticChar")->get() };
        check_uint("staticChar", v, 0xffffu);
    }

    // String fields: the wrapper's ->get() returns empty string for reference
    // types; use get_as_string() on the field_proxy directly.
    check_str("staticString",
        ex.get_field("staticString")->get_as_string(), "fortnite");

    check_int("staticCalled", ex.get_static_called(), 0);

    // ── Non-static scalar fields (via Example.instance) ───────────────────────
    // get_instance() with ->get() returns nullptr; access the static field
    // directly via field_proxy::get_as<> to obtain a live wrapper.
    auto inst{ ex.get_field("instance")->get_as<example>() };

    if (!inst)
    {
        const std::string msg{ "[FAIL] Example.instance : expected=non-null got=null" };
        std::println("{}", msg);
        g_log << msg << '\n';
        ++g_failed;
    }
    else
    {
        check_int ("notStaticBool",  inst->get_not_static_bool()   ? 1 : 0, 1);
        {
            const std::int8_t v{ inst->get_field("notStaticByte")->get() };
            check_int("notStaticByte", v, 0xf);
        }
        check_int ("notStaticShort", inst->get_not_static_short(), 0xff);
        check_int ("notStaticInt",   inst->get_not_static_int(),                        0xffff);
        check_int ("notStaticLong",  inst->get_not_static_long(),                       0xffffffffLL);
        check_float ("notStaticFloat",  inst->get_not_static_float(),  expected_float);
        check_double("notStaticDouble", inst->get_not_static_double(), expected_double);

        {
            const std::uint16_t v{ inst->get_field("notStaticChar")->get() };
            check_uint("notStaticChar", v, 0xffffu);
        }

        check_str("notStaticString",
            inst->get_field("notStaticString")->get_as_string(), "big yahu");

        check_int("nonStaticCalled", inst->get_non_static_called(), 0);
    }

    // ── Static array fields ───────────────────────────────────────────────────
    // Array wrappers call ->get() on a reference field which returns the
    // compressed OOP cast to the vector element count — wrong and potentially
    // unsafe.  Access the field_proxy directly for all array types.

    check_bool_vec("staticBoolArray",
        ex.get_field("staticBoolArray")->get_as_vector_bool(),
        { true, false, true });

    check_vec<std::byte>("staticByteArray",
        ex.get_field("staticByteArray")->get_as_vector<std::byte>(),
        { std::byte{0x1}, std::byte{0x2}, std::byte{0x3} });

    check_vec<std::int16_t>("staticShortArray",
        ex.get_field("staticShortArray")->get_as_vector<std::int16_t>(),
        { 0x10, 0x20, 0x30 });

    check_vec<std::int32_t>("staticIntArray",
        ex.get_field("staticIntArray")->get_as_vector<std::int32_t>(),
        { 0x100, 0x200, 0x300 });

    check_vec<std::int64_t>("staticLongArray",
        ex.get_field("staticLongArray")->get_as_vector<std::int64_t>(),
        { 0x1000LL, 0x2000LL, 0x3000LL });

    check_float_vec("staticFloatArray",
        ex.get_field("staticFloatArray")->get_as_vector<float>(),
        { 1.0f, 2.0f, 3.0f });

    check_double_vec("staticDoubleArray",
        ex.get_field("staticDoubleArray")->get_as_vector<double>(),
        { 1.0, 2.0, 3.0 });

    // Java char array – use uint16_t to preserve the full 16-bit value.
    check_vec<std::uint16_t>("staticCharArray",
        ex.get_field("staticCharArray")->get_as_vector<std::uint16_t>(),
        { std::uint16_t{'A'}, std::uint16_t{'B'}, std::uint16_t{'C'} });

    check_str_vec("staticStringArray",
        ex.get_field("staticStringArray")->get_as_string_vector(),
        { "hello", "world", "!" });

    // ── Non-static array fields ───────────────────────────────────────────────
    if (inst)
    {
        check_bool_vec("notStaticBoolArray",
            inst->get_field("notStaticBoolArray")->get_as_vector_bool(),
            { true, false, true });

        check_vec<std::byte>("notStaticByteArray",
            inst->get_field("notStaticByteArray")->get_as_vector<std::byte>(),
            { std::byte{0x1}, std::byte{0x2}, std::byte{0x3} });

        check_vec<std::int16_t>("notStaticShortArray",
            inst->get_field("notStaticShortArray")->get_as_vector<std::int16_t>(),
            { 0x10, 0x20, 0x30 });

        check_vec<std::int32_t>("notStaticIntArray",
            inst->get_field("notStaticIntArray")->get_as_vector<std::int32_t>(),
            { 0x100, 0x200, 0x300 });

        check_vec<std::int64_t>("notStaticLongArray",
            inst->get_field("notStaticLongArray")->get_as_vector<std::int64_t>(),
            { 0x1000LL, 0x2000LL, 0x3000LL });

        check_float_vec("notStaticFloatArray",
            inst->get_field("notStaticFloatArray")->get_as_vector<float>(),
            { 1.0f, 2.0f, 3.0f });

        check_double_vec("notStaticDoubleArray",
            inst->get_field("notStaticDoubleArray")->get_as_vector<double>(),
            { 1.0, 2.0, 3.0 });

        check_vec<std::uint16_t>("notStaticCharArray",
            inst->get_field("notStaticCharArray")->get_as_vector<std::uint16_t>(),
            { std::uint16_t{'X'}, std::uint16_t{'Y'}, std::uint16_t{'Z'} });

        check_str_vec("notStaticStringArray",
            inst->get_field("notStaticStringArray")->get_as_string_vector(),
            { "we", "like", "vmhook" });
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
