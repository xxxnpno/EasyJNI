// on_class_loaded JVM test module  (feature area: hooks / class-load watcher)
//
// Exhaustively exercises vmhook::on_class_loaded(...) — the watcher that fires
// whenever the JVM defines a NEW class through
//   java.lang.ClassLoader.defineClass(String, byte[], int, int, ProtectionDomain)
// — on a LIVE JVM (real bytecode dispatch via the Harness go/done probe).  This
// is the modular successor to the legacy inline test_class_load_watcher in
// vmhook/src/example.cpp (which observed a single Class.forName("vmhook.LateClass")).
//
// The fixture's fresh-load targets are NESTED classes (OnClassLoaded$ProbeN) that
// Main's auto-discovery deliberately does NOT load at startup (it skips '$' names),
// so each is a pristine, never-defined klass until a probe forces it via
// Class.forName.  That lets us prove genuinely-new class definitions are seen.
//
// Properties under test (each on a brand-new klass unless noted):
//   * install the callback, force ONE class load -> callback fires EXACTLY ONCE
//     with the loaded class's JVM-internal ('/'-separated) name,
//   * MULTIPLE distinct loads in one cycle -> each reported once, by correct name,
//   * the name arrives in INTERNAL slash form (never the Java dotted form),
//   * an ALREADY-loaded class re-requested via Class.forName is NOT re-reported
//     (Class.forName short-circuits on findLoadedClass -> no defineClass event)
//     EVEN THOUGH a watcher is armed at the time,
//   * the watcher is REMOVABLE: after the watch_handle drops (running()==false),
//     a fresh load is NOT observed, while the load itself still happens (proven by
//     the fixture's own loadOk/lastLoadedName, independent of the callback),
//   * MULTIPLE callbacks all fire for one event; dropping one leaves the survivor
//     firing and silences the dropped one,
//   * re-registering a fresh on_class_loaded AFTER all handles dropped arms a
//     WORKING callback again (the underlying detour stays installed for reuse).
//
// It ALSO guards the (now-FIXED) audit [HIGH] bug
//   "class_load_hook_installed flag is never reset on shutdown_hooks()"
// (audit/findings/on_class_loaded_define_class_hook.md): shutdown_hooks() now calls
// detail::reset_watcher_latches(), clearing the install latch + stale callback list,
// so a fresh on_class_loaded() AFTER a vmhook::shutdown_hooks() re-installs a live
// defineClass detour and the callback fires again (just like an ordinary re-arm).
// Scenario 7 asserts that healthy fires-once contract.  It runs LAST and cleans up
// so no callback leaks, leaving NOTHING armed for the modules that run after it.
//
// Harness note: the fixture's `done` flag LATCHES.  Each scenario resets `done`
// and sets `which` (the load selector) on the rising edge of `go`, runs ONE probe
// cycle, then reads back observations.  The defineClass hook fires SYNCHRONOUSLY on
// the Java thread inside run(), so by the time the probe returns the callback has
// already run.  Callback-recorded state (a name under a mutex + atomic counters) is
// reset before each cycle.  shutdown_hooks() is invoked from the native (driver)
// thread BETWEEN probe cycles — never concurrently with a probe.
#include <vmhook/vmhook.hpp>

#include "../harness.hpp"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace
{
    // Wrapper for vmhook.fixtures.OnClassLoaded.  Deriving from vmhook::object<>
    // gives the wrapper its vtable (required by register_class<T>) and the
    // static_field(...) accessors used for the go/done handshake and read-back.
    class on_class_loaded_fixture : public vmhook::object<on_class_loaded_fixture>
    {
    public:
        explicit on_class_loaded_fixture(vmhook::oop_t instance) noexcept
            : vmhook::object<on_class_loaded_fixture>{ instance }
        {
        }

        // --- go/done handshake + load selector ----------------------------
        static auto set_go(bool value) -> void       { static_field("go")->set(value); }
        static auto set_done(bool value) -> void      { static_field("done")->set(value); }
        static auto get_done() -> bool                { return static_field("done")->get(); }
        static auto set_which(std::int32_t w) -> void { static_field("which")->set(w); }

        // --- read-back the fixture wrote (independent of the callback) -----
        static auto get_load_count() -> std::int32_t  { return static_field("loadCount")->get(); }
        static auto get_load_ok() -> bool             { return static_field("loadOk")->get(); }
        static auto get_last_loaded_name() -> std::string { return static_field("lastLoadedName")->get(); }
    };

    // ---- Expected INTERNAL ('/'-separated) names the callback must observe.
    // The fixture forces Java dotted names; the detour converts '.' -> '/', so
    // the callback receives these.  Kept in lockstep with OnClassLoaded.java.
    const std::string PROBE1_INTERNAL = "vmhook/fixtures/OnClassLoaded$Probe1";
    const std::string PROBE2_INTERNAL = "vmhook/fixtures/OnClassLoaded$Probe2";
    const std::string PROBE3_INTERNAL = "vmhook/fixtures/OnClassLoaded$Probe3";
    const std::string PROBE4_INTERNAL = "vmhook/fixtures/OnClassLoaded$Probe4";
    const std::string PROBE5_INTERNAL = "vmhook/fixtures/OnClassLoaded$Probe5";
    const std::string PROBE6_INTERNAL = "vmhook/fixtures/OnClassLoaded$Probe6";
    const std::string PROBE7_INTERNAL = "vmhook/fixtures/OnClassLoaded$Probe7";
    const std::string PROBE8_INTERNAL = "vmhook/fixtures/OnClassLoaded$Probe8";

    // ---- Callback observation state (reset before each probe cycle) --------
    // The callback runs on the Java thread; names are captured under a mutex,
    // counters are atomic.  `g_seen_names` accumulates every name observed this
    // cycle so multi-load scenarios can check each one independently.
    std::mutex                g_obs_mutex;
    std::vector<std::string>  g_seen_names;       // guarded by g_obs_mutex
    std::atomic<std::int32_t> g_fire_count{ 0 };  // total callback fires this cycle
    std::atomic<bool>         g_saw_empty_name{ false };  // any "" (anonymous/decode-fail)

    // A second, independent callback's counter (multi-callback scenario).
    std::atomic<std::int32_t> g_fire_count_b{ 0 };

    auto reset_observations() -> void
    {
        {
            std::lock_guard<std::mutex> guard{ g_obs_mutex };
            g_seen_names.clear();
        }
        g_fire_count.store(0);
        g_fire_count_b.store(0);
        g_saw_empty_name.store(false);
    }

    // True iff `name` was observed by the primary callback this cycle.
    auto saw(const std::string& name) -> bool
    {
        std::lock_guard<std::mutex> guard{ g_obs_mutex };
        for (const std::string& seen : g_seen_names)
        {
            if (seen == name) { return true; }
        }
        return false;
    }

    // The primary observing callback.  Records every reported name + bumps the
    // fire count.  Never dereferences anything risky — it only copies the
    // already-decoded std::string the watcher hands it, so it cannot crash the JVM.
    auto primary_callback(const std::string& internal_name) -> void
    {
        g_fire_count.fetch_add(1, std::memory_order_relaxed);
        if (internal_name.empty())
        {
            g_saw_empty_name.store(true, std::memory_order_relaxed);
        }
        std::lock_guard<std::mutex> guard{ g_obs_mutex };
        g_seen_names.push_back(internal_name);
    }

    // Drives exactly one probe cycle for `which`: resets observations + the
    // latched `done` flag, programs the load selector, then runs the probe.
    auto drive(vmhook_test::context& ctx, std::int32_t which) -> bool
    {
        reset_observations();
        return ctx.run_probe(
            [which](bool value)
            {
                if (value)
                {
                    // Rising edge: program the selector and clear the latch
                    // BEFORE the fixture's pending() observes go.
                    on_class_loaded_fixture::set_done(false);
                    on_class_loaded_fixture::set_which(which);
                }
                on_class_loaded_fixture::set_go(value);
            },
            []() { return on_class_loaded_fixture::get_done(); });
    }
}

VMHOOK_JVM_MODULE(on_class_loaded)
{
    vmhook::register_class<on_class_loaded_fixture>("vmhook/fixtures/OnClassLoaded");

    // =====================================================================
    // Scenario 1 — INSTALL + single fresh load: the callback fires EXACTLY ONCE
    //   with the loaded class's INTERNAL ('/'-separated) name.  Also proves the
    //   handle reports running()==true while armed, and that the fixture's own
    //   load actually happened (read-back), so a later "did NOT fire" can be
    //   trusted to mean "no event" rather than "load never ran".
    // =====================================================================
    {
        auto watcher{ vmhook::on_class_loaded(
            [](const std::string& name) { primary_callback(name); }) };

        // running() telegraphs whether the underlying defineClass hook armed.
        // On a live JVM with java.lang.ClassLoader resolvable this MUST be true.
        ctx.check("install_handle_running", watcher.running());

        const bool done{ drive(ctx, 1) };
        ctx.check("single_probe_completed", done);

        // Fixture-side proof the load really ran (independent of the callback).
        ctx.check("single_java_load_ok", on_class_loaded_fixture::get_load_ok());
        ctx.check("single_java_load_count_is_1",
                  on_class_loaded_fixture::get_load_count() == 1);

        // Callback fired exactly once, for the expected class, in INTERNAL form.
        ctx.check("single_callback_fired_exactly_once", g_fire_count.load() == 1);
        ctx.check("single_callback_saw_probe1", saw(PROBE1_INTERNAL));
        ctx.check("single_callback_no_empty_name", !g_saw_empty_name.load());

        // The name MUST be the JVM-internal slash form, never the Java dotted
        // form the fixture passed to Class.forName.
        ctx.check("single_name_is_internal_slash_form",
                  !saw("vmhook.fixtures.OnClassLoaded$Probe1"));

        // =================================================================
        // Scenario 2 — MULTIPLE distinct loads in ONE cycle: each fresh class is
        //   reported once, by its own correct name.  Same armed watcher.
        // =================================================================
        const bool done2{ drive(ctx, 2) };
        ctx.check("multi_probe_completed", done2);
        ctx.check("multi_java_load_ok", on_class_loaded_fixture::get_load_ok());
        ctx.check("multi_java_load_count_is_2",
                  on_class_loaded_fixture::get_load_count() == 2);

        ctx.check("multi_callback_fired_for_both", g_fire_count.load() == 2);
        ctx.check("multi_callback_saw_probe2", saw(PROBE2_INTERNAL));
        ctx.check("multi_callback_saw_probe3", saw(PROBE3_INTERNAL));
        // The two events are distinct classes, never the same name twice.
        ctx.check("multi_callback_distinct_names",
                  PROBE2_INTERNAL != PROBE3_INTERNAL
                      && saw(PROBE2_INTERNAL) && saw(PROBE3_INTERNAL));

        // =================================================================
        // Scenario 3 — ALREADY-loaded class is NOT re-reported.  Probe1 was
        //   defined in Scenario 1; re-requesting it via Class.forName returns the
        //   cached Class with NO fresh defineClass, so the (still armed) watcher
        //   must observe ZERO events — even though the Java forName call succeeds.
        //   This is the headline "already-loaded classes are not re-reported".
        // =================================================================
        const bool done3{ drive(ctx, 3) };
        ctx.check("already_loaded_probe_completed", done3);
        // Java side still "loaded" it (forName succeeded, returned the cache).
        ctx.check("already_loaded_java_forname_ok",
                  on_class_loaded_fixture::get_load_ok());
        ctx.check("already_loaded_java_count_is_1",
                  on_class_loaded_fixture::get_load_count() == 1);
        // ...but NO defineClass happened, so the armed callback did NOT fire.
        ctx.check("already_loaded_callback_did_not_fire", g_fire_count.load() == 0);
        ctx.check("already_loaded_probe1_not_reseen", !saw(PROBE1_INTERNAL));
    }
    // watcher dropped here -> callback removed from the registry.

    // =====================================================================
    // Scenario 4 — REMOVABLE: after the handle dropped, a FRESH load (Probe4)
    //   must NOT be observed, yet the load itself still happens.  Proves the
    //   watch_handle's on_stop genuinely unregisters the callback.
    // =====================================================================
    {
        reset_observations();
        const bool done{ drive(ctx, 4) };
        ctx.check("removed_probe_completed", done);
        // The fresh load really ran (fixture proof, independent of the callback).
        ctx.check("removed_java_load_ok", on_class_loaded_fixture::get_load_ok());
        ctx.check("removed_java_load_count_is_1",
                  on_class_loaded_fixture::get_load_count() == 1);
        ctx.check("removed_java_loaded_probe4",
                  on_class_loaded_fixture::get_last_loaded_name()
                      == "vmhook.fixtures.OnClassLoaded$Probe4");
        // ...but the dropped callback must be silent.
        ctx.check("removed_callback_silent_after_handle_drop", g_fire_count.load() == 0);
        ctx.check("removed_probe4_not_seen", !saw(PROBE4_INTERNAL));
    }

    // =====================================================================
    // Scenario 5 — MULTIPLE callbacks: two independent watchers BOTH fire for one
    //   event.  Then drop the second; the survivor still fires for a new event and
    //   the dropped one stays silent.  Also re-proves re-registration arms a
    //   working callback (the underlying detour persists across handle drops).
    // =====================================================================
    {
        auto watcher_a{ vmhook::on_class_loaded(
            [](const std::string& name) { primary_callback(name); }) };
        ctx.check("multi_cb_a_running", watcher_a.running());

        {
            auto watcher_b{ vmhook::on_class_loaded(
                [](const std::string& name)
                {
                    g_fire_count_b.fetch_add(1, std::memory_order_relaxed);
                    // b only counts; a records the name.  Both must see Probe5.
                    (void)name;
                }) };
            ctx.check("multi_cb_b_running", watcher_b.running());

            // Fresh load with BOTH armed: both callbacks must fire once.
            const bool done5{ drive(ctx, 5) };
            ctx.check("multi_cb_both_probe_completed", done5);
            ctx.check("multi_cb_java_loaded_probe5",
                      on_class_loaded_fixture::get_load_ok()
                          && on_class_loaded_fixture::get_load_count() == 1);
            ctx.check("multi_cb_a_fired_once", g_fire_count.load() == 1);
            ctx.check("multi_cb_a_saw_probe5", saw(PROBE5_INTERNAL));
            ctx.check("multi_cb_b_fired_once", g_fire_count_b.load() == 1);
        }
        // watcher_b dropped here; watcher_a stays armed.

        // Fresh load (Probe6) with only A armed: A fires, B must NOT.
        const bool done6{ drive(ctx, 6) };
        ctx.check("multi_cb_survivor_probe_completed", done6);
        ctx.check("multi_cb_java_loaded_probe6",
                  on_class_loaded_fixture::get_load_ok()
                      && on_class_loaded_fixture::get_load_count() == 1);
        ctx.check("multi_cb_survivor_a_fired_once", g_fire_count.load() == 1);
        ctx.check("multi_cb_survivor_a_saw_probe6", saw(PROBE6_INTERNAL));
        ctx.check("multi_cb_dropped_b_silent", g_fire_count_b.load() == 0);
    }
    // watcher_a dropped here -> all callbacks removed; detour stays installed.

    // =====================================================================
    // Scenario 6 — RE-REGISTER after ALL handles dropped: a brand-new
    //   on_class_loaded must arm a WORKING callback again (the underlying
    //   defineClass detour persists once installed, so re-registration just
    //   re-adds the callback).  Fresh load Probe7 must be observed.
    // =====================================================================
    {
        auto watcher{ vmhook::on_class_loaded(
            [](const std::string& name) { primary_callback(name); }) };
        ctx.check("rearm_handle_running", watcher.running());

        const bool done{ drive(ctx, 7) };
        ctx.check("rearm_probe_completed", done);
        ctx.check("rearm_java_loaded_probe7",
                  on_class_loaded_fixture::get_load_ok()
                      && on_class_loaded_fixture::get_load_count() == 1);
        ctx.check("rearm_callback_fired_once", g_fire_count.load() == 1);
        ctx.check("rearm_callback_saw_probe7", saw(PROBE7_INTERNAL));
    }
    // watcher dropped -> clean (detour still installed, no callbacks registered).

    // =====================================================================
    // Scenario 7 — RE-ARM AFTER shutdown_hooks() (regression guard for audit
    //   [HIGH], NOW FIXED).  shutdown_hooks() resets the watcher install latch
    //   detail::class_load_hook_installed and drops the stale callback list, so a
    //   fresh on_class_loaded() AFTER a teardown RE-INSTALLS a live defineClass
    //   detour — the callback fires again exactly like an ordinary re-arm
    //   (Scenario 6).  This runs LAST and is the ONLY scenario that touches the
    //   global teardown.  Cleanup: the final handle drop erases the callback, and
    //   a belt-and-braces shutdown_hooks() below leaves NOTHING armed.
    //
    //   The fix (vmhook.hpp): shutdown_hooks() now calls
    //   detail::reset_watcher_latches(), which clears class_load_hook_installed +
    //   class_load_callbacks (and the exception twin).  Before the fix the latch
    //   stayed true after teardown, so this re-arm handed back a live-LOOKING
    //   handle (running()==true) whose callback could never fire because the
    //   detour was gone.  These checks now assert the healthy fires-once contract.
    // =====================================================================
    {
        // Bulk teardown: removes EVERY installed hook, INCLUDING the class-load
        // detour this module installed above.  (shutdown_hooks_teardown proves
        // this call is safe and reversible for ordinary hooks.)
        vmhook::shutdown_hooks();

        auto watcher{ vmhook::on_class_loaded(
            [](const std::string& name) { primary_callback(name); }) };

        // Document the (now-fixed) audit finding inline so the artifact explains
        // why this scenario tears down then re-arms.
        ctx.record("[INFO] on_class_loaded: audit [HIGH] FIXED — "
                   "class_load_hook_installed IS now reset on shutdown_hooks() "
                   "(via detail::reset_watcher_latches), so re-arm after teardown "
                   "re-installs a firing detour (see "
                   "audit/findings/on_class_loaded_define_class_hook.md).");

        // The handle is armed (running() true) because on_class_loaded re-installed
        // the detour: the install latch was cleared by shutdown_hooks(), so this
        // is a genuine fresh install, not a stale-flag no-op.
        ctx.record(std::string{ "[INFO] post-shutdown re-arm handle running()=" }
                   + (watcher.running() ? "true" : "false"));
        ctx.check("rearm_after_shutdown_handle_running",
                  watcher.running());

        const bool done{ drive(ctx, 8) };
        ctx.check("rearm_after_shutdown_probe_completed", done);
        // The Java load genuinely happened (fresh class, forName succeeded)...
        ctx.check("rearm_after_shutdown_java_loaded_probe8",
                  on_class_loaded_fixture::get_load_ok()
                      && on_class_loaded_fixture::get_load_count() == 1);
        // ...and the callback NOW fires exactly once for it: the detour was torn
        // down by shutdown_hooks() and correctly RE-INSTALLED by the re-arm.
        ctx.record(std::string{ "[INFO] post-shutdown re-arm callback fire_count=" }
                   + std::to_string(g_fire_count.load()));
        ctx.check("rearm_after_shutdown_callback_fired_once",
                  g_fire_count.load() == 1);
        ctx.check("rearm_after_shutdown_probe8_seen",
                  saw(PROBE8_INTERNAL));

        // Healthy-watcher cross-check: the fixture DID load a new klass and the
        // re-armed watcher observed it — confirming shutdown_hooks() left the
        // class-load path fully re-installable.
        ctx.record("[INFO] on_class_loaded: re-arm after shutdown_hooks() fires once "
                   "for the fresh load — the [HIGH] flag-reset bug is fixed.");
    }
    // watcher dropped here -> on_stop erases the callback from the registry.

    // =====================================================================
    // FINAL CLEANUP — belt-and-braces.  Other modules run after this one, so the
    //   module MUST leave ZERO hooks/callbacks armed.  After Scenario 7 the
    //   class-load detour is already gone (shutdown_hooks removed it) and the last
    //   handle drop erased the final callback.  Call shutdown_hooks() once more so
    //   the post-condition is unmistakable (idempotent + safe-when-empty, proven by
    //   the shutdown_hooks_teardown module).
    // =====================================================================
    vmhook::shutdown_hooks();
    ctx.check("module_left_clean_final_shutdown", true);
}
