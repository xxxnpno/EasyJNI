# Changelog

All notable changes to this project are documented in this file.
The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and the project follows [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added
- `vmhook::scoped_hook<T>(name, callback)` + `class hook_handle` — RAII variant
  of `vmhook::hook<T>`.  The returned `hook_handle` uninstalls just that hook
  when it goes out of scope, restoring the method's original entry points and
  clearing the no-inline / no-compile flags.  Other hooks are unaffected;
  `shutdown_hooks()` still works as a hard reset.
- `vmhook::for_each_instance<T>(visitor, max_visits)` — walks the live heap
  (`Universe::_collectedHeap::_reserved`) and invokes the visitor with a fresh
  `std::unique_ptr<T>` for every object whose narrow-klass header matches
  `T`'s registered class.  Best-effort on region-based GCs (G1); unsupported
  on colored-pointer GCs (ZGC / Shenandoah).
- `return_value::stack_trace(max_depth = 64)` — walks the saved-rbp chain from
  inside a hook callback and returns every interpreter frame as a
  `caller_info`.  Stops at the first compiled / native frame so the result
  reflects only the interpreted portion of the call stack.
- `vmhook::for_each_loaded_class(visitor)` — enumerates every Klass reachable
  through the `ClassLoaderDataGraph`.  Snapshot of the loaded set at call
  time; pair with `vmhook::on_class_loaded` for live notifications.
- `vmhook::on_exception(callback)` — event-driven hook on
  `java.lang.Throwable::fillInStackTrace()`.  Fires whenever any Throwable
  subclass is constructed through one of the public constructors and
  reports the dynamic class name (read from the oop's narrow-klass header).
- Optional vmhook-vs-pure-JNI microbench at `vmhook/src/speedtest.cpp`.
  Lives in its own translation unit so `<jni.h>` never leaks into
  `vmhook.hpp`; opt-in via CMake's `find_package(JNI)` and runs at the end
  of the JVM integration suite, printing `[BENCH]` lines with ns/call for
  both paths.

### Changed
- `method_proxy::call_jni` now handles **static methods** and every
  primitive return type (`Z B C S I J F D V` plus `Ljava/lang/String;`).
  Previously it only supported instance methods returning `void` or
  `String`, which broke on modern JDKs where
  `StubRoutines::_call_stub_entry` is no longer in VMStructs and the
  fallback path was the only way to dispatch.
- `method_proxy` caches `jmethodID` / `jclass` / return-type char on
  first call and reuses them across subsequent invocations.  Combined
  with packing JNI args on the stack instead of through a
  `std::vector`, this brings a tight `Math.abs`-style call loop from
  ~36× slower than pure JNI down to ~1.5× — most of the residual gap
  is the type-safe variant return and the thread-local attach probe.
- `call()` short-circuits straight into `call_jni` on JDKs where
  `_call_stub_entry` is unavailable, instead of doing the call-stub
  prep work (overload resolution, signature reload) on every call only
  to throw it away.

### Fixed
- `for_each_loaded_class` returned nothing on JDK 8.  The internal
  `ClassLoaderDataGraph::for_each_klass` only walked
  `ClassLoaderData::_klasses` (JDK 21+); the JDK 8 path walks
  per-CLD `_dictionary` hashtables plus `SystemDictionary::_dictionary`
  and `_shared_dictionary` for bootstrap classes, which is now wired up.

## [0.4.0] — 2026-05-14

### Added
- `vmhook::return_value::caller()` — from inside any hook callback, returns a
  `caller_info { method*, class_name, method_name, signature }` describing the
  method that invoked the currently-hooked one.  Walks the saved-rbp chain on
  the HotSpot interpreter stack with the same safe-pointer validation the rest
  of the header uses.  Returns `valid() == false` when the parent frame is
  compiled, native, or otherwise unreadable.
- `vmhook::on_class_loaded(callback)` — event-driven hook on
  `java.lang.ClassLoader::defineClass(String, byte[], int, int,
  ProtectionDomain)`.  Fires synchronously on the Java thread that triggered
  the load, with the internal class name (`/`-separated).  Zero polling, zero
  idle cost.  Returns an RAII `watch_handle` that removes the callback when
  destroyed.
- `vmhook::watch_static_field<T, V>(name, callback)` — installs a hardware
  data breakpoint (DR0–DR3 + DR7 write trap) on the field's address.  The
  trap fires instantly on every write with zero idle cost; the callback
  runs synchronously on the writing thread inside a vectored exception
  handler.  Up to four simultaneous watches per process.  Windows × x86_64
  only — on other platforms the function logs an error and returns an
  empty `watch_handle` (no polling fallback; `VMHOOK_HAS_HW_DATA_BREAKPOINTS`
  is the compile-time capability flag).
- New hook overload: `vmhook::hook<T>(name, signature, callback)` selects the
  target method by matching both name AND JVM descriptor.  Needed for classes
  with overloaded methods sharing a name (e.g. ClassLoader's five
  `defineClass` overloads).
- `field_proxy::raw_address()` — exposes the backing memory pointer so the
  hardware-breakpoint watcher can hand it to the kernel.
- `VMHOOK_VERSION_MAJOR/MINOR/PATCH` macros and a `VMHOOK_VERSION` packed
  integer for consumer feature-gating.
- New Java fixtures: `CallerProbe`, `TickerProbe`, `LateClass`.  178 unit
  tests covering everything above plus existing scenarios.

### Changed
- Java fixtures `A` and `B` reorganised for consistency: every class now has
  a Javadoc header documenting its role; constructors are `public` where
  the C++ side needs them; field visibility is uniform.
- `vmhook::object<T>` exposes both deducing-this and static-fallback
  overloads of `get_field`/`get_method` on MSVC and Clang 18+; GCC and
  Android NDK Clang fall back to inherited non-static + explicit
  `static_field`/`static_method` aliases (overload-resolution divergence
  is documented in the header).

### Fixed
- iOS / Android NDK builds now compile cleanly (mach_vm gating, sig*
  POSIX includes, deducing-this disabled where buggy).
- macOS arm64 `allocate_rwx` falls back to PROT_READ|PROT_WRITE when the
  kernel rejects W^X without a JIT entitlement.
- The hardware-breakpoint watcher needs `<mutex>` and `<tlhelp32.h>` —
  added to the master include list.

## [0.3.0] — 2026-05-14

### Added
- macOS, iOS, Android platform detection (`VMHOOK_OS_MACOS`, `_IOS`,
  `_ANDROID`).  Header now compiles cleanly with Apple Clang and the
  Android NDK Clang.
- `VMHOOK_ARCH_X86_64` / `VMHOOK_ARCH_ARM64` macros.
- `VMHOOK_RUNTIME_HOOKING_AVAILABLE` flag — `1` on x86_64 + non-iOS,
  `0` elsewhere.  Runtime hooking trampolines no-op on arm64 / iOS.
- System V AMD64 trampoline for Linux / macOS / Android x86_64.
- CI matrix now covers Windows × {MSVC, clang, MinGW-GCC}, Linux ×
  {GCC, clang}, macOS × clang, plus Android NDK and iOS Xcode cross-compile
  jobs (build-only).
- JVM integration runs against Java 8, 11, 17, 21, 24, 25 on every
  build that produces a usable artefact.

### Changed
- Massive expansion of the Java probe suite (101 → 178 tests).  Now
  covers every primitive type at boundary values, string edge cases,
  multi-dimensional arrays, enums, interface default methods,
  static + non-static nested classes, overloaded methods, throwing
  methods, every primitive return type plus void and null-returning.

## [0.2.0] — 2026-05-14

### Added
- CMake build system (`CMakeLists.txt`) with `vmhook::vmhook` INTERFACE
  target, optional example DLL, optional injector, opt-in
  warnings-as-errors.
- `vmhook::os` portable wrappers for module lookup, memory protection,
  region query, safe memory reads, and thread IDs.  Backed by Win32 on
  Windows and dlopen/mmap/mprotect/process_vm_readv + signal fallback
  on Linux.
- Standalone unit-test suite under `tests/` exercising the OS layer,
  type traits, ODR sanity, and the API surface without a JVM.

### Changed
- Header builds cleanly on MSVC, Clang, and GCC.  Replaced
  `std::print`/`std::println` with a portable formatter helper.
- Logging now goes through `vmhook::detail::format_log` + `emit_log_line`.

## [0.1.0] — 2026-04-29

### Added
- Original Windows-only release: single-header HotSpot hooking library
  with field access, method calling, interpreter-stub hooks (force-return,
  cancel, arg mutation), `make_unique` Java-object allocation, class
  lookup via VMStructs with JNI fallback.

[Unreleased]: https://github.com/xxxnpno/vmhook/compare/v0.4.0...HEAD
[0.4.0]: https://github.com/xxxnpno/vmhook/compare/v0.3.0...v0.4.0
[0.3.0]: https://github.com/xxxnpno/vmhook/compare/v0.2.0...v0.3.0
[0.2.0]: https://github.com/xxxnpno/vmhook/compare/v0.1.0...v0.2.0
[0.1.0]: https://github.com/xxxnpno/vmhook/releases/tag/v0.1.0
