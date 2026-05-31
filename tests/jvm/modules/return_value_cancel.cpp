// return_value_cancel JVM test module — exhaustively exercises
// vmhook::return_value::cancel() (the force-CANCEL path) on a LIVE JVM.
//
// Feature under test: vmhook/ext/vmhook/vmhook.hpp:1205-1209.
//   cancel() is a 3-line setter that flips return_slot::cancel = true WITHOUT
//   touching return_slot::retval.  When the trampoline takes the cancel path it
//   unconditionally loads the (zero-initialised) retval cell at [rsp+8] into BOTH
//   rax (integer return) and xmm0 (float/double return) — regardless of the Java
//   method's return descriptor (vmhook.hpp:5371-5372 Win64 / 5468-5469 SysV).
//   Consequences this module locks in on a real JVM:
//     * void method            -> original body is SKIPPED (no side effect),
//     * int / long returner     -> Java caller receives 0 / 0L,
//     * double returner         -> Java caller receives +0.0 (NOT NaN, sign +),
//     * boolean returner        -> Java caller receives false,
//     * char returner           -> Java caller receives U+0000,
//     * reference returner       -> Java caller receives null.
//   These are exactly the "silent footgun on non-void methods" + the zero-fill /
//   xmm0 / reference-null scenarios the audit finding raises
//   (audit/findings/return_value_cancel.md, the [jvm_integration] [new] cases).
//
// Strategy mirrors return_set_primitives.cpp / pilot.cpp: the fixture is a dumb
// actor (ReturnValueCancel.java) that, per probe, calls each orig* method and
// records what the Java caller OBSERVED.  Each orig* returns a fixed non-zero /
// non-null value the native side NEVER forces, so an observed 0 / null / +0.0
// can only mean the cancel path delivered it.  Every round installs fresh
// scoped_hooks in a nested block (never shutdown_hooks): they disarm at block
// end so the next round installs clean, which also lets us re-prove the cancel
// path is stable across repeated arm/disarm cycles.
#include <vmhook/vmhook.hpp>

#include "../harness.hpp"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>

namespace
{
    // Wrapper for vmhook.fixtures.ReturnValueCancel.
    class rvc_fixture : public vmhook::object<rvc_fixture>
    {
    public:
        explicit rvc_fixture(vmhook::oop_t instance) noexcept
            : vmhook::object<rvc_fixture>{ instance }
        {
        }

        // ── go/done handshake + mode selector ──────────────────────────────
        static auto set_go(bool value)   -> void { static_field("go")->set(value); }
        static auto get_done()           -> bool { return static_field("done")->get(); }
        static auto set_done(bool value) -> void { static_field("done")->set(value); }
        static auto set_mode(std::int32_t m) -> void { static_field("mode")->set(m); }

        // ── observed return values (instance dispatch) ─────────────────────
        static auto obs_int()    -> std::int32_t { return static_field("obsInt")->get(); }
        static auto obs_long()   -> std::int64_t { return static_field("obsLong")->get(); }
        static auto obs_double() -> double        { return static_field("obsDouble")->get(); }
        static auto obs_bool()   -> bool          { return static_field("obsBool")->get(); }
        static auto obs_char()   -> std::uint16_t { return static_field("obsChar")->get(); }
        static auto obs_ref_is_null()  -> bool          { return static_field("obsRefIsNull")->get(); }
        static auto obs_ref_identity() -> std::int32_t  { return static_field("obsRefIdentity")->get(); }
        static auto obs_double_was_nan()      -> bool { return static_field("obsDoubleWasNaN")->get(); }
        static auto obs_double_was_neg_zero() -> bool { return static_field("obsDoubleWasNegZero")->get(); }

        // ── observed return values (static dispatch) ───────────────────────
        static auto obs_static_int()    -> std::int32_t { return static_field("obsStaticInt")->get(); }
        static auto obs_static_double() -> double        { return static_field("obsStaticDouble")->get(); }

        // ── void side-effect witnesses ─────────────────────────────────────
        static auto side_effect()        -> std::int32_t { return static_field("sideEffect")->get(); }
        static auto static_side_effect() -> std::int32_t { return static_field("staticSideEffect")->get(); }
        static auto side_effect_after_call1() -> std::int32_t { return static_field("sideEffectAfterCall1")->get(); }
        static auto side_effect_after_call2() -> std::int32_t { return static_field("sideEffectAfterCall2")->get(); }

        // ── control ────────────────────────────────────────────────────────
        static auto saw_exception() -> bool         { return static_field("sawException")->get(); }
        static auto round_count()   -> std::int32_t { return static_field("roundCount")->get(); }
    };

    // Probe-mode selectors — MUST match the MODE_* constants in
    // ReturnValueCancel.java.  The native side writes `mode` before raising
    // `go`; the fixture's action branches on it.
    struct mode_consts
    {
        static constexpr std::int32_t observe_all{ 0 };
        static constexpr std::int32_t void_twice{ 1 };
    };

    // +0.0 is delivered as all-zero bits; -0.0 has the sign bit set.  Bit-exact
    // so we can prove the cancel path produced a TRUE positive zero (not -0.0,
    // not a NaN with a zero-ish payload).
    auto is_positive_zero(double value) -> bool
    {
        std::uint64_t bits{};
        std::memcpy(&bits, &value, sizeof(bits));
        return bits == 0u;
    }

    // ── per-round detour bookkeeping (reset before each probe) ──────────────
    std::atomic<int>  g_inst_fires{ 0 };
    std::atomic<int>  g_stat_fires{ 0 };
    std::atomic<bool> g_inst_all_saw_self{ true };

    auto reset_counters() -> void
    {
        g_inst_fires.store(0, std::memory_order_relaxed);
        g_stat_fires.store(0, std::memory_order_relaxed);
        g_inst_all_saw_self.store(true, std::memory_order_relaxed);
    }

    auto note_inst(const std::unique_ptr<rvc_fixture>& self) -> void
    {
        g_inst_fires.fetch_add(1, std::memory_order_relaxed);
        if (self == nullptr)
        {
            g_inst_all_saw_self.store(false, std::memory_order_relaxed);
        }
    }

    auto note_static() -> void
    {
        g_stat_fires.fetch_add(1, std::memory_order_relaxed);
    }

    // Drive ONE MODE_OBSERVE_ALL probe and return whether it completed.
    auto run_observe_probe(vmhook_test::context& ctx, const std::string& tag) -> bool
    {
        rvc_fixture::set_mode(mode_consts::observe_all);
        rvc_fixture::set_done(false);
        const bool done{ ctx.run_probe(
            [](bool value) { rvc_fixture::set_go(value); },
            []() { return rvc_fixture::get_done(); }) };
        ctx.check(tag + "_probe_completed", done);
        return done;
    }
}

VMHOOK_JVM_MODULE(return_value_cancel)
{
    vmhook::register_class<rvc_fixture>("vmhook/fixtures/ReturnValueCancel");

    // ===================================================================
    // ROUND 0 — BASELINE: no hooks installed at all.  Every orig* body
    // runs and its non-zero / non-null value flows to the Java caller.
    // This is the control: it proves the later "observed 0/null/+0.0"
    // results are caused by cancel(), not by some ambient effect, and
    // that the fixture's own plumbing reports the original values.
    // ===================================================================
    {
        reset_counters();
        rvc_fixture::set_mode(mode_consts::observe_all);
        rvc_fixture::set_done(false);
        const bool done{ ctx.run_probe(
            [](bool value) { rvc_fixture::set_go(value); },
            []() { return rvc_fixture::get_done(); }) };
        ctx.check("baseline_probe_completed", done);
        if (done)
        {
            ctx.check("baseline_no_hook_fired",
                      g_inst_fires.load() == 0 && g_stat_fires.load() == 0);
            ctx.check("baseline_no_java_exception", !rvc_fixture::saw_exception());
            // Original values flow unchanged.
            ctx.check("baseline_int_is_1111",    rvc_fixture::obs_int()  == 1111);
            ctx.check("baseline_long_is_orig",   rvc_fixture::obs_long() == static_cast<std::int64_t>(0x7FFFFFFF00000001LL));
            ctx.check("baseline_double_is_11_25",rvc_fixture::obs_double() == 11.25);
            ctx.check("baseline_bool_is_true",   rvc_fixture::obs_bool() == true);
            ctx.check("baseline_char_is_A",      rvc_fixture::obs_char() == static_cast<std::uint16_t>('A'));
            ctx.check("baseline_ref_not_null",   !rvc_fixture::obs_ref_is_null());
            ctx.check("baseline_ref_identity_nonzero", rvc_fixture::obs_ref_identity() != 0);
            ctx.check("baseline_static_int_2222",rvc_fixture::obs_static_int() == 2222);
            ctx.check("baseline_static_double_22_25", rvc_fixture::obs_static_double() == 22.25);
            // Void bodies ran: their side effects advanced.
            ctx.check("baseline_void_side_effect_ran",        rvc_fixture::side_effect() == 7);
            ctx.check("baseline_static_void_side_effect_ran", rvc_fixture::static_side_effect() == 13);
        }
    }

    // Snapshots of the void side-effect counters AFTER the baseline ran, so the
    // cancel rounds can assert "did NOT advance" relative to this point.
    const std::int32_t side_effect_after_baseline{ rvc_fixture::side_effect() };
    const std::int32_t static_side_effect_after_baseline{ rvc_fixture::static_side_effect() };

    // ===================================================================
    // ROUND 1 — CANCEL-WITHOUT-SET on every method.  Each detour calls
    // ONLY retval.cancel() (never set()).  This is the heart of the
    // feature: the trampoline must skip every original body and deliver
    // the zero-filled slot as 0 / 0L / +0.0 / false / U+0000 / null.
    // ===================================================================
    {
        reset_counters();

        // INSTANCE hooks — cancel only.
        auto h_void  { vmhook::scoped_hook<rvc_fixture>("origVoid",
            [](vmhook::return_value& r, const std::unique_ptr<rvc_fixture>& self)
            { note_inst(self); r.cancel(); }) };
        auto h_int   { vmhook::scoped_hook<rvc_fixture>("origInt",
            [](vmhook::return_value& r, const std::unique_ptr<rvc_fixture>& self)
            { note_inst(self); r.cancel(); }) };
        auto h_long  { vmhook::scoped_hook<rvc_fixture>("origLong",
            [](vmhook::return_value& r, const std::unique_ptr<rvc_fixture>& self)
            { note_inst(self); r.cancel(); }) };
        auto h_double{ vmhook::scoped_hook<rvc_fixture>("origDouble",
            [](vmhook::return_value& r, const std::unique_ptr<rvc_fixture>& self)
            { note_inst(self); r.cancel(); }) };
        auto h_bool  { vmhook::scoped_hook<rvc_fixture>("origBool",
            [](vmhook::return_value& r, const std::unique_ptr<rvc_fixture>& self)
            { note_inst(self); r.cancel(); }) };
        auto h_char  { vmhook::scoped_hook<rvc_fixture>("origChar",
            [](vmhook::return_value& r, const std::unique_ptr<rvc_fixture>& self)
            { note_inst(self); r.cancel(); }) };
        auto h_ref   { vmhook::scoped_hook<rvc_fixture>("origRef",
            [](vmhook::return_value& r, const std::unique_ptr<rvc_fixture>& self)
            { note_inst(self); r.cancel(); }) };

        // STATIC hooks — cancel only (no 'this').
        auto hs_void  { vmhook::scoped_hook<rvc_fixture>("origStaticVoid",
            [](vmhook::return_value& r) { note_static(); r.cancel(); }) };
        auto hs_int   { vmhook::scoped_hook<rvc_fixture>("origStaticInt",
            [](vmhook::return_value& r) { note_static(); r.cancel(); }) };
        auto hs_double{ vmhook::scoped_hook<rvc_fixture>("origStaticDouble",
            [](vmhook::return_value& r) { note_static(); r.cancel(); }) };

        const bool all_installed{
            h_void.installed()  && h_int.installed()  && h_long.installed()  &&
            h_double.installed()&& h_bool.installed() && h_char.installed()  &&
            h_ref.installed()   &&
            hs_void.installed() && hs_int.installed() && hs_double.installed() };
        ctx.check("cancel_all_10_hooks_installed", all_installed);

        if (run_observe_probe(ctx, "cancel"))
        {
            // Every detour fired exactly once.
            ctx.check("cancel_instance_hooks_fired_7", g_inst_fires.load() == 7);
            ctx.check("cancel_static_hooks_fired_3",   g_stat_fires.load() == 3);
            ctx.check("cancel_instance_hooks_saw_self", g_inst_all_saw_self.load());
            ctx.check("cancel_no_java_exception",       !rvc_fixture::saw_exception());

            // VOID: original body SKIPPED — side effect did NOT advance past
            // the baseline snapshot.
            ctx.check("cancel_void_body_skipped",
                      rvc_fixture::side_effect() == side_effect_after_baseline);
            ctx.check("cancel_static_void_body_skipped",
                      rvc_fixture::static_side_effect() == static_side_effect_after_baseline);

            // NON-VOID instance: the documented zero-fill fallback.
            ctx.check("cancel_int_returns_zero",     rvc_fixture::obs_int()  == 0);
            ctx.check("cancel_long_returns_zero",    rvc_fixture::obs_long() == 0);
            ctx.check("cancel_bool_returns_false",   rvc_fixture::obs_bool() == false);
            ctx.check("cancel_char_returns_nul",     rvc_fixture::obs_char() == 0);
            // double: +0.0 via the movq xmm0 epilogue — NOT NaN, sign bit clear.
            ctx.check("cancel_double_returns_zero",        rvc_fixture::obs_double() == 0.0);
            ctx.check("cancel_double_is_positive_zero",    is_positive_zero(rvc_fixture::obs_double()));
            ctx.check("cancel_double_not_nan",             !rvc_fixture::obs_double_was_nan());
            ctx.check("cancel_double_not_neg_zero",        !rvc_fixture::obs_double_was_neg_zero());
            // reference: null.
            ctx.check("cancel_ref_returns_null",     rvc_fixture::obs_ref_is_null());
            ctx.check("cancel_ref_identity_zero",    rvc_fixture::obs_ref_identity() == 0);

            // NON-VOID static: same zero-fill on the static dispatch path.
            ctx.check("cancel_static_int_returns_zero",    rvc_fixture::obs_static_int() == 0);
            ctx.check("cancel_static_double_returns_zero", rvc_fixture::obs_static_double() == 0.0);
            ctx.check("cancel_static_double_is_positive_zero",
                      is_positive_zero(rvc_fixture::obs_static_double()));
        }
    }

    // ===================================================================
    // ROUND 2 — ALLOW-THROUGH: hooks installed on the SAME methods but the
    // detour does NOT call cancel() (or set()).  The original body must run
    // and its value flows unchanged.  This isolates cancel() as the cause of
    // round 1's suppression: same hooks, only the cancel() call removed.
    // ===================================================================
    {
        reset_counters();
        const std::int32_t side_effect_before{ rvc_fixture::side_effect() };
        const std::int32_t static_side_effect_before{ rvc_fixture::static_side_effect() };

        auto h_void  { vmhook::scoped_hook<rvc_fixture>("origVoid",
            [](vmhook::return_value&, const std::unique_ptr<rvc_fixture>& self) { note_inst(self); }) };
        auto h_int   { vmhook::scoped_hook<rvc_fixture>("origInt",
            [](vmhook::return_value&, const std::unique_ptr<rvc_fixture>& self) { note_inst(self); }) };
        auto h_double{ vmhook::scoped_hook<rvc_fixture>("origDouble",
            [](vmhook::return_value&, const std::unique_ptr<rvc_fixture>& self) { note_inst(self); }) };
        auto h_ref   { vmhook::scoped_hook<rvc_fixture>("origRef",
            [](vmhook::return_value&, const std::unique_ptr<rvc_fixture>& self) { note_inst(self); }) };
        auto hs_int  { vmhook::scoped_hook<rvc_fixture>("origStaticInt",
            [](vmhook::return_value&) { note_static(); }) };

        const bool all_installed{
            h_void.installed() && h_int.installed() && h_double.installed() &&
            h_ref.installed()  && hs_int.installed() };
        ctx.check("allow_hooks_installed", all_installed);

        if (run_observe_probe(ctx, "allow"))
        {
            ctx.check("allow_instance_hooks_fired", g_inst_fires.load() >= 4);
            ctx.check("allow_static_hook_fired",    g_stat_fires.load() >= 1);
            ctx.check("allow_no_java_exception",    !rvc_fixture::saw_exception());

            // Original bodies ran despite the hook being present.
            ctx.check("allow_void_body_ran",        rvc_fixture::side_effect() == side_effect_before + 7);
            ctx.check("allow_static_void_body_ran", rvc_fixture::static_side_effect() == static_side_effect_before + 13);
            ctx.check("allow_int_is_original",      rvc_fixture::obs_int()  == 1111);
            ctx.check("allow_double_is_original",   rvc_fixture::obs_double() == 11.25);
            ctx.check("allow_ref_not_null",         !rvc_fixture::obs_ref_is_null());
            ctx.check("allow_static_int_is_original", rvc_fixture::obs_static_int() == 2222);
        }
    }

    // ===================================================================
    // ROUND 3 — CANCEL + SET, "cancel THEN set" order.  cancel() flips the
    // flag; set() then writes a real value over the (still-cancelled) slot.
    // Documents the "last write wins" contract: the Java caller sees the
    // SET value, not the zero-fill.  (audit standalone case
    // return_value_cancel_then_set_overrides, proven here end-to-end.)
    // ===================================================================
    {
        reset_counters();
        auto h_int   { vmhook::scoped_hook<rvc_fixture>("origInt",
            [](vmhook::return_value& r, const std::unique_ptr<rvc_fixture>& self)
            { note_inst(self); r.cancel(); r.set(static_cast<std::int32_t>(42)); }) };
        auto h_double{ vmhook::scoped_hook<rvc_fixture>("origDouble",
            [](vmhook::return_value& r, const std::unique_ptr<rvc_fixture>& self)
            { note_inst(self); r.cancel(); r.set(2.5); }) };
        auto hs_int  { vmhook::scoped_hook<rvc_fixture>("origStaticInt",
            [](vmhook::return_value& r) { note_static(); r.cancel(); r.set(static_cast<std::int32_t>(4242)); }) };

        ctx.check("cancel_then_set_hooks_installed",
                  h_int.installed() && h_double.installed() && hs_int.installed());

        if (run_observe_probe(ctx, "cancel_then_set"))
        {
            ctx.check("cancel_then_set_no_java_exception", !rvc_fixture::saw_exception());
            // set() AFTER cancel() wins: the forced value is delivered.
            ctx.check("cancel_then_set_int_is_42",        rvc_fixture::obs_int()  == 42);
            ctx.check("cancel_then_set_double_is_2_5",    rvc_fixture::obs_double() == 2.5);
            ctx.check("cancel_then_set_static_int_is_4242", rvc_fixture::obs_static_int() == 4242);
        }
    }

    // ===================================================================
    // ROUND 4 — SET + CANCEL, "set THEN cancel" order.  set() writes the
    // value AND raises cancel; the subsequent cancel() must NOT zero the
    // retval that set() stored.  This is the subtle case: a user who set()s
    // early then conditionally cancel()s expects the value to SURVIVE.
    // (audit standalone case return_value_set_then_cancel_preserves_value.)
    // ===================================================================
    {
        reset_counters();
        auto h_int   { vmhook::scoped_hook<rvc_fixture>("origInt",
            [](vmhook::return_value& r, const std::unique_ptr<rvc_fixture>& self)
            { note_inst(self); r.set(static_cast<std::int32_t>(77)); r.cancel(); }) };
        auto h_double{ vmhook::scoped_hook<rvc_fixture>("origDouble",
            [](vmhook::return_value& r, const std::unique_ptr<rvc_fixture>& self)
            { note_inst(self); r.set(-3.5); r.cancel(); }) };
        auto h_long  { vmhook::scoped_hook<rvc_fixture>("origLong",
            [](vmhook::return_value& r, const std::unique_ptr<rvc_fixture>& self)
            { note_inst(self); r.set(static_cast<std::int64_t>(0x0123456789ABCDEFLL)); r.cancel(); }) };

        ctx.check("set_then_cancel_hooks_installed",
                  h_int.installed() && h_double.installed() && h_long.installed());

        if (run_observe_probe(ctx, "set_then_cancel"))
        {
            ctx.check("set_then_cancel_no_java_exception", !rvc_fixture::saw_exception());
            // cancel() after set() preserves the stored value (does not zero it).
            ctx.check("set_then_cancel_int_preserved",   rvc_fixture::obs_int()  == 77);
            ctx.check("set_then_cancel_double_preserved",rvc_fixture::obs_double() == -3.5);
            ctx.check("set_then_cancel_long_preserved",
                      rvc_fixture::obs_long() == static_cast<std::int64_t>(0x0123456789ABCDEFLL));
        }
    }

    // ===================================================================
    // ROUND 5 — DOUBLE cancel() in the same detour (idempotent).  Two
    // back-to-back cancel() calls must behave exactly like one: the body is
    // skipped and the caller still sees +0.0 (no accumulated side effect).
    // (audit standalone case return_value_cancel_idempotent, on a live JVM.)
    // ===================================================================
    {
        reset_counters();
        const std::int32_t side_effect_before{ rvc_fixture::side_effect() };
        auto h_void  { vmhook::scoped_hook<rvc_fixture>("origVoid",
            [](vmhook::return_value& r, const std::unique_ptr<rvc_fixture>& self)
            { note_inst(self); r.cancel(); r.cancel(); }) };
        auto h_double{ vmhook::scoped_hook<rvc_fixture>("origDouble",
            [](vmhook::return_value& r, const std::unique_ptr<rvc_fixture>& self)
            { note_inst(self); r.cancel(); r.cancel(); }) };

        ctx.check("double_cancel_hooks_installed", h_void.installed() && h_double.installed());

        if (run_observe_probe(ctx, "double_cancel"))
        {
            ctx.check("double_cancel_no_java_exception", !rvc_fixture::saw_exception());
            ctx.check("double_cancel_void_body_skipped", rvc_fixture::side_effect() == side_effect_before);
            ctx.check("double_cancel_double_returns_zero",     rvc_fixture::obs_double() == 0.0);
            ctx.check("double_cancel_double_is_positive_zero", is_positive_zero(rvc_fixture::obs_double()));
        }
    }

    // ===================================================================
    // ROUND 6 — PER-INVOCATION cancel state.  A single hook on origVoid()
    // cancels ONLY the first call of each probe and lets the second through.
    // The probe (MODE_VOID_TWICE) calls origVoid() twice and snapshots the
    // side-effect counter after each.  This proves the cancel flag lives in
    // the per-call trampoline-allocated return_slot on the native stack and
    // does NOT stick across invocations.  (audit case
    // test_cancel_then_original_runs_on_next_call.)
    // ===================================================================
    {
        reset_counters();
        std::atomic<int> call_index{ 0 };
        const std::int32_t side_effect_before{ rvc_fixture::side_effect() };

        auto h_void{ vmhook::scoped_hook<rvc_fixture>("origVoid",
            [&call_index](vmhook::return_value& r, const std::unique_ptr<rvc_fixture>& self)
            {
                note_inst(self);
                const int idx{ call_index.fetch_add(1, std::memory_order_relaxed) };
                if (idx == 0)
                {
                    r.cancel();   // suppress the FIRST call only
                }
                // second call: do nothing -> original body runs
            }) };
        ctx.check("per_invocation_hook_installed", h_void.installed());

        rvc_fixture::set_mode(mode_consts::void_twice);
        rvc_fixture::set_done(false);
        const bool done{ ctx.run_probe(
            [](bool value) { rvc_fixture::set_go(value); },
            []() { return rvc_fixture::get_done(); }) };
        ctx.check("per_invocation_probe_completed", done);

        if (done)
        {
            ctx.check("per_invocation_no_java_exception", !rvc_fixture::saw_exception());
            ctx.check("per_invocation_hook_fired_twice",  g_inst_fires.load() == 2);
            // 1st call cancelled -> counter unchanged after call 1.
            ctx.check("per_invocation_call1_cancelled",
                      rvc_fixture::side_effect_after_call1() == side_effect_before);
            // 2nd call allowed -> counter advanced by 7 after call 2.
            ctx.check("per_invocation_call2_ran",
                      rvc_fixture::side_effect_after_call2() == side_effect_before + 7);
            // Net effect: exactly ONE body executed across the two calls.
            ctx.check("per_invocation_net_single_body",
                      rvc_fixture::side_effect() == side_effect_before + 7);
        }
    }

    // ===================================================================
    // ROUND 7 — STABILITY: re-run the canonical cancel-only round a SECOND
    // time at the very end.  Each round installs fresh scoped_hooks and tears
    // them down at block exit; this guards against state left behind by a
    // previous round's arm/disarm cycle, proving cancel() is repeatable.
    // ===================================================================
    {
        reset_counters();
        const std::int32_t side_effect_before{ rvc_fixture::side_effect() };
        auto h_void  { vmhook::scoped_hook<rvc_fixture>("origVoid",
            [](vmhook::return_value& r, const std::unique_ptr<rvc_fixture>& self)
            { note_inst(self); r.cancel(); }) };
        auto h_int   { vmhook::scoped_hook<rvc_fixture>("origInt",
            [](vmhook::return_value& r, const std::unique_ptr<rvc_fixture>& self)
            { note_inst(self); r.cancel(); }) };
        auto h_ref   { vmhook::scoped_hook<rvc_fixture>("origRef",
            [](vmhook::return_value& r, const std::unique_ptr<rvc_fixture>& self)
            { note_inst(self); r.cancel(); }) };

        ctx.check("repeat_cancel_hooks_installed",
                  h_void.installed() && h_int.installed() && h_ref.installed());

        if (run_observe_probe(ctx, "repeat_cancel"))
        {
            ctx.check("repeat_cancel_no_java_exception", !rvc_fixture::saw_exception());
            ctx.check("repeat_cancel_void_body_skipped", rvc_fixture::side_effect() == side_effect_before);
            ctx.check("repeat_cancel_int_returns_zero",  rvc_fixture::obs_int() == 0);
            ctx.check("repeat_cancel_ref_returns_null",  rvc_fixture::obs_ref_is_null());
        }
    }

    // ===================================================================
    // ROUND 8 — FINAL ALLOW-THROUGH: with all hooks now disarmed (every
    // scoped_hook above went out of scope), one more bare probe proves the
    // teardown was clean — original values flow again, exactly like the
    // baseline.  Closes the arm/cancel/disarm lifecycle.
    // ===================================================================
    {
        reset_counters();
        rvc_fixture::set_mode(mode_consts::observe_all);
        rvc_fixture::set_done(false);
        const bool done{ ctx.run_probe(
            [](bool value) { rvc_fixture::set_go(value); },
            []() { return rvc_fixture::get_done(); }) };
        ctx.check("final_baseline_probe_completed", done);
        if (done)
        {
            ctx.check("final_baseline_no_hook_fired",
                      g_inst_fires.load() == 0 && g_stat_fires.load() == 0);
            ctx.check("final_baseline_int_is_1111",  rvc_fixture::obs_int()  == 1111);
            ctx.check("final_baseline_double_is_11_25", rvc_fixture::obs_double() == 11.25);
            ctx.check("final_baseline_ref_not_null", !rvc_fixture::obs_ref_is_null());
        }
    }

    ctx.record("[INFO] return_value_cancel: proved cancel() skips the original "
               "body on void (instance+static) and forces 0/0L/+0.0/false/U+0000/null "
               "on int/long/double/bool/char/reference returns (instance+static); "
               "verified allow-through vs cancel, cancel+set both orders (last-write-wins), "
               "idempotent double-cancel, +0.0 (not NaN/-0.0) via the xmm0 epilogue, "
               "per-invocation cancel state across two calls, and a clean arm/disarm "
               "lifecycle bracketed by no-hook baselines.");
}
