# vmhook

vmhook is a C++23 single-header library for inspecting, wrapping, calling, and
hooking a running HotSpot JVM from native code. It is intended for injected
tooling that controls the target environment and needs direct access to HotSpot
metadata without shipping a JVMTI agent.

The library currently supports:

- SDK-style C++ wrappers for Java classes
- primitive, string, array, and object field access
- Java method calls with descriptor-aware overload matching
- Java object allocation through registered wrapper types
- interpreted Java method hooks
- hook return cancellation and forced return values
- hook argument replacement through `vmhook::return_value::set_arg`
- HotSpot metadata lookup with JNI fallbacks where needed

## Scope

JNI and JVMTI are the supported Java native interfaces. vmhook is lower-level:
it reads HotSpot VMStruct metadata, resolves JVM internals directly, and patches
interpreter entry stubs for hooks. Use it only when the target JVM and runtime
layout are part of your compatibility surface.

## Platform

- Windows x64
- MSVC with C++23
- HotSpot JVM
- CI coverage for Java 8, 11, 17, 21, and 24

Build the solution:

```powershell
MSBuild vmhook.slnx /p:Configuration=Release /p:Platform=x64 /m
```

## Debug And Release Logging

`VMHOOK_DEBUG_LOGS` controls internal diagnostic logging.

- Debug builds enable vmhook logs by default.
- Release builds disable vmhook logs by default.
- Define `VMHOOK_DEBUG_LOGS=1` to force logs on.
- Define `VMHOOK_DEBUG_LOGS=0` to force logs off.

Release builds keep the same behavior, but avoid vmhook diagnostic output on hot
paths.

## Wrapper Pattern

Register each C++ wrapper with the Java internal class name, then keep field and
method names inside wrapper methods.

```cpp
#include <vmhook/vmhook.hpp>

namespace mapping
{
    namespace player
    {
        inline const char* clazz{ "net/minecraft/client/entity/EntityPlayerSP" };
        inline const char* send_chat_message{ "sendChatMessage" };
    }
}

class player : public vmhook::object<player>
{
public:
    explicit player(vmhook::oop_t instance) noexcept
        : vmhook::object<player>{ instance }
    {
    }

    auto send_chat_message(const std::string& message) const
        -> void
    {
        this->get_method(mapping::player::send_chat_message)->call(message);
    }
};

auto register_sdk()
    -> void
{
    vmhook::register_class<player>(mapping::player::clazz);
}
```

## Fields

Instance and static fields use `get_field(name)`. Instance calls resolve fields
from the live object. Static calls resolve fields from the registered class.

```cpp
class example : public vmhook::object<example>
{
public:
    explicit example(vmhook::oop_t instance) noexcept
        : vmhook::object<example>{ instance }
    {
    }

    auto get_name() const
        -> std::string
    {
        return this->get_field("name")->get();
    }

    auto set_score(std::int32_t value) const
        -> void
    {
        this->get_field("score")->set(value);
    }
};
```

Supported field values include bools, byte-sized and integral values, floats,
doubles, chars, strings, arrays as `std::vector<T>`, and registered object
references as `std::unique_ptr<wrapper_type>`.

## Methods

Method calls use `get_method(name)->call(args...)`.

```cpp
auto add_score(std::int32_t amount) const
    -> void
{
    this->get_method("addScore")->call(amount);
}

auto compute(std::int32_t value) const
    -> std::int32_t
{
    return this->get_method("compute")->call(value);
}
```

When obfuscated classes contain several methods with the same short name,
`method_proxy::call(...)` checks same-name overloads against the C++ argument
types and JVM descriptors before invoking the selected method.

Use explicit descriptors when the wrapper boundary should be exact:

```cpp
this->get_method("a", "(Ljava/lang/String;)V")->call(message);
```

## Hooks

Hooks receive `vmhook::return_value&` first, followed by decoded Java arguments.
For instance methods, include the wrapped Java `this` argument first.

```cpp
vmhook::hook<player>(mapping::player::send_chat_message,
    [](vmhook::return_value& return_value,
       const std::unique_ptr<player>& self,
       const std::string& message)
    {
        if (message.starts_with("/native"))
        {
            return_value.cancel();
        }
    });
```

Force a return value:

```cpp
vmhook::hook<example>("compute",
    [](vmhook::return_value& return_value,
       const std::unique_ptr<example>& self,
       std::int32_t value)
    {
        return_value.set(static_cast<std::int32_t>(12345));
    });
```

Replace an argument and allow the original Java method to continue:

```cpp
vmhook::hook<player>(mapping::player::send_chat_message,
    [](vmhook::return_value& return_value,
       const std::unique_ptr<player>& self,
       const std::string& message)
    {
        if (message.contains("secret"))
        {
            return_value.set_arg(1, std::string{ "[redacted]" });
        }
    });
```

`set_arg` accepts primitive values, object wrappers, object pointers, string
views, strings, and C strings. For Java strings, vmhook creates a JNI string
when possible and falls back to direct Java string allocation.

Remove installed hooks before unloading:

```cpp
vmhook::shutdown_hooks();
```

## Object Construction

`vmhook::make_unique<T>(args...)` allocates a Java object for a registered
wrapper type. If the wrapper exposes `construct(args...)`, vmhook calls it after
allocation.

```cpp
class chat_component_text : public vmhook::object<chat_component_text>
{
public:
    explicit chat_component_text(vmhook::oop_t instance) noexcept
        : vmhook::object<chat_component_text>{ instance }
    {
    }

    auto construct(const std::string& text) const
        -> void
    {
        this->get_method("<init>", "(Ljava/lang/String;)V")->call(text);
    }
};

auto component{ vmhook::make_unique<chat_component_text>("hello") };
```

## Class Lookup

vmhook first searches HotSpot class metadata through VMStructs. If that path
cannot see application classes, it falls back to JNI class-loader lookup:

- current thread context class loader
- system class loader
- LaunchWrapper's `net.minecraft.launchwrapper.Launch.classLoader`

This matters in custom class-loader environments where injected native threads
often do not have the same visibility as the application thread.

## Example And CI

The complete example lives in `vmhook/src/example.cpp` with Java fixtures in
`example/vmhook`. It covers:

- field get and set for primitives, strings, arrays, and object references
- method calls and overload resolution
- instance and static hooks
- forced return values and void-method cancellation
- hook argument mutation with `set_arg`
- Java object allocation with `make_unique`
- superclass field and method lookup
- `java.util.List` conversion to `std::vector<std::unique_ptr<T>>`

The GitHub Actions matrix builds and runs the native harness against multiple
JDK versions. The harness writes `test_results.txt`; CI fails if any `[FAIL]`
line appears.

## Notes

- Register wrapper classes before using fields, methods, allocation, or hooks.
- Install hooks and call `shutdown_hooks()` from controlled native code paths.
- Cached HotSpot pointers assume process-lifetime injection; class unloading is
  not handled.
- JVM updates can change HotSpot internals and may require compatibility fixes.
