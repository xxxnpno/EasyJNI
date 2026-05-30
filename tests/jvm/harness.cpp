// Modular JVM test harness — registry implementation.
//
// Holds the global list of self-registered feature modules and runs them.
// Deliberately tiny and dependency-free (no vmhook.hpp) so it links into the
// example DLL alongside example.cpp without ODR or include-order surprises.
#include "harness.hpp"

#include <exception>
#include <vector>

namespace vmhook_test
{
    namespace
    {
        struct entry
        {
            const char* name;
            module_fn   fn;
        };

        // Function-local static so registration from other TUs' static
        // initializers (which may run before/after this TU's) always sees a
        // constructed vector — avoids the static-init-order fiasco.
        auto registry() -> std::vector<entry>&
        {
            static std::vector<entry> modules;
            return modules;
        }
    }

    auto register_module(const char* const name, const module_fn fn) -> void
    {
        if (name && fn)
        {
            registry().push_back(entry{ name, fn });
        }
    }

    auto run_all(context& ctx) -> std::size_t
    {
        std::size_t ran{ 0 };
        for (const entry& module_entry : registry())
        {
            if (ctx.record)
            {
                ctx.record(std::string{ "[INFO] === module: " } + module_entry.name + " ===");
            }
            try
            {
                module_entry.fn(ctx);
            }
            catch (const std::exception& ex)
            {
                if (ctx.check)
                {
                    ctx.check(std::string{ "module_" } + module_entry.name + "_no_throw", false);
                }
                if (ctx.record)
                {
                    ctx.record(std::string{ "[INFO] module " } + module_entry.name
                               + " threw: " + ex.what());
                }
            }
            catch (...)
            {
                if (ctx.check)
                {
                    ctx.check(std::string{ "module_" } + module_entry.name + "_no_throw", false);
                }
            }
            // Paired with the "=== module: X ===" line above: with per-line
            // flushing in the driver, if a module crashes the JVM (SEH, not a
            // C++ throw we can catch) the results file ends at its "=== module:
            // X ===" with no matching "--- done ---", naming the culprit.
            if (ctx.record)
            {
                ctx.record(std::string{ "[INFO] --- module " } + module_entry.name + " done ---");
            }
            ++ran;
        }
        return ran;
    }
}
