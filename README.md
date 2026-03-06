### EasyJNI
- no env managment
- no need for java signatures
- easy to use getter and setter for fields
- easy to use call for methods
- cpp polymorphism works

currently working on java data structures and constructors

cpp23

### API
```cpp
// call jni::init before using EasyJNI
auto jni::init(const std::uint8_t max_envs = 10)
    -> bool

// call jni::shutdown before uninjecting
auto jni::shutdown()
    -> void

// use this function before closing a thread that uses EasyJNI
auto jni::exit_thread()
    -> void
```

I'm going to create an example based on Minecraft 1.8.9 source code.  
The Minecraft class is the main class of the game and stores a static field with its instance.

See example: [main.cpp](./EasyJNI/src/main.cpp)

```cpp
// you need to register your classes before using them
jni::register_class<minecraft>("net/minecraft/client/Minecraft");
jni::register_class<entity_player_sp>("net/minecraft/client/entity/EntityPlayerSP");
```

### Usage
```cpp
// recreate the java class in cpp and derive it from jni::object
class minecraft final : public jni::object
{
public:
    // create a constructor that takes a jobject and supers the jobject to jni::object
    explicit minecraft(const jobject instance)
        : jni::object{ instance }
    {

    }

    // even if theMinecraft is static, don't make get_minecraft() static see main.cpp
    // methods never return jobject they always place them in unique ptrs
    auto get_minecraft()
        -> std::unique_ptr<minecraft>
    {
        // give to get_field the type of the field, its name and if it is static
        // then use ->get
        return get_field<minecraft>("theMinecraft", jni::field_type::STATIC)->get();
    }

    // same as get_minecraft but the field is not static
    auto get_the_player()
        -> std::unique_ptr<entity_player_sp>
    {
        return get_field<entity_player_sp>("thePlayer")->get();
    }
};
```
```cpp
class entity_player : public jni::object
{
public:
    explicit entity_player(const jobject instance)
        : jni::object{ instance }
    {

    }

    // always use cpp primitives types instead of jni ones
    // for instance use std::string instead of jstring
    // there is void, short, int, long long, float
    // double, char, bool, std::string
    auto get_name()
        -> std::string
    {
        // get method returns a std::string and takes no argument
        return get_method<std::string>("getName")->call();
    }
};

class entity_player_sp final : public entity_player
{
public:
    explicit entity_player_sp(const jobject instance)
        : entity_player{ instance }
    {

    }

    auto set_sprinting(const bool value)
        -> void
    {
        // setSprinting returns void and takes a bool
        // if a method takes a jobject, pass the unique ptr not the jobject
        get_method<void, bool>("setSprinting")->call(value);
    }
};
```
