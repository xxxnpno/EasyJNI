# Changelog

All notable changes to this project are documented in this file.
The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and the project follows [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

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
