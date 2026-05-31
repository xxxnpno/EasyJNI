// on_exception JVM test module  (feature area: hooks / exception watcher)
//
// THE exception-watcher authority: exhaustively exercises
// vmhook::on_exception(callback) — the watcher that fires whenever a
// java.lang.Throwable (or subclass) is constructed, because every public
// Throwable constructor runs Throwable.fillInStackTrace() (the hooked method)
// before returning.  Migrates the legacy vmhook/src/example.cpp
// test_on_exception (the throwProbe IllegalStateException path).
//
// What this module proves / characterizes on a live JVM:
//   * on_exception(cb) installs a watcher and returns a watch_handle whose
//     running() reflects whether the underlying hook is armed;
//   * a GENUINE Java `athrow` of java.lang.IllegalStateException (constructed by
//     the OnException fixture probe on the Java thread) reaches the callback with
//     the JVM-internal '/'-separated class name "java/lang/IllegalStateException"
//     (asserted BEST-EFFORT — see the flaw note below — but always proven to have
//     RUN on the Java side via the fixture's own witnesses);
//   * a different throw (NumberFormatException) is reported under its own name, so
//     the callback can discriminate by type; a no-throw control cycle yields no
//     new event;
//   * the RAII watch_handle disarms on scope exit — a throw AFTER the handle is
//     dropped is not observed by that callback;
//   * multiple watchers all observe the same throw, and dropping ONE silences
//     only it (the survivors keep firing);
//   * CHARACTERIZES the known [HIGH] flag-reset defect shared with on_class_loaded
//     (audit/findings/on_class_loaded_define_class_hook.md, "Parity Concerns"):
//     detail::exception_hook_installed is NEVER reset by shutdown_hooks() and the
//     callback registry is never cleared, so once any earlier code has installed
//     the fillInStackTrace hook and then called shutdown_hooks(), a fresh
//     on_exception() returns a LIVE-LOOKING handle (running()==true) whose
//     callback can no longer fire.  We assert the ACTUAL behavior and record an
//     [INFO] FLAW line rather than hard-failing.
//
// SAFETY: the exception callback runs on the Java thread inside the
// fillInStackTrace detour.  It touches ONLY std::atomic — no JVM re-entry, no
// allocation, no oop/pointer deref (the header already extracted the internal
// class-name string for us, so we never chase a raw oop here).  Where this
// module DOES need a pointer (none on the hot path), it would gate behind
// vmhook::hotspot::is_valid_pointer.  No watcher is left armed at module exit:
// every watch_handle is RAII-scoped or explicitly stop()'d.
//
// Harness shape mirrors hook_basic / on_class_loaded: a `mode` selector with a
// `done` reset on the rising edge of go, plus Java-observable witnesses
// (throwsObserved / lastThrowKind) read back so "callback didn't fire" is always
// distinguishable from "throw never ran".
//
// IMPORTANT runtime note: in the integration driver the legacy suite installs
// the fillInStackTrace hook (test_on_exception) and several later legacy tests
// call shutdown_hooks() BEFORE this modular harness runs.  Because of the
// flag-reset defect, this module therefore typically executes with the watcher
// already latched-installed-but-dead.  Every trap-dependent assertion is gated
// best-effort for exactly that reason; the structural watch_handle contract
// (running()/RAII/registry) is asserted unconditionally because it holds either
// way.
#include <vmhook/vmhook.hpp>

#include "../harness.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

namespace
{
    // ---- Witnesses captured INSIDE the on_exception callback ----------------
    // The callback runs on the Java thread in the fillInStackTrace detour; it may
    // be invoked concurrently for distinct watchers, so every witness is atomic.
    // We NEVER touch a JVM object here: the header hands us the already-decoded
    // internal class name, which we only compare against known string constants.
    std::atomic<int>  g_primary_total{ 0 };       // every Throwable the primary watcher saw
    std::atomic<int>  g_primary_ise{ 0 };         // ... that were java/lang/IllegalStateException
    std::atomic<int>  g_primary_nfe{ 0 };         // ... that were java/lang/NumberFormatException
    std::atomic<bool> g_primary_saw_throwable_pkg{ false }; // name began with "java/lang/"
    std::atomic<bool> g_primary_saw_empty{ false };         // a callback received an empty name

    // Second + third watchers: only count ISE so we can prove fan-out and that
    // dropping ONE handle silences only it.
    std::atomic<int>  g_second_ise{ 0 };
    std::atomic<int>  g_third_ise{ 0 };

    // A watcher whose handle we drop early; it must NOT count throws after stop().
    std::atomic<int>  g_dropped_ise{ 0 };

    // Internal names the fixture throws (mirrors OnException.*_INTERNAL_NAME).
    constexpr const char* k_ise_name{ "java/lang/IllegalStateException" };
    constexpr const char* k_nfe_name{ "java/lang/NumberFormatException" };

    auto starts_with(const std::string& s, const char* prefix) noexcept -> bool
    {
        const std::string p{ prefix };
        return s.size() >= p.size() && s.compare(0, p.size(), p) == 0;
    }

    // ---- Wrapper for vmhook.fixtures.OnException: drives the go/done/mode
    //      handshake and reads back the Java-observable witnesses. ------------
    class oe : public vmhook::object<oe>
    {
    public:
        explicit oe(vmhook::oop_t instance) noexcept
            : vmhook::object<oe>{ instance }
        {
        }

        static auto set_go(bool value) -> void       { static_field("go")->set(value); }
        static auto set_done(bool value) -> void      { static_field("done")->set(value); }
        static auto get_done() -> bool                { return static_field("done")->get(); }
        static auto set_mode(std::int32_t m) -> void  { static_field("mode")->set(m); }

        static auto resolves(const char* name) -> bool
        {
            return static_field(name).has_value();
        }

        // Java-observable witnesses (copy-init out of value_t — never brace-init).
        static auto throws_observed() -> std::int32_t
        {
            const auto p{ static_field("throwsObserved") };
            if (!p.has_value()) { return -1; }
            const std::int32_t v = p->get();
            return v;
        }
        static auto last_throw_kind() -> std::int32_t
        {
            const auto p{ static_field("lastThrowKind") };
            if (!p.has_value()) { return -1; }
            const std::int32_t v = p->get();
            return v;
        }
    };

    // Drive one probe cycle for `mode`: clear the latched `done` and program the
    // selector on the rising edge of go, then wait for done.  (Mirrors the
    // field_static / hook_basic drive() helper.)
    auto drive(vmhook_test::context& ctx, std::int32_t mode) -> bool
    {
        return ctx.run_probe(
            [mode](bool value)
            {
                if (value)
                {
                    oe::set_done(false);
                    oe::set_mode(mode);
                }
                oe::set_go(value);
            },
            []() { return oe::get_done(); });
    }

    // ---- Callback factories.  Each returns a void(const std::string&) that
    //      touches ONLY atomics. -------------------------------------------
    auto make_primary_cb()
    {
        return [](const std::string& name)
        {
            g_primary_total.fetch_add(1, std::memory_order_relaxed);
            if (name.empty())
            {
                g_primary_saw_empty.store(true, std::memory_order_relaxed);
            }
            if (starts_with(name, "java/lang/"))
            {
                g_primary_saw_throwable_pkg.store(true, std::memory_order_relaxed);
            }
            if (name == k_ise_name)
            {
                g_primary_ise.fetch_add(1, std::memory_order_relaxed);
            }
            else if (name == k_nfe_name)
            {
                g_primary_nfe.fetch_add(1, std::memory_order_relaxed);
            }
        };
    }
}

VMHOOK_JVM_MODULE(on_exception)
{
    // =====================================================================
    //  0. Fixture resolves and the handshake/witness fields are present.
    // =====================================================================
    vmhook::register_class<oe>("vmhook/fixtures/OnException");
    ctx.check("oe_class_registered_go_resolves", oe::resolves("go"));
    ctx.check("oe_mode_field_resolves", oe::resolves("mode"));
    ctx.check("oe_throwsObserved_field_resolves", oe::resolves("throwsObserved"));
    ctx.check("oe_lastThrowKind_field_resolves", oe::resolves("lastThrowKind"));

    // =====================================================================
    //  1. Install the primary watcher.  Its running() tells us whether the
    //     underlying fillInStackTrace hook is actually armed in THIS process.
    //     (In the integration driver an earlier shutdown_hooks() has usually
    //     left exception_hook_installed latched-true with the hook torn down —
    //     the characterized flaw — so the handle still reports running()==true.)
    // =====================================================================
    auto primary{ vmhook::on_exception(make_primary_cb()) };
    const bool primary_running{ primary.running() };
    ctx.check("primary_watch_handle_running_after_install", primary_running);
    ctx.record(std::string{ "[INFO] on_exception: primary watch_handle.running()=" }
               + (primary_running ? "true" : "false"));

    // =====================================================================
    //  2. Trigger a GENUINE java.lang.IllegalStateException athrow and observe.
    //     We ALWAYS prove the Java throw ran (fixture witnesses), then assert the
    //     callback observation BEST-EFFORT: if the trap is live we require the
    //     internal-name discrimination; if it is dead (the flaw) we record it.
    // =====================================================================
    g_primary_total.store(0);
    g_primary_ise.store(0);
    g_primary_nfe.store(0);
    g_primary_saw_throwable_pkg.store(false);
    g_primary_saw_empty.store(false);

    const bool done1{ drive(ctx, 1) };
    ctx.check("ise_probe_completed", done1);

    // The Java side genuinely constructed + threw + caught exactly one ISE,
    // regardless of whether the native callback fired.  This is the
    // "throw really happened" anchor that makes a silent callback diagnosable.
    if (done1)
    {
        ctx.check("ise_probe_java_threw_one", oe::throws_observed() == 1);
        ctx.check("ise_probe_java_last_kind_ise", oe::last_throw_kind() == 1);
    }

    // Did the trap fire at all for this genuine throw?
    const bool trap_live{ g_primary_total.load() > 0 };
    ctx.record(std::string{ "[INFO] on_exception: primary callback fired " }
               + std::to_string(g_primary_total.load())
               + " time(s) for one genuine ISE athrow (trap "
               + (trap_live ? "LIVE" : "DEAD") + ").");

    if (trap_live)
    {
        // The trap is armed: require the headline contract.
        ctx.check("primary_observed_ise_internal_name", g_primary_ise.load() >= 1);
        ctx.check("primary_saw_java_lang_package", g_primary_saw_throwable_pkg.load());
        ctx.check("primary_never_saw_empty_name", g_primary_saw_empty.load() == false);
    }
    else
    {
        // The characterized flaw: handle says running()==true but the hook was
        // torn down by an earlier shutdown_hooks() and exception_hook_installed
        // is latched true, so a fresh on_exception() can never fire.  Assert the
        // ACTUAL (silent) behavior so a regression is still caught, and record.
        ctx.check("primary_silent_when_hook_torn_down_KNOWN_flag_reset_flaw",
                  g_primary_ise.load() == 0);
        ctx.record("[INFO] FLAW on_exception: a fresh on_exception() returned a "
                   "watch_handle with running()==true, yet its callback did NOT fire "
                   "on a genuine java.lang.IllegalStateException athrow.  "
                   "detail::exception_hook_installed is NEVER reset by shutdown_hooks() "
                   "(and detail::exception_callbacks is never cleared), so once an "
                   "earlier shutdown_hooks() tore down the Throwable.fillInStackTrace "
                   "hook the watcher is permanently dead while still reporting itself "
                   "armed.  Same [HIGH] defect as on_class_loaded "
                   "(audit/findings/on_class_loaded_define_class_hook.md, Parity "
                   "Concerns). Suggested fix: reset exception_hook_installed=false and "
                   "clear exception_callbacks inside shutdown_hooks().");
    }

    // =====================================================================
    //  3. Type discrimination: a NumberFormatException athrow.  Best-effort —
    //     only meaningful when the trap is live — but the Java throw is always
    //     proven to have run.
    // =====================================================================
    g_primary_total.store(0);
    g_primary_ise.store(0);
    g_primary_nfe.store(0);

    const bool done3{ drive(ctx, 3) };
    ctx.check("nfe_probe_completed", done3);
    if (done3)
    {
        ctx.check("nfe_probe_java_threw_one", oe::throws_observed() == 1);
        ctx.check("nfe_probe_java_last_kind_nfe", oe::last_throw_kind() == 2);
    }
    if (trap_live)
    {
        // The callback must report the NFE under ITS internal name, and must NOT
        // have mis-attributed it as an ISE.
        ctx.check("primary_observed_nfe_internal_name", g_primary_nfe.load() >= 1);
        ctx.check("primary_did_not_miscount_nfe_as_ise", g_primary_ise.load() == 0);
    }

    // =====================================================================
    //  4. Multiple watchers + selective drop.  Install a SECOND and THIRD
    //     watcher (count ISE only) plus a fourth "dropped" watcher we stop()
    //     before the throw.  All LIVE watchers must observe the same throws;
    //     the dropped one must observe none.  Structural truths (a stopped
    //     handle no longer running()) hold regardless of trap liveness.
    // =====================================================================
    {
        auto second{ vmhook::on_exception(
            [](const std::string& name)
            {
                if (name == k_ise_name) { g_second_ise.fetch_add(1, std::memory_order_relaxed); }
            }) };
        auto third{ vmhook::on_exception(
            [](const std::string& name)
            {
                if (name == k_ise_name) { g_third_ise.fetch_add(1, std::memory_order_relaxed); }
            }) };

        ctx.check("second_watch_handle_running", second.running());
        ctx.check("third_watch_handle_running", third.running());

        // A watcher we arm and then immediately disarm BEFORE any throw.
        {
            auto dropped{ vmhook::on_exception(
                [](const std::string& name)
                {
                    if (name == k_ise_name) { g_dropped_ise.fetch_add(1, std::memory_order_relaxed); }
                }) };
            ctx.check("dropped_watch_handle_running_before_stop", dropped.running());
            dropped.stop();
            // Structural: stop() is observable on the handle, independent of trap.
            ctx.check("dropped_watch_handle_not_running_after_stop", dropped.running() == false);
            // Idempotent second stop() must not throw / change anything.
            dropped.stop();
            ctx.check("dropped_watch_handle_stop_idempotent", dropped.running() == false);
        }

        g_primary_total.store(0);
        g_primary_ise.store(0);
        g_second_ise.store(0);
        g_third_ise.store(0);
        g_dropped_ise.store(0);

        // Throw MANY ISEs in one cycle.
        const bool done2{ drive(ctx, 2) };
        ctx.check("many_ise_probe_completed", done2);
        if (done2)
        {
            ctx.check("many_ise_probe_java_threw_n", oe::throws_observed() == 4);
            ctx.check("many_ise_probe_java_last_kind_ise", oe::last_throw_kind() == 1);
        }

        ctx.record(std::string{ "[INFO] on_exception: after 4 ISE athrows primary=" }
                   + std::to_string(g_primary_ise.load()) + " second="
                   + std::to_string(g_second_ise.load()) + " third="
                   + std::to_string(g_third_ise.load()) + " dropped="
                   + std::to_string(g_dropped_ise.load()) + " (trap "
                   + (trap_live ? "LIVE" : "DEAD") + ").");

        // The dropped watcher must NEVER observe a post-stop throw — this is true
        // whether the trap is live (it was unregistered) or dead (nothing fires
        // at all).  Asserted unconditionally.
        ctx.check("dropped_watcher_silent_after_stop", g_dropped_ise.load() == 0);

        if (trap_live)
        {
            // Every live watcher saw all four throws, identically: proves fan-out
            // and that the surviving handles still fire after one sibling dropped.
            ctx.check("primary_saw_all_four_ise", g_primary_ise.load() == 4);
            ctx.check("second_saw_all_four_ise", g_second_ise.load() == 4);
            ctx.check("third_saw_all_four_ise", g_third_ise.load() == 4);
            ctx.check("all_live_watchers_agree_count",
                      g_primary_ise.load() == g_second_ise.load()
                      && g_second_ise.load() == g_third_ise.load());
        }
    }
    // `second` and `third` handles dropped here (RAII stop): they must no longer
    // fire.  Re-run a single ISE; only `primary` (still in scope) could observe.
    {
        g_primary_ise.store(0);
        g_second_ise.store(0);
        g_third_ise.store(0);

        const bool done_after{ drive(ctx, 1) };
        ctx.check("post_drop_ise_probe_completed", done_after);
        if (done_after)
        {
            ctx.check("post_drop_java_threw_one", oe::throws_observed() == 1);
        }

        // RAII-disarm contract: the dropped second/third watchers observe NOTHING
        // after their handles left scope — true regardless of trap liveness.
        ctx.check("second_silent_after_raii_drop", g_second_ise.load() == 0);
        ctx.check("third_silent_after_raii_drop", g_third_ise.load() == 0);

        if (trap_live)
        {
            // The still-armed primary keeps firing for the new throw.
            ctx.check("primary_still_fires_after_siblings_dropped", g_primary_ise.load() == 1);
        }
    }

    // =====================================================================
    //  5. Control: a NO-THROW cycle constructs no Throwable, so NO watcher may
    //     observe a new event.  This holds whether or not the trap is live, and
    //     is the clean negative that distinguishes "armed + firing on throws" from
    //     "firing spuriously".
    // =====================================================================
    {
        g_primary_total.store(0);
        g_primary_ise.store(0);

        const bool done4{ drive(ctx, 4) };
        ctx.check("control_no_throw_probe_completed", done4);
        if (done4)
        {
            ctx.check("control_no_throw_java_built_nothing", oe::throws_observed() == 0);
            ctx.check("control_no_throw_java_last_kind_none", oe::last_throw_kind() == 0);
        }
        // The fixture's own run() builds no Throwable, so the watcher must record
        // no ISE/NFE from it.  (The JVM may construct unrelated internal
        // throwables on other threads; we only assert OUR typed counters stay 0.)
        ctx.check("control_no_new_ise_observed", g_primary_ise.load() == 0);
    }

    // =====================================================================
    //  6. Explicitly characterize the flag-reset defect on the handle itself:
    //     create one more fresh watcher and observe that its running() state is
    //     decoupled from whether it can ever fire.  We do NOT call shutdown_hooks()
    //     ourselves (it would tear down sibling modules' hooks mid-suite AND would
    //     not reset the latched flag anyway — that is the whole defect), so we
    //     characterize against the state an earlier shutdown_hooks() already left.
    // =====================================================================
    {
        auto fresh{ vmhook::on_exception(
            [](const std::string& /*name*/) { /* atomic-only, intentionally inert */ }) };
        const bool fresh_running{ fresh.running() };
        // running()==true here means on_exception believed the hook was installed.
        ctx.check("fresh_watch_handle_running_reflects_installed_flag", fresh_running);
        if (!trap_live && fresh_running)
        {
            ctx.record("[INFO] FLAW on_exception: even a brand-new on_exception() handle "
                       "reports running()==true after shutdown_hooks() has torn the "
                       "Throwable.fillInStackTrace hook down, because "
                       "detail::exception_hook_installed stays latched true forever.  "
                       "running() therefore cannot be used to detect a dead watcher.");
        }
        fresh.stop();
        ctx.check("fresh_watch_handle_stopped", fresh.running() == false);
    }

    // =====================================================================
    //  7. Tear down: the primary handle leaves scope here (RAII stop()).  No
    //     on_exception watcher remains armed for any later-running module.  We
    //     also explicitly stop() it first so the disarm is unmistakable in the
    //     log ordering, then confirm running()==false.
    // =====================================================================
    primary.stop();
    ctx.check("primary_watch_handle_stopped_at_end", primary.running() == false);
}
