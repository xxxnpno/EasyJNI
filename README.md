# EasyJNI — dev/jni-refactor

A C++23 **header-only** library for interacting with a running HotSpot JVM from an injected DLL — **without including any JNI or JVMTI headers**. All struct offsets, type sizes and entry points are discovered at runtime by reading `gHotSpotVMStructs` and `gHotSpotVMTypes`, two symbol tables exported by `jvm.dll`, making the library version-agnostic across HotSpot builds.

## Why no JNI headers?

The standard JNI API requires `jni.h` and a call to `JNI_GetCreatedJavaVMs`. This branch removes that dependency entirely: every field offset and type size the library needs is read from HotSpot's own introspection tables at startup. The result is a single header with no external dependencies beyond `windows.h`.

---

## Build

Open `EasyJNI.slnx` in **Visual Studio 2022**. Build **Release|x64**.

| Configuration | Output | Notes |
|---|---|---|
| Release\|x64 | `EasyJNI\build\EasyJNI.dll` | No external deps |
| Debug\|x64   | `EasyJNI\build\EasyJNI.dll` | Requires `EasyJNI\ext\jni\jvm.lib` (copy from `%JAVA_HOME%\lib`) |

Intermediate files go to `etc_easyjni\` (EasyJNI) and `etc_injector\` (Injector) at the solution root.

---

## Injector

`Injector\build\Injector.exe` is a console tool that finds all `javaw.exe` processes, injects `EasyJNI.dll`, and reports status. If exactly one `javaw.exe` is running it injects automatically; otherwise it lists the candidates and asks you to choose.

Typical workflow:
1. Build **Release|x64** — produces both `EasyJNI.dll` and `Injector.exe`
2. Launch Minecraft with `launch_minecraft.bat`
3. Wait until the main menu is visible
4. Run `Injector.exe` — a console appears inside the Minecraft process
5. Follow the on-screen instructions and watch `C:\repos\cpp\EasyJNI\log.txt`
6. Press **DELETE** inside the game window to unload the DLL

---

## API

### Class registration

```cpp
// Associate a C++ type with a Java class name (call once at startup).
// Required before get_field() or hook<T>() can be used on that type.
jni::register_class<my_class>("com/example/MyClass");
```

### Object wrappers

Derive from `jni::object`. The constructor takes a `jni::oop` (decoded Java object pointer, obtained from hook frame arguments):

```cpp
class http_client : public jni::object
{
public:
    explicit http_client(jni::oop instance)
        : jni::object{ instance }
    {}

    // get_field() handles both instance and static fields automatically —
    // JVM_ACC_STATIC is read from the klass, no separate call needed.
    auto is_connected() { return get_field("isConnected")->get(); }
    auto get_health()   { return get_field("health")->get(); }

    // Cast inside the method if you want a concrete return type:
    auto get_timeout()  { return static_cast<int>(get_field("timeout")->get()); }

    // Static field — same call, routed to the java.lang.Class mirror automatically:
    auto get_version()  { return get_field("VERSION")->get(); }

    // Writing:
    auto set_health(int hp) { get_field("health")->set(hp); }
};

jni::register_class<http_client>("com/example/HttpClient");
```

### Field proxy (`field_proxy`)

`get_field(name)` returns `std::optional<field_proxy>`. The proxy's `.get()` reads the field and returns a `field_proxy::value` that implicitly converts to any numeric or bool type via `std::visit` + `static_cast`:

```cpp
bool ok = client.get_field("connected")->get();  // implicit bool
int  hp = client.get_field("health"  )->get();   // implicit int
float x = client.get_field("posX"    )->get();   // implicit float

if (client.get_field("alive")->get()) { ... }    // contextual bool

// Writing
client.get_field("health")->set(100);
client.get_field("flag"  )->set(true);
```

For **reference-type** fields (signature `L…` or `[…`) the value is returned as `uint32_t` (the raw 32-bit compressed OOP). Pass it to `hotspot::decode_oop_ptr()` to recover the real address.

### Low-level field access

```cpp
jni::hotspot::klass* k = jni::find_class("com/example/Foo");

// Instance field
int  v = jni::get_field<int>(object_ptr, k, "fieldName");
         jni::set_field<int>(object_ptr, k, "fieldName", 42);

// Static field
int  s = jni::get_static_field<int>(k, "STATIC_FIELD");
         jni::set_static_field<int>(k, "STATIC_FIELD", 0);
```

### Class lookup

```cpp
// Walks ClassLoaderDataGraph on first call; subsequent calls return the cached klass*.
jni::hotspot::klass* k = jni::find_class("java/lang/String");
```

### Method hooking

Hooks patch the **interpreter-to-interpreter (i2i) entry stub** of the target method. Any method that has already been JIT-compiled will not be intercepted — hook before the method is first called.

```cpp
jni::hook<http_client>("sendRequest",
    [](jni::hotspot::frame* f, jni::hotspot::java_thread*, bool* cancel)
    {
        auto [self] = f->get_arguments<jni::oop>();
        http_client client{ self };
        int hp = client.get_field("health")->get();
        std::println("[HOOK] sendRequest — health = {}", hp);
    });

jni::shutdown_hooks(); // restore all patched bytes
```

---

## Field encoding (internals)

Fields are parsed from `InstanceKlass._fields` (an `Array<u2>`, 6 u2 slots per field, JDK 8–21). The byte offset is recovered as `((high_packed << 16) | low_packed) >> 2`. Static fields live in the `java.lang.Class` mirror at their stored offset; instance fields live at `decoded_oop + offset`.

## JVM type → C++ mapping

| JVM sig | `get()` returns |
|---------|-----------------|
| `Z` | `bool` |
| `B` | `int8_t` |
| `S` | `int16_t` |
| `I` | `int32_t` |
| `J` | `int64_t` |
| `F` | `float` |
| `D` | `double` |
| `C` | `uint16_t` |
| `L…` / `[…` | `uint32_t` (compressed OOP) |

## HotSpot internals namespace (`jni::hotspot::`)

| Type | Purpose |
|------|---------|
| `VMStructEntry` / `VMTypeEntry` | Raw entries from `gHotSpotVMStructs` / `gHotSpotVMTypes` |
| `symbol` | Interned JVM string (class/method/field names) |
| `klass` | Java class metadata; `find_field()`, `get_java_mirror()` |
| `method` | Java method; i2i entry, access flags, name, signature |
| `constant_pool` | Per-class constant pool |
| `class_loader_data_graph` | Root of all class loaders; `find_klass()` |
| `java_thread` | Current JVM thread; thread state |
| `frame` | Interpreter stack frame; `get_arguments<Types...>()` |
| `midi2i_hook` | 5-byte JMP patch at the i2i stub |
| `field_entry` | Field offset + signature + `is_static` |
| `decode_oop_ptr()` | Compressed OOP → real 64-bit address |
| `decode_klass_ptr()` | Compressed klass ptr → real 64-bit address |
