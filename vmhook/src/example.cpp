// Prevent windows.h (included by vmhook.hpp) from defining min/max macros.
#define NOMINMAX

#include <vmhook/vmhook.hpp>

#include <bit>
#include <limits>

// ─── Java class wrappers ────────────────────────────────────────────────────

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

    // ── Static scalar getters ────────────────────────────────────────────
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

    // Java char is an unsigned 16-bit code unit; char16_t is the correct C++ type.
    auto get_static_char()
        -> char16_t
    {
        return static_cast<char16_t>(static_cast<std::uint16_t>(this->get_field("staticChar")->get()));
    }

    auto set_static_char(char16_t value)
        -> void
    {
        this->get_field("staticChar")->set(static_cast<std::uint16_t>(value));
    }

    auto get_static_string()
        -> std::string
    {
        return this->get_field("staticString")->get_as_string();
    }

    auto set_static_string(const std::string& value)
        -> void
    {
        this->get_field("staticString")->set(value);
    }

    // ── Non-static scalar getters ────────────────────────────────────────
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
        -> char16_t
    {
        return static_cast<char16_t>(static_cast<std::uint16_t>(this->get_field("notStaticChar")->get()));
    }

    auto set_not_static_char(char16_t value)
        -> void
    {
        this->get_field("notStaticChar")->set(static_cast<std::uint16_t>(value));
    }

    auto get_not_static_string()
        -> std::string
    {
        return this->get_field("notStaticString")->get_as_string();
    }

    auto set_not_static_string(const std::string& value)
        -> void
    {
        this->get_field("notStaticString")->set(value);
    }

    // ── Static array getters ─────────────────────────────────────────────
    auto get_static_bool_array()
        -> std::vector<bool>
    {
        return this->get_field("staticBoolArray")->get_as_vector_bool();
    }

    auto set_static_bool_array(const std::vector<bool>& value)
        -> void
    {
        this->get_field("staticBoolArray")->set(value);
    }

    auto get_static_byte_array()
        -> std::vector<std::byte>
    {
        return this->get_field("staticByteArray")->get_as_vector<std::byte>();
    }

    auto set_static_byte_array(const std::vector<std::byte>& value)
        -> void
    {
        this->get_field("staticByteArray")->set(value);
    }

    auto get_static_short_array()
        -> std::vector<std::int16_t>
    {
        return this->get_field("staticShortArray")->get_as_vector<std::int16_t>();
    }

    auto set_static_short_array(const std::vector<std::int16_t>& value)
        -> void
    {
        this->get_field("staticShortArray")->set(value);
    }

    auto get_static_int_array()
        -> std::vector<std::int32_t>
    {
        return this->get_field("staticIntArray")->get_as_vector<std::int32_t>();
    }

    auto set_static_int_array(const std::vector<std::int32_t>& value)
        -> void
    {
        this->get_field("staticIntArray")->set(value);
    }

    auto get_static_long_array()
        -> std::vector<std::int64_t>
    {
        return this->get_field("staticLongArray")->get_as_vector<std::int64_t>();
    }

    auto set_static_long_array(const std::vector<std::int64_t>& value)
        -> void
    {
        this->get_field("staticLongArray")->set(value);
    }

    auto get_static_float_array()
        -> std::vector<float>
    {
        return this->get_field("staticFloatArray")->get_as_vector<float>();
    }

    auto set_static_float_array(const std::vector<float>& value)
        -> void
    {
        this->get_field("staticFloatArray")->set(value);
    }

    auto get_static_double_array()
        -> std::vector<double>
    {
        return this->get_field("staticDoubleArray")->get_as_vector<double>();
    }

    auto set_static_double_array(const std::vector<double>& value)
        -> void
    {
        this->get_field("staticDoubleArray")->set(value);
    }

    // char16_t matches the JVM's unsigned 16-bit Java char exactly.
    auto get_static_char_array()
        -> std::vector<char16_t>
    {
        return this->get_field("staticCharArray")->get_as_vector<char16_t>();
    }

    auto set_static_char_array(const std::vector<char16_t>& value)
        -> void
    {
        this->get_field("staticCharArray")->set(value);
    }

    auto get_static_string_array()
        -> std::vector<std::string>
    {
        return this->get_field("staticStringArray")->get_as_string_vector();
    }

    auto set_static_string_array(const std::vector<std::string>& value)
        -> void
    {
        this->get_field("staticStringArray")->set(value);
    }

    // ── Non-static array getters ─────────────────────────────────────────
    auto get_not_static_bool_array()
        -> std::vector<bool>
    {
        return this->get_field("notStaticBoolArray")->get_as_vector_bool();
    }

    auto set_not_static_bool_array(const std::vector<bool>& value)
        -> void
    {
        this->get_field("notStaticBoolArray")->set(value);
    }

    auto get_not_static_byte_array()
        -> std::vector<std::byte>
    {
        return this->get_field("notStaticByteArray")->get_as_vector<std::byte>();
    }

    auto set_not_static_byte_array(const std::vector<std::byte>& value)
        -> void
    {
        this->get_field("notStaticByteArray")->set(value);
    }

    auto get_not_static_short_array()
        -> std::vector<std::int16_t>
    {
        return this->get_field("notStaticShortArray")->get_as_vector<std::int16_t>();
    }

    auto set_not_static_short_array(const std::vector<std::int16_t>& value)
        -> void
    {
        this->get_field("notStaticShortArray")->set(value);
    }

    auto get_not_static_int_array()
        -> std::vector<std::int32_t>
    {
        return this->get_field("notStaticIntArray")->get_as_vector<std::int32_t>();
    }

    auto set_not_static_int_array(const std::vector<std::int32_t>& value)
        -> void
    {
        this->get_field("notStaticIntArray")->set(value);
    }

    auto get_not_static_long_array()
        -> std::vector<std::int64_t>
    {
        return this->get_field("notStaticLongArray")->get_as_vector<std::int64_t>();
    }

    auto set_not_static_long_array(const std::vector<std::int64_t>& value)
        -> void
    {
        this->get_field("notStaticLongArray")->set(value);
    }

    auto get_not_static_float_array()
        -> std::vector<float>
    {
        return this->get_field("notStaticFloatArray")->get_as_vector<float>();
    }

    auto set_not_static_float_array(const std::vector<float>& value)
        -> void
    {
        this->get_field("notStaticFloatArray")->set(value);
    }

    auto get_not_static_double_array()
        -> std::vector<double>
    {
        return this->get_field("notStaticDoubleArray")->get_as_vector<double>();
    }

    auto set_not_static_double_array(const std::vector<double>& value)
        -> void
    {
        this->get_field("notStaticDoubleArray")->set(value);
    }

    auto get_not_static_char_array()
        -> std::vector<char16_t>
    {
        return this->get_field("notStaticCharArray")->get_as_vector<char16_t>();
    }

    auto set_not_static_char_array(const std::vector<char16_t>& value)
        -> void
    {
        this->get_field("notStaticCharArray")->set(value);
    }

    auto get_not_static_string_array()
        -> std::vector<std::string>
    {
        return this->get_field("notStaticStringArray")->get_as_string_vector();
    }

    auto set_not_static_string_array(const std::vector<std::string>& value)
        -> void
    {
        this->get_field("notStaticStringArray")->set(value);
    }

    // ── Misc getters ─────────────────────────────────────────────────────
    auto get_instance()
        -> std::unique_ptr<example>
    {
        return this->get_field("instance")->get_as<example>();
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
        return this->get_field("string")->get_as_string();
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

static auto emit(bool passed, const std::string& name, const std::string& actual, const std::string& expected)
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

// Integer comparison – upcasts to int64 so int8_t/byte print as numbers, not chars.
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

static auto check_str(const std::string& name, const std::string& actual, const std::string& expected)
    -> void
{
    emit(actual == expected, name, actual, expected);
}

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
            if constexpr (std::is_same_v<element_type, std::byte>)
                s += std::format("{}", std::to_integer<int>(v[i]));
            else if constexpr (std::is_same_v<element_type, char16_t>)
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
        for (std::size_t i{}; i < v.size(); ++i) { if (i) s += ", "; s += v[i] ? "true" : "false"; }
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
        for (std::size_t i{}; i < v.size(); ++i) { if (i) s += ", "; s += "\"" + v[i] + "\""; }
        return s + "]";
    }};
    emit(actual == expected, name, to_str(actual), to_str(expected));
}

// ─── Thread entry point ──────────────────────────────────────────────────────

static auto WINAPI thread_entry(HMODULE module)
    -> DWORD
{
    // Allow the JVM to finish loading all classes (including vmhook/Example via
    // Class.forName in Main.java) before we walk HotSpot internals.
    Sleep(2000);

    g_log.open("test_results.txt", std::ios::out | std::ios::trunc);

    std::println("[VMHook Test] Starting unit tests...");
    g_log << "[VMHook Test] Starting unit tests...\n";

    vmhook::register_class<main_class>("vmhook/Main");
    vmhook::register_class<example>("vmhook/Example");
    vmhook::register_class<a_class>("vmhook/A");

    // Expected values taken verbatim from Example.java.
    // Float/double expected values match Java's Float/Double.intBitsToFloat / longBitsToDouble.
    static constexpr float  expected_float  = std::numeric_limits<float>::max();   // 0x7f7fffff
    static constexpr double expected_double = std::numeric_limits<double>::max();  // 0x7fefffffffffffff

    // ── Static scalar fields ──────────────────────────────────────────────────
    example ex{ nullptr };

    check_int ("staticBool",   ex.get_static_bool()   ? 1 : 0,              1);
    check_int ("staticByte",   std::to_integer<int>(ex.get_static_byte()),  0xf);
    check_int ("staticShort",  ex.get_static_short(),                       0xff);
    check_int ("staticInt",    ex.get_static_int(),                         0xffff);
    check_int ("staticLong",   ex.get_static_long(),                        0xffffffffLL);
    check_float ("staticFloat",  ex.get_static_float(),  expected_float);
    check_double("staticDouble", ex.get_static_double(), expected_double);
    check_uint("staticChar",   static_cast<std::uint64_t>(ex.get_static_char()), 0xffffu);
    check_str ("staticString", ex.get_static_string(),  "fortnite");
    check_int ("staticCalled", ex.get_static_called(),  0);

    // ── Non-static scalar fields (via Example.instance) ───────────────────────
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
        check_int ("notStaticBool",   inst->get_not_static_bool()   ? 1 : 0,              1);
        check_int ("notStaticByte",   std::to_integer<int>(inst->get_not_static_byte()),  0xf);
        check_int ("notStaticShort",  inst->get_not_static_short(),                       0xff);
        check_int ("notStaticInt",    inst->get_not_static_int(),                         0xffff);
        check_int ("notStaticLong",   inst->get_not_static_long(),                        0xffffffffLL);
        check_float ("notStaticFloat",  inst->get_not_static_float(),  expected_float);
        check_double("notStaticDouble", inst->get_not_static_double(), expected_double);
        check_uint("notStaticChar",   static_cast<std::uint64_t>(inst->get_not_static_char()), 0xffffu);
        check_str ("notStaticString", inst->get_not_static_string(),  "big yahu");
        check_int ("nonStaticCalled", inst->get_non_static_called(),  0);
    }

    // ── Static array fields ───────────────────────────────────────────────────
    check_bool_vec  ("staticBoolArray",
        ex.get_static_bool_array(),   { true, false, true });
    check_vec<std::byte>("staticByteArray",
        ex.get_static_byte_array(),   { std::byte{0x1}, std::byte{0x2}, std::byte{0x3} });
    check_vec<std::int16_t>("staticShortArray",
        ex.get_static_short_array(),  { 0x10, 0x20, 0x30 });
    check_vec<std::int32_t>("staticIntArray",
        ex.get_static_int_array(),    { 0x100, 0x200, 0x300 });
    check_vec<std::int64_t>("staticLongArray",
        ex.get_static_long_array(),   { 0x1000LL, 0x2000LL, 0x3000LL });
    check_float_vec ("staticFloatArray",
        ex.get_static_float_array(),  { 1.0f, 2.0f, 3.0f });
    check_double_vec("staticDoubleArray",
        ex.get_static_double_array(), { 1.0, 2.0, 3.0 });
    check_vec<char16_t>("staticCharArray",
        ex.get_static_char_array(),
        { char16_t{ 'A' }, char16_t{ 'B' }, char16_t{ 'C' } });
    check_str_vec("staticStringArray",
        ex.get_static_string_array(), { "hello", "world", "!" });

    // ── Non-static array fields ───────────────────────────────────────────────
    if (inst)
    {
        check_bool_vec  ("notStaticBoolArray",
            inst->get_not_static_bool_array(),   { true, false, true });
        check_vec<std::byte>("notStaticByteArray",
            inst->get_not_static_byte_array(),   { std::byte{0x1}, std::byte{0x2}, std::byte{0x3} });
        check_vec<std::int16_t>("notStaticShortArray",
            inst->get_not_static_short_array(),  { 0x10, 0x20, 0x30 });
        check_vec<std::int32_t>("notStaticIntArray",
            inst->get_not_static_int_array(),    { 0x100, 0x200, 0x300 });
        check_vec<std::int64_t>("notStaticLongArray",
            inst->get_not_static_long_array(),   { 0x1000LL, 0x2000LL, 0x3000LL });
        check_float_vec ("notStaticFloatArray",
            inst->get_not_static_float_array(),  { 1.0f, 2.0f, 3.0f });
        check_double_vec("notStaticDoubleArray",
            inst->get_not_static_double_array(), { 1.0, 2.0, 3.0 });
        check_vec<char16_t>("notStaticCharArray",
            inst->get_not_static_char_array(),
            { char16_t{ 'X' }, char16_t{ 'Y' }, char16_t{ 'Z' } });
        check_str_vec("notStaticStringArray",
            inst->get_not_static_string_array(), { "we", "like", "vmhook" });
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
