# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build

Open `VMHook.slnx` in Visual Studio 2022 and build **Release|x64**. Outputs:
- `build\VMHook.dll` — the injectable HotSpot hook library
- `build\Injector.exe` — the DLL injector tool

Win32 configurations are legacy/unused — x64 is the only relevant target.

**External dependency not tracked in git:** `VMHook\ext\jni\jvm.lib` (from the JDK). Copy from `%JAVA_HOME%\lib\jvm.lib`. Only needed for Debug|x64; Release|x64 omits it.

**Intermediate files** go to `etc\vmhook\` and `etc\injector\` at the solution root. Both binaries land in the single `build\` folder at the solution root.

There is no test suite, no build script, and no CI. Testing means injecting `VMHook.dll` into a running JVM via `Injector.exe`.

## Repository layout

```
VMHook.slnx
build\                   ← both VMHook.dll and Injector.exe land here
etc\
  vmhook\                ← VMHook intermediate files
  injector\              ← Injector intermediate files
VMHook\
  src\main.cpp           ← DLL entry point (DllMain + worker thread)
  ext\vmhook\vmhook.hpp  ← the header-only library (single file, all logic)
Injector\
  src\main.cpp           ← finds javaw.exe, injects VMHook.dll
launch_minecraft.ps1     ← extracts natives + launches MC 1.8.9 offline
launch_minecraft.bat     ← thin wrapper that calls the .ps1
```

## Architecture

`VMHook` is a **single-header, header-only** C++ library. All logic lives in `VMHook/ext/vmhook/vmhook.hpp`. `src/main.cpp` is the DLL entry point — it is the consumer.

### Design principle: no JNI/JVMTI

The library reads `gHotSpotVMStructs` and `gHotSpotVMTypes` — symbol tables exported from `jvm.dll` — to discover every field offset and type size at runtime. No `jni.h`, no JVMTI, no Java headers of any kind.

### Namespace layout inside `vmhook.hpp`

- **`jni::hotspot::`** — raw HotSpot internals:
  - `VMStructEntry` / `VMTypeEntry` — gHotSpotVMStructs / gHotSpotVMTypes entries
  - `symbol`, `constant_pool`, `const_method`, `method`, `klass` — JVM metadata
  - `class_loader_data`, `class_loader_data_graph`, `dictionary` — class registry walk
  - `java_thread`, `java_thread_state` — thread representation
  - `frame` — interpreter stack frame; `get_arguments<Types...>()` unpacks method args
  - `midi2i_hook` — 5-byte JMP patch at the i2i stub
  - `field_entry` — field offset + signature + is_static flag

- **`jni::`** — public API:
  - `class_map` — `type_index → Java class name` registry
  - `classes_hs` — `class name → klass*` cache
  - `field_cache` — `(klass*, name) → field_entry` cache
  - `register_class<T>(class_name)` — verifies class is loaded and caches it
  - `find_class(class_name)` — walks `ClassLoaderDataGraph` to find a `klass*`
  - `find_field(klass*, name)` — looks up + caches a `field_entry`
  - `get_field<T>` / `set_field<T>` — low-level instance field read/write
  - `get_static_field<T>` / `set_static_field<T>` — low-level static field read/write
  - `field_proxy` / `field_proxy::value` — typed field accessor returned by `jni::object`
  - `object` — base class for Java object wrappers; `get_field(name)` auto-detects static
  - `hook<T>(method_name, detour)` — hooks a Java method via `midi2i_hook`
  - `shutdown_hooks()` — removes all hooks and restores original bytes

### Field access

`klass::find_field(name)` parses `InstanceKlass._fields` (an `Array<u2>`, 6 u2 slots per field, JDK 8–21). Byte offset = `((high_packed << 16) | low_packed) >> 2`. Static fields live in the `java.lang.Class` mirror at their stored offset; accessed via `klass::get_java_mirror()` which follows `Klass::_java_mirror` (an OopHandle, JDK 17+).

### Method hooking internals

`hook<T>()` patches the i2i entry stub with a 5-byte relative JMP to a per-trampoline stub allocated nearby. The injection point is found by scanning for a known x64 byte pattern (`find_hook_location()`). All trampolines share `common_detour` as the single C++ entry point. **Any method already JIT-compiled will not be intercepted** — hook early.

### Compressed OOP / Klass pointer decoding

`decode_oop_ptr()` and `decode_klass_ptr()` recover real 64-bit addresses from 32-bit compressed pointers using `CompressedOops::_narrow_oop._base/_shift` and `CompressedKlassPointers::_narrow_klass._base/_shift` read from VMStructs.
