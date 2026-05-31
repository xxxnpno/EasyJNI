// for_each_thread JVM test module  (feature area: threads / HotSpot thread list)
//
// Exhaustively exercises vmhook::for_each_thread() against a LIVE JVM.
// for_each_thread (vmhook.hpp:6602) enumerates every live HotSpot JavaThread by
// walking, in order of availability:
//   * Path 1 — the classic intrusive Threads::_thread_list chain (JDK 8-9, and
//     later builds that still ship the VMStruct entry), de-duplicated through an
//     unordered_set and hard-capped at 4096 entries; AND
//   * Path 2 — the JDK 10+ Safe-Memory-Reclamation ThreadsList snapshot
//     (ThreadsSMRSupport::_java_thread_list), iterated [0, _length).
// The visitor receives a thread_info{ JavaThread*, state, os_thread_id } per
// live Java thread.  There is NO name in thread_info, so this module cannot
// match a spawned thread by name; it proves enumeration TRACKS a newly-created
// Java thread by an exact LIVE-COUNT/POINTER-SET DELTA instead (see Part E).
//
// This module MIGRATES + EXTENDS the legacy inline test_for_each_thread from
// vmhook/src/example.cpp (Rework D).  The legacy test asserted only four things
// (visited>=1, count<4096, saw a running/native state, saw current).  Here we
// additionally prove, angle by angle:
//   - every enumerated JavaThread* is non-null AND passes is_valid_pointer
//     (no bogus pointer ever reaches the visitor);
//   - the enumeration TERMINATES under a wall-clock bound (the audit flags a
//     legacy Path-1 _thread_list CYCLE hazard; we cannot forge a cycle on a live
//     JVM without corrupting it, so we CHARACTERISE the guarantee empirically:
//     bounded count + bounded time + no duplicate pointer observed);
//   - the visit count is "sane": >= 1 and strictly < the 4096 runaway cap;
//   - NO JavaThread* is reported twice in a single enumeration (Path 1 dedupes;
//     Path 2 does not — the audit's [LOW] "Path 2 no cycle detection" item — so
//     on a healthy JVM a duplicate here would surface that gap);
//   - every reported os_thread_id is non-zero (the OSThread chain decoded);
//   - REPEATED enumeration is STABLE in a quiescent window (same count, same
//     pointer set, current thread present both times) — proves no per-call state
//     corruption / no progressive drift;
//   - a freshly-spawned, parked, NAMED Java thread is OBSERVED by enumeration
//     (live count rises by >=1 and a brand-new valid JavaThread* appears) and
//     DISAPPEARS again once released (count drops back) — the executable proof
//     that for_each_thread reflects real thread lifecycle, not a stale snapshot;
//   - the same visitor body run twice with different capture shapes never
//     crashes and never hangs (cap + validity guards hold under all visitors).
//
// HARD SAFETY: every JavaThread deref is guarded by is_valid_pointer (mirroring
// for_each_thread's own invoke_visitor guard); the module never forces a cycle,
// never mutates the thread list, and bounds every poll loop so it can neither
// crash nor hang the JVM.  No hooks are installed (pure enumeration module), so
// there is nothing to tear down — scoped_hook is intentionally absent.
//
// Harness note: the worker-thread lifecycle is driven through the standard
// go/done + mode probe (mode 1 = start the named daemon worker and return while
// it parks alive; the worker self-times-out so an aborted run cannot leak it).
// The worker's "I am up" / "please stop" bridge flags are read/written via
// static_field(...)->get()/set() — plain heap accesses that work off the Java
// thread, no bytecode dispatch needed.
#include <vmhook/vmhook.hpp>

#include "../harness.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

namespace
{
    // Wrapper for vmhook.fixtures.ForEachThread.  Deriving from
    // vmhook::object<> gives the wrapper the static_field(...) accessors used to
    // drive the go/done/mode handshake and the worker lifecycle bridge flags.
    class fet_fixture : public vmhook::object<fet_fixture>
    {
    public:
        explicit fet_fixture(vmhook::oop_t instance) noexcept
            : vmhook::object<fet_fixture>{ instance }
        {
        }

        // ── go / done / mode handshake ───────────────────────────────────────
        static auto set_go(bool value) -> void    { static_field("go")->set(value); }
        static auto set_done(bool value) -> void  { static_field("done")->set(value); }
        static auto get_done() -> bool            { return static_field("done")->get(); }
        static auto set_mode(std::int32_t m) -> void { static_field("mode")->set(m); }

        // ── worker lifecycle bridge (plain heap reads/writes) ────────────────
        static auto get_thread_up() -> bool       { return static_field("threadUp")->get(); }
        static auto set_stop(bool value) -> void  { static_field("stop")->set(value); }
    };

    // The runaway cap inside for_each_thread (vmhook.hpp:6633).  A healthy JVM
    // must enumerate STRICTLY fewer than this; reaching it means either a
    // pathological thread count or a cycle that escaped detection.
    constexpr std::int32_t FOR_EACH_THREAD_CAP{ 4096 };

    // A snapshot of one for_each_thread enumeration: the visited pointers (in
    // visit order), plus derived health tallies.  Collected by enumerate().
    struct enumeration
    {
        std::vector<vmhook::hotspot::java_thread*> pointers;
        std::int32_t                               count{ 0 };
        bool                                       all_pointers_valid{ true };
        bool                                       all_os_tids_nonzero{ true };
        bool                                       any_running_or_native{ false };
        bool                                       any_state_out_of_range{ false };
        bool                                       saw_current{ false };
        double                                     elapsed_ms{ 0.0 };
    };

    // True if a state value is within the HotSpot java_thread_state enum range
    // (used to assert the visitor never hands back a garbage state byte).
    auto state_in_range(const vmhook::hotspot::java_thread_state s) -> bool
    {
        const auto v{ static_cast<std::int32_t>(s) };
        return v >= static_cast<std::int32_t>(vmhook::hotspot::java_thread_state::_thread_uninitialized)
            && v <= static_cast<std::int32_t>(vmhook::hotspot::java_thread_state::_thread_max_state);
    }

    // Runs ONE for_each_thread enumeration and folds every per-thread invariant
    // into an `enumeration`.  Each JavaThread deref inside the visitor is already
    // safe (for_each_thread only invokes the visitor for is_valid_pointer
    // threads), but we re-check the pointer here too so the module's own asserts
    // are self-contained and never deref a pointer the harness wouldn't.
    auto enumerate() -> enumeration
    {
        enumeration e{};
        const auto current_jt{ vmhook::hotspot::current_java_thread };

        const auto start{ std::chrono::steady_clock::now() };
        vmhook::for_each_thread([&](const vmhook::thread_info& info)
        {
            ++e.count;

            // Pointer validity — the load-bearing safety invariant.
            const bool ptr_ok{ info.thread != nullptr
                               && vmhook::hotspot::is_valid_pointer(info.thread) };
            if (!ptr_ok)
            {
                e.all_pointers_valid = false;
            }
            else
            {
                e.pointers.push_back(info.thread);
                if (info.thread == current_jt)
                {
                    e.saw_current = true;
                }
            }

            if (!state_in_range(info.state))
            {
                e.any_state_out_of_range = true;
            }
            if (info.state == vmhook::hotspot::java_thread_state::_thread_in_Java
             || info.state == vmhook::hotspot::java_thread_state::_thread_in_native)
            {
                e.any_running_or_native = true;
            }
            if (info.os_thread_id == 0)
            {
                e.all_os_tids_nonzero = false;
            }
        });
        const auto finish{ std::chrono::steady_clock::now() };
        e.elapsed_ms = std::chrono::duration<double, std::milli>{ finish - start }.count();
        return e;
    }

    // Count of DISTINCT pointers in a vector (Path 1 dedupes internally, but the
    // audit notes Path 2 does NOT — so a healthy JVM should still show
    // distinct == size; a mismatch surfaces a real duplicate-visit gap).
    auto distinct_count(const std::vector<vmhook::hotspot::java_thread*>& v) -> std::size_t
    {
        std::unordered_set<vmhook::hotspot::java_thread*> s{ v.begin(), v.end() };
        return s.size();
    }

    // Pointers present in `with` but not in `without` (the threads added between
    // two enumerations).  Used to detect the spawned worker's JavaThread.
    auto added_pointers(const std::vector<vmhook::hotspot::java_thread*>& with,
                        const std::vector<vmhook::hotspot::java_thread*>& without)
        -> std::vector<vmhook::hotspot::java_thread*>
    {
        const std::unordered_set<vmhook::hotspot::java_thread*> base{ without.begin(), without.end() };
        std::vector<vmhook::hotspot::java_thread*> added{};
        for (vmhook::hotspot::java_thread* const p : with)
        {
            if (base.find(p) == base.end())
            {
                added.push_back(p);
            }
        }
        return added;
    }

    // Same pointer multiset (order-independent) — STABILITY check for two
    // back-to-back enumerations in a quiescent window.
    auto same_pointer_set(const std::vector<vmhook::hotspot::java_thread*>& a,
                          const std::vector<vmhook::hotspot::java_thread*>& b) -> bool
    {
        return distinct_count(a) == distinct_count(b)
            && added_pointers(a, b).empty()
            && added_pointers(b, a).empty();
    }

    // Drives exactly one probe cycle for `mode`: clears the latched done and
    // programs the selector on the rising edge of go, then runs the probe.
    auto drive(vmhook_test::context& ctx, std::int32_t mode) -> bool
    {
        return ctx.run_probe(
            [mode](bool value)
            {
                if (value)
                {
                    fet_fixture::set_done(false);
                    fet_fixture::set_mode(mode);
                }
                fet_fixture::set_go(value);
            },
            []() { return fet_fixture::get_done(); });
    }

    // Bounded poll on a plain static-bool flag (heap read; no bytecode dispatch).
    // Returns true if `read()` reached `want` within ~`max_ms`.  Bounded so the
    // module can never hang the JVM even if the worker never flips the flag.
    template<typename read_fn>
    auto poll_flag(read_fn&& read, bool want, int max_ms) -> bool
    {
        for (int waited{ 0 }; waited < max_ms; ++waited)
        {
            if (read() == want)
            {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{ 1 });
        }
        return read() == want;
    }
}

VMHOOK_JVM_MODULE(for_each_thread)
{
    vmhook::register_class<fet_fixture>("vmhook/fixtures/ForEachThread");

    // =====================================================================
    // PART A — baseline enumeration: count sane, current thread present, at
    //          least one running/native thread (migrated from the legacy test).
    // =====================================================================
    const enumeration base{ enumerate() };

    ctx.record(std::string{ "[INFO] for_each_thread baseline: visited " }
               + std::to_string(base.count) + " JavaThread(s) in "
               + std::to_string(base.elapsed_ms) + " ms");

    // Legacy parity (was forEachThreadVisitedAtLeastOne): at least the
    // main/test thread is live, so enumeration MUST report >= 1.
    ctx.check("baseline_visited_at_least_one", base.count >= 1);

    // Legacy parity (was forEachThreadReasonableCount): the count is strictly
    // below the 4096 runaway cap on a healthy JVM.
    ctx.check("baseline_count_below_cap", base.count < FOR_EACH_THREAD_CAP);

    // Legacy parity (was forEachThreadSawRunningOrNative): some thread is
    // executing Java or native code (this test thread is at least one).
    ctx.check("baseline_saw_running_or_native", base.any_running_or_native);

    // Legacy parity (was forEachThreadSawCurrent, guarded on current_jt): if the
    // running thread has an identified JavaThread, enumeration must include it.
    if (vmhook::hotspot::current_java_thread)
    {
        ctx.check("baseline_saw_current_thread", base.saw_current);
    }
    else
    {
        ctx.record("[INFO] current_java_thread is null on this thread; "
                   "skipping baseline_saw_current_thread (parity with legacy guard).");
    }

    // =====================================================================
    // PART B — per-thread visitor invariants (NEW vs legacy): every reported
    //          JavaThread* is non-null + valid, every state is in range, every
    //          OS thread id decoded to a non-zero value.
    // =====================================================================
    ctx.check("baseline_all_pointers_valid", base.all_pointers_valid);
    ctx.check("baseline_no_state_out_of_range", !base.any_state_out_of_range);
    ctx.check("baseline_all_os_tids_nonzero", base.all_os_tids_nonzero);

    // The collected (valid) pointer vector length must equal the visit count
    // when every pointer was valid — i.e. no visit was silently dropped.
    if (base.all_pointers_valid)
    {
        ctx.check("baseline_collected_equals_count",
                  static_cast<std::int32_t>(base.pointers.size()) == base.count);
    }

    // =====================================================================
    // PART C — CYCLE / CAP CHARACTERISATION (audit: legacy _thread_list cycle).
    //   We cannot forge a corrupted intrusive list on a live JVM without
    //   crashing it, so we prove the GUARANTEES the cycle-guard provides,
    //   empirically: (1) enumeration TERMINATES (bounded wall-clock), (2) the
    //   count stays under the 4096 cap, and (3) NO JavaThread* is visited twice
    //   in one pass.  (3) also exercises the audit's Path-2-has-no-dedupe gap:
    //   on a healthy JVM a duplicate here would be a real defect.
    // =====================================================================

    // (1) Termination: a real infinite cycle would blow far past this; the
    //     4096-cap walk over a few dozen threads completes in well under 250 ms
    //     even on a slow CI box.  This is the executable "does not hang" proof.
    ctx.check("enumeration_terminates_bounded_time", base.elapsed_ms < 250.0);

    // (2) Cap is a guard, not a normal outcome.
    ctx.check("count_strictly_below_runaway_cap", base.count < FOR_EACH_THREAD_CAP);

    // (3) No duplicate pointer in a single enumeration (Path 1 dedupe holds; a
    //     duplicate would also reveal the Path-2-no-dedupe audit gap on JDK 10+).
    const std::size_t base_distinct{ distinct_count(base.pointers) };
    ctx.record(std::string{ "[INFO] baseline distinct pointers = " }
               + std::to_string(base_distinct) + " of "
               + std::to_string(base.pointers.size()) + " visited");
    ctx.check("no_duplicate_pointer_in_one_pass",
              base_distinct == base.pointers.size());

    // =====================================================================
    // PART D — REPEATED enumeration is STABLE in a quiescent window (NEW):
    //   two back-to-back passes agree on count + pointer set, and both include
    //   the current thread.  Proves no per-call state corruption / drift.
    // =====================================================================
    const enumeration again{ enumerate() };

    ctx.check("repeat_visited_at_least_one", again.count >= 1);
    ctx.check("repeat_all_pointers_valid", again.all_pointers_valid);
    ctx.check("repeat_terminates_bounded_time", again.elapsed_ms < 250.0);

    // Counts match in a quiescent window.  (Recorded either way; the JVM should
    // not be spinning up/down service threads while this module runs.)
    ctx.record(std::string{ "[INFO] repeat enumeration visited " }
               + std::to_string(again.count) + " (baseline was "
               + std::to_string(base.count) + ")");
    ctx.check("repeat_count_matches_baseline", again.count == base.count);

    // The exact pointer SET is identical across the two passes.
    ctx.check("repeat_pointer_set_stable",
              same_pointer_set(base.pointers, again.pointers));

    if (vmhook::hotspot::current_java_thread)
    {
        ctx.check("repeat_saw_current_thread", again.saw_current);
    }

    // =====================================================================
    // PART E — enumeration TRACKS a freshly-spawned NAMED Java thread (NEW):
    //   spawn a parked daemon worker, prove the live count rises and a brand-new
    //   valid JavaThread* appears, then release it and prove it disappears.
    //   This is the lifecycle proof the legacy test never made — and the closest
    //   portable analogue of "find the spawned thread in the enumeration", since
    //   thread_info carries no name.
    // =====================================================================
    {
        // Snapshot the count just before we ask the JVM for an extra thread.
        const enumeration before{ enumerate() };
        ctx.check("spawn_before_at_least_one", before.count >= 1);

        // mode 1: start the named daemon worker; the probe returns once the
        // worker is started, while the worker parks itself alive.
        fet_fixture::set_stop(false);
        const bool started{ drive(ctx, 1) };
        ctx.check("spawn_probe_started_worker", started);

        // Wait (bounded) for the worker to announce it is up & parked, so it is
        // certainly a fully-attached JavaThread before we re-enumerate.
        const bool up{ poll_flag(&fet_fixture::get_thread_up, true, 2000) };
        ctx.check("spawn_worker_reported_up", up);

        if (up)
        {
            // Re-enumerate WITH the worker parked alive.
            const enumeration with_worker{ enumerate() };
            ctx.check("spawn_with_worker_all_pointers_valid", with_worker.all_pointers_valid);

            ctx.record(std::string{ "[INFO] live count before=" }
                       + std::to_string(before.count) + " with_worker="
                       + std::to_string(with_worker.count));

            // The live count rose: at least the worker was added.
            ctx.check("spawn_live_count_increased",
                      with_worker.count >= before.count + 1);

            // A brand-new, valid JavaThread* appeared — that IS the worker
            // (modulo any unrelated concurrent JVM thread, which would only ADD
            // to this set, never empty it).
            const std::vector<vmhook::hotspot::java_thread*> added{
                added_pointers(with_worker.pointers, before.pointers) };
            ctx.record(std::string{ "[INFO] enumeration gained " }
                       + std::to_string(added.size()) + " new JavaThread pointer(s) while worker parked");
            ctx.check("spawn_new_pointer_appeared", !added.empty());

            const bool all_added_valid{ std::all_of(
                added.begin(), added.end(),
                [](vmhook::hotspot::java_thread* const p)
                { return p != nullptr && vmhook::hotspot::is_valid_pointer(p); }) };
            ctx.check("spawn_new_pointers_valid", all_added_valid);

            // Now release the worker and prove enumeration reflects its exit.
            fet_fixture::set_stop(true);
            const bool down{ poll_flag(&fet_fixture::get_thread_up, false, 5000) };
            ctx.check("spawn_worker_reported_down", down);

            if (down)
            {
                // Give the JVM a brief, BOUNDED moment to unwind the JavaThread
                // off the thread list after the Java run() method returns, then
                // re-enumerate.  We poll for the count to drop rather than
                // sleeping a fixed time.
                enumeration after{ enumerate() };
                for (int tries{ 0 };
                     tries < 200 && after.count >= with_worker.count;
                     ++tries)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds{ 5 });
                    after = enumerate();
                }

                ctx.record(std::string{ "[INFO] live count after release = " }
                           + std::to_string(after.count));
                ctx.check("spawn_after_all_pointers_valid", after.all_pointers_valid);

                // The live count dropped back from the with-worker peak: the
                // worker's JavaThread was reclaimed and is no longer enumerated.
                ctx.check("spawn_live_count_dropped",
                          after.count < with_worker.count);

                // None of the pointers that were ADDED for the worker phase
                // survive in a fresh enumeration (the worker is gone).
                const std::vector<vmhook::hotspot::java_thread*> still_present{
                    added_pointers(after.pointers, before.pointers) };
                const bool any_added_survived{ std::any_of(
                    added.begin(), added.end(),
                    [&](vmhook::hotspot::java_thread* const p)
                    {
                        return std::find(after.pointers.begin(), after.pointers.end(), p)
                            != after.pointers.end();
                    }) };
                ctx.record(std::string{ "[INFO] pointers added-vs-before still present after release = " }
                           + std::to_string(still_present.size()));
                ctx.check("spawn_worker_pointer_gone", !any_added_survived);
            }
        }
        else
        {
            // Worker never came up — still release the (possibly slow) thread so
            // we never leak it, and record the skip.
            fet_fixture::set_stop(true);
            ctx.record("[INFO] worker did not report up within budget; "
                       "skipped the spawn-delta assertions.");
        }
    }

    // =====================================================================
    // PART F — robustness: the same enumeration under different visitor capture
    //   shapes never crashes and never hangs (cap + validity guards hold).  An
    //   empty visitor (counts nothing) and a heavy-capture visitor both must
    //   return cleanly and bounded.
    // =====================================================================
    {
        // Empty visitor: pure walk, no observation — must simply return.
        const auto t0{ std::chrono::steady_clock::now() };
        vmhook::for_each_thread([](const vmhook::thread_info&) { /* no-op */ });
        const auto t1{ std::chrono::steady_clock::now() };
        const double empty_ms{ std::chrono::duration<double, std::milli>{ t1 - t0 }.count() };
        ctx.check("empty_visitor_returns_bounded", empty_ms < 250.0);

        // Heavy-capture visitor: accumulates into a local vector by value.
        std::vector<vmhook::os::thread_id_t> tids{};
        std::int32_t heavy_count{ 0 };
        const auto h0{ std::chrono::steady_clock::now() };
        vmhook::for_each_thread([&](const vmhook::thread_info& info)
        {
            ++heavy_count;
            if (info.thread != nullptr && vmhook::hotspot::is_valid_pointer(info.thread))
            {
                tids.push_back(info.os_thread_id);
            }
        });
        const auto h1{ std::chrono::steady_clock::now() };
        const double heavy_ms{ std::chrono::duration<double, std::milli>{ h1 - h0 }.count() };
        ctx.check("heavy_visitor_returns_bounded", heavy_ms < 250.0);
        ctx.check("heavy_visitor_visited_at_least_one", heavy_count >= 1);
        ctx.check("heavy_visitor_count_below_cap", heavy_count < FOR_EACH_THREAD_CAP);
    }
}
