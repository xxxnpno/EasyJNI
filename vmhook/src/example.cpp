#include <vmhook/vmhook.hpp>

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

class example_class : public vmhook::object<example_class>
{
public:
    explicit example_class(vmhook::oop_t instance)
        : vmhook::object<example_class>{ instance }
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
        -> std::unique_ptr<example_class>
    {
        return this->get_field("instance")->get();
    }

    auto set_instance(const std::unique_ptr<example_class>& value)
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

static auto WINAPI thread_entry(HMODULE module)
    -> DWORD
{
    vmhook::register_class<main_class>("vmhook/Main");
    vmhook::register_class<example_class>("vmhook/Example");
    vmhook::register_class<a_class>("vmhook/A");

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
