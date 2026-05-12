# vmhook

vmhook is a C++23 single-header library for wrapping and hooking a running
HotSpot JVM from native code. It is designed for injected tooling where you want
small C++ wrapper classes around Java classes:

- read and write Java fields
- call Java methods
- allocate Java objects
- hook interpreted Java method execution

vmhook uses HotSpot VMStructs for the core metadata work. It does not require a
JVMTI agent. It can use JNI as a compatibility fallback for thread attachment,
class-loader lookup, object construction, and method calls when a JVM does not
export a needed HotSpot entry point.

## JNI, JVMTI, And vmhook

vmhook is not a replacement for JNI or JVMTI.

JNI is the supported interface for native Java interop. JVMTI is the supported
interface for agents, instrumentation, class retransformation, and VM events.
vmhook is a lower-level HotSpot-specific approach: it reads VM metadata directly
from the target process and patches interpreter entry stubs for method hooks.

Use vmhook when you control the target environment and need direct HotSpot
metadata access or method hooks from an injected native module. Use JNI/JVMTI
when you need a portable, supported integration surface.

## Platform

- Windows x64
- MSVC with C++23
- HotSpot JVM

Build the example solution:

```powershell
MSBuild vmhook.slnx /p:Configuration=Release /p:Platform=x64 /m
```

## Wrapper Pattern

The intended API style is the same as a small SDK layer. Register each wrapper
against the Java internal class name, then keep field and method names inside
wrapper methods.

```cpp
#include <vmhook/vmhook.hpp>

namespace mapping
{
    namespace minecraft
    {
        inline const char* clazz{ "net/minecraft/client/Minecraft" };
        inline const char* thePlayer{ "thePlayer" };
        inline const char* runTick{ "runTick" };
    }

    namespace player
    {
        inline const char* clazz{ "net/minecraft/client/entity/EntityPlayerSP" };
        inline const char* sendChatMessage{ "sendChatMessage" };
    }
}

class player : public vmhook::object<player>
{
public:
    explicit player(vmhook::oop_t instance) noexcept
        : vmhook::object<player>{ instance }
    {
    }

    auto send_chat_message(const std::string& message) const -> void
    {
        get_method(mapping::player::sendChatMessage)->call(message);
    }
};

class minecraft : public vmhook::object<minecraft>
{
public:
    explicit minecraft(vmhook::oop_t instance) noexcept
        : vmhook::object<minecraft>{ instance }
    {
    }

    static auto get_instance() -> std::unique_ptr<minecraft>
    {
        return get_field("theMinecraft")->get();
    }

    auto get_player() const -> std::unique_ptr<player>
    {
        return get_field(mapping::minecraft::thePlayer)->get();
    }
};

auto register_sdk() -> void
{
    vmhook::register_class<minecraft>(mapping::minecraft::clazz);
    vmhook::register_class<player>(mapping::player::clazz);
}
```

This keeps application code clean:

```cpp
auto mc{ minecraft::get_instance() };
auto local_player{ mc->get_player() };
local_player->send_chat_message("hello from native code");
```

## Fields

Instance and static fields use the same `get_field(name)` API. In instance
methods the live object is used; in static wrapper methods vmhook resolves the
registered class.

```cpp
class example : public vmhook::object<example>
{
public:
    explicit example(vmhook::oop_t instance) noexcept
        : vmhook::object<example>{ instance }
    {
    }

    static auto get_instance() -> std::unique_ptr<example>
    {
        return get_field("instance")->get();
    }

    auto get_name() const -> std::string
    {
        return get_field("name")->get();
    }

    auto set_score(std::int32_t value) const -> void
    {
        get_field("score")->set(value);
    }
};
```

Supported scalar field types include `bool`, `std::byte`, integral types,
`float`, `double`, `char`, and `std::string`. Arrays map to `std::vector<T>`.
Object references map to `std::unique_ptr<wrapper_type>` when the wrapper type
has been registered.

## Methods

Method calls use `get_method(name)->call(args...)`.

```cpp
auto add_score(std::int32_t amount) const -> void
{
    get_method("addScore")->call(amount);
}

auto compute(std::int32_t value) const -> std::int32_t
{
    return get_method("compute")->call(value);
}
```

When obfuscated classes contain several methods with the same short name,
`method_proxy::call(...)` re-checks same-name overloads against the C++ argument
types and JVM descriptors before invoking the method.

If you want explicit descriptor matching at the wrapper boundary, use:

```cpp
get_method("a", "(Ljava/lang/String;)V")->call(message);
```

## Hooks

Hooks receive `vmhook::return_value&` first, followed by decoded Java arguments.
For instance methods, the first Java argument is the wrapped `this`.

```cpp
vmhook::hook<minecraft>(mapping::minecraft::runTick,
    [](vmhook::return_value& retval, const std::unique_ptr<minecraft>& self)
    {
        // Leave retval untouched to let the original method run.
    });
```

Force a return value:

```cpp
vmhook::hook<example>("compute",
    [](vmhook::return_value& retval,
       const std::unique_ptr<example>& self,
       std::int32_t value)
    {
        retval.set(static_cast<std::int32_t>(12345));
    });
```

Cancel a void method:

```cpp
vmhook::hook<player>("sendChatMessage",
    [](vmhook::return_value& retval,
       const std::unique_ptr<player>& self,
       const std::string& message)
    {
        if (message.starts_with("/native"))
        {
            retval.cancel();
        }
    });
```

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

    auto construct(const std::string& text) const -> void
    {
        get_method("<init>", "(Ljava/lang/String;)V")->call(text);
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

This matters in environments such as Forge, where injected native threads often
do not have the same class-loader visibility as the game thread.

## Example

The complete example lives in `vmhook/src/example.cpp` with Java fixtures in
`example/vmhook`. It demonstrates:

- SDK-style C++ wrappers
- field get/set for primitives, strings, arrays, and object references
- method calls and descriptor-aware overload resolution
- instance and static hooks
- forced return values and void-method cancellation
- Java object allocation with `make_unique`
- superclass field and method lookup
- `java.util.List` conversion to `std::vector<std::unique_ptr<T>>`

The native test harness writes `test_results.txt`; CI fails if any `[FAIL]`
line appears.

## Notes

- Register wrapper classes before using fields, methods, allocation, or hooks.
- Hook installation and `shutdown_hooks()` should run from one native thread.
- vmhook targets HotSpot internals, so JVM updates can require compatibility
  fixes.
- Cached `klass*` pointers assume process-lifetime injection; class unloading is
  not handled.
