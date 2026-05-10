# vmhook

> `vmhook` is a C++23 single-header library for interacting with a running HotSpot
Java Virtual Machine from native code, without JNI or JVMTI. A C++ wrapper
class is registered against a Java class name. Wrapper methods can then read
fields, write fields, call Java methods, and hook method execution entirely
through HotSpot's own VMStructs, making the approach transparent to the JVM and
independent of the JNI/JVMTI permission model.

## Platform

Windows x64, MSVC. C++23 or later is required.

```powershell
MSBuild vmhook.slnx /p:Configuration=Release /p:Platform=x64 /m
```

## Quick Start

Include the header and register your wrapper classes once at startup:

```cpp
#include <vmhook/vmhook.hpp>

// Recreate your Java class in C++.
class player : public vmhook::object<player>
{
public:
    explicit player(vmhook::oop_t instance)
        : vmhook::object<player>{ instance }
    {
    }

    // Getter format
    auto get_health() -> std::int32_t
    {
        return get_field("health")->get();
    }

    auto get_name() -> std::string
    {
        return get_field("name")->get();
    }

    auto get_best_friend() -> std::unique_ptr<player>
    {
        return get_field("bestFriend")->get();
    }

    // Static fields use the same format.

    // Setter format
    auto set_health(std::int32_t value) -> void
    {
        get_field("health")->set(value);
    }

    auto set_name(const std::string& name) -> void
    {
        get_field("name")->set(name);
    }

    auto set_best_friend(const std::unique_ptr<player>& player) -> void
    {
        get_field("bestFriend")->set(player);
    }

    // Method call format
    auto say_hi() -> void
    {
        get_method("sayHi")->call();
    }

    auto print(const std::string& text) -> void
    {
        get_method("print")->call(text);
    }

    auto hug_friend(
        const std::unique_ptr<player>& player1,
        const std::unique_ptr<player>& player2
    ) -> void
    {
        get_method("hugFriend")->call(player1, player2);
    }

    // Static methods use the same format.
};

// Associate your C++ class with the Java class.
vmhook::register_class<player>("com/example/Player");
```

## Inheritance and Polymorphism

`vmhook` automatically walks the superclass chain when resolving fields and
methods. If a field or method is not declared on the concrete class, `vmhook`
searches each superclass in order.

```java
// Java
class A {}
class B extends A {}
```

```cpp
class a : public vmhook::object<a>
{
public:
    explicit a(vmhook::oop_t instance)
        : vmhook::object<a>{ instance }
    {
    }
};

class b : public a
{
public:
    explicit b(vmhook::oop_t instance)
        : a{ instance }
    {
    }
};

vmhook::register_class<a>("vmhook/A");
vmhook::register_class<b>("vmhook/B");
```

## Collections and Lists

`vmhook` provides `vmhook::collection` and `vmhook::list` wrapper classes for
`java.util.Collection` and `java.util.List` objects. These wrappers resolve the
concrete klass directly from the object header, so no `register_class<T>()` call
is needed. They work with any implementation, such as `ArrayList`,
`LinkedList`, and others.

### `vmhook::collection`

`vmhook::collection` exposes `to_vector<T>()`.

> `vmhook::list` inherits from `vmhook::collection`.

```java
// Java
public List<A> listOfAs;
```

```cpp
// C++
auto get_list_of_as() -> std::vector<std::unique_ptr<a>>
{
    return get_field("listOfAs")->get().to_vector<a>();
}
```

## Hooks

Hooks receive `vmhook::return_value&` as the first argument, followed by decoded
Java arguments. Instance method hooks receive the wrapped `this` as the first
Java argument.

```cpp
// Observe an instance method call without modifying behavior.
vmhook::hook<example_class>(
    "nonStaticCallMe",
    [](vmhook::return_value& /* retval */,
       const std::unique_ptr<example_class>& self,
       std::int32_t value)
    {
        // Leave retval untouched. The original Java body runs normally.
    }
);
```

To replace the return value, call `retval.set(...)`:

```cpp
vmhook::hook<example_class>(
    "nonStaticReturnMe",
    [](vmhook::return_value& retval,
       const std::unique_ptr<example_class>& self,
       std::int32_t value)
    {
        retval.set(static_cast<std::int32_t>(12345));
    }
);
```

To cancel a void method and skip the original body:

```cpp
vmhook::hook<example_class>(
    "nonStaticCancelMe",
    [](vmhook::return_value& retval,
       const std::unique_ptr<example_class>& self,
       std::int32_t value)
    {
        retval.cancel();
    }
);
```

Static method hooks omit the wrapper argument:

```cpp
vmhook::hook<example_class>(
    "staticReturnMe",
    [](vmhook::return_value& retval, std::int32_t value)
    {
        retval.set(static_cast<std::int32_t>(24680));
    }
);
```

Remove all installed hooks when done:

```cpp
vmhook::shutdown_hooks();
```

## Object Construction

`vmhook::make_unique<T>(arg1, arg2, ...)` allocates a new Java object for the
registered wrapper type using HotSpot internals only, without JNI and without
executing Java constructor bytecode directly.

It first uses the current hook's `JavaThread` when one is available, then falls
back to HotSpot's `JavaThread` lists. This means it can be used both inside and
outside hook detours.

The function returns a `std::unique_ptr<T>`. If the wrapper class exposes
`construct(arg1, arg2, ...)`, `vmhook` calls it after allocation, allowing you to
initialize fields through the normal wrapper API.

```cpp
class chat_component_text final : public sdk::i_chat_component
{
public:
    explicit chat_component_text(vmhook::oop_type_t instance) noexcept
        : sdk::i_chat_component{ instance }
    {
    }

    // Recreate your different constructors.
    auto construct(const std::string& text) const noexcept -> void
    {
        get_method("<init>")->call(text);
    }
};

// Construct the object.
auto message = vmhook::make_unique<sdk::chat_component_text>("Hey guys!");
```

## Example

The complete example lives in `vmhook/src/example.cpp`.
