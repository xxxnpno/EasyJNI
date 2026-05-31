// jni_local_ref_hygiene — exhaustive JVM test for vmhook's JNI LOCAL-REFERENCE
// discipline: it proves that vmhook does NOT leak JNI local references on the
// paths that create them, when those paths run from a long-lived attached detour
// thread in a tight loop FAR past HotSpot's default 16-entry local-ref table.
//
// ── WHY THIS MATTERS (audit:jni_delete_local_ref_table_slot_23.md +
//    method_proxy_call_jni_local_ref_leaks.md) ────────────────────────────────
// vmhook detour threads attach to the JVM and STAY attached — they never push or
// pop a JNI frame, so there is no implicit per-call teardown that would reclaim
// local references. Every operation below allocates one (or two) JNI local refs
// that vmhook MUST release via JNIEnv::DeleteLocalRef (vmhook.hpp
// jni_delete_local_ref, slot 23) or the table — capacity 16 by default — fills
// up. Once full, HotSpot logs "JNI local reference table overflow" and the
// allocating JNI call (NewStringUTF / Call(Static)?ObjectMethodA / FindClass)
// starts returning null, so the OBSERVABLE symptom of a leak is: String returns
// come back "", reference returns become null, and injected String args stop
// reaching the body. We drive each path 100+ times and assert the result stays
// correct on every iteration — the behavioural proof the refs are released.
//
// The local-ref-creating paths exercised here:
//   * call() String return     -> CallObjectMethodA jstring local ref, decoded
//                                  and released (String-RETURN loop).
//   * call() fresh String       -> a brand-new heap String each call (no
//                                  constant-pool reuse to mask a leak).
//   * call(String arg)          -> NewStringUTF local ref (arg) + the returned
//                                  jstring local ref = TWO refs/iter (echo loop).
//   * call() Object/array return -> CallObjectMethodA local ref on the 'L'/'['
//                                  arm, released after decode.
//   * STATIC dispatch           -> FindClass jclass local ref (+ the static
//                                  CallStaticObjectMethodA result ref).
//   * INSTANCE dispatch         -> GetObjectClass jclass local ref.
//   * return_value::set_arg(String) -> NewStringUTF local ref + DeleteLocalRef
//                                  (vmhook.hpp return_value::set_arg, the v0.4.x
//                                  leak fix). Driven by hooking inject(String)
//                                  and letting the probe dispatch it in a loop.
//
// SAFETY: the loops are BOUNDED (a few hundred iterations). If a leak existed it
// would surface as the benign table-overflow warning + degraded return values —
// which THIS module catches as a [FAIL] via the stability assertions — never an
// access violation. We never unbound-spin and never take the JVM down.
//
// call() must run where current_java_thread is set, i.e. inside a hook detour, so
// we hook JniLocalRef.trigger() and run all the call()/return loops in that
// detour against the live receiver + the static methods. The set_arg(String)
// loop is driven separately by hooking inject(String): the probe dispatches
// inject(...) JniLocalRef.INJECT_ITERATIONS times, the detour injects a fresh
// String each time, and the Java body records what it received.
//
// This module does NOT modify vmhook.hpp. If it ever observed a real unreleased
// ref it would FAIL the stability assertions (characterizing the leak); see the
// [INFO] breadcrumbs for which path degraded.
#include <vmhook/vmhook.hpp>

#include "../harness.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

namespace
{
    // Wrapper for vmhook.fixtures.JniLocalRef. Instance helpers drive the
    // reference-returning call() paths; static helpers exercise the
    // FindClass-based static jclass resolution.
    class jni_local_ref : public vmhook::object<jni_local_ref>
    {
    public:
        explicit jni_local_ref(vmhook::oop_t instance) noexcept
            : vmhook::object<jni_local_ref>{ instance }
        {
        }

        // go / done handshake + side-effect readback.
        static auto set_go(bool v) -> void              { static_field("go")->set(v); }
        static auto get_done() -> bool                  { bool x = static_field("done")->get(); return x; }
        static auto get_trigger_count() -> std::int32_t { std::int32_t x = static_field("triggerCount")->get(); return x; }

        // inject() loop observables (set_arg(String) path).
        static auto get_inject_count() -> std::int32_t       { std::int32_t x = static_field("injectCount")->get(); return x; }
        static auto get_inject_body_ran() -> bool            { bool x = static_field("injectBodyRan")->get(); return x; }
        static auto get_inject_len_seen() -> std::int32_t    { std::int32_t x = static_field("injectLenSeen")->get(); return x; }
        static auto get_inject_seen() -> std::string         { std::string x = static_field("injectSeen")->get(); return x; }
        static auto get_inject_nonempty_count() -> std::int32_t { std::int32_t x = static_field("injectNonEmptyCount")->get(); return x; }
        static auto get_inject_iterations() -> std::int32_t  { std::int32_t x = static_field("INJECT_ITERATIONS")->get(); return x; }
    };

    // ── Captured observations: the detour writes, the module body reads. ──────
    std::atomic<int>  g_detour_calls{ 0 };
    std::atomic<bool> g_detour_saw_self{ false };
    std::atomic<bool> g_call_stub_present{ false };
    std::atomic<std::uintptr_t> g_receiver_instance{ 0 };

    // String-RETURN loop (call() -> String, CallObjectMethodA + decode + release).
    std::atomic<int> g_str_loop_iters{ 0 };
    std::atomic<int> g_str_loop_distinct{ -1 };   // distinct values seen; 1 == leak-free
    std::atomic<int> g_str_loop_empties{ -1 };    // # of iterations that came back ""

    // FRESH-String-RETURN loop (a new heap String each call; harshest leak case).
    std::atomic<int> g_fresh_loop_iters{ 0 };
    std::atomic<int> g_fresh_loop_mismatches{ -1 };

    // String-ARG echo loop (NewStringUTF arg ref + returned String ref = 2/iter).
    std::atomic<int> g_echo_loop_iters{ 0 };
    std::atomic<int> g_echo_loop_mismatches{ -1 };

    // Object-RETURN loop (call() -> non-String reference; 'L' arm release).
    std::atomic<int> g_obj_loop_iters{ 0 };
    std::atomic<int> g_obj_loop_nonnull{ -1 };     // # iterations the wrapper decoded non-null
    std::atomic<int> g_obj_loop_identity_ok{ -1 }; // # iterations OOP == receiver (call_stub path)

    // Array-RETURN loop (call() -> '[' reference; 'L'/'[' arm release).
    std::atomic<int> g_arr_loop_iters{ 0 };
    std::atomic<int> g_arr_loop_nonnull{ -1 };

    // STATIC String-RETURN loop (FindClass jclass ref + CallStaticObjectMethodA).
    std::atomic<int> g_sstr_loop_iters{ 0 };
    std::atomic<int> g_sstr_loop_distinct{ -1 };

    // STATIC Object-RETURN loop (FindClass ref + CallStaticObjectMethodA result).
    std::atomic<int> g_sobj_loop_iters{ 0 };
    std::atomic<int> g_sobj_loop_nonnull{ -1 };

    // INTERLEAVED loop: one of every path per iteration, so the table sees the
    // worst-case mix of simultaneously-live refs before each release fires.
    std::atomic<int> g_mix_loop_iters{ 0 };
    std::atomic<int> g_mix_loop_failures{ -1 };

    // Post-loop sanity: a single call AFTER every loop still works.
    std::atomic<bool> g_post_loop_str_ok{ false };

    // set_arg(String) loop driven by the inject() hook.
    std::atomic<int>  g_inject_hook_calls{ 0 };
    std::atomic<int>  g_inject_setarg_ok{ 0 };

    // The fresh String the inject() hook injects each dispatch (built once so the
    // lambda captures a stable reference; the leak we test is the per-call
    // NewStringUTF ref, not the C++ string identity).
    const std::string k_inject_payload{ "set-arg-local-ref-loop" };
    const std::string k_echo_payload{ "echo-local-ref-loop" };

    // Run the whole detour-side battery on the live receiver.
    auto run_loops(const std::unique_ptr<jni_local_ref>& self) -> void
    {
        if (!self)
        {
            return;
        }
        jni_local_ref& s = *self;
        g_receiver_instance.store(
            reinterpret_cast<std::uintptr_t>(s.get_instance()),
            std::memory_order_relaxed);

        // Iteration counts chosen WELL past the 16-slot default table so a single
        // un-released ref per call overflows it ~7-15x over the loop.
        constexpr int kStr   = 160;  // String return
        constexpr int kFresh = 160;  // fresh String return
        constexpr int kEcho  = 160;  // String arg + String return (2 refs/iter)
        constexpr int kObj   = 160;  // Object return
        constexpr int kArr   = 160;  // array return
        constexpr int kSStr  = 160;  // static String return (FindClass)
        constexpr int kSObj  = 160;  // static Object return (FindClass)
        constexpr int kMix   = 64;   // interleaved (every path each iter)

        // ── String-RETURN loop: stable single value across all iterations ─────
        // A starved table makes CallObjectMethodA return null -> the decoded
        // String becomes "" -> a SECOND distinct value appears. distinct == 1 and
        // zero empties == leak-free String-return decoding.
        {
            auto proxy{ s.get_method("makeString") };
            std::string first{};
            bool have_first{ false };
            int distinct{ 0 };
            int empties{ 0 };
            for (int i{ 0 }; i < kStr; ++i)
            {
                if (!proxy.has_value())
                {
                    distinct = -1;
                    break;
                }
                const std::string r{ proxy->call().as_string() };
                if (r.empty())
                {
                    ++empties;
                }
                if (!have_first)
                {
                    first = r;
                    have_first = true;
                    distinct = 1;
                }
                else if (r != first)
                {
                    ++distinct;
                }
            }
            g_str_loop_iters.store(kStr);
            g_str_loop_distinct.store(distinct);
            g_str_loop_empties.store(empties);
        }

        // ── FRESH-String-RETURN loop: a new heap String each call ─────────────
        // freshString() always evaluates to "fresh-77" but allocates a new
        // object every iteration (StringBuilder), so no constant-pool reuse can
        // mask a leak. Every iteration must equal "fresh-77".
        {
            auto proxy{ s.get_method("freshString") };
            int mism{ 0 };
            for (int i{ 0 }; i < kFresh; ++i)
            {
                if (!proxy.has_value())
                {
                    mism = kFresh;
                    break;
                }
                if (proxy->call().as_string() != "fresh-77")
                {
                    ++mism;
                }
            }
            g_fresh_loop_iters.store(kFresh);
            g_fresh_loop_mismatches.store(mism);
        }

        // ── String-ARG echo loop: NewStringUTF arg ref + returned String ref ──
        // TWO local refs per iteration -> the table starves twice as fast if
        // EITHER release is missing. Every echo must round-trip to the payload;
        // a starved table yields "" mismatches.
        {
            auto proxy{ s.get_method("echo") };
            int mism{ 0 };
            for (int i{ 0 }; i < kEcho; ++i)
            {
                if (!proxy.has_value())
                {
                    mism = kEcho;
                    break;
                }
                if (proxy->call(k_echo_payload).as_string() != k_echo_payload)
                {
                    ++mism;
                }
            }
            g_echo_loop_iters.store(kEcho);
            g_echo_loop_mismatches.store(mism);
        }

        // ── Object-RETURN loop: non-String reference, 'L' arm release ─────────
        // self() returns the receiver. On the call_stub path the reference
        // decodes to the receiver's real OOP (identity holds); on the call_jni
        // path the 'L' arm decodes+re-encodes the handle, so non-null still
        // holds. The leak guard is "decoded non-null on every iteration" (a
        // starved CallObjectMethodA would return null -> null wrapper).
        {
            auto proxy{ s.get_method("self") };
            int nonnull{ 0 };
            int identity_ok{ 0 };
            const std::uintptr_t recv{ g_receiver_instance.load(std::memory_order_relaxed) };
            for (int i{ 0 }; i < kObj; ++i)
            {
                if (!proxy.has_value())
                {
                    nonnull = -1;
                    break;
                }
                // copy-init (=), NOT brace-init: value_t's templated conversion
                // operator makes unique_ptr<T>{ value_t } ambiguous under MSVC.
                std::unique_ptr<jni_local_ref> w = proxy->call();
                if (w)
                {
                    ++nonnull;
                    if (reinterpret_cast<std::uintptr_t>(w->get_instance()) == recv && recv != 0)
                    {
                        ++identity_ok;
                    }
                }
            }
            g_obj_loop_iters.store(kObj);
            g_obj_loop_nonnull.store(nonnull);
            g_obj_loop_identity_ok.store(identity_ok);
        }

        // ── Array-RETURN loop: '[' reference, 'L'/'[' arm release ─────────────
        // makeArray() returns a fresh int[] each call. Decode to a non-null oop
        // via the value_t void* conversion (without walking it) on every
        // iteration.
        {
            auto proxy{ s.get_method("makeArray") };
            int nonnull{ 0 };
            for (int i{ 0 }; i < kArr; ++i)
            {
                if (!proxy.has_value())
                {
                    nonnull = -1;
                    break;
                }
                const auto v{ proxy->call() };
                void* const arr{ static_cast<void*>(v) };
                if (arr != nullptr)
                {
                    ++nonnull;
                }
            }
            g_arr_loop_iters.store(kArr);
            g_arr_loop_nonnull.store(nonnull);
        }

        // ── STATIC String-RETURN loop: FindClass jclass ref each dispatch ─────
        // The static path resolves the declaring jclass via FindClass (a local
        // ref) on top of the CallStaticObjectMethodA result ref. Stable single
        // value across the loop == both refs released.
        {
            auto proxy{ jni_local_ref::static_method("staticMakeString") };
            std::string first{};
            bool have_first{ false };
            int distinct{ 0 };
            for (int i{ 0 }; i < kSStr; ++i)
            {
                if (!proxy.has_value())
                {
                    distinct = -1;
                    break;
                }
                const std::string r{ proxy->call().as_string() };
                if (!have_first)
                {
                    first = r;
                    have_first = true;
                    distinct = 1;
                }
                else if (r != first)
                {
                    ++distinct;
                }
            }
            g_sstr_loop_iters.store(kSStr);
            g_sstr_loop_distinct.store(distinct);
        }

        // ── STATIC Object-RETURN loop: FindClass ref + result ref ─────────────
        {
            auto proxy{ jni_local_ref::static_method("staticSelf") };
            int nonnull{ 0 };
            for (int i{ 0 }; i < kSObj; ++i)
            {
                if (!proxy.has_value())
                {
                    nonnull = -1;
                    break;
                }
                std::unique_ptr<jni_local_ref> w = proxy->call();  // copy-init (MSVC C2440)
                if (w)
                {
                    ++nonnull;
                }
            }
            g_sobj_loop_iters.store(kSObj);
            g_sobj_loop_nonnull.store(nonnull);
        }

        // ── INTERLEAVED loop: every path once per iteration ───────────────────
        // The harshest mix: a String return, a String-arg echo (2 refs), an
        // Object return, an array return, a static String return (FindClass),
        // and a static Object return — all within a single iteration, so the
        // table holds several simultaneously-live refs before each release fires.
        // A single missing release anywhere overflows it within a handful of
        // iterations. failures == 0 across the whole loop is the strongest
        // single proof of full local-ref hygiene under realistic mixed pressure.
        {
            auto p_ms{ s.get_method("makeString") };
            auto p_echo{ s.get_method("echo") };
            auto p_self{ s.get_method("self") };
            auto p_arr{ s.get_method("makeArray") };
            auto p_sms{ jni_local_ref::static_method("staticMakeString") };
            auto p_sself{ jni_local_ref::static_method("staticSelf") };
            int failures{ 0 };
            for (int i{ 0 }; i < kMix; ++i)
            {
                bool ok{ p_ms.has_value() && p_echo.has_value() && p_self.has_value()
                         && p_arr.has_value() && p_sms.has_value() && p_sself.has_value() };
                if (ok && p_ms->call().as_string() != "local-ref-stable")              { ok = false; }
                if (ok && p_echo->call(k_echo_payload).as_string() != k_echo_payload)   { ok = false; }
                if (ok)
                {
                    std::unique_ptr<jni_local_ref> w = p_self->call();
                    if (!w) { ok = false; }
                }
                if (ok)
                {
                    const auto v{ p_arr->call() };
                    if (static_cast<void*>(v) == nullptr) { ok = false; }
                }
                if (ok && p_sms->call().as_string() != "static-local-ref-stable")      { ok = false; }
                if (ok)
                {
                    std::unique_ptr<jni_local_ref> w = p_sself->call();
                    if (!w) { ok = false; }
                }
                if (!ok)
                {
                    ++failures;
                }
            }
            g_mix_loop_iters.store(kMix);
            g_mix_loop_failures.store(failures);
        }

        // ── POST-LOOP sanity: a single String call after all the loops works ──
        // Hundreds of allocate+release cycles later, the table is healthy and a
        // fresh dispatch still decodes its String.
        {
            auto proxy{ s.get_method("makeString") };
            if (proxy.has_value())
            {
                g_post_loop_str_ok.store(proxy->call().as_string() == "local-ref-stable",
                                         std::memory_order_relaxed);
            }
        }
    }
}

VMHOOK_JVM_MODULE(jni_local_ref_hygiene)
{
    vmhook::register_class<jni_local_ref>("vmhook/fixtures/JniLocalRef");

    // Record which call() dispatch path the live JDK uses. Both paths allocate
    // and must release the same JNI local refs for the cases under test; the
    // module is correct (not skipped) on either.
    g_call_stub_present.store(vmhook::detail::find_call_stub_entry() != nullptr,
                              std::memory_order_relaxed);

    {
        // Hook 1: trigger() — establishes current_java_thread; the detour runs
        // every call()/return leak loop here.
        auto h_trigger{ vmhook::scoped_hook<jni_local_ref>(
            "trigger",
            [](vmhook::return_value&,
               const std::unique_ptr<jni_local_ref>& self)
            {
                g_detour_calls.fetch_add(1, std::memory_order_relaxed);
                g_detour_saw_self.store(self != nullptr, std::memory_order_relaxed);
                run_loops(self);
            }) };

        // Hook 2: inject(String) — each dispatch the detour calls set_arg(0, ...)
        // to inject a FRESH Java String into slot 0 (NewStringUTF local ref +
        // DeleteLocalRef). The probe dispatches inject() in a loop, so this hook
        // exercises the set_arg(String) release path far past the 16-slot table.
        auto h_inject{ vmhook::scoped_hook<jni_local_ref>(
            "inject", "(Ljava/lang/String;)V",
            [](vmhook::return_value& ret,
               const std::unique_ptr<jni_local_ref>&,
               const std::string& /*original*/)
            {
                g_inject_hook_calls.fetch_add(1, std::memory_order_relaxed);
                // set_arg(slot, std::string_view) routes through jni_new_string_utf
                // + jni_delete_local_ref (vmhook.hpp return_value::set_arg). On an
                // instance method slot 0 is `this`, so the String arg `value` lives
                // at slot 1 — inject there.
                if (ret.set_arg(1, std::string_view{ k_inject_payload }))
                {
                    g_inject_setarg_ok.fetch_add(1, std::memory_order_relaxed);
                }
            }) };

        ctx.check("jlr_trigger_hook_installed", h_trigger.installed());
        ctx.check("jlr_inject_hook_installed", h_inject.installed());

        const bool done{ ctx.run_probe(
            [](bool v) { jni_local_ref::set_go(v); },
            []() { return jni_local_ref::get_done(); }) };

        ctx.check("jlr_probe_completed", done);
        ctx.check("jlr_detour_fired", g_detour_calls.load(std::memory_order_relaxed) >= 1);
        ctx.check("jlr_detour_saw_self", g_detour_saw_self.load(std::memory_order_relaxed));
        ctx.check("jlr_trigger_count_advanced", jni_local_ref::get_trigger_count() >= 1);

        const bool stub{ g_call_stub_present.load(std::memory_order_relaxed) };
        ctx.record(std::string{ "[INFO] jni_local_ref_hygiene call() dispatch path: " }
                   + (stub ? "call_stub fast path (StubRoutines::_call_stub_entry present)"
                           : "call_jni JNI fallback (Call(Static)?ObjectMethodA — "
                             "the path whose local refs this module stresses)"));

        // ════════════════ String-RETURN local-ref discipline ══════════════════
        // distinct == 1 AND zero empties: every one of the calls decoded the same
        // non-empty String. A leaked CallObjectMethodA ref would starve the table
        // and later calls would return "" (a 2nd distinct value + nonzero empties).
        ctx.check("jlr_string_return_loop_ran", g_str_loop_iters.load() == 160);
        ctx.check("jlr_string_return_no_leak_single_distinct",
                  g_str_loop_distinct.load() == 1);
        ctx.check("jlr_string_return_no_empty_results",
                  g_str_loop_empties.load() == 0);

        // ════════════════ FRESH-String-RETURN discipline ══════════════════════
        // A brand-new heap String each call: zero mismatches proves the fresh
        // jstring local ref is released every iteration.
        ctx.check("jlr_fresh_string_loop_ran", g_fresh_loop_iters.load() == 160);
        ctx.check("jlr_fresh_string_no_leak_zero_mismatches",
                  g_fresh_loop_mismatches.load() == 0);

        // ════════════════ String-ARG echo (2 refs/iter) discipline ════════════
        // Every echo round-trips: both the NewStringUTF arg ref and the returned
        // jstring ref are released each iteration.
        ctx.check("jlr_echo_loop_ran", g_echo_loop_iters.load() == 160);
        ctx.check("jlr_echo_no_leak_zero_mismatches",
                  g_echo_loop_mismatches.load() == 0);

        // ════════════════ Object-RETURN ('L' arm) discipline ══════════════════
        // Non-null on every iteration: the CallObjectMethodA result ref is
        // released each time (a starved table would return null -> null wrapper).
        ctx.check("jlr_object_return_loop_ran", g_obj_loop_iters.load() == 160);
        ctx.check("jlr_object_return_all_iters_non_null",
                  g_obj_loop_nonnull.load() == 160);
        // On the call_stub path the decoded OOP equals the receiver every time
        // (identity preserved across the whole loop); on call_jni the handle is
        // re-encoded so identity is path-dependent — record it either way.
        if (stub)
        {
            ctx.check("jlr_object_return_identity_preserved_call_stub",
                      g_obj_loop_identity_ok.load() == 160);
        }
        else
        {
            ctx.record("[INFO] jlr object-return identity matches receiver on "
                       + std::to_string(g_obj_loop_identity_ok.load()) + "/160 iters "
                       "(call_jni re-encodes the handle; non-null is the leak guard).");
        }

        // ════════════════ Array-RETURN ('[' arm) discipline ═══════════════════
        ctx.check("jlr_array_return_loop_ran", g_arr_loop_iters.load() == 160);
        ctx.check("jlr_array_return_all_iters_non_null",
                  g_arr_loop_nonnull.load() == 160);

        // ════════════════ STATIC String-RETURN (FindClass) discipline ═════════
        // The FindClass jclass local ref AND the CallStaticObjectMethodA result
        // ref are both released each dispatch: stable single value across the loop.
        ctx.check("jlr_static_string_loop_ran", g_sstr_loop_iters.load() == 160);
        ctx.check("jlr_static_string_no_leak_single_distinct",
                  g_sstr_loop_distinct.load() == 1);

        // ════════════════ STATIC Object-RETURN (FindClass) discipline ═════════
        ctx.check("jlr_static_object_loop_ran", g_sobj_loop_iters.load() == 160);
        ctx.check("jlr_static_object_all_iters_non_null",
                  g_sobj_loop_nonnull.load() == 160);

        // ════════════════ INTERLEAVED mixed-pressure discipline ════════════════
        // The strongest single proof: every path once per iteration, several
        // live refs in flight before each release. Zero failures across the loop
        // == full hygiene under realistic mixed local-ref pressure.
        ctx.check("jlr_interleaved_loop_ran", g_mix_loop_iters.load() == 64);
        ctx.check("jlr_interleaved_no_leak_zero_failures",
                  g_mix_loop_failures.load() == 0);

        // ════════════════ POST-LOOP non-degradation ═══════════════════════════
        // After hundreds of allocate+release cycles a fresh dispatch still works.
        ctx.check("jlr_post_loop_call_still_works",
                  g_post_loop_str_ok.load(std::memory_order_relaxed));

        // ════════════════ set_arg(String) local-ref discipline ════════════════
        // The probe dispatched inject() JniLocalRef.INJECT_ITERATIONS times; each
        // dispatch the detour injected a fresh Java String via set_arg(1, ...),
        // which allocates a NewStringUTF local ref and releases it
        // (jni_delete_local_ref). Far past the 16-slot table, so a missing
        // release would overflow it and later set_arg calls would fail / inject
        // "" — which the body records as a shorter / empty injectSeen.
        const std::int32_t inject_iters{ jni_local_ref::get_inject_iterations() };
        ctx.record("[INFO] jlr set_arg(String) loop: fixture INJECT_ITERATIONS="
                   + std::to_string(inject_iters)
                   + ", inject() hook fired " + std::to_string(g_inject_hook_calls.load())
                   + " time(s), set_arg returned true "
                   + std::to_string(g_inject_setarg_ok.load()) + " time(s); body ran "
                   + std::to_string(jni_local_ref::get_inject_count())
                   + " time(s), observed non-empty injected value "
                   + std::to_string(jni_local_ref::get_inject_nonempty_count()) + " time(s).");

        ctx.check("jlr_setarg_loop_well_past_16_slots", inject_iters >= 100);
        ctx.check("jlr_setarg_inject_hook_fired_each_dispatch",
                  g_inject_hook_calls.load() == inject_iters && inject_iters > 0);
        // Every set_arg call succeeded — no internal NewStringUTF failure / table
        // exhaustion derailed any injection across the whole loop.
        ctx.check("jlr_setarg_all_injections_returned_true",
                  g_inject_setarg_ok.load() == g_inject_hook_calls.load()
                  && g_inject_hook_calls.load() > 0);
        ctx.check("jlr_setarg_body_ran", jni_local_ref::get_inject_body_ran());
        // Every body observed a non-null, non-empty injected String -> the
        // injection reached an unstarved local slot on every one of the 120
        // dispatches (a leaked ref would eventually inject "" or fail).
        ctx.check("jlr_setarg_every_body_saw_nonempty",
                  jni_local_ref::get_inject_nonempty_count() == jni_local_ref::get_inject_count()
                  && jni_local_ref::get_inject_count() == inject_iters);
        // The LAST body observed the exact injected payload (length + content):
        // proof the set_arg(String) path delivered the right bytes after the
        // whole loop, not a truncated / starved value.
        ctx.check("jlr_setarg_last_body_len_exact",
                  jni_local_ref::get_inject_len_seen()
                      == static_cast<std::int32_t>(k_inject_payload.size()));
        ctx.check("jlr_setarg_last_body_content_exact",
                  jni_local_ref::get_inject_seen() == k_inject_payload);
    }
}
