// Modular JVM test harness — shared API for per-feature test modules.
//
// Every feature gets ONE self-registering test module in tests/jvm/modules/.
// A module is a plain function that receives a `context` (its result sink + the
// Java-coordination probe) and runs as many JVM checks as it can imagine for
// its feature.  Modules self-register at DLL load via the VMHOOK_JVM_MODULE
// macro, so adding a feature is "drop a .cpp in tests/jvm/modules/" — no shared
// file to edit, no merge conflicts between parallel authors.
//
// The driver (example.cpp's run_test_suite) calls vmhook_test::run_all(ctx)
// once the JVM is live and the baseline wrappers are registered; results land in
// the same test_results.txt the CI greps for [FAIL].
#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace vmhook_test
{
    // The result sink + Java coordination handed to every module.  Implemented
    // by the driver (example.cpp) so modules never touch the ofstream directly.
    struct context
    {
        // Record a [PASS]/[FAIL] line and bump the pass/fail counters.
        std::function<void(const std::string& name, bool ok)> check;
        // Record a raw line (e.g. "[INFO] ...") without touching counters.
        std::function<void(const std::string& line)> record;

        // Java coordination.  set_go(true) raises the fixture's request flag;
        // poll get_done() until the Java loop has run the fixture's action.
        // Returns true if the action completed within the timeout.  This is the
        // ONLY way to make a hooked Java method actually run from native code:
        // the interpreter hook fires only on real Java bytecode dispatch, which
        // happens on the Java thread inside the fixture's registered action.
        std::function<bool(std::function<void(bool)> set_go,
                           std::function<bool()>     get_done)> run_probe;
    };

    using module_fn = void (*)(context&);

    // Register a module (called from the VMHOOK_JVM_MODULE static initializer).
    auto register_module(const char* name, module_fn fn) -> void;

    // Run every registered module in registration order.  Each module's
    // failures are isolated (a throwing module is caught and logged, the rest
    // still run).  Returns the number of modules executed.
    auto run_all(context& ctx) -> std::size_t;
}

// Define a self-registering feature test module.
//
//   #include <vmhook/vmhook.hpp>
//   #include "../harness.hpp"
//   VMHOOK_JVM_MODULE(field_primitives)
//   {
//       // ctx.check("...", cond); ctx.run_probe(...); ...
//   }
#define VMHOOK_JVM_MODULE(modname)                                            \
    static void modname##_body(vmhook_test::context& ctx);                    \
    namespace                                                                 \
    {                                                                         \
        struct modname##_registrar                                            \
        {                                                                     \
            modname##_registrar() noexcept                                    \
            {                                                                 \
                vmhook_test::register_module(#modname, &modname##_body);      \
            }                                                                 \
        } modname##_registrar_instance;                                       \
    }                                                                         \
    static void modname##_body(vmhook_test::context& ctx)
