#include <vmhook/vmhook.hpp>

#include <cmath>
#include <cstddef>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

/*
    These wrapper classes are the public example surface.
    Each Java field or method has one C++ function that hides the lower-level
    vmhook proxy objects from the rest of the example.
*/
class main_class : public vmhook::object<main_class>
{
public:
    explicit main_class(vmhook::oop_t instance)
        : vmhook::object<main_class>{ instance }
    {
    }

    static auto get_field(const std::string_view name)
        -> std::optional<vmhook::field_proxy>
    {
        return get_static_field(name);
    }

    static auto get_stop_jvm()
        -> bool
    {
        return get_field("stopJVM")->get();
    }

    static auto set_stop_jvm(bool value)
        -> void
    {
        get_field("stopJVM")->set(value);
    }
};

class example_class : public vmhook::object<example_class>
{
public:
    explicit example_class(vmhook::oop_t instance)
        : vmhook::object<example_class>{ instance }
    {
    }

    static auto get_field(const std::string_view name)
        -> std::optional<vmhook::field_proxy>
    {
        return get_static_field(name);
    }

    static auto get_method(const std::string_view method_name)
        -> std::optional<vmhook::method_proxy>
    {
        return get_static_method(method_name);
    }

    static auto get_static_bool()
        -> bool
    {
        return get_field("staticBool")->get();
    }

    static auto set_static_bool(bool value)
        -> void
    {
        get_field("staticBool")->set(value);
    }

    static auto get_static_byte()
        -> std::byte
    {
        return get_field("staticByte")->get();
    }

    static auto set_static_byte(std::byte value)
        -> void
    {
        get_field("staticByte")->set(value);
    }

    static auto get_static_short()
        -> std::int16_t
    {
        return get_field("staticShort")->get();
    }

    static auto set_static_short(std::int16_t value)
        -> void
    {
        get_field("staticShort")->set(value);
    }

    static auto get_static_int()
        -> std::int32_t
    {
        return get_field("staticInt")->get();
    }

    static auto set_static_int(std::int32_t value)
        -> void
    {
        get_field("staticInt")->set(value);
    }

    static auto get_static_long()
        -> std::int64_t
    {
        return get_field("staticLong")->get();
    }

    static auto set_static_long(std::int64_t value)
        -> void
    {
        get_field("staticLong")->set(value);
    }

    static auto get_static_float()
        -> float
    {
        return get_field("staticFloat")->get();
    }

    static auto set_static_float(float value)
        -> void
    {
        get_field("staticFloat")->set(value);
    }

    static auto get_static_double()
        -> double
    {
        return get_field("staticDouble")->get();
    }

    static auto set_static_double(double value)
        -> void
    {
        get_field("staticDouble")->set(value);
    }

    static auto get_static_char()
        -> char
    {
        return get_field("staticChar")->get();
    }

    static auto set_static_char(char value)
        -> void
    {
        get_field("staticChar")->set(value);
    }

    static auto get_static_string()
        -> std::string
    {
        return get_field("staticString")->get();
    }

    static auto set_static_string(const std::string& value)
        -> void
    {
        get_field("staticString")->set(value);
    }

    auto get_not_static_bool()
        -> bool
    {
        return this->vmhook::object_base::get_field("notStaticBool")->get();
    }

    auto set_not_static_bool(bool value)
        -> void
    {
        this->vmhook::object_base::get_field("notStaticBool")->set(value);
    }

    auto get_not_static_byte()
        -> std::byte
    {
        return this->vmhook::object_base::get_field("notStaticByte")->get();
    }

    auto set_not_static_byte(std::byte value)
        -> void
    {
        this->vmhook::object_base::get_field("notStaticByte")->set(value);
    }

    auto get_not_static_short()
        -> std::int16_t
    {
        return this->vmhook::object_base::get_field("notStaticShort")->get();
    }

    auto set_not_static_short(std::int16_t value)
        -> void
    {
        this->vmhook::object_base::get_field("notStaticShort")->set(value);
    }

    auto get_not_static_int()
        -> std::int32_t
    {
        return this->vmhook::object_base::get_field("notStaticInt")->get();
    }

    auto set_not_static_int(std::int32_t value)
        -> void
    {
        this->vmhook::object_base::get_field("notStaticInt")->set(value);
    }

    auto get_not_static_long()
        -> std::int64_t
    {
        return this->vmhook::object_base::get_field("notStaticLong")->get();
    }

    auto set_not_static_long(std::int64_t value)
        -> void
    {
        this->vmhook::object_base::get_field("notStaticLong")->set(value);
    }

    auto get_not_static_float()
        -> float
    {
        return this->vmhook::object_base::get_field("notStaticFloat")->get();
    }

    auto set_not_static_float(float value)
        -> void
    {
        this->vmhook::object_base::get_field("notStaticFloat")->set(value);
    }

    auto get_not_static_double()
        -> double
    {
        return this->vmhook::object_base::get_field("notStaticDouble")->get();
    }

    auto set_not_static_double(double value)
        -> void
    {
        this->vmhook::object_base::get_field("notStaticDouble")->set(value);
    }

    auto get_not_static_char()
        -> char
    {
        return this->vmhook::object_base::get_field("notStaticChar")->get();
    }

    auto set_not_static_char(char value)
        -> void
    {
        this->vmhook::object_base::get_field("notStaticChar")->set(value);
    }

    auto get_not_static_string()
        -> std::string
    {
        return this->vmhook::object_base::get_field("notStaticString")->get();
    }

    auto set_not_static_string(const std::string& value)
        -> void
    {
        this->vmhook::object_base::get_field("notStaticString")->set(value);
    }

    static auto get_static_bool_array()
        -> std::vector<bool>
    {
        return get_field("staticBoolArray")->get();
    }

    static auto set_static_bool_array(const std::vector<bool>& value)
        -> void
    {
        get_field("staticBoolArray")->set(value);
    }

    static auto get_static_byte_array()
        -> std::vector<std::byte>
    {
        return get_field("staticByteArray")->get();
    }

    static auto set_static_byte_array(const std::vector<std::byte>& value)
        -> void
    {
        get_field("staticByteArray")->set(value);
    }

    static auto get_static_short_array()
        -> std::vector<std::int16_t>
    {
        return get_field("staticShortArray")->get();
    }

    static auto set_static_short_array(const std::vector<std::int16_t>& value)
        -> void
    {
        get_field("staticShortArray")->set(value);
    }

    static auto get_static_int_array()
        -> std::vector<std::int32_t>
    {
        return get_field("staticIntArray")->get();
    }

    static auto set_static_int_array(const std::vector<std::int32_t>& value)
        -> void
    {
        get_field("staticIntArray")->set(value);
    }

    static auto get_static_long_array()
        -> std::vector<std::int64_t>
    {
        return get_field("staticLongArray")->get();
    }

    static auto set_static_long_array(const std::vector<std::int64_t>& value)
        -> void
    {
        get_field("staticLongArray")->set(value);
    }

    static auto get_static_float_array()
        -> std::vector<float>
    {
        return get_field("staticFloatArray")->get();
    }

    static auto set_static_float_array(const std::vector<float>& value)
        -> void
    {
        get_field("staticFloatArray")->set(value);
    }

    static auto get_static_double_array()
        -> std::vector<double>
    {
        return get_field("staticDoubleArray")->get();
    }

    static auto set_static_double_array(const std::vector<double>& value)
        -> void
    {
        get_field("staticDoubleArray")->set(value);
    }

    static auto get_static_char_array()
        -> std::vector<char>
    {
        return get_field("staticCharArray")->get();
    }

    static auto set_static_char_array(const std::vector<char>& value)
        -> void
    {
        get_field("staticCharArray")->set(value);
    }

    static auto get_static_string_array()
        -> std::vector<std::string>
    {
        return get_field("staticStringArray")->get();
    }

    static auto set_static_string_array(const std::vector<std::string>& value)
        -> void
    {
        get_field("staticStringArray")->set(value);
    }

    auto get_not_static_bool_array()
        -> std::vector<bool>
    {
        return this->vmhook::object_base::get_field("notStaticBoolArray")->get();
    }

    auto set_not_static_bool_array(const std::vector<bool>& value)
        -> void
    {
        this->vmhook::object_base::get_field("notStaticBoolArray")->set(value);
    }

    auto get_not_static_byte_array()
        -> std::vector<std::byte>
    {
        return this->vmhook::object_base::get_field("notStaticByteArray")->get();
    }

    auto set_not_static_byte_array(const std::vector<std::byte>& value)
        -> void
    {
        this->vmhook::object_base::get_field("notStaticByteArray")->set(value);
    }

    auto get_not_static_short_array()
        -> std::vector<std::int16_t>
    {
        return this->vmhook::object_base::get_field("notStaticShortArray")->get();
    }

    auto set_not_static_short_array(const std::vector<std::int16_t>& value)
        -> void
    {
        this->vmhook::object_base::get_field("notStaticShortArray")->set(value);
    }

    auto get_not_static_int_array()
        -> std::vector<std::int32_t>
    {
        return this->vmhook::object_base::get_field("notStaticIntArray")->get();
    }

    auto set_not_static_int_array(const std::vector<std::int32_t>& value)
        -> void
    {
        this->vmhook::object_base::get_field("notStaticIntArray")->set(value);
    }

    auto get_not_static_long_array()
        -> std::vector<std::int64_t>
    {
        return this->vmhook::object_base::get_field("notStaticLongArray")->get();
    }

    auto set_not_static_long_array(const std::vector<std::int64_t>& value)
        -> void
    {
        this->vmhook::object_base::get_field("notStaticLongArray")->set(value);
    }

    auto get_not_static_float_array()
        -> std::vector<float>
    {
        return this->vmhook::object_base::get_field("notStaticFloatArray")->get();
    }

    auto set_not_static_float_array(const std::vector<float>& value)
        -> void
    {
        this->vmhook::object_base::get_field("notStaticFloatArray")->set(value);
    }

    auto get_not_static_double_array()
        -> std::vector<double>
    {
        return this->vmhook::object_base::get_field("notStaticDoubleArray")->get();
    }

    auto set_not_static_double_array(const std::vector<double>& value)
        -> void
    {
        this->vmhook::object_base::get_field("notStaticDoubleArray")->set(value);
    }

    auto get_not_static_char_array()
        -> std::vector<char>
    {
        return this->vmhook::object_base::get_field("notStaticCharArray")->get();
    }

    auto set_not_static_char_array(const std::vector<char>& value)
        -> void
    {
        this->vmhook::object_base::get_field("notStaticCharArray")->set(value);
    }

    auto get_not_static_string_array()
        -> std::vector<std::string>
    {
        return this->vmhook::object_base::get_field("notStaticStringArray")->get();
    }

    auto set_not_static_string_array(const std::vector<std::string>& value)
        -> void
    {
        this->vmhook::object_base::get_field("notStaticStringArray")->set(value);
    }

    static auto get_instance()
        -> std::unique_ptr<example_class>
    {
        return get_field("instance")->get();
    }

    static auto set_instance(const std::unique_ptr<example_class>& value)
        -> void
    {
        get_field("instance")->set(value);
    }

    static auto get_static_called()
        -> std::int32_t
    {
        return get_field("staticCalled")->get();
    }

    static auto set_static_called(std::int32_t value)
        -> void
    {
        get_field("staticCalled")->set(value);
    }

    auto get_non_static_called()
        -> std::int32_t
    {
        return this->vmhook::object_base::get_field("nonStaticCalled")->get();
    }

    auto set_non_static_called(std::int32_t value)
        -> void
    {
        this->vmhook::object_base::get_field("nonStaticCalled")->set(value);
    }

    static auto static_call_me(std::int32_t value)
        -> void
    {
        get_method("staticCallMe")->call(value);
    }

    auto not_static_call_me(std::int32_t value)
        -> void
    {
        this->vmhook::object_base::get_method("notStaticCallMe")->call(value);
    }

    auto use_a(const std::unique_ptr<class a_class>& value)
        -> void
    {
        this->vmhook::object_base::get_method("useA")->call(value);
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

namespace
{
    /*
        The GitHub Actions workflow reads test_results.txt after injection.
        Every check below uses the wrapper classes above instead of calling
        lower-level field helpers directly.
    */
    std::ofstream test_log{};
    std::size_t passed_checks{};
    std::size_t failed_checks{};

    auto write_result(const std::string& line)
        -> void
    {
        if (test_log.is_open())
        {
            test_log << line << '\n';
        }
    }

    auto check(const std::string& name, const bool condition)
        -> void
    {
        if (condition)
        {
            ++passed_checks;
            write_result("[PASS] " + name);
            return;
        }

        ++failed_checks;
        write_result("[FAIL] " + name);
    }

    template <typename value_type>
    auto check_equal(const std::string& name, const value_type& actual, const value_type& expected)
        -> void
    {
        check(name, actual == expected);
    }

    auto check_equal(const std::string& name, const std::byte actual, const std::byte expected)
        -> void
    {
        check(name, std::to_integer<int>(actual) == std::to_integer<int>(expected));
    }

    auto check_float(const std::string& name, const float actual, const float expected)
        -> void
    {
        check(name, std::fabs(actual - expected) < 0.0001F);
    }

    auto check_double(const std::string& name, const double actual, const double expected)
        -> void
    {
        check(name, std::fabs(actual - expected) < 0.0001);
    }

    template <typename value_type>
    auto check_vector(const std::string& name, const std::vector<value_type>& actual, const std::vector<value_type>& expected)
        -> void
    {
        check(name, actual == expected);
    }

    auto write_summary()
        -> void
    {
        std::ostringstream line{};
        line << "TOTAL: " << passed_checks << "/" << (passed_checks + failed_checks) << " PASSED";
        write_result(line.str());
    }

    auto set_expected_values(example_class& instance)
        -> void
    {
        /*
            Static Java fields are accessed as C++ static methods. Instance Java
            fields are accessed through the wrapper returned by get_instance().
        */
        example_class::set_static_bool(false);
        example_class::set_static_byte(std::byte{ 7 });
        example_class::set_static_short(127);
        example_class::set_static_int(0x7fff);
        example_class::set_static_long(0x7fffffffL);
        example_class::set_static_float(1.0F);
        example_class::set_static_double(2.0);
        example_class::set_static_char('A');
        example_class::set_static_string("java_ftw");

        instance.set_not_static_bool(false);
        instance.set_not_static_byte(std::byte{ 7 });
        instance.set_not_static_short(127);
        instance.set_not_static_int(0x7fff);
        instance.set_not_static_long(0x7fffffffL);
        instance.set_not_static_float(1.0F);
        instance.set_not_static_double(2.0);
        instance.set_not_static_char('B');
        instance.set_not_static_string("cppwins!");

        example_class::set_static_bool_array({ false, true, false });
        example_class::set_static_byte_array({ std::byte{ 10 }, std::byte{ 20 }, std::byte{ 30 } });
        example_class::set_static_short_array({ 256, 512, 768 });
        example_class::set_static_int_array({ 4096, 8192, 12288 });
        example_class::set_static_long_array({ 65536L, 131072L, 196608L });
        example_class::set_static_float_array({ 4.0F, 5.0F, 6.0F });
        example_class::set_static_double_array({ 4.0, 5.0, 6.0 });
        example_class::set_static_char_array({ 'X', 'Y', 'Z' });
        example_class::set_static_string_array({ "alpha", "omega", "?" });

        instance.set_not_static_bool_array({ false, true, false });
        instance.set_not_static_byte_array({ std::byte{ 10 }, std::byte{ 20 }, std::byte{ 30 } });
        instance.set_not_static_short_array({ 256, 512, 768 });
        instance.set_not_static_int_array({ 4096, 8192, 12288 });
        instance.set_not_static_long_array({ 65536L, 131072L, 196608L });
        instance.set_not_static_float_array({ 4.0F, 5.0F, 6.0F });
        instance.set_not_static_double_array({ 4.0, 5.0, 6.0 });
        instance.set_not_static_char_array({ 'D', 'E', 'F' });
        instance.set_not_static_string_array({ "ab", "love", "coding" });
    }

    auto verify_expected_values(example_class& instance)
        -> void
    {
        /*
            The verification intentionally repeats the wrapper API used by users:
            get() for every field type, set() for every field type, and call(...)
            for methods.
        */
        check_equal("staticBool", example_class::get_static_bool(), false);
        check_equal("staticByte", example_class::get_static_byte(), std::byte{ 7 });
        check_equal("staticShort", example_class::get_static_short(), static_cast<std::int16_t>(127));
        check_equal("staticInt", example_class::get_static_int(), static_cast<std::int32_t>(0x7fff));
        check_equal("staticLong", example_class::get_static_long(), static_cast<std::int64_t>(0x7fffffffL));
        check_float("staticFloat", example_class::get_static_float(), 1.0F);
        check_double("staticDouble", example_class::get_static_double(), 2.0);
        check_equal("staticChar", example_class::get_static_char(), 'A');
        check_equal("staticString", example_class::get_static_string(), std::string{ "java_ftw" });

        check_equal("notStaticBool", instance.get_not_static_bool(), false);
        check_equal("notStaticByte", instance.get_not_static_byte(), std::byte{ 7 });
        check_equal("notStaticShort", instance.get_not_static_short(), static_cast<std::int16_t>(127));
        check_equal("notStaticInt", instance.get_not_static_int(), static_cast<std::int32_t>(0x7fff));
        check_equal("notStaticLong", instance.get_not_static_long(), static_cast<std::int64_t>(0x7fffffffL));
        check_float("notStaticFloat", instance.get_not_static_float(), 1.0F);
        check_double("notStaticDouble", instance.get_not_static_double(), 2.0);
        check_equal("notStaticChar", instance.get_not_static_char(), 'B');
        check_equal("notStaticString", instance.get_not_static_string(), std::string{ "cppwins!" });

        check_vector("staticBoolArray", example_class::get_static_bool_array(), std::vector<bool>{ false, true, false });
        check_vector("staticByteArray", example_class::get_static_byte_array(), std::vector<std::byte>{ std::byte{ 10 }, std::byte{ 20 }, std::byte{ 30 } });
        check_vector("staticShortArray", example_class::get_static_short_array(), std::vector<std::int16_t>{ 256, 512, 768 });
        check_vector("staticIntArray", example_class::get_static_int_array(), std::vector<std::int32_t>{ 4096, 8192, 12288 });
        check_vector("staticLongArray", example_class::get_static_long_array(), std::vector<std::int64_t>{ 65536L, 131072L, 196608L });
        check_vector("staticFloatArray", example_class::get_static_float_array(), std::vector<float>{ 4.0F, 5.0F, 6.0F });
        check_vector("staticDoubleArray", example_class::get_static_double_array(), std::vector<double>{ 4.0, 5.0, 6.0 });
        check_vector("staticCharArray", example_class::get_static_char_array(), std::vector<char>{ 'X', 'Y', 'Z' });
        check_vector("staticStringArray", example_class::get_static_string_array(), std::vector<std::string>{ "alpha", "omega", "?" });

        check_vector("notStaticBoolArray", instance.get_not_static_bool_array(), std::vector<bool>{ false, true, false });
        check_vector("notStaticByteArray", instance.get_not_static_byte_array(), std::vector<std::byte>{ std::byte{ 10 }, std::byte{ 20 }, std::byte{ 30 } });
        check_vector("notStaticShortArray", instance.get_not_static_short_array(), std::vector<std::int16_t>{ 256, 512, 768 });
        check_vector("notStaticIntArray", instance.get_not_static_int_array(), std::vector<std::int32_t>{ 4096, 8192, 12288 });
        check_vector("notStaticLongArray", instance.get_not_static_long_array(), std::vector<std::int64_t>{ 65536L, 131072L, 196608L });
        check_vector("notStaticFloatArray", instance.get_not_static_float_array(), std::vector<float>{ 4.0F, 5.0F, 6.0F });
        check_vector("notStaticDoubleArray", instance.get_not_static_double_array(), std::vector<double>{ 4.0, 5.0, 6.0 });
        check_vector("notStaticCharArray", instance.get_not_static_char_array(), std::vector<char>{ 'D', 'E', 'F' });
        check_vector("notStaticStringArray", instance.get_not_static_string_array(), std::vector<std::string>{ "ab", "love", "coding" });
    }

    auto call_example_methods(example_class& instance)
        -> void
    {
        /*
            Method calls intentionally mirror field access. The wrapper chooses
            the Java method name, and the call site passes only the C++ arguments.
        */
        example_class::static_call_me(1);
        instance.not_static_call_me(2);
    }
}

static auto WINAPI thread_entry(HMODULE module)
    -> DWORD
{
    Sleep(2000);

    test_log.open("test_results.txt", std::ios::out | std::ios::trunc);

    vmhook::register_class<main_class>("vmhook/Main");
    vmhook::register_class<example_class>("vmhook/Example");
    vmhook::register_class<a_class>("vmhook/A");

    const auto instance{ example_class::get_instance() };

    if (instance)
    {
        call_example_methods(*instance);
        set_expected_values(*instance);
        verify_expected_values(*instance);
    }
    else
    {
        check("Example.instance", false);
    }

    write_summary();

    main_class::set_stop_jvm(true);

    if (test_log.is_open())
    {
        test_log.close();
    }

    FreeLibraryAndExitThread(module, 0);

    return 0;
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(module);

        HANDLE worker_thread{ CreateThread(nullptr, 0, reinterpret_cast<LPTHREAD_START_ROUTINE>(thread_entry), module, 0, nullptr) };

        if (worker_thread)
        {
            CloseHandle(worker_thread);
        }
    }

    return TRUE;
}
