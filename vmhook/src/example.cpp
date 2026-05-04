#include <vmhook/vmhook.hpp>

#include <windows.h>

// here we recreate the JVM source code structure
// to achieve that we recreate all the java classes we cant to work with in cpp 
// for instance gere we recreate the java Main class and we are going to associate it with our main cpp class
// the name of the cpp class doesn't matter you can call it whatever you want but I recommand calling it the same as your java class for an easy use
class main : public vmhook::object // derive from vmhook::object simply as that 
{
public:
    // you need to create a constructor that takes a vmhook::oop_type_t 
    // but only create one single construtor
    // even if the java class has 0 or 10 constructors you only have to create one in the cpp class
    main(vmhook::oop_type_t instance)
        : vmhook::object{ instance } {} // and super the instance

    // now in java Main has a field static boolean stopJVM
	// we are going to create one getter and one setter for this field in cpp
	// the name of the getter and setter doesn't matter but I recommand calling them get_ + field name and set_ + field name for an easy use
	static bool get_stop_jvm() // make it static because the field is static in java
    {
		// for the getter we simply call get_field with the name of the field and then call get() on the result to get the value of the field
		// very easy and straightforward, no signature, no nothing just the name of the field
        return get_field("stopJVM")->get();
    }

    static void set_stop_jvm(bool value) // make it static because the field is static in java
    {
        // for the setter we simply call get_field with the name of the field and then call set() on the result to set the value of the field
        // very easy and straightforward, no signature, no nothing just the name of the field
        get_field("stopJVM")->set(value);
    }
}

class example : public vmhook::object
{
public:
    example(vmhook::oop_type_t instance)
        : vmhook::object{ instance } {}

    static bool get_static_bool() 
    { 
        return get_field("staticBool")->get(); 
    }

    static void set_static_bool(bool value) 
    { 
        get_field("staticBool")->set(value); 
    }

    static std::byte get_static_byte() 
    { 
        return get_field("staticByte")->get(); 
    }

    static void set_static_byte(std::byte value) 
    { 
        get_field("staticByte")->set(value); 
    }

    static short get_static_short() 
    { 
        return get_field("staticShort")->get(); 
    }

    static void set_static_short(short value) 
    { 
        get_field("staticShort")->set(value); 
    }

    static int get_static_int() 
    { 
        return get_field("staticInt")->get(); 
    }

    static void set_static_int(int value) 
    { 
        get_field("staticInt")->set(value); 
    }

    static long long get_static_long() 
    { 
        return get_field("staticLong")->get(); 
    }

    static void set_static_long(long long value) 
    { 
        get_field("staticLong")->set(value); 
    }

    static float get_static_float() 
    { 
        return get_field("staticFloat")->get(); 
    }

    static void set_static_float(float value) 
    { 
        get_field("staticFloat")->set(value); 
    }

    static double get_static_double() 
    { 
        return get_field("staticDouble")->get(); 
    }

    static void set_static_double(double value) 
    { 
        get_field("staticDouble")->set(value); 
    }

    static char get_static_char() 
    {
        return get_field("staticChar")->get(); 
    }

    static void set_static_char(char value) 
    { 
        get_field("staticChar")->set(value); 
    }

    static std::string get_static_string() 
    { 
        return get_field("staticString")->get(); 
    }

    static void set_static_string(const std::string& value) 
    { 
        get_field("staticString")->set(value); 
    }

    bool get_not_static_bool() 
    { 
        return get_field("notStaticBool")->get(); 
    }

    void set_not_static_bool(bool value) 
    { 
        get_field("notStaticBool")->set(value); 
    }

    std::byte get_not_static_byte() 
    {
        return get_field("notStaticByte")->get();
    }

    void set_not_static_byte(std::byte value) 
    { 
        get_field("notStaticByte")->set(value); 
    }

    short get_not_static_short() 
    {
        return get_field("notStaticShort")->get(); 
    }

    void set_not_static_short(short value) 
    { 
        get_field("notStaticShort")->set(value); 
    }

    int get_not_static_int() 
    { 
        return get_field("notStaticInt")->get(); 
    }

    void set_not_static_int(int value) 
    { 
        get_field("notStaticInt")->set(value); 
    }

    long long get_not_static_long() 
    { 
        return get_field("notStaticLong")->get(); 
    }

    void set_not_static_long(long long value) 
    { 
        get_field("notStaticLong")->set(value); 
    }

    float get_not_static_float() 
    { 
        return get_field("notStaticFloat")->get(); 
    }

    void set_not_static_float(float value) 
    { 
        get_field("notStaticFloat")->set(value); 
    }

    double get_not_static_double() 
    { 
        return get_field("notStaticDouble")->get(); 
    }

    void set_not_static_double(double value) 
    { 
        get_field("notStaticDouble")->set(value); 
    }

    char get_not_static_char() 
    { 
        return get_field("notStaticChar")->get(); 
    }

    void set_not_static_char(char value) 
    { 
        get_field("notStaticChar")->set(value); 
    }

    std::string get_not_static_string() 
    {
        return get_field("notStaticString")->get(); 
    }

    void set_not_static_string(const std::string& value) 
    { 
        get_field("notStaticString")->set(value); 
    }

	// for arrays we do the same thing but with std::vector of the type of the array
    static std::vector<bool> get_static_bool_array()
    { 
        return get_field("staticBoolArray")->get();
    }

    static void set_static_bool_array(const std::vector<bool>& value) 
    { 
        get_field("staticBoolArray")->set(value); 
    }

    static std::vector<std::byte> get_static_byte_array() 
    {
        return get_field("staticByteArray")->get(); 
    }

    static void set_static_byte_array(const std::vector<std::byte>& value) 
    { 
        get_field("staticByteArray")->set(value); 
    }

    static std::vector<short> get_static_short_array() 
    { 
        return get_field("staticShortArray")->get(); 
    }

    static void set_static_short_array(const std::vector<short>& value) 
    { 
        get_field("staticShortArray")->set(value);
    }

    static std::vector<int> get_static_int_array() 
    { 
        return get_field("staticIntArray")->get(); 
    }

    static void set_static_int_array(const std::vector<int>& value) 
    {
        get_field("staticIntArray")->set(value); 
    }

    static std::vector<long long> get_static_long_array() 
    { 
        return get_field("staticLongArray")->get(); 
    }

    static void set_static_long_array(const std::vector<long long>& value) 
    { 
        get_field("staticLongArray")->set(value); 
    }

    static std::vector<float> get_static_float_array() 
    { 
        return get_field("staticFloatArray")->get(); 
    }

    static void set_static_float_array(const std::vector<float>& value) 
    { 
        get_field("staticFloatArray")->set(value); 
    }

    static std::vector<double> get_static_double_array() 
    {
        return get_field("staticDoubleArray")->get(); 
    }

    static void set_static_double_array(const std::vector<double>& value)
    { 
        get_field("staticDoubleArray")->set(value); 
    }

    static std::vector<char> get_static_char_array() 
    { 
        return get_field("staticCharArray")->get(); 
    }

    static void set_static_char_array(const std::vector<char>& value) 
    { 
        get_field("staticCharArray")->set(value); 
    }

    static std::vector<std::string> get_static_string_array() 
    { 
        return get_field("staticStringArray")->get(); 
    }

    static void set_static_string_array(const std::vector<std::string>& value) 
    { 
        get_field("staticStringArray")->set(value); 
    }

    std::vector<bool> get_not_static_bool_array() 
    {
        return get_field("notStaticBoolArray")->get(); 
    }

    void set_not_static_bool_array(const std::vector<bool>& value) 
    {
        get_field("notStaticBoolArray")->set(value); 
    }

    std::vector<std::byte> get_not_static_byte_array() 
    { 
        return get_field("notStaticByteArray")->get(); 
    }

    void set_not_static_byte_array(const std::vector<std::byte>& value) 
    { 
        get_field("notStaticByteArray")->set(value); 
    }

    std::vector<short> get_not_static_short_array() 
    { 
        return get_field("notStaticShortArray")->get(); 
    }

    void set_not_static_short_array(const std::vector<short>& value) 
    { 
        get_field("notStaticShortArray")->set(value); 
    }

    std::vector<int> get_not_static_int_array() 
    { 
        return get_field("notStaticIntArray")->get(); 
    }

    void set_not_static_int_array(const std::vector<int>& value) 
    { 
        get_field("notStaticIntArray")->set(value); 
    }

    std::vector<long long> get_not_static_long_array() 
    { 
        return get_field("notStaticLongArray")->get(); 
    }

    void set_not_static_long_array(const std::vector<long long>& value) 
    { 
        get_field("notStaticLongArray")->set(value);
    }

    std::vector<float> get_not_static_float_array() 
    { 
        return get_field("notStaticFloatArray")->get(); 
    }

    void set_not_static_float_array(const std::vector<float>& value) 
    { 
        get_field("notStaticFloatArray")->set(value);
    }

    std::vector<double> get_not_static_double_array() 
    { 
        return get_field("notStaticDoubleArray")->get(); 
    }

    void set_not_static_double_array(const std::vector<double>& value)
    { 
        get_field("notStaticDoubleArray")->set(value); 
    }

    std::vector<char> get_not_static_char_array() 
    { 
        return get_field("notStaticCharArray")->get(); 
    }

    void set_not_static_char_array(const std::vector<char>& value) 
    {
        get_field("notStaticCharArray")->set(value); 
    }

    std::vector<std::string> get_not_static_string_array() 
    { 
        return get_field("notStaticStringArray")->get(); 
    }

    void set_not_static_string_array(const std::vector<std::string>& value) 
    {
        get_field("notStaticStringArray")->set(value); 
    }

    // don't return directly an example return an std::unique_ptr<example>
    // the unique ptr will never be nullptr (unless no ram left ofc) but the underlything oop_t may be nullptr if the Object is null in the jvm
    static std::unique_ptr<example> get_instance()
    {
        return get_field("instance")->get();
    }

	// pass an std::unique_ptr<example> not just example
	// if you want to make the java value to null do not pass a nullptr, pass a std::unique_ptr<example> with a nullptr oop_t like this std::make_unique<example>(nullptr)
    static void set_instance(const std::unique_ptr<example>& value)
    {
        get_field("instance")->set(value);
    }

    static int get_static_called() 
    { 
        return get_field("staticCalled")->get(); 
    }

    static void set_static_called(int value) 
    { 
        get_field("staticCalled")->set(value); 
    }

    int get_non_static_called() 
    { 
        return get_field("nonStaticCalled")->get(); 
    }

    void set_non_static_called(int value)
    {
        get_field("nonStaticCalled")->set(value);
    }

    // for method calling it is also very straightforward
	static void static_call_me(int value)
	{
		get_method("staticCallMe")->call(value);
	}

	void not_static_call_me(int value)
	{
		get_method("notStaticCallMe")->call(value);
	}

	// same here, pass an std::unique_ptr<example> not just example, pass a std::unique_ptr<example> with a nullptr oop_type_t to make the java value null (std::make_unique<a>(nullptr))
	void use_a(const std::unique_ptr<a>& value)
	{
		get_method("useA")->call(value);
	}
};

class a : public vmhook::object
{
public:
    a(vmhook::oop_type_t instance)
        : vmhook::object{ instance } {}

    std::string get_string()
    {
        return get_field("string")->get();
    }

    std::string set_string(const std::string& value)
    {
        return get_field("string")->set(value);
    }

    static int get_counter()
    {
        return get_field("counter")->get();
    }

    static void set_counter(int value)
    {
        get_field("counter")->set(value);
    }

    int get_val()
    {
        return get_field("val")->get();
    }

    void set_val(int value)
    {
        get_field("val")->set(value);
    }
};

static DWORD WINAPI thread_entry(HMODULE module)
{


    FreeLibraryAndExitThread(module, 0);

    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(hModule);

        HANDLE worker_thread{ CreateThread(nullptr, 0, reinterpret_cast<LPTHREAD_START_ROUTINE>(thread_entry), hModule, 0, nullptr) };

        if (worker_thread)
        {
            CloseHandle(worker_thread);
        }
    }
    return TRUE;
}