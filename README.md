# vmhook

vmhook is a C++20/23 single-header library for inspecting, wrapping, calling,
and hooking a running HotSpot JVM from native code. It is intended for injected
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

## Platform & toolchain matrix

The header compiles on every combination listed below.  Runtime hooking
needs a HotSpot JVM in-process plus an x86_64 ABI; the columns reflect
what CI actually exercises for each target.

| OS / Arch                | Compiler                  | Header builds | OS layer | Hook trampoline | Real-JVM tests |
|--------------------------|---------------------------|:-------------:|:--------:|:---------------:|:--------------:|
| Windows x86_64           | MSVC 19.36+               | yes           | yes      | yes (Win64 ABI) | yes            |
| Windows x86_64           | clang / clang-cl 16+      | yes           | yes      | yes (Win64 ABI) | yes            |
| Windows x86_64           | MinGW-w64 GCC 13+         | yes           | yes      | yes (Win64 ABI) | yes            |
| Linux x86_64             | GCC 13+                   | yes           | yes      | yes (SysV ABI)  | yes            |
| Linux x86_64             | Clang 16+                 | yes           | yes      | yes (SysV ABI)  | yes            |
| macOS x86_64             | Apple Clang               | yes           | yes      | yes (SysV ABI)  | best-effort    |
| macOS arm64              | Apple Clang               | yes           | yes      | no (arm64)      | best-effort    |
| Android (ARM64 / x86_64) | NDK clang                 | yes           | yes      | x86_64 only     | n/a (no HotSpot) |
| iOS (device / simulator) | Apple Clang               | yes           | yes      | no              | n/a (no HotSpot) |

Notes:
- *Hook trampoline* is x86_64-only.  On arm64 hosts the header still
  builds and the OS layer is fully functional; `vmhook::hook<T>` returns
  false at runtime so consumers can degrade gracefully.
- *iOS / Android* ship their own VMs (Apple's locked-down JVM
  alternatives, Android's ART).  vmhook will compile on those platforms
  for cross-platform-code-sharing reasons, but there's no HotSpot
  libjvm to hook into, so `vmhook::find_class`, `vmhook::hook<T>`,
  etc. return null / false at runtime.
- The CI matrix runs the full JVM integration test for Java 8, 11, 17,
  21, 24, and 25 against every compiler that produces a working
  artefact on a hosted runner.

### Building with CMake (recommended)

The repository ships a `CMakeLists.txt` that builds the example shared library,
the Windows-only injector, and the unit-test suite:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

CMake options:

- `VMHOOK_BUILD_EXAMPLE`  (default `ON`)  build the test shared library
- `VMHOOK_BUILD_INJECTOR` (default `ON`)  build the Windows-only injector
- `VMHOOK_BUILD_TESTS`    (default `ON`)  build standalone unit tests
- `VMHOOK_WARNINGS_AS_ERRORS` (default `OFF`)  enforce `-Werror`

Consumers can pull the library into their own CMake project:

```cmake
add_subdirectory(third_party/vmhook)
target_link_libraries(my_target PRIVATE vmhook::vmhook)
```

### Building with MSBuild (legacy)

```powershell
MSBuild vmhook.slnx /p:Configuration=Release /p:Platform=x64 /m
```

The MSBuild solution still works for the Visual Studio IDE workflow; the
GitHub Actions CI also builds it to catch regressions.

## Running the tests

Two test suites ship with the project:

1. **Standalone unit tests** (no JVM required)  built into `build/tests` by
   the CMake build, run via `ctest`.  Covers header compilation on every
   compiler/platform, ODR sanity across translation units, type-trait static
   asserts, the `vmhook::os` portability layer (page size, allocate/protect
   round-trips, safe pointer reads, etc.), and the public API surface.
2. **JVM integration tests**  the injector loads the example DLL into a
   running JVM and the DLL asserts every field/method/hook scenario before
   asking the JVM to exit.  Results land in `test_results.txt`; CI fails if
   any line starts with `[FAIL]`.  The integration suite covers:
   - Every primitive type (bool, byte, short, int, long, float, double, char)
     at regular and boundary values (MIN/MAX, NaN, +/- infinity, negatives).
   - String fields (regular, empty, unicode, long, interned, null).
   - 1-D arrays of every primitive and of String (regular, empty, large).
   - Object reference fields, including `final` and `volatile`.
   - Inheritance: own + inherited (protected) fields and methods.
   - Enums (`Color.RED/GREEN/BLUE` with constructor params and instance methods).
   - Interfaces: static methods on the interface (Animal.kingdomCount), method
     overrides on concrete implementations (Dog.speak overriding Animal).
   - Nested classes (static nested + non-static inner with synthetic outer
     reference).
   - Overloaded methods (same name, three signatures).
   - Each primitive return type plus void / null-returning methods.
   - Methods that throw exceptions.
   - Hooks: installation, force-return, cancel, arg mutation (int + string),
     method calling from inside the detour, `make_unique` allocating
     wrappers from a hook.

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

Instance field access is `get_field("name")`.  Static field access has two
forms depending on the compiler you target:

- On **MSVC** and **Clang**, `get_field("name")` resolves correctly in both
  instance and static contexts: the C++23 deducing-this overload is excluded
  from static-call overload resolution, so the static fallback wins.
- On **GCC** the deducing-this overload is still considered viable in a
  static-call context and produces a compile error.  Call `static_field("name")`
  from static methods to stay portable across all three compilers.

```cpp
class example : public vmhook::object<example>
{
public:
    explicit example(vmhook::oop_t instance) noexcept
        : vmhook::object<example>{ instance }
    {
    }

    // Instance field accessor — works on every compiler.
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

    // Static field accessor — pick one:
    //
    //   static_field("...")           portable: MSVC, Clang, GCC
    //   get_field("...")              MSVC and Clang only
    static auto get_total_count()
        -> std::int32_t
    {
        return static_field("totalCount")->get();
    }
};
```

Supported field values include bools, byte-sized and integral values, floats,
doubles, chars, strings, arrays as `std::vector<T>`, and registered object
references as `std::unique_ptr<wrapper_type>`.

## Methods

Same pattern as fields.  `get_method("name")->call(args...)` works for
instance access on every compiler.  Static access is `static_method(...)` for
portable code (or `get_method(...)` on MSVC / Clang).

```cpp
// Instance:
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

// Static (portable):
static auto reset_global_state()
    -> void
{
    static_method("resetGlobalState")->call();
}
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
        this->get_method("<init>")->call(text);
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
