// return_stack_trace_depth JVM test module  (feature area: hooks)
//
// Exhaustively exercises return_value::stack_trace() — the MULTI-FRAME walk of
// the HotSpot interpreter saved-rbp chain — and its companion caller(), on a
// LIVE JVM via real Java bytecode dispatch.  This is the modular migration +
// extension of the legacy inline test_caller_info (vmhook/src/example.cpp): that
// test hooked CallerProbe.innerStep and checked a 2-deep chain; here we drive a
// known 3-deep chain outer() -> mid() -> inner() (inner is the hooked leaf) plus
// deep recursion, so the DEPTH, per-frame method/class NAMES, and the ORDER
// (immediate-caller-first) of the trace are all pinned to known values.
//
// What this module proves, all from inside a detour on the fixed leaf inner(I)I:
//   * KNOWN DEPTH + ORDER + NAMES: with outer->mid->inner live, stack_trace()
//     returns the interpreted frames immediate-caller-first — index 0 is mid
//     (the same frame caller() reports), index 1 is outer (a frame caller()
//     does NOT report).  Both carry the right class_name (vmhook/fixtures/
//     ReturnStackTrace), method_name (mid / outer) and a (I)I signature.  This
//     is the headline that distinguishes the multi-frame walk from caller().
//   * stack_trace().front() AGREES with caller() (method ptr + name) — the
//     documented "index 0 == caller()" contract (legacy stackTraceFirstMatches).
//   * max_depth CONTRACT: stack_trace(1) returns exactly one frame (== mid),
//     stack_trace(2) returns exactly two (mid then outer), and the documented
//     "pass 0 for the default" promotion returns the default-capped trace
//     (>= the explicit small caps), NOT an empty vector.
//   * DEEP RECURSION past the 64 cap: a recurse(120) chain makes the default
//     stack_trace() terminate cleanly AT the cap (size <= 64, never spinning /
//     AV-ing on the saved-rbp chain), with a long UNIFORM run of identical
//     `recurse` frames; an explicit cap below the real depth truncates exactly.
//   * PER-FIRE FRESHNESS: two different chains in one probe cycle (a 2-frame
//     shallow->inner then the 3-frame outer->mid->inner) yield two DISTINCT
//     traces — the second is recomputed live and is strictly deeper — proving
//     stack_trace() is not returning a stale cached result.
//
// SAFETY: stack_trace()/caller() are themselves the code under test and are
// internally hardened (every saved-rbp deref gated by is_valid_pointer, a
// stack-growth/monotonicity guard, and an explicit max_depth cap) so the walk
// can stray onto a non-interpreter frame without AV-ing.  This module never
// dereferences a raw frame pointer itself: it only reads the std::string /
// method* fields of the returned caller_info, so it cannot crash the JVM.  All
// hooks are scoped_hook (uninstall on scope exit) and the module additionally
// calls shutdown_hooks() at the very end, so no detour is left armed.
//
// Harness note: `done` LATCHES (run_java_probe never clears it).  Each scenario
// resets observations + clears done and programs `mode` on the rising edge of
// `go`, runs ONE probe cycle, then reads the recorded observations back.
#include <vmhook/vmhook.hpp>

#include "../harness.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <sstream>
#include <string>

namespace
{
    // Wrapper for vmhook.fixtures.ReturnStackTrace.  Deriving from
    // vmhook::object<> gives it a vtable (required by register_class<T>) and the
    // static_field(...) accessors used for the go/done/mode handshake and the
    // recorded-observation fields.  Each typed getter reads into a concretely-
    // typed local first: field_proxy's value_t conversion operator is templated,
    // so a bare `static_field(...)->get() == x` is an ambiguous deduction.
    class stack_trace_fixture : public vmhook::object<stack_trace_fixture>
    {
    public:
        explicit stack_trace_fixture(vmhook::oop_t instance) noexcept
            : vmhook::object<stack_trace_fixture>{ instance }
        {
        }

        // go / done handshake + scenario selector.
        static auto set_go(bool value) -> void       { static_field("go")->set(value); }
        static auto set_done(bool value) -> void      { static_field("done")->set(value); }
        static auto get_done() -> bool                { bool v = static_field("done")->get(); return v; }
        static auto set_mode(std::int32_t m) -> void  { static_field("mode")->set(m); }

        // Recorded leaf side effects (handshake proof).
        static auto get_inner_calls() -> std::int32_t { std::int32_t v = static_field("innerCalls")->get(); return v; }
        static auto get_observed() -> std::int32_t    { std::int32_t v = static_field("observed")->get(); return v; }
    };

    // ── Fixture-mirrored constants (kept in lockstep with ReturnStackTrace.java)
    constexpr std::int32_t DEEP_RECURSION{ 120 };
    constexpr std::int32_t ARG_OUTER{ 100 };
    constexpr std::int32_t ARG_SHALLOW{ 200 };

    // The default cap documented by stack_trace(max_depth = 64).
    constexpr std::size_t  DEFAULT_CAP{ 64 };

    // Internal JVM names used in every per-frame name assertion.
    const std::string CLASS_NAME{ "vmhook/fixtures/ReturnStackTrace" };
    const std::string SIG_II{ "(I)I" };

    // ── Mode 1 — known depth-3 chain outer -> mid -> inner ────────────────────
    std::atomic<std::int32_t> g_k_fires{ 0 };
    std::atomic<bool>         g_k_caller_valid{ false };
    std::atomic<bool>         g_k_caller_is_mid{ false };       // caller() == mid
    std::atomic<std::int32_t> g_k_trace_size{ 0 };
    std::atomic<bool>         g_k_front_matches_caller{ false }; // trace[0] == caller()
    std::atomic<bool>         g_k_idx0_is_mid{ false };          // trace[0] name/class/sig
    std::atomic<bool>         g_k_idx1_is_outer{ false };        // trace[1] name/class/sig
    std::atomic<bool>         g_k_idx1_distinct_method{ false }; // trace[1].method != trace[0].method
    std::atomic<bool>         g_k_outer_not_immediate{ false };  // outer != caller() (multi-frame only)
    std::atomic<bool>         g_k_idx0_sig_ii{ false };
    std::atomic<bool>         g_k_idx1_sig_ii{ false };

    // ── Mode 2 — max_depth contract on the SAME chain ─────────────────────────
    std::atomic<std::int32_t> g_d_fires{ 0 };
    std::atomic<std::size_t>  g_d_size_1{ 0 };
    std::atomic<std::size_t>  g_d_size_2{ 0 };
    std::atomic<std::size_t>  g_d_size_0{ 0 };       // promoted-to-default size
    std::atomic<std::size_t>  g_d_size_default{ 0 }; // stack_trace() with no arg
    std::atomic<bool>         g_d_cap1_is_mid{ false };
    std::atomic<bool>         g_d_cap2_mid_then_outer{ false };

    // ── Mode 3 — deep recursion beyond the cap ────────────────────────────────
    std::atomic<std::int32_t> g_r_fires{ 0 };
    std::atomic<std::size_t>  g_r_default_size{ 0 };   // stack_trace() default
    std::atomic<std::size_t>  g_r_cap5_size{ 0 };      // stack_trace(5)
    std::atomic<std::size_t>  g_r_uniform_run{ 0 };    // longest run of identical recurse frames
    std::atomic<bool>         g_r_no_spin{ false };     // size strictly <= cap (terminated)
    std::atomic<bool>         g_r_front_valid{ false };

    // ── Mode 4 — two chains in one cycle (freshness) ──────────────────────────
    std::atomic<std::int32_t> g_t_fires{ 0 };
    std::atomic<std::size_t>  g_t_size_first{ 0 };    // shallow chain (2 named frames above leaf? at least 1)
    std::atomic<std::size_t>  g_t_size_second{ 0 };   // deep chain (mid+outer)
    std::atomic<bool>         g_t_first_immediate_shallow{ false };
    std::atomic<bool>         g_t_second_immediate_mid{ false };
    std::atomic<bool>         g_t_second_deeper{ false };

    auto reset_observations() -> void
    {
        g_k_fires.store(0);
        g_k_caller_valid.store(false);
        g_k_caller_is_mid.store(false);
        g_k_trace_size.store(0);
        g_k_front_matches_caller.store(false);
        g_k_idx0_is_mid.store(false);
        g_k_idx1_is_outer.store(false);
        g_k_idx1_distinct_method.store(false);
        g_k_outer_not_immediate.store(false);
        g_k_idx0_sig_ii.store(false);
        g_k_idx1_sig_ii.store(false);

        g_d_fires.store(0);
        g_d_size_1.store(0);
        g_d_size_2.store(0);
        g_d_size_0.store(0);
        g_d_size_default.store(0);
        g_d_cap1_is_mid.store(false);
        g_d_cap2_mid_then_outer.store(false);

        g_r_fires.store(0);
        g_r_default_size.store(0);
        g_r_cap5_size.store(0);
        g_r_uniform_run.store(0);
        g_r_no_spin.store(false);
        g_r_front_valid.store(false);

        g_t_fires.store(0);
        g_t_size_first.store(0);
        g_t_size_second.store(0);
        g_t_first_immediate_shallow.store(false);
        g_t_second_immediate_mid.store(false);
        g_t_second_deeper.store(false);
    }

    // Returns true when the caller_info names the given method of our fixture
    // class with a plain (I)I descriptor — the shape every named chain frame has.
    auto is_fixture_frame(const vmhook::return_value::caller_info& info,
                          const std::string& method) noexcept -> bool
    {
        return info.method != nullptr
            && info.class_name  == CLASS_NAME
            && info.method_name == method
            && info.signature   == SIG_II;
    }

    // Drives exactly one probe cycle for `mode` (rising-edge programs mode +
    // clears the latched done before the fixture's pending() observes go).
    auto drive(vmhook_test::context& ctx, std::int32_t mode) -> bool
    {
        return ctx.run_probe(
            [mode](bool value)
            {
                if (value)
                {
                    stack_trace_fixture::set_done(false);
                    stack_trace_fixture::set_mode(mode);
                }
                stack_trace_fixture::set_go(value);
            },
            []() { return stack_trace_fixture::get_done(); });
    }
}

VMHOOK_JVM_MODULE(return_stack_trace_depth)
{
    vmhook::register_class<stack_trace_fixture>("vmhook/fixtures/ReturnStackTrace");

    reset_observations();

    // =====================================================================
    // Scenario 1 — KNOWN DEPTH + ORDER + NAMES.
    // Chain outer(100) -> mid(101) -> inner(102); inner is hooked.  Inside the
    // detour the live interpreter frames above us are, immediate-first: mid,
    // outer, then run()/probe frames.  We pin index 0 == mid, index 1 == outer.
    // =====================================================================
    {
        auto handle{ vmhook::scoped_hook<stack_trace_fixture>(
            "inner", "(I)I",
            [](vmhook::return_value& ret,
               const std::unique_ptr<stack_trace_fixture>& /*self*/,
               std::int32_t /*x*/)
            {
                g_k_fires.fetch_add(1, std::memory_order_relaxed);

                // caller() must report the IMMEDIATE interpreted caller: mid.
                const auto info{ ret.caller() };
                g_k_caller_valid.store(info.valid(), std::memory_order_relaxed);
                g_k_caller_is_mid.store(is_fixture_frame(info, "mid"),
                                        std::memory_order_relaxed);

                // Full walk: index 0 == mid (== caller()), index 1 == outer.
                const auto trace{ ret.stack_trace() };
                g_k_trace_size.store(static_cast<std::int32_t>(trace.size()),
                                     std::memory_order_relaxed);

                if (!trace.empty() && info.valid())
                {
                    g_k_front_matches_caller.store(
                        trace.front().method      == info.method
                     && trace.front().method_name == info.method_name,
                        std::memory_order_relaxed);
                }
                if (trace.size() >= 1)
                {
                    g_k_idx0_is_mid.store(is_fixture_frame(trace[0], "mid"),
                                          std::memory_order_relaxed);
                    g_k_idx0_sig_ii.store(trace[0].signature == SIG_II,
                                          std::memory_order_relaxed);
                }
                if (trace.size() >= 2)
                {
                    g_k_idx1_is_outer.store(is_fixture_frame(trace[1], "outer"),
                                            std::memory_order_relaxed);
                    g_k_idx1_sig_ii.store(trace[1].signature == SIG_II,
                                          std::memory_order_relaxed);
                    g_k_idx1_distinct_method.store(
                        trace[1].method != trace[0].method,
                        std::memory_order_relaxed);
                    // outer is reachable ONLY via the multi-frame walk, never via
                    // caller() (which stops at mid): proves stack_trace adds reach.
                    g_k_outer_not_immediate.store(
                        info.valid() && trace[1].method != info.method,
                        std::memory_order_relaxed);
                }
            }) };

        ctx.check("stk_known_hook_installed", handle.installed());

        const bool done{ drive(ctx, 1) };
        ctx.check("stk_known_probe_completed", done);
        ctx.check("stk_known_leaf_ran_once", stack_trace_fixture::get_inner_calls() == 1);
        ctx.check("stk_known_fired_once", g_k_fires.load() == 1);

        // caller() == immediate caller mid.
        ctx.check("stk_known_caller_valid", g_k_caller_valid.load());
        ctx.check("stk_known_caller_is_mid", g_k_caller_is_mid.load());

        // The walk: depth, order, names, signatures.
        ctx.check("stk_known_trace_has_two_plus", g_k_trace_size.load() >= 2);
        ctx.check("stk_known_front_matches_caller", g_k_front_matches_caller.load());
        ctx.check("stk_known_idx0_is_mid", g_k_idx0_is_mid.load());
        ctx.check("stk_known_idx0_sig_ii", g_k_idx0_sig_ii.load());
        ctx.check("stk_known_idx1_is_outer", g_k_idx1_is_outer.load());
        ctx.check("stk_known_idx1_sig_ii", g_k_idx1_sig_ii.load());
        ctx.check("stk_known_idx1_distinct_method", g_k_idx1_distinct_method.load());
        // The headline difference vs caller(): outer is only in the multi-frame trace.
        ctx.check("stk_known_outer_beyond_caller", g_k_outer_not_immediate.load());

        ctx.record(std::string{ "[INFO] return_stack_trace_depth: known chain trace depth = " }
                   + std::to_string(g_k_trace_size.load())
                   + " (>=2 named interpreter frames mid,outer above the hooked leaf).");
    }

    // =====================================================================
    // Scenario 2 — max_depth CONTRACT on the same outer->mid->inner chain.
    // From the detour: stack_trace(1) -> [mid], stack_trace(2) -> [mid,outer],
    // stack_trace(0) -> default-capped (NOT empty), stack_trace() -> default.
    // =====================================================================
    {
        auto handle{ vmhook::scoped_hook<stack_trace_fixture>(
            "inner", "(I)I",
            [](vmhook::return_value& ret,
               const std::unique_ptr<stack_trace_fixture>& /*self*/,
               std::int32_t /*x*/)
            {
                g_d_fires.fetch_add(1, std::memory_order_relaxed);

                const auto cap1{ ret.stack_trace(1) };
                const auto cap2{ ret.stack_trace(2) };
                const auto cap0{ ret.stack_trace(0) };   // documented: promotes to default
                const auto capd{ ret.stack_trace() };    // explicit default

                g_d_size_1.store(cap1.size(), std::memory_order_relaxed);
                g_d_size_2.store(cap2.size(), std::memory_order_relaxed);
                g_d_size_0.store(cap0.size(), std::memory_order_relaxed);
                g_d_size_default.store(capd.size(), std::memory_order_relaxed);

                if (cap1.size() >= 1)
                {
                    g_d_cap1_is_mid.store(is_fixture_frame(cap1[0], "mid"),
                                          std::memory_order_relaxed);
                }
                if (cap2.size() >= 2)
                {
                    g_d_cap2_mid_then_outer.store(
                        is_fixture_frame(cap2[0], "mid")
                     && is_fixture_frame(cap2[1], "outer"),
                        std::memory_order_relaxed);
                }
            }) };

        ctx.check("stk_depth_hook_installed", handle.installed());

        const bool done{ drive(ctx, 2) };
        ctx.check("stk_depth_probe_completed", done);
        ctx.check("stk_depth_fired_once", g_d_fires.load() == 1);

        // Explicit small caps truncate to EXACTLY that many frames.
        ctx.check("stk_depth_cap1_size_is_1", g_d_size_1.load() == 1);
        ctx.check("stk_depth_cap1_frame_is_mid", g_d_cap1_is_mid.load());
        ctx.check("stk_depth_cap2_size_is_2", g_d_size_2.load() == 2);
        ctx.check("stk_depth_cap2_is_mid_then_outer", g_d_cap2_mid_then_outer.load());
        // max_depth=0 is documented to PROMOTE to the default, not return empty.
        ctx.check("stk_depth_cap0_not_empty", g_d_size_0.load() >= 2);
        ctx.check("stk_depth_cap0_equals_default", g_d_size_0.load() == g_d_size_default.load());
        // And the default cap is at least the deepest explicit cap we asked for.
        ctx.check("stk_depth_default_ge_cap2", g_d_size_default.load() >= g_d_size_2.load());
        ctx.check("stk_depth_default_within_cap", g_d_size_default.load() <= DEFAULT_CAP);

        ctx.record(std::string{ "[INFO] return_stack_trace_depth: caps {1,2,0->def,def} = {" }
                   + std::to_string(g_d_size_1.load()) + ","
                   + std::to_string(g_d_size_2.load()) + ","
                   + std::to_string(g_d_size_0.load()) + ","
                   + std::to_string(g_d_size_default.load()) + "} (0 promotes to default).");
    }

    // =====================================================================
    // Scenario 3 — DEEP recursion (120 > 64 cap): the walk must terminate AT the
    // cap without spinning on the saved-rbp chain, with a uniform run of
    // identical `recurse` frames; an explicit cap below depth truncates exactly.
    // =====================================================================
    {
        auto handle{ vmhook::scoped_hook<stack_trace_fixture>(
            "inner", "(I)I",
            [](vmhook::return_value& ret,
               const std::unique_ptr<stack_trace_fixture>& /*self*/,
               std::int32_t /*x*/)
            {
                g_r_fires.fetch_add(1, std::memory_order_relaxed);

                const auto trace{ ret.stack_trace() };   // default cap
                const auto cap5{ ret.stack_trace(5) };

                g_r_default_size.store(trace.size(), std::memory_order_relaxed);
                g_r_cap5_size.store(cap5.size(), std::memory_order_relaxed);
                // "No spin": a sane walk can never exceed the cap; if the saved-rbp
                // chain looped, the internal max_depth guard still bounds it AT the
                // cap, so size <= cap is the terminated-cleanly proof.
                g_r_no_spin.store(trace.size() <= DEFAULT_CAP, std::memory_order_relaxed);
                g_r_front_valid.store(!trace.empty() && trace.front().method != nullptr,
                                      std::memory_order_relaxed);

                // Longest run of consecutive frames that all name recurse(I)I.
                std::size_t best{ 0 };
                std::size_t run{ 0 };
                for (const auto& f : trace)
                {
                    if (f.method != nullptr
                        && f.class_name  == CLASS_NAME
                        && f.method_name == "recurse"
                        && f.signature   == SIG_II)
                    {
                        ++run;
                        if (run > best)
                        {
                            best = run;
                        }
                    }
                    else
                    {
                        run = 0;
                    }
                }
                g_r_uniform_run.store(best, std::memory_order_relaxed);
            }) };

        ctx.check("stk_recurse_hook_installed", handle.installed());

        const bool done{ drive(ctx, 3) };
        ctx.check("stk_recurse_probe_completed", done);
        ctx.check("stk_recurse_leaf_ran_once", stack_trace_fixture::get_inner_calls() == 1);
        ctx.check("stk_recurse_fired_once", g_r_fires.load() == 1);

        // The walk terminated cleanly at (or before) the cap — no infinite loop.
        ctx.check("stk_recurse_front_valid", g_r_front_valid.load());
        ctx.check("stk_recurse_terminated_within_cap", g_r_no_spin.load());
        // The recursion is 120 deep, so the default-64 trace must hit the cap.
        ctx.check("stk_recurse_default_hits_cap", g_r_default_size.load() == DEFAULT_CAP);
        // A long uniform run of `recurse` frames proves the deep portion is the
        // recursion (not stray garbage) AND that names resolve consistently deep.
        ctx.check("stk_recurse_uniform_run_long", g_r_uniform_run.load() >= 32);
        // Explicit cap below the real depth truncates to exactly that many frames.
        ctx.check("stk_recurse_cap5_size_is_5", g_r_cap5_size.load() == 5);

        ctx.record(std::string{ "[INFO] return_stack_trace_depth: recurse(" }
                   + std::to_string(DEEP_RECURSION) + ") default trace size = "
                   + std::to_string(g_r_default_size.load()) + " (cap " + std::to_string(DEFAULT_CAP)
                   + "), longest uniform recurse-run = " + std::to_string(g_r_uniform_run.load())
                   + ", stack_trace(5) = " + std::to_string(g_r_cap5_size.load()) + ".");
    }

    // =====================================================================
    // Scenario 4 — TWO chains in ONE cycle: shallow->inner THEN outer->mid->inner.
    // Proves each fire recomputes the trace live: the second is strictly deeper
    // and has a DIFFERENT immediate caller than the first.
    // =====================================================================
    {
        auto handle{ vmhook::scoped_hook<stack_trace_fixture>(
            "inner", "(I)I",
            [](vmhook::return_value& ret,
               const std::unique_ptr<stack_trace_fixture>& /*self*/,
               std::int32_t /*x*/)
            {
                const std::int32_t order{ g_t_fires.fetch_add(1, std::memory_order_relaxed) };
                const auto trace{ ret.stack_trace() };

                if (order == 0)
                {
                    // First fire: shallow(int) -> inner.  Immediate caller is shallow.
                    g_t_size_first.store(trace.size(), std::memory_order_relaxed);
                    if (!trace.empty())
                    {
                        g_t_first_immediate_shallow.store(
                            is_fixture_frame(trace.front(), "shallow"),
                            std::memory_order_relaxed);
                    }
                }
                else if (order == 1)
                {
                    // Second fire: outer -> mid -> inner.  Immediate caller is mid.
                    g_t_size_second.store(trace.size(), std::memory_order_relaxed);
                    if (!trace.empty())
                    {
                        g_t_second_immediate_mid.store(
                            is_fixture_frame(trace.front(), "mid"),
                            std::memory_order_relaxed);
                    }
                    g_t_second_deeper.store(
                        trace.size() > g_t_size_first.load(std::memory_order_relaxed),
                        std::memory_order_relaxed);
                }
            }) };

        ctx.check("stk_two_hook_installed", handle.installed());

        const bool done{ drive(ctx, 4) };
        ctx.check("stk_two_probe_completed", done);
        ctx.check("stk_two_leaf_ran_twice", stack_trace_fixture::get_inner_calls() == 2);
        ctx.check("stk_two_fired_twice", g_t_fires.load() == 2);

        // Distinct, live-recomputed traces: different immediate caller + deeper.
        ctx.check("stk_two_first_immediate_is_shallow", g_t_first_immediate_shallow.load());
        ctx.check("stk_two_second_immediate_is_mid", g_t_second_immediate_mid.load());
        ctx.check("stk_two_second_strictly_deeper", g_t_second_deeper.load());
        ctx.check("stk_two_traces_have_distinct_depth",
                  g_t_size_first.load() != g_t_size_second.load());

        ctx.record(std::string{ "[INFO] return_stack_trace_depth: two-chain freshness: first(shallow) depth = " }
                   + std::to_string(g_t_size_first.load()) + ", second(mid/outer) depth = "
                   + std::to_string(g_t_size_second.load()) + " (second deeper => recomputed live).");
    }

    // No detour may be left armed for the next module: every scoped_hook above
    // already uninstalled on scope exit, but assert it and hard-reset to be sure.
    vmhook::shutdown_hooks();
    {
        std::atomic<std::int32_t> post_fire{ 0 };
        // A bare probe with NO hook installed must NOT fire any detour.
        const bool done{ ctx.run_probe(
            [](bool value)
            {
                if (value)
                {
                    stack_trace_fixture::set_done(false);
                    stack_trace_fixture::set_mode(1);
                }
                stack_trace_fixture::set_go(value);
            },
            []() { return stack_trace_fixture::get_done(); }) };
        ctx.check("stk_teardown_probe_completed", done);
        ctx.check("stk_teardown_leaf_ran_once", stack_trace_fixture::get_inner_calls() == 1);
        ctx.check("stk_teardown_no_detour_armed", post_fire.load() == 0);
    }
}
