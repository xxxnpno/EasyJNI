# VMHook

A C++23 **header-only** library for interacting with a running HotSpot JVM from an injected DLL — **without JNI or JVMTI headers**. All struct offsets, type sizes, and entry points are discovered at runtime by reading `gHotSpotVMStructs` and `gHotSpotVMTypes`, two symbol tables exported by `jvm.dll`. The library is version-agnostic across every HotSpot build from JDK 8 through JDK 21+.

---

## Why no JNI headers?

The standard JNI API requires `jni.h` and a call to `JNI_GetCreatedJavaVMs`. VMHook removes that dependency entirely — every field offset and type size the library needs is read from HotSpot's own introspection tables at startup. The result is a **single header with no external dependencies** beyond `windows.h`.

---

## Repository layout

```
VMHook.slnx                    ← Visual Studio solution (not tracked in git)
build\                         ← VMHook.dll + Injector.exe (gitignored)
etc\                           ← intermediate files (gitignored)
VMHook\
  src\main.cpp                 ← DLL entry point (worker thread + test harness)
  ext\vmhook\vmhook.hpp        ← the single-header library (all logic lives here)
  VMHook.vcxproj
Injector\
  src\main.cpp                 ← finds java.exe / javaw.exe and injects VMHook.dll
  Injector.vcxproj
example\
  src\Player.java              ← example Java class with instance + static fields
  src\Main.java                ← long-running target process (run first, then inject)
  build_and_run.bat            ← compiles and launches the example app
```

---

## Build

Open `VMHook.slnx` in **Visual Studio 2022** (or build with MSBuild directly). Build **Release|x64**.

Both projects write their output to the **root** `build\` folder:

| Output | Description |
|--------|-------------|
| `build\VMHook.dll` | The injectable HotSpot hook library |
| `build\Injector.exe` | DLL injector — finds java/javaw process and injects |

Intermediate files go to `etc\vmhook\` and `etc\injector\`.

> **External dependency (Debug|x64 only):** `VMHook\ext\jni\jvm.lib` — copy from `%JAVA_HOME%\lib\jvm.lib`. Release|x64 does **not** require it.

---

## Quick start — example target

1. Build **Release|x64** (produces `build\VMHook.dll` and `build\Injector.exe`)
2. In a terminal: `example\build_and_run.bat` — compiles and launches the example JVM
3. In a second terminal: run `build\Injector.exe` (auto-injects since only one JVM is running)
4. A VMHook console appears inside the java.exe process — check the output
5. Press **DELETE** in the VMHook console to unload cleanly
6. Results are also written to `log.txt` at the repo root

---

## API reference

### Namespace

Everything lives in the `vmhook::` namespace.  
HotSpot internals are in `vmhook::hotspot::`.

### Class registration

```cpp
// Associate a C++ type with a Java class name (call once at startup, before get_field / hook).
vmhook::register_class<MyClass>("com/example/MyClass");
```

### Class lookup

```cpp
// Walks ClassLoaderDataGraph on first call; subsequent calls return the cached klass*.
vmhook::hotspot::klass* string_klass = vmhook::find_class("java/lang/String");
```

### Field lookup and access

```cpp
vmhook::hotspot::klass* player_klass = vmhook::find_class("vmhook/example/Player");

// Low-level (instance field)
float  hp  = vmhook::get_field<float>(object_ptr, player_klass, "health");
         vmhook::set_field<float>(object_ptr, player_klass, "health", 50.0f);

// Low-level (static field — reads from the java.lang.Class mirror automatically)
int count  = vmhook::get_static_field<int>(player_klass, "count");
             vmhook::set_static_field<int>(player_klass, "count", 0);
```

`find_field()` auto-detects the field storage format at runtime:
- **JDK 8–20** — `InstanceKlass._fields` (`Array<u2>`, 6 slots per field, packed offset `>> 2`)
- **JDK 21+** — `InstanceKlass._fieldinfo_stream` (`Array<u1>`, UNSIGNED5-compressed)

### Object wrappers (high-level API)

Derive from `vmhook::object` for a typed façade over a live Java object:

```cpp
class Player : public vmhook::object
{
public:
    explicit Player(vmhook::oop instance) : vmhook::object{ instance } {}

    // get_field() handles both instance and static fields automatically.
    auto health()               { return get_field("health")->get(); }
    auto set_health(float hp)   { get_field("health")->set(hp); }
    auto x()                    { return get_field("x")->get(); }
    auto count()                { return get_field("count")->get(); }  // static
};

vmhook::register_class<Player>("vmhook/example/Player");

// Inside a hook detour:
auto [self] = frame->get_arguments<vmhook::oop>();
Player player{ self };
float hp = player.health();   // implicit conversion to float
player.set_health(100.0f);
```

### `field_proxy` — typed field access

`get_field(name)` returns `std::optional<vmhook::field_proxy>`. The proxy's `.get()` returns a `field_proxy::value` that implicitly converts to any numeric or bool type:

```cpp
float  hp  = player.get_field("health")->get();
double x   = player.get_field("x")->get();
bool   ok  = client.get_field("connected")->get();
int    cnt = player.get_field("count")->get();   // static — routed to Class mirror

// Writing
player.get_field("health")->set(100.0f);
```

For **reference-type** fields (`L…` or `[…` signature) the value is a `uint32_t` compressed OOP. Pass it to `vmhook::hotspot::decode_oop_ptr()` to recover the real 64-bit address.

### Method hooking

Hooks patch the **interpreter-to-interpreter (i2i) entry stub** of the target method. Hook **before** the method is first called — any method already JIT-compiled will not be intercepted.

```cpp
vmhook::hook<Player>("tick",
    [](vmhook::hotspot::frame* frame, vmhook::hotspot::java_thread*, bool* cancel)
    {
        auto [self] = frame->get_arguments<vmhook::oop>();
        Player player{ self };
        float hp = player.health();
        std::println("[HOOK] Player::tick — health = {}", hp);
    });

vmhook::shutdown_hooks();  // restore all patched bytes before unloading
```

---

## Field encoding (internals)

### JDK 8–20 — `InstanceKlass._fields` (`Array<u2>`)

Six consecutive `u2` slots per field (`FieldInfo::field_slots`):

| Slot | Field |
|------|-------|
| 0 | `access_flags` — JVM_ACC_* bits; bit 3 (0x0008) = static |
| 1 | `name_index` — constant-pool index of the field name (Symbol) |
| 2 | `signature_index` — cp index of the JVM type descriptor |
| 3 | `initval_index` — cp index of ConstantValue attribute (or 0) |
| 4 | `low_packed` — bits [1:0] = FIELDINFO_TAG, bits [15:2] = offset_low |
| 5 | `high_packed` — upper 16 bits of packed offset |

Byte offset: `((high_packed << 16) | low_packed) >> 2`.  
`Array<u2>._data` starts at `array_ptr + 4` (no alignment padding needed for 2-byte elements).

### JDK 21+ — `InstanceKlass._fieldinfo_stream` (`Array<u1>`, UNSIGNED5)

Stream grammar: `j(num_java) k(num_injected) Field[j+k] End(0x00)`  
Per field: `name_idx sig_idx offset access_flags field_flags [gsig_idx?] [initval_idx?] [group?]`

Optional trailing entries (bit flags in `field_flags`):
- bit 0 (`0x01`): generic signature index follows
- bit 2 (`0x04`): initializer value index follows
- bit 3 (`0x08`): `@Contended` group follows

`Array<u1>._data` starts at `array_ptr + 4` (1-byte elements, no padding).

### Static fields

Static field values live in the `java.lang.Class` **mirror** object at `Klass::_java_mirror`.  
`_java_mirror` is an `OopHandle` (since JDK 17): an indirection through OopStorage — read the pointer at `klass + offset`, then dereference it to get the full 64-bit Class object address.

### Compressed OOPs

`vmhook::hotspot::decode_oop_ptr(uint32_t compressed)` recovers the real address:  
`real_address = narrow_oop_base + (compressed << narrow_oop_shift)`  
Both values are read from `CompressedOops::_narrow_oop` via VMStructs.

---

## JVM type → C++ mapping

| JVM signature | `get()` returns |
|---------------|-----------------|
| `Z` | `bool` |
| `B` | `int8_t` |
| `S` | `int16_t` |
| `I` | `int32_t` |
| `J` | `int64_t` |
| `F` | `float` |
| `D` | `double` |
| `C` | `uint16_t` (char) |
| `L…` / `[…` | `uint32_t` (compressed OOP — pass to `decode_oop_ptr()`) |

---

## HotSpot internals namespace (`vmhook::hotspot::`)

| Type | Purpose |
|------|---------|
| `VMStructEntry` / `VMTypeEntry` | Raw entries from `gHotSpotVMStructs` / `gHotSpotVMTypes` |
| `symbol` | Interned JVM string (class/method/field names) |
| `klass` | Java class metadata; `find_field()`, `get_java_mirror()`, `get_methods_ptr()` |
| `method` | Java method; i2i entry, access flags, name, signature |
| `constant_pool` | Per-class constant pool; `get_base()` → pointer to first cp entry |
| `class_loader_data_graph` | Root of all class loaders; `find_klass()` |
| `java_thread` | Current JVM thread; thread state |
| `frame` | Interpreter stack frame; `get_arguments<Types...>()` |
| `midi2i_hook` | 5-byte JMP patch at the i2i stub |
| `field_entry` | Field offset + signature + `is_static` |
| `decode_oop_ptr()` | Compressed OOP → real 64-bit address |
| `decode_klass_ptr()` | Compressed klass pointer → real 64-bit address |
