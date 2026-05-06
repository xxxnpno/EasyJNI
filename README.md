# vmhook

vmhook is a C++23 single-header library for interacting with a running HotSpot
Java Virtual Machine from native code without JNI or JVMTI. A C++ wrapper class
is registered against a Java class name, then wrapper methods read fields, write
fields, and call Java methods through a small proxy API.

## Platform

vmhook currently targets Windows and MSVC.

```powershell
MSBuild vmhook.slnx /p:Configuration=Release /p:Platform=x64 /m
```

## Basic Model

Include the header:

```cpp
#include <vmhook/vmhook.hpp>
```

Create one C++ wrapper class for each Java class you want to use:

```cpp
class example_class : public vmhook::object<example_class>
{
public:
    explicit example_class(vmhook::oop_t instance)
        : vmhook::object<example_class>{ instance }
    {
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

    auto tick(std::int32_t amount)
        -> void
    {
        this->get_method("tick")->call(amount);
    }
};
```

Register the wrapper once before using it:

```cpp
vmhook::register_class<example_class>("vmhook/Example");
```

Wrap a decoded Java object pointer when you need instance fields or instance
methods:

```cpp
example_class example{ instance_oop };

std::int32_t counter{ example.get_counter() };
example.set_counter(counter + 1);
example.tick(5);
```

## Fields

Fields use one read function and one write function:

```cpp
this->get_field("fieldName")->get();
this->get_field("fieldName")->set(value);
```

`get()` converts to the C++ return type selected by your wrapper method. The
same API is used for primitives, strings, object wrappers, and vectors:

```cpp
auto get_name()
    -> std::string
{
    return this->get_field("name")->get();
}

auto get_flags()
    -> std::vector<bool>
{
    return this->get_field("flags")->get();
}

auto set_flags(const std::vector<bool>& value)
    -> void
{
    this->get_field("flags")->set(value);
}
```

## Static Fields

Static Java fields use the same `get_field(...)` API. Expose them as static
C++ methods, then call `get_field(...)` through a default-constructed wrapper:

```cpp
class example_class : public vmhook::object<example_class>
{
public:
    example_class() = default;

    explicit example_class(vmhook::oop_t instance)
        : vmhook::object<example_class>{ instance }
    {
    }


    static auto get_static_double()
        -> double
    {
        return example_class{}.get_field("staticDouble")->get();
    }

    static auto set_static_double(double value)
        -> void
    {
        example_class{}.get_field("staticDouble")->set(value);
    }
};
```

The temporary wrapper has a null Java object pointer. That is fine for static
fields because vmhook resolves them through the registered Java class metadata
and reads/writes the `java.lang.Class` mirror.

```cpp
example_class::set_static_double(2.0);
double value{ example_class::get_static_double() };
```

## Methods

Method calls intentionally mirror fields:

```cpp
this->get_method("methodName")->call(arg1, arg2, arg3);
```

Expose Java methods through wrapper methods so the rest of your C++ code never
uses raw method names:

```cpp
auto add_score(std::int32_t amount, const std::string& reason)
    -> void
{
    this->get_method("addScore")->call(amount, reason);
}
```

Static Java methods use the same `get_method(...)` API:

```cpp

static auto static_call_me(std::int32_t value)
    -> void
{
    example_class{}.get_method("staticCallMe")->call(value);
}
```

For instance methods, call through `this`:

```cpp
auto non_static_call_me(std::int32_t value)
    -> void
{
    this->get_method("nonStaticCallMe")->call(value);
}
```

## Hooks

Hooks receive `vmhook::return_value&` first, followed by the decoded Java
arguments. Instance methods receive the wrapped `this` object as the first Java
argument:

```cpp
vmhook::hook<example_class>("nonStaticCallMe",
    [](vmhook::return_value& return_value, const std::unique_ptr<example_class>& self, std::int32_t value)
    {
        // Leave return_value untouched to let the original Java method run.
    });
```

To skip the original method body, set or cancel the return slot:

```cpp
vmhook::hook<example_class>("nonStaticReturnMe",
    [](vmhook::return_value& return_value, const std::unique_ptr<example_class>& self, std::int32_t value)
    {
        return_value.set(static_cast<std::int32_t>(12345));
    });

vmhook::hook<example_class>("nonStaticVoidMethod",
    [](vmhook::return_value& return_value, const std::unique_ptr<example_class>& self)
    {
        return_value.cancel();
    });
```

Static method hooks omit the wrapper argument:

```cpp
vmhook::hook<example_class>("staticReturnMe",
    [](vmhook::return_value& return_value, std::int32_t value)
    {
        return_value.set(static_cast<std::int32_t>(24680));
    });
```

## Object Construction

`vmhook::make_unique<T>(arg1, arg2, ...)` is present in the public header, but
VM-side allocation and constructor dispatch are not implemented yet. The current
behavior is to return `nullptr` after resolving the registered class. The example
unit tests cover that status explicitly so it is not mistaken for working Java
object construction.

## Example

The complete example lives in `vmhook/src/example.cpp`. It demonstrates:

- wrapper classes for `vmhook.Main`, `vmhook.Example`, and `vmhook.A`
- static field getters and setters as static C++ methods
- instance field getters and setters as instance C++ methods
- `get()` for all supported field reads
- `set(...)` for all supported field writes
- `get_method("...")->call(...)` for method calls
- method hooks that observe arguments and allow the original method
- method hooks that force a return value
- method hooks that cancel a void method
- static method hooks
- current `vmhook::make_unique<T>(...)` behavior
- a GitHub Actions test harness that writes `test_results.txt`

## Notes

vmhook works directly with HotSpot internals. Field names and method names must
match the Java class exactly, and wrapper classes must be registered before use.
