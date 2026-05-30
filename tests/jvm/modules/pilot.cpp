// Pilot JVM test module — validates the modular harness end-to-end:
// fixture discovery (Main scans vmhook.fixtures.*), the native run_probe
// handshake, and an interpreter hook firing on a real Java bytecode dispatch
// through the new modular path.  Every real feature module follows this shape.
#include <vmhook/vmhook.hpp>

#include "../harness.hpp"

#include <atomic>
#include <cstdint>
#include <memory>

namespace
{
    // Wrapper for vmhook.fixtures.Pilot.
    class pilot_fixture : public vmhook::object<pilot_fixture>
    {
    public:
        explicit pilot_fixture(vmhook::oop_t instance) noexcept
            : vmhook::object<pilot_fixture>{ instance }
        {
        }

        static auto set_go(bool value) -> void { static_field("go")->set(value); }
        static auto get_done() -> bool          { return static_field("done")->get(); }
        static auto get_observed() -> std::int32_t { return static_field("observed")->get(); }
    };

    std::atomic<int>          g_hook_calls{ 0 };
    std::atomic<std::int32_t> g_hook_arg{ -1 };
    std::atomic<bool>         g_hook_saw_self{ false };
}

VMHOOK_JVM_MODULE(pilot)
{
    vmhook::register_class<pilot_fixture>("vmhook/fixtures/Pilot");

    // scoped_hook isolates this module: the hook uninstalls when the handle
    // leaves scope at the end of the module, so modules never tear down each
    // other's hooks (which a bare shutdown_hooks() would).  This is the pattern
    // every per-feature module should follow.
    {
        auto handle{ vmhook::scoped_hook<pilot_fixture>(
            "touch",
            [](vmhook::return_value&,
               const std::unique_ptr<pilot_fixture>& self,
               std::int32_t delta)
            {
                g_hook_calls.fetch_add(1, std::memory_order_relaxed);
                g_hook_arg.store(delta, std::memory_order_relaxed);
                g_hook_saw_self.store(self != nullptr, std::memory_order_relaxed);
            }) };
        ctx.check("pilot_hook_installed", handle.installed());

        const bool done{ ctx.run_probe(
            [](bool value) { pilot_fixture::set_go(value); },
            []() { return pilot_fixture::get_done(); }) };

        ctx.check("pilot_probe_completed", done);
        ctx.check("pilot_hook_fired", g_hook_calls.load(std::memory_order_relaxed) >= 1);
        ctx.check("pilot_hook_saw_self", g_hook_saw_self.load(std::memory_order_relaxed));
        ctx.check("pilot_hook_saw_arg_42", g_hook_arg.load(std::memory_order_relaxed) == 42);
        // touch() returns seed(1000)+42; the hook does not force-return, so the
        // original body runs and Pilot.observed == 1042.
        ctx.check("pilot_observed_is_1042", pilot_fixture::get_observed() == 1042);
    }
}
