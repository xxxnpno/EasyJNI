# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Git workflow

**Always push to GitHub after every improvement:**
```
git add -A
git commit -m "<description>"
git push
```
Push even for incremental fixes — every improvement that builds and passes tests should be on the remote.

## Build

Build with **Release|x64** using MSBuild:

```
"C:\Program Files\Microsoft Visual Studio\18\Insiders\MSBuild\Current\Bin\MSBuild.exe" VMHook\VMHook.vcxproj /p:Configuration=Release /p:Platform=x64 /t:Build /m /nologo
"C:\Program Files\Microsoft Visual Studio\18\Insiders\MSBuild\Current\Bin\MSBuild.exe" Injector\Injector.vcxproj /p:Configuration=Release /p:Platform=x64 /t:Build /m /nologo
```

Both binaries land in the root `build\` folder. Intermediate files go to root `etc\`.
Win32 configs are legacy — only x64 is used.

**External dependency (Debug|x64 only):** `VMHook\ext\jni\jvm.lib` from `%JAVA_HOME%\lib\`. Not needed for Release|x64.

## Testing — single JDK

```
cd example
run_tests.bat
```

This compiles the Java sources, launches the target process with `-Xint` (interpreter-only, required for hooks to fire), injects `build\Injector.exe`, and reads the results from `log.txt`. Press DELETE inside the VMHook console to unload.

## Testing — all JDK versions (REQUIRED after any change to vmhook.hpp)

```powershell
powershell -ExecutionPolicy Bypass -File example\test_all_jdks.ps1
```

This iterates every JDK in the registry at the top of the script and runs the full 93-assertion suite against each one. Every JDK must show **93 passed, 0 failed** before a change is committed.

### Installed JDK registry

The script maintains a `$jdks` ordered hashtable mapping major version → install path:

| Major | Path | Distribution |
|-------|------|--------------|
| 8  | `C:\Program Files\Eclipse Adoptium\jdk-8.0.482.8-hotspot` | Temurin 8u482 |
| 11 | `C:\jdks\jdk-11` | Temurin 11.0.30 |
| 17 | `C:\jdks\jdk-17` | Temurin 17.0.18 |
| 21 | `C:\Program Files\Eclipse Adoptium\jdk-21.0.10.7-hotspot` | Temurin 21.0.10 |
| 25 | `C:\jdks\jdk-25` | Temurin 25.0.2 (LTS) |
| 26 | `C:\jdks\jdk-26` | Temurin 26 (latest GA) |

### Adding a new JDK

When a new JDK release appears (check https://adoptium.net/temurin/releases/):

1. Download the **ZIP** (not MSI) for Windows x64 from Adoptium:
   ```
   https://api.adoptium.net/v3/binary/latest/<major>/ga/windows/x64/jdk/hotspot/normal/eclipse
   ```
2. Extract to `C:\jdks\jdk-<major>\` so that `C:\jdks\jdk-<major>\bin\java.exe` exists.
3. Add an entry to the `$jdks` hashtable in `example\test_all_jdks.ps1`.
4. Run `example\test_all_jdks.ps1` and fix any failures before committing.

**Only HotSpot distributions are supported** (Temurin, Corretto, Liberica, Microsoft, etc.).
OpenJ9 and GraalVM use different internal structures and are out of scope.

### Why -Xint is required for tests

The `-Xint` flag disables JIT compilation so every method call goes through the HotSpot interpreter. VMHook patches the **interpreter-to-interpreter (i2i) entry stub**, so hooks only fire for interpreted calls. Without `-Xint`, a method that has already been JIT-compiled before injection is called via compiled code and the hook never fires. C1 compiles hot methods after ~200 invocations (~2 seconds at 100/s); `-Xint` eliminates this race.

## Repository layout

```
build\                         ← VMHook.dll + Injector.exe (gitignored)
etc\                           ← intermediate files (gitignored)
VMHook\
  src\main.cpp                 ← DLL entry point: AllocConsole, class enumeration, runs test suite
  src\test.hpp                 ← full unit test harness (93 assertions across all API categories)
  ext\vmhook\vmhook.hpp        ← the single-header library (all logic)
  VMHook.vcxproj
Injector\
  src\main.cpp                 ← finds java.exe / javaw.exe; injects via LoadLibraryW; exits immediately
  Injector.vcxproj
example\
  src\Player.java              ← demo class with float/double/String/static fields
  src\TestTarget.java          ← test subject: every primitive type + static fields + hookable method
  src\Main.java                ← long-running target process; JDK-8-compatible
  build_and_run.bat            ← compile + launch (no -Xint; for manual testing)
  run_tests.bat                ← compile + launch with -Xint + wait for results
  test_all_jdks.ps1            ← iterate all JDKs, run full suite, print summary table
```

## Architecture

VMHook is a **single-header, header-only** C++ library. All logic lives in `VMHook/ext/vmhook/vmhook.hpp`. `src/main.cpp` is the consumer/test harness.

### Design: no JNI/JVMTI

Every field offset, type size, and entry point is read from `gHotSpotVMStructs` and `gHotSpotVMTypes` — symbol tables exported by `jvm.dll`. No `jni.h`, no JVMTI, no Java headers.

### Namespace layout

- **`vmhook::hotspot::`** — raw HotSpot internals:
  - `VMStructEntry` / `VMTypeEntry` — gHotSpotVMStructs / gHotSpotVMTypes entries
  - `symbol`, `constant_pool`, `const_method`, `method`, `klass` — JVM metadata
  - `class_loader_data`, `class_loader_data_graph`, `dictionary` — class registry walk
  - `java_thread`, `java_thread_state` — thread representation
  - `frame` — interpreter stack frame; `get_arguments<Types...>()` unpacks method args
  - `midi2i_hook` — 5-byte JMP patch at the i2i stub
  - `field_entry` — field offset + signature + is_static flag

- **`vmhook::`** — public API:
  - `type_to_class_map` — C++ type_index → Java class name registry
  - `klass_lookup_cache` — class name → klass* cache
  - `g_field_cache` — (klass*, name) → field_entry cache
  - `register_class<T>(class_name)` — verifies class is loaded and caches it
  - `find_class(class_name)` — walks ClassLoaderDataGraph to find a klass*
  - `find_field(klass*, name)` — looks up + caches a field_entry
  - `get_field<T>` / `set_field<T>` — instance field read/write
  - `get_static_field<T>` / `set_static_field<T>` — static field read/write
  - `field_proxy` / `field_proxy::value` — typed field accessor returned by `vmhook::object`
  - `object` — base class for Java object wrappers; `get_field(name)` auto-detects static
  - `hook<T>(method_name, detour)` — hooks a Java method via midi2i_hook
  - `shutdown_hooks()` — removes all hooks and restores original bytes

### Field access — version detection

`klass::find_field(name)` auto-detects the JDK field storage format via VMStructs:

- **JDK 8–20** (`InstanceKlass._fields` present): `Array<u2>`, 6 u2 slots per field.
  Layout: `[access_flags][name_idx][sig_idx][initval_idx][low_packed][high_packed]`
  Byte offset = `((high_packed << 16) | low_packed) >> 2` (strips 2-bit FIELDINFO_TAG).
  `Array<u2>._data` starts at `array_ptr + 4` (no padding for 2-byte elements).

- **JDK 21+** (`InstanceKlass._fieldinfo_stream` present): `Array<u1>`, UNSIGNED5-compressed.
  Grammar: `j(num_java) k(num_injected) Field[j+k] End(0x00)`
  Per field: `name_idx sig_idx offset access_flags field_flags [optional extras]`
  Optional extras signalled by `field_flags` bits: 0x01=generic_sig, 0x04=initval, 0x08=contended.
  `Array<u1>._data` starts at `array_ptr + 4` (no padding for 1-byte elements).

> **Critical:** `Array<Method*>._data` starts at `+8` (8-byte pointers need 8-byte alignment,
> requiring 4 bytes of padding after `_length`). Arrays of u1/u2 use `+4` — getting this
> wrong silently misaligns ALL field reads.

### get_java_mirror() — JDK version detection

Static fields live in the `java.lang.Class` mirror object. How `_java_mirror` is stored changed:

- **JDK 8–16**: `_java_mirror` is a plain `oop` (direct 64-bit pointer). VMStruct type_string = `"oop"`.
  One dereference: `mirror = *reinterpret_cast<void**>(klass + offset)`.

- **JDK 17+**: `_java_mirror` is an `OopHandle` (pointer into OopStorage). VMStruct type_string = `"OopHandle"`.
  Two dereferences: `oop_ptr = *reinterpret_cast<void**>(klass + offset); mirror = *reinterpret_cast<void**>(oop_ptr)`.

Detection uses `entry->type_string` from VMStructs at runtime — no hardcoded version numbers.

### Method hooking — known limitations

- **JDK 21+ locals pointer (FIXED)**: JDK 21+ spills a slot *index* (not the locals pointer itself) at `[rbp-56]`. `frame::get_locals()` now detects this: values ≤ 0xFFFF are treated as a slot index → `r14 = rbp + index * 8`. Proof from hs_err: `[rbp-56]=3`, `R14=RBP+24`. All JDK 8-26 now pass argument-capture tests.

- **JIT-compiled methods (PARTIALLY FIXED)**: When a method is JIT-compiled at hook-install time, `hook()` now:
  1. Saves original `_code`, `_from_interpreted_entry`, `_from_compiled_entry`.
  2. Nulls `_code` (deoptimises dispatch without freeing the nmethod).
  3. Resets `_from_interpreted_entry` → `_i2i_entry` (our patched stub).
  4. Resets `_from_compiled_entry` → c2i adapter (compiled callers re-enter interpreter).
  `shutdown_hooks()` restores all three fields.

  **Remaining limitation:** compiled callers that have a *stale monomorphic inline cache* pointing directly at the old nmethod still bypass the hook. These caches are updated lazily by HotSpot at the next safe-point evaluation or IC miss. In practice (C1 tier, virtual `invokevirtual`, `Thread.sleep` in loop) hooks fire within one or two IC-miss cycles. Fully eliminating this race requires either JVMTI retransformation or patching all call sites in the code cache — out of scope for the no-JVMTI design.

### ClassLoaderDataGraph walk strategy

- **JDK 21+**: `ClassLoaderData._klasses` present → walk the klass linked list per CLD
- **JDK 11–20**: `ClassLoaderData._dictionary` present → walk per-CLD Dictionary hashtable
- **JDK 8 fallback**: if per-CLD dict yields nothing, fall back to
  `SystemDictionary._dictionary` and `SystemDictionary._shared_dictionary`
