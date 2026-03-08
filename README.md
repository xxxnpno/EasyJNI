# EasyJNI

A C++23 header-only library that wraps the JNI API to make interacting with a running JVM clean and simple, no manual env management, no Java signatures, no boilerplate.

## Features

- **No env management** threads are attached to the JVM automatically
- **No Java signatures** types are inferred from your C++ template parameters
- **Simple field access** get and set fields with a single call
- **Simple method calls** call Java methods as if they were C++ methods
- **C++ polymorphism** inheritance works the way you'd expect
- **Java data structure support** built-in wrappers for `Collection` and `List`
- **Constructor support** create Java objects with `jni::make_unique`

> Requires **C++23**. Currently expanding Java data structure.

---

## Quick Start

```cpp
#include <easy_jni/easy_jni.hpp>

// Initialize before using anything else
jni::init();

// Register your Java class wrappers
jni::register_class<MyClass>("com/example/MyClass");

// Shut down before uninjecting
jni::shutdown();
```

---

## API Reference

```cpp
// Initialize EasyJNI. Call this before anything else.
// max_envs: max number of thread envs to cache before clearing (default: UINT8_MAX)
auto jni::init(const std::uint8_t max_envs = UINT8_MAX) -> bool;

// Shut down EasyJNI. Call this before uninjecting your DLL.
auto jni::shutdown() -> void;

// Detach the current thread from the JVM. Call this when a thread that uses EasyJNI exits.
auto jni::exit_thread() -> void;
```

---

## Wrapping Java Classes

For every Java class you want to interact with, create a C++ wrapper that inherits from `jni::object`.

```cpp
class entity_player : public jni::object
{
public:
    entity_player(jobject instance)
        : jni::object{ instance }
    {
    }

    // Wrap a method — use C++ types, never JNI types directly
    auto get_name() -> std::string
    {
        return get_method<std::string>("getName")->call();
    }

    // Method with an argument — specify the argument type in the template, pass the value to call()
    auto add_chat_message(const std::unique_ptr<i_chat_component> value) -> void
    {
        get_method<void, i_chat_component>("addChatMessage")->call(value);
    }
};
```

**Inheritance** works naturally, just inherit from your wrapper class instead of `jni::object`:

```cpp
class entity_player_sp : public entity_player
{
public:
    entity_player_sp(jobject instance)
        : entity_player{ instance }
    {
    }
};
```

---

## Fields

```cpp
// Instance field
auto get_the_player() -> std::unique_ptr<entity_player_sp>
{
    return get_field<entity_player_sp>("thePlayer")->get();
}

// Static field — pass jni::field_type::STATIC
auto get_minecraft() -> std::unique_ptr<minecraft>
{
    return get_field<minecraft>("theMinecraft", jni::field_type::STATIC)->get();
}

// Setter
get_field<int>("someField")->set(42);
```

---

## Methods

```cpp
// No arguments
get_method<std::string>("getName")->call();

// With arguments — list arg types in the template, pass values to call()
get_method<void, i_chat_component>("addChatMessage")->call(my_component);

// Static method
get_method<std::unique_ptr<minecraft>>("getInstance", jni::method_type::STATIC)->call();
```

**Supported types:** `void`, `short`, `int`, `long long`, `float`, `double`, `bool`, `char`, `std::string`, and any class inheriting from `jni::object`.

---

## Java Collections

`jni::list` and `jni::collection` are built-in wrappers. Use `to_vector<T>()` to get a `std::vector` of wrapped objects:

```cpp
auto get_player_entities() -> std::vector<std::unique_ptr<entity_player>>
{
    return get_field<jni::list>("playerEntities")->get()->to_vector<entity_player>();
}
```

---

## Constructors

Use `jni::make_unique` (not `std::make_unique`) to construct Java objects from C++:

```cpp
player->add_chat_message(jni::make_unique<chat_component_text, std::string>("Hello World"));
```

---

## Checking for Null

`std::unique_ptr` wrappers are **never null** themselves, but the underlying Java instance might be. Always check `get_instance()`:

```cpp
if (the_player->get_instance() && the_world->get_instance())
{
    // safe to use
}
```

---

## Full Example

A complete working example using the Minecraft 1.8.9 deobfuscated source — covering static fields, instance fields, method calls, collections, and constructors:

→ [main.cpp](./EasyJNI/src/main.cpp)
