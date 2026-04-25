# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build

Open `EasyJNI.slnx` in Visual Studio 2022 and build the **Debug|x64** or **Release|x64** configuration. The output is `build\EasyJNI.dll`.

Win32 configurations are legacy/unused — x64 is the only relevant target.

**External dependency not tracked in git:** `EasyJNI\ext\jni\jvm.lib` (from the JDK installation). Copy it from `%JAVA_HOME%\lib\jvm.lib`. This is only needed for the Debug|x64 linker; Release|x64 omits it.

There is no test suite, no build script, and no CI. The only way to test is to inject the DLL into a running JVM process.

## Architecture

This is a **single-header, header-only** C++ library: all logic lives in `EasyJNI/ext/easy_jni/easy_jni.hpp`. The `src/main.cpp` is only a DLL entry point (`DllMain` + a worker thread stub) — it is the consumer, not the library.

### Design principle: no JNI/JVMTI

The library deliberately avoids the JNI and JVMTI APIs entirely. Instead it reads `gHotSpotVMStructs` and `gHotSpotVMTypes` — two symbol tables exported from `jvm.dll` — to discover every field offset and type size at runtime. This makes the library version-agnostic across HotSpot builds without needing `jni.h` or any Java headers.

### Namespace layout inside `easy_jni.hpp`

- **`jni::hotspot::`** — raw HotSpot internal structures mirrored in C++:
  - `VMStructEntry` / `VMTypeEntry` — entries from `gHotSpotVMStructs` / `gHotSpotVMTypes`
  - `symbol`, `constant_pool`, `const_method`, `method`, `klass` — JVM metadata objects
  - `class_loader_data`, `class_loader_data_graph`, `dictionary` — class registry walk
  - `java_thread`, `java_thread_state` — thread representation
  - `frame` — interpreter stack frame; `get_arguments<Types...>()` unpacks method args
  - `midi2i_hook` — patches a 5-byte JMP into the i2i stub of a method

- **`jni::`** (top-level) — public API:
  - `class_map` — `type_index → Java class name` registry, populated by `register_class<T>()`
  - `classes_hs` — `class name → klass*` cache, populated lazily by `find_class()`
  - `register_class<T>(class_name)` — verifies the class is loaded and caches it
  - `find_class(class_name)` — walks `ClassLoaderDataGraph` to find a `klass*`
  - `hook<T>(method_name, detour)` — hooks a Java method via `midi2i_hook`
  - `shutdown_hooks()` — removes all hooks and restores original bytes

### Method hooking internals

`hook<T>()` installs hooks at the **i2i (interpreter-to-interpreter) entry stub** of the target method, not at the Java bytecode level. The injection point is found by scanning for a known x64 byte pattern in the stub (`find_hook_location()`). A 5-byte relative JMP replaces the original bytes, redirecting execution to a per-entry trampoline allocated nearby (`allocate_nearby_memory()`).

All trampolines share `common_detour` as the single C++ entry point; it dispatches to the per-method detour stored in `hooked_methods`. Because the hook fires inside the interpreter, **any method that has already been JIT-compiled will not be intercepted** — hook early.

When a hook is installed the method is flagged `NO_COMPILE` and `_dont_inline` to prevent future JIT compilation.

### Compressed OOP / Klass pointer decoding

HotSpot compresses heap object references to 32 bits. `decode_oop_ptr()` and `decode_klass_ptr()` recover the real 64-bit address using `CompressedOops::_narrow_oop._base/_shift` and `CompressedKlassPointers::_narrow_klass._base/_shift` from VMStructs.

### Current branch state (`dev/jni-refactor`)

The branch is a work-in-progress rewrite. The header currently contains only the HotSpot internals layer and the hooking subsystem. The higher-level wrapper API described in README.md (`jni::object`, `get_method`, `get_field`, `jni::init`, `jni::shutdown`, `jni::make_unique`, `jni::list`, etc.) has not yet been re-implemented in this branch. `main.cpp` is a skeleton with no usage code.
