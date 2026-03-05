### API
```cpp
// call jni::init before using EasyJNI
auto jni::init(const std::uint8_t maxEnvs = 10) -> bool
// call jni::shutdown before uninjecting
auto jni::shutdown() -> void

// use this function before closing a thread that uses EasyJNI
auto jni::exit_thread() -> void
```

I'm going to create an example based on Minecraft 1.8.9 source code.  
The Minecraft class is the main class of the game and stores a static field with its instance.

EasyJNI currently only support getters and setters for normal classes  
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
    minecraft(const jobject instance)
        : jni::object{ instance }
    {

    }

    // even if theMinecraft is static don't make get_minecraft() static see main.cpp
    // methods never return jobject they always place them in unique ptrs
    auto get_minecraft() -> std::unique_ptr<minecraft>
    {
        // give to get_field the type of the field, its name and if it is static
        // then use ->get
        return get_field<minecraft>("theMinecraft", jni::field_type::STATIC)->get();
    }

    // same as get_minecraft but the field is not static
    auto get_the_player() -> std::unique_ptr<entity_player_sp>
    {
        return get_field<entity_player_sp>("thePlayer")->get();
    }
};
```
```cpp
class entity_player_sp final : public jni::object
{
public:
    entity_player_sp(const jobject instance)
        : jni::object{ instance }
    {

    }

    // for primitives getter use cpp types; bool istead of jbool
    auto get_server_sprinting_state() -> bool
    {
        return get_field<bool>("serverSprintState")->get();
    }

    // same as getters
    auto set_server_sprinting_state(const bool value) -> void
    {
        // no need to specify the type to ->set
        get_field<bool>("serverSprintState")->set(value);
    }
};
```
