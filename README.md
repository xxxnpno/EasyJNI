# vmhook

vmhook is a C++23 single-header library for interacting with a running HotSpot
Java Virtual Machine from native code, without JNI or JVMTI. A C++ wrapper
class is registered against a Java class name; wrapper methods then read fields,
write fields, call Java methods, and hook method execution entirely through
HotSpot's own VMStructs, making the approach transparent to the JVM and
independent of the JNI/JVMTI permission model.

## Architecture

### Why not JNI / JVMTI?

| Aspect | JNI / JVMTI | vmhook |
|---|---|---|
| Requires JVM permission | Yes (attached agent / native library) | No — reads from already-mapped JVM data |
| Visible to JVM security | Yes | No |
| Header only | No | Yes |
| Supported Java versions | All | Java 8 – 24+ |

### How it works

1. **VMStructs** — HotSpot exports a global symbol `gHotSpotVMStructs` that
   describes every internal field by name, type, and offset. vmhook parses this
   table at startup to locate fields inside `InstanceKlass`, `Method`, `Symbol`,
   and other internal types without hardcoding any offsets.

2. **ClassLoaderDataGraph** — vmhook walks the `ClassLoaderDataGraph` to find
   the `InstanceKlass*` for a registered Java class name. The result is cached
   for the process lifetime.

3. **OOP decoding** — compressed oops and compressed klass pointers are decoded
   using the base/shift values from VMStructs (`CompressedOops`,
   `CompressedKlassPointers`, or `Universe` depending on the JDK version).

4. **Method hooks** — vmhook patches the interpreter entry point of a `Method`
   object. The trampoline saves registers, calls the C++ detour, and either
   returns the native-supplied value or falls through to the original bytecode.

## Platform

Windows x64, MSVC (C++23 or later required).

```powershell
MSBuild vmhook.slnx /p:Configuration=Release /p:Platform=x64 /m
```

## Quick start

Include the header and register your wrapper classes once at startup:

```cpp
#include <vmhook/vmhook.hpp>

class player_class : public vmhook::object<player_class>
{
public:
    explicit player_class(vmhook::oop_t instance)
        : vmhook::object<player_class>{ instance }
    {
    }

    auto get_health() -> std::int32_t { return get_field("health")->get(); }
    auto set_health(std::int32_t v)   { get_field("health")->set(v); }
    auto respawn()                    { get_method("respawn")->call(); }
};

// Call once before any wrapper usage:
vmhook::register_class<player_class>("com/example/Player");
```

## Fields

### Instance fields

`get()` converts to whatever C++ type your wrapper method returns. The same API
works for primitives, strings, object wrappers, and vectors:

```cpp
auto get_name()   -> std::string            { return get_field("name")->get(); }
auto get_flags()  -> std::vector<bool>      { return get_field("flags")->get(); }
auto get_score()  -> std::int32_t           { return get_field("score")->get(); }
```

`set(arg)` works the same as get:

```cpp
auto set_name(const std::string& value)         -> void         { return get_field("name")->set(value); }
auto set_flags(const std::vector<bool>& value)  -> void         { return get_field("flags")->set(value); }
auto set_score(std::int32_t value)              -> void         { return get_field("score")->set(value); }
```

Supported scalar types: `bool`, `std::byte`, `std::int16_t`, `std::int32_t`,
`std::int64_t`, `float`, `double`, `char`, `std::string`.

Supported vector types: `std::vector<T>` for each scalar above.

Object-reference fields return a `std::unique_ptr<wrapper_type>` when the
returned type is a registered wrapper:

```cpp
auto get_a() -> std::unique_ptr<a_class> { return get_field("a")->get(); }

auto set_a(const std::unique_ptr<a_class>& value) -> void { return get_field("a")->set(value); }
```

### Static fields

Static Java fields use the same `get_field(...)` API. Expose them as C++ static
methods:

```cpp
static auto get_instance_count() -> std::int32_t
{
    return get_field("instanceCount")->get();
}

static auto set_instance_count(std::int32_t v)
{
    get_field("instanceCount")->set(v);
}
```

## Methods

Instance methods:

```cpp
auto add_score(std::int32_t amount) -> void
{
    get_method("addScore")->call(amount);
}

auto compute(std::int32_t x) -> std::int32_t
{
    return get_method("compute")->call<std::int32_t>(x);
}
```

Static methods use the same syntax and can be called from static C++ methods:

```cpp
static auto global_reset() -> void
{
    get_method("globalReset")->call();
}
```

## Inheritance and Polymorphism

vmhook automatically walks the superclass chain when resolving fields and
methods. If a field or method is not declared on the concrete class, vmhook
searches each superclass in order.

```java
// Java
class A {
    protected int protectedInt = 1337;
    protected int protectedAdd(int x) { return x + protectedInt; }
}
class B extends A {
    public int bInt = 42;
}
```

```cpp
class b_class;

class a_class : public vmhook::object<b_class>
{
public:
    explicit a_class(vmhook::oop_t instance)
        : vmhook::object<b_class>{ instance }
    {
    }
};

class b_class : public a_class
{
public:
    explicit b_class(vmhook::oop_t instance)
        : a_class{ instance }
    {
    }

    // own field on B
    auto get_b_int()        -> std::int32_t { return get_field("bInt")->get(); }

    // inherited field from A — find_field walks the superclass chain
    auto get_protected_int() -> std::int32_t { return get_field("protectedInt")->get(); }

    // inherited method from A — get_method walks the superclass chain
    auto protected_add(std::int32_t x) -> std::int32_t
    {
        return get_method("protectedAdd")->call<std::int32_t>(x);
    }
};

vmhook::register_class<b_class>("vmhook/B");
```

## Collections and Lists

vmhook provides `vmhook::collection` and `vmhook::list` wrapper classes for
`java.util.Collection` and `java.util.List` objects. These wrappers resolve the
concrete klass directly from the object header so no `register_class<T>()` call
is needed and they work with any implementation (ArrayList, LinkedList, etc.).

### vmhook::collection

`vmhook::collection` exposes `size()`, `is_empty()`, and `to_vector<T>()`.
`to_vector<T>()` reads an ArrayList backing array directly from the JVM heap
when possible, with a generic `get(int)` fallback for other collection/list
implementations.

### vmhook::list

`vmhook::list` extends `collection`, so it has the same `to_vector<T>()` API.

```java
// Java
public List<A> listOfAs;
```

```cpp
// C++
auto get_list_of_as() -> std::vector<std::unique_ptr<a_class>> { get_field("listOfAs")->get().to_vector<a_class>(); }
```

Null Java elements become `nullptr` entries in the returned vector. Elements are
wrapped with `std::make_unique<element_type>(oop)`, so `element_type` must
accept a `vmhook::oop_t` in its constructor.

## Hooks

Hooks receive `vmhook::return_value&` as the first argument, followed by
decoded Java arguments. Instance method hooks receive the wrapped `this` as the
first Java argument.

```cpp
// Observe an instance method call without modifying behaviour:
vmhook::hook<example_class>("nonStaticCallMe",
    [](vmhook::return_value& /*retval*/,
       const std::unique_ptr<example_class>& self,
       std::int32_t value)
    {
        // Leave retval untouched — original Java body runs normally.
    });
```

To replace the return value, call `retval.set(...)`:

```cpp
vmhook::hook<example_class>("nonStaticReturnMe",
    [](vmhook::return_value& retval,
       const std::unique_ptr<example_class>& self,
       std::int32_t value)
    {
        retval.set(static_cast<std::int32_t>(12345));
    });
```

To cancel a void method (skip the original body):

```cpp
vmhook::hook<example_class>("nonStaticCancelMe",
    [](vmhook::return_value& retval,
       const std::unique_ptr<example_class>& self,
       std::int32_t value)
    {
        retval.cancel();
    });
```

Static method hooks omit the wrapper argument:

```cpp
vmhook::hook<example_class>("staticReturnMe",
    [](vmhook::return_value& retval, std::int32_t value)
    {
        retval.set(static_cast<std::int32_t>(24680));
    });
```

Remove all installed hooks when done:

```cpp
vmhook::shutdown_hooks();
```

## Object Construction

`vmhook::make_unique<T>(arg1, arg2, ...)` allocates a new Java object for the
registered wrapper type using HotSpot internals only (no JNI, no Java
constructor bytecode). It first uses the current hook's JavaThread when one is
available, then falls back to HotSpot's JavaThread lists, so it can be used both
inside and outside hook detours.

The function returns a `std::unique_ptr<T>`. If the wrapper class exposes
`construct(arg1, arg2, ...)`, vmhook calls it after allocation to let you
initialize fields via the normal wrapper API:

```cpp
class a_class : public vmhook::object<a_class>
{
public:
    explicit a_class(vmhook::oop_t instance)
        : vmhook::object<a_class>{ instance }
    {
    }

    auto set_val(std::int32_t value)   { get_field("val")->set(value); }
    auto construct(std::int32_t value) { set_val(value); }
};

vmhook::hook<example_class>("nonStaticCallMe",
    [](vmhook::return_value& /*retval*/,
       const std::unique_ptr<example_class>& self,
       std::int32_t value)
    {
        auto obj{ vmhook::make_unique<a_class>(1337) };
        // obj->get_val() == 1337
    });

auto obj{ vmhook::make_unique<a_class>(2026) }; // also valid outside hooks
```

## Example

The complete example lives in `vmhook/src/example.cpp`. It demonstrates:

- Wrapper classes for `vmhook.Main`, `vmhook.Example`, `vmhook.A`, and
  `vmhook.B`
- Static field getters and setters as static C++ methods
- Instance field getters and setters as instance C++ methods
- `get()` and `set(...)` for all supported primitive and array field types
- `get_method("...")->call(...)` for instance and static method calls
- Method hooks that observe arguments and allow the original method
- Method hooks that force a return value
- Method hooks that cancel a void method
- Static method hooks
- `vmhook::make_unique<T>(...)` before hooks, inside hooks, and after hooks
- `get_field("...")->get().to_vector<T>()` for Java collections/lists
- Superclass field and method resolution via `vmhook::find_field` and
  `get_method` hierarchy walk
- A GitHub Actions CI harness that writes `test_results.txt` and fails on any
  `[FAIL]` line

## Notes

- Field names and method names must match the Java source exactly.
- Wrapper classes must be registered with `vmhook::register_class<T>(name)`
  before any wrapper methods are called.
- vmhook targets x64 HotSpot. Compressed oops and compressed class pointers are
  assumed to be enabled (the default for heaps under 32 GB).
- Class unloading is not handled — klass pointers cached by vmhook are only safe
  for process-lifetime injection scenarios.
