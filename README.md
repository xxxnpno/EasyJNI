# EasyJNI

A C++23 header-only library that wraps the JNI API to make interacting with a running JVM straightforward. No manual environment management, no raw Java signatures, no boilerplate.

## Features

- **Automatic thread management** : threads are attached to the JVM on demand
- **No Java signatures** : types are inferred from your C++ template parameters
- **Field access** : get and set instance and static fields with a single call
- **Method calls** : call Java methods as if they were C++ functions
- **Constructor support** : create Java objects from C++ with `jni::make_unique`
- **C++ polymorphism** : inheritance works naturally across wrapper classes
- **Collection support** : built-in wrappers for `java.util.Collection` and `java.util.List`
- **Method hooking** : intercept Java method calls at the JVM level with C++ lambdas

## Requirements

- C++23 compiler
- Windows (the hooking subsystem uses the Win32 API)
- A running JVM (the library attaches to it via `JNI_GetCreatedJavaVMs`)

## Setup

```cpp
#include <easy_jni/easy_jni.hpp>

// 1. Initialize before using anything else
jni::init();

// 2. Register your Java class wrappers
jni::register_class<MyClass>("com/example/MyClass");

// 3. Call shutdown before unloading your DLL
jni::shutdown();
```

## Core API

```cpp
// Initialize EasyJNI. Call once before using any other function.
// max_envs: how many thread environments to cache before the map is cleared (default: UINT8_MAX)
auto jni::init(std::uint8_t max_envs = UINT8_MAX) -> bool;

// Shut down EasyJNI. Cleans up all global refs, hooks, and cached environments.
auto jni::shutdown() -> void;

// Detach the current thread from the JVM.
// Call this when a thread you manage yourself exits after using EasyJNI.
// You do NOT need to call this inside hook detours.
auto jni::exit_thread() -> void;
```

## Wrapping Java Classes

For every Java class you want to interact with, create a C++ class that inherits from `jni::object` and register it.

```cpp
class http_client : public jni::object
{
public:
    explicit http_client(jobject instance)
        : jni::object{ instance }
    {
    }

    auto get_base_url() -> std::string
    {
        return get_method<std::string>("getBaseUrl")->call();
    }

    auto set_timeout(int milliseconds) -> void
    {
        get_method<void, int>("setTimeout")->call(milliseconds);
    }

    auto is_connected() -> bool
    {
        return get_method<bool>("isConnected")->call();
    }
};

jni::register_class<http_client>("com/example/HttpClient");
```

Inheritance works naturally. Inherit from your wrapper class rather than directly from `jni::object`:

```cpp
class secure_http_client : public http_client
{
public:
    explicit secure_http_client(jobject instance)
        : http_client{ instance }
    {
    }

    auto get_certificate_path() -> std::string
    {
        return get_method<std::string>("getCertificatePath")->call();
    }
};

jni::register_class<secure_http_client>("com/example/SecureHttpClient");
```

## Fields

Use `get_field<T>(name)` to get a field accessor, then call `.get()` or `.set()` on it.

```cpp
// Instance field (read)
auto get_max_retries() -> int
{
    return get_field<int>("maxRetries")->get();
}

// Object field (read)
auto get_inner_client() -> std::unique_ptr<http_client>
{
    return get_field<http_client>("innerClient")->get();
}

// Primitive field (write)
auto set_max_retries(int value) -> void
{
    get_field<int>("maxRetries")->set(value);
}

// String field (write)
auto set_config_path(const std::string& path) -> void
{
    get_field<std::string>("configPath")->set(path);
}
```

### Static fields

There is no special C++ static method involved. Just add a regular method that passes `jni::field_type::STATIC`, and call it on a null instance when you need it from outside the class:

```cpp
class config_manager : public jni::object
{
public:
    explicit config_manager(jobject instance)
        : jni::object{ instance }
    {
    }

    // Reading a static field: same as an instance field, just pass field_type::STATIC.
    auto get_default_timeout() -> int
    {
        return get_field<int>("DEFAULT_TIMEOUT", jni::field_type::STATIC)->get();
    }

    // Writing a static field works the same way.
    auto set_default_timeout(int value) -> void
    {
        get_field<int>("DEFAULT_TIMEOUT", jni::field_type::STATIC)->set(value);
    }
};
```

To call a static field accessor from outside the class, construct a temporary null instance:

```cpp
int timeout = config_manager{ nullptr }.get_default_timeout();
```

## Methods

Use `get_method<ReturnType, ArgTypes...>(name)` to get a method accessor, then call `.call(args...)` on it.

```cpp
// No arguments
auto get_connection_string() -> std::string
{
    return get_method<std::string>("getConnectionString")->call();
}

// Primitive argument
auto set_pool_size(int size) -> void
{
    get_method<void, int>("setPoolSize")->call(size);
}

// Object argument, pass the unique_ptr directly
auto execute_query(const std::unique_ptr<sql_query>& query) -> std::unique_ptr<result_set>
{
    return get_method<result_set, sql_query>("executeQuery")->call(query);
}
```

### Static methods

Same pattern as static fields: a regular method with `method_type::STATIC`, called on a null instance from outside the class.

```cpp
class session_manager : public jni::object
{
public:
    explicit session_manager(jobject instance)
        : jni::object{ instance }
    {
    }

    // Reading a static field that returns the singleton instance.
    auto get_instance() -> std::unique_ptr<session_manager>
    {
        return get_field<session_manager>("instance", jni::field_type::STATIC)->get();
    }

    // Calling a static method.
    auto create_session(const std::string& user_id) -> std::unique_ptr<session_manager>
    {
        return get_method<session_manager, std::string>("createSession", jni::method_type::STATIC)->call(user_id);
    }
};
```

To use from outside the class:

```cpp
auto session = session_manager{ nullptr }.get_instance();
auto new_session = session_manager{ nullptr }.create_session("user-42");
```

### Supported types

| C++ type          | Java type |
|-------------------|-----------|
| `void`            | `void`    |
| `short`           | `short`   |
| `int`             | `int`     |
| `long long`       | `long`    |
| `float`           | `float`   |
| `double`          | `double`  |
| `bool`            | `boolean` |
| `char`            | `char`    |
| `std::string`     | `String`  |
| `T : jni::object` | any class |

## Constructors

Use `jni::make_unique<T, ArgTypes...>(args...)` (not `std::make_unique`) to call a Java constructor and get back a managed C++ wrapper.

```cpp
// Single argument constructor
auto query = jni::make_unique<sql_query, std::string>("SELECT * FROM users WHERE id = ?");

// Multi-argument constructor
auto request = jni::make_unique<http_request, std::string, int>("https://api.example.com", 5000);

// Pass the result directly to a method
auto results = db->execute_query(query);
```

## Collections

`jni::list` and `jni::collection` are built-in wrappers. Call `to_vector<T>()` to convert them to a `std::vector`.

```cpp
// Returns std::vector<std::unique_ptr<user>>
auto get_all_users() -> std::vector<std::unique_ptr<user>>
{
    return get_field<jni::list>("users")->get()->to_vector<user>();
}

// Returns std::vector<std::string>
auto get_user_names() -> std::vector<std::string>
{
    return get_field<jni::list>("userNames")->get()->to_vector<std::string>();
}
```

## Null Checking

`std::unique_ptr` wrappers are never null themselves, but the underlying Java object might be. Always check `get_instance()` before using a wrapped object:

```cpp
auto connection = database_connection{ nullptr }.get_instance();

if (not connection->get_instance())
{
    std::println("[ERROR] Failed to get database connection.");
    return;
}

auto pool_size = connection->get_pool_size();
```

## Method Hooking

Use `jni::hook<T>(method_name, detour)` to intercept calls to a Java method before they execute.

The detour receives:
- `frame*` : the current stack frame, used to read arguments
- `java_thread*` : the calling Java thread
- `bool* cancel` : set to `true` (via `jni::set_return_value`) to suppress the original method and return a custom value

Use `frame->get_arguments<Types...>()` to unpack arguments. **The first type is always `self`**, the instance the method was called on.

```cpp
// Observe a call without intercepting it
auto on_request = [](jni::hotspot::frame* frame, jni::hotspot::java_thread* thread, bool* cancel)
{
    auto [self, url, timeout] = frame->get_arguments<http_client, std::string, int>();
    std::println("[HOOK] request to {} (timeout: {}ms)", url, timeout);
};

jni::hook<http_client>("sendRequest", on_request);
```

```cpp
// Suppress the original call and return a custom value
auto on_get_status = [](jni::hotspot::frame* frame, jni::hotspot::java_thread* thread, bool* cancel)
{
    auto [self] = frame->get_arguments<http_client>();
    jni::set_return_value(cancel, 200);
};

jni::hook<http_client>("getStatusCode", on_get_status);
```

Hooks are cleaned up automatically when `jni::shutdown()` is called. You do **not** need to call `jni::exit_thread()` inside a hook detour. Hooks run on Java threads that are already attached to and managed by the JVM.

`jni::exit_thread()` is only needed for threads **you create yourself** that call into EasyJNI and then exit without going through `jni::shutdown()`.

## Full Example

A complete working example targeting Minecraft 1.8.9 (MCP deobfuscated names) can be found in [EasyJNI/src/main.cpp](./EasyJNI/src/main.cpp). It covers wrappers, inheritance, field access (instance and static), method calls, constructors, collections and hooks.
