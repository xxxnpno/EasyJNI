// method_throwing_call_site JVM test module  (feature area: method invocation)
//
// THE throwing-call-site authority: invokes a Java method that THROWS, via
// vmhook::method_proxy::call(), from inside a detour, and proves the native call
// site COMPLETES and the JVM is left in a clean, usable state afterwards.  This
// is the legacy `test_throwing_method` scenario carried into the modular harness
// and HARDENED — where the legacy Example path merely let Java call the throwing
// method and catch it in Java, here the NATIVE side drives the throwing call
// through method_proxy::call(), so the exception unwinds from Java back into
// native code.  That is the single most crash-sensitive thing vmhook does, so
// the contract under test is deliberately conservative:
//
//   PRIMARY (hard asserts):
//     * no detour access-violation and no suite truncation: the line AFTER the
//       throwing call() is reached (g_reached_after_boom), the probe's `done`
//       flag is observed, and this module's body runs to completion;
//     * the throwing method genuinely RAN with the right argument: the fixture's
//       boomEntered counter is >=1 and boomLastArg == -1 (the throw cannot hide
//       that the body executed and received our marshalled arg);
//     * the JVM/thread is HEALTHY AFTER the throw: a subsequent benign call()
//       (safeAdd(41) -> 42) succeeds, an instance field read succeeds, and a
//       static field read succeeds — all on the SAME detour thread, AFTER the
//       throwing call;
//     * the thread is NOT left in ExceptionOccurred state for the next module:
//       after our defensive clear, a fresh ExceptionCheck reports no pending
//       exception (g_clean_after_clear).
//
//   CHARACTERIZED (recorded via [INFO], NOT asserted, because it is JDK-variant
//   and dispatch-path dependent):
//     * HOW vmhook surfaces the thrown exception.  Two dispatch paths exist
//       (method_proxy::call, vmhook.hpp ~13096):
//         - JNI-FALLBACK path (no StubRoutines::_call_stub_entry; JDK 21+ and in
//           practice every CI JDK): call_jni runs check_callee_exception() after
//           the JNI Call*MethodA — it detects the pending exception, calls
//           ExceptionDescribe (which PRINTS *and CLEARS* it), extracts
//           Throwable.toString() into the vmhook log, and returns the JNI
//           default-return value (0 for an `I` method) as value_t{int32 0}.  The
//           thread is left clean BY VMHOOK.
//         - CALL-STUB fast path (JDK 8..20 with the stub present): the raw
//           call-stub invocation does NOT run check_callee_exception(); call()
//           decodes result_holder (0 for `I`) into value_t{int32 0} and returns
//           WITHOUT clearing — so a pending exception can remain set on the
//           thread.  THIS MODULE'S DEFENSIVE jni_exception_clear() after the call
//           is what guarantees cleanliness on that path; we record whether a
//           pending exception was observed pre-clear (g_pending_pre_clear) so the
//           live path is visible in the results.
//     * the returned value_t shape (is_void()? variant index? the int value IF
//       it decoded to the int32 alternative).  We do NOT assert a specific
//       return value from the throwing call — a throwing call's "return" is the
//       dispatcher's default cell, which is not a vmhook contract.
//
// SAFETY POSTURE (this is the highest-risk module):
//   * every pointer deref is gated with vmhook::hotspot::is_valid_pointer;
//   * the throwing call() and EVERY post-throw operation happen inside the
//     detour where current_java_thread is live (the only legal context);
//   * immediately after the throwing call() we ALWAYS run
//     vmhook::detail::jni_exception_clear() (idempotent) so no pending exception
//     can poison subsequent JNI on this thread or leak into the next module;
//   * value_t is read by COPY-INIT (never brace-init from call()/value_t — that
//     is ambiguous on MSVC because the templated conversion operator can also
//     produce const char*);
//   * hooks are torn down with vmhook::shutdown_hooks() before the module
//     returns, leaving nothing armed for later modules.
//
// Harness shape mirrors method_call_primitives / method_call_jni_fallback:
// register_class, hook the trigger() method, drive it via run_probe(); the
// detour captures every observation into atomics the module body reads back.
#include <vmhook/vmhook.hpp>

#include "../harness.hpp"

#include <atomic>
#include <cstdint>
#include <string>

namespace
{
    // Wrapper for vmhook.fixtures.ThrowingMethod.  Instance-context accessors
    // only (we always have a real `self` from the detour), so this stays
    // portable across compilers without touching the deducing-this static
    // get_method overloads the field_static module had to avoid on GCC.
    class throwfix : public vmhook::object<throwfix>
    {
    public:
        explicit throwfix(vmhook::oop_t instance) noexcept
            : vmhook::object<throwfix>{ instance }
        {
        }

        // ---- handshake (static fields on the fixture) ----
        static auto set_go(bool value) -> void  { static_field("go")->set(value); }
        static auto set_done(bool value) -> void { static_field("done")->set(value); }
        static auto get_done() -> bool           { return static_field("done")->get(); }

        // ---- witnesses the fixture records (read back by the module body) ----
        static auto get_trigger_count() -> std::int32_t { const std::int32_t v = static_field("triggerCount")->get(); return v; }
        static auto get_boom_entered() -> std::int32_t  { const std::int32_t v = static_field("boomEntered")->get();  return v; }
        static auto get_boom_last_arg() -> std::int32_t { const std::int32_t v = static_field("boomLastArg")->get();  return v; }
        static auto get_safe_add_calls() -> std::int32_t{ const std::int32_t v = static_field("safeAddCalls")->get(); return v; }
        static auto get_static_health() -> std::int32_t { const std::int32_t v = static_field("staticHealthField")->get(); return v; }
    };

    // ---- Sentinels chosen so "did the detour run / capture?" is unambiguous.
    constexpr std::int64_t k_uncaptured{ static_cast<std::int64_t>(0xDEADBEEFCAFEF00DULL) };

    // ---- Observations captured INSIDE the trigger() detour (runs on the Java
    //      thread, where current_java_thread is set).  Read back by the body. ----
    std::atomic<int>          g_detour_calls{ 0 };
    std::atomic<bool>         g_self_valid{ false };       // self pointer survived is_valid_pointer
    std::atomic<bool>         g_boom_resolved{ false };    // get_method("boom","(I)I") resolved
    std::atomic<bool>         g_boom_identity_ok{ false }; // proxy name()=="boom" && signature()=="(I)I"

    std::atomic<bool>         g_reached_after_boom{ false };   // *** THE proof: line after call() ran ***
    std::atomic<bool>         g_boom_ret_is_void{ false };     // call() returned monostate?
    std::atomic<int>          g_boom_ret_variant{ -1 };        // value_t variant index
    std::atomic<bool>         g_boom_ret_is_int32{ false };    // decoded to the int32 alternative?
    std::atomic<std::int64_t> g_boom_ret_int{ k_uncaptured };  // the int value IF int32 (characterization)

    std::atomic<int>          g_pending_pre_clear{ -1 };   // JNI ExceptionCheck BEFORE our clear: 1/0/-1(unknown)
    std::atomic<bool>         g_clear_invoked{ false };    // jni_exception_clear() was called
    std::atomic<int>          g_pending_post_clear{ -1 };  // JNI ExceptionCheck AFTER our clear: must be 0
    std::atomic<bool>         g_clean_after_clear{ false };// post-clear ExceptionCheck == 0

    // ---- post-throw JVM-health observations (all AFTER the throwing call) ----
    std::atomic<bool>         g_safe_add_resolved{ false };
    std::atomic<bool>         g_safe_add_reached_after{ false }; // line after safeAdd call() ran
    std::atomic<int>          g_safe_add_variant{ -1 };
    std::atomic<std::int64_t> g_safe_add_value{ k_uncaptured };  // expect 42 (41 + 1)
    std::atomic<bool>         g_health_field_read_ok{ false };   // instance field read succeeded
    std::atomic<std::int64_t> g_health_field_value{ k_uncaptured };
    std::atomic<bool>         g_static_health_read_ok{ false };  // static field read succeeded
    std::atomic<std::int64_t> g_static_health_value{ k_uncaptured };

    // Read the current thread's JNI ExceptionCheck (slot 228) WITHOUT clearing.
    // Fully guarded; returns 1 (pending), 0 (none), or -1 (could not determine).
    auto jni_exception_pending() noexcept -> int
    {
        void* const env{ vmhook::hotspot::current_jni_env };
        if (!env)
        {
            return -1;
        }
        using exception_check_t = std::uint8_t (*)(void*);
        exception_check_t const exc_check{
            vmhook::detail::jni_function<228, exception_check_t>(env) };
        if (!exc_check)
        {
            return -1;
        }
        return exc_check(env) != 0u ? 1 : 0;
    }

    // The trigger() detour: performs the throwing call() and the post-throw
    // health checks.  Written to be no-throw BY CONSTRUCTION — every operation is
    // a guarded pointer access, an atomic store, or a noexcept vmhook/JNI call —
    // so no C++ exception can escape into the interpreter trampoline.  (It is NOT
    // marked `noexcept`: vmhook's hook<>() function_traits has no specialization
    // for noexcept-qualified function pointers, matching how every example detour
    // is a plain non-noexcept callable.)
    auto on_trigger(vmhook::return_value& /*retval*/,
                    const std::unique_ptr<throwfix>& self,
                    std::int32_t /*delta*/) -> void
    {
        ++g_detour_calls;

        // Guard the receiver before doing anything with it.
        if (!self)
        {
            return;
        }
        void* const self_oop{ self->get_instance() };
        if (!self_oop || !vmhook::hotspot::is_valid_pointer(self_oop))
        {
            return;
        }
        g_self_valid.store(true);

        // -----------------------------------------------------------------
        //  Resolve boom(int):int.  Explicit descriptor keeps resolution
        //  unambiguous and avoids any overload-walk surprises.
        // -----------------------------------------------------------------
        auto boom{ self->get_method("boom", "(I)I") };
        if (!boom.has_value())
        {
            return;
        }
        g_boom_resolved.store(true);

        // Confirm the proxy resolved to EXACTLY the method we intend to drive
        // before we dispatch through it (a mis-resolved overload could otherwise
        // be invoked).  method_proxy has no raw_address(); name()/signature() are
        // the available identity surface, and call() itself additionally guards
        // its backing Method* with is_valid_pointer internally (vmhook.hpp ~13100).
        if (boom->name() != "boom" || boom->signature() != std::string_view{ "(I)I" })
        {
            return;
        }
        g_boom_identity_ok.store(true);

        // =================================================================
        //  THE THROWING CALL.  boom(-1) throws IllegalStateException inside
        //  Java; method_proxy::call() must unwind back here without an AV.
        //  Reaching the line AFTER this call is the whole proof.
        // =================================================================
        {
            // Copy-init the value_t (NEVER brace-init: the templated conversion
            // operator makes `vmhook::method_proxy::value_t v{ call(...) }`
            // ambiguous on MSVC).
            const vmhook::method_proxy::value_t result = boom->call(std::int32_t{ -1 });

            // *** If we are here, the throwing call() returned to native cleanly. ***
            g_reached_after_boom.store(true);

            // Characterize HOW the throw surfaced in the return value (no asserts
            // on the value itself — a throwing call's "return" is the
            // dispatcher's default cell, not a vmhook contract).
            g_boom_ret_is_void.store(result.is_void());
            g_boom_ret_variant.store(static_cast<int>(result.data.index()));
            const bool is_int32{ std::holds_alternative<std::int32_t>(result.data) };
            g_boom_ret_is_int32.store(is_int32);
            if (is_int32)
            {
                // Copy-init via the conversion operator.
                const std::int32_t iv = result;
                g_boom_ret_int.store(static_cast<std::int64_t>(iv));
            }
        }

        // Snapshot whether a pending exception remained on the thread BEFORE we
        // clear it (characterizes the call-stub vs JNI-fallback paths).
        g_pending_pre_clear.store(jni_exception_pending());

        // -----------------------------------------------------------------
        //  DEFENSIVE CLEAR — always, idempotent.  On the call-stub fast path
        //  vmhook does NOT auto-clear, so without this the thrown exception
        //  would poison the safeAdd()/field reads below and leak into the next
        //  module.  On the JNI-fallback path vmhook already cleared it; this is
        //  then a no-op.
        // -----------------------------------------------------------------
        vmhook::detail::jni_exception_clear();
        g_clear_invoked.store(true);

        const int post{ jni_exception_pending() };
        g_pending_post_clear.store(post);
        g_clean_after_clear.store(post == 0);

        // =================================================================
        //  POST-THROW JVM-HEALTH CHECKS (all on THIS thread, AFTER the throw).
        //  These prove the throwing call left the JVM/thread usable.
        // =================================================================

        // (1) A benign call() must still dispatch and return the right value.
        {
            auto safe_add{ self->get_method("safeAdd", "(I)I") };
            if (safe_add.has_value()
                && safe_add->name() == "safeAdd"
                && safe_add->signature() == std::string_view{ "(I)I" })
            {
                g_safe_add_resolved.store(true);
                const vmhook::method_proxy::value_t r = safe_add->call(std::int32_t{ 41 });
                g_safe_add_reached_after.store(true);
                g_safe_add_variant.store(static_cast<int>(r.data.index()));
                if (std::holds_alternative<std::int32_t>(r.data))
                {
                    const std::int32_t v = r;
                    g_safe_add_value.store(static_cast<std::int64_t>(v));
                }
                // Belt-and-braces: clear again in case the benign call itself
                // tripped anything (it must not, but stay clean for the next op).
                vmhook::detail::jni_exception_clear();
            }
        }

        // (2) An instance field read must still succeed.
        {
            auto hf{ self->get_field("healthField") };
            if (hf.has_value())
            {
                const std::int32_t v = hf->get();
                g_health_field_value.store(static_cast<std::int64_t>(v));
                g_health_field_read_ok.store(true);
            }
        }

        // (3) A static field read must still succeed.
        {
            auto shf{ throwfix::static_field("staticHealthField") };
            if (shf.has_value())
            {
                const std::int32_t v = shf->get();
                g_static_health_value.store(static_cast<std::int64_t>(v));
                g_static_health_read_ok.store(true);
            }
        }

        // Final defensive clear so absolutely nothing pending escapes the detour.
        vmhook::detail::jni_exception_clear();
    }

    // Drive one probe cycle: clear `done`, raise `go`, wait for the fixture's
    // probe action (which calls trigger() -> fires on_trigger).
    auto drive(vmhook_test::context& ctx) -> bool
    {
        return ctx.run_probe(
            [](bool value)
            {
                if (value)
                {
                    throwfix::set_done(false);
                }
                throwfix::set_go(value);
            },
            []() { return throwfix::get_done(); });
    }
}

VMHOOK_JVM_MODULE(method_throwing_call_site)
{
    vmhook::register_class<throwfix>("vmhook/fixtures/ThrowingMethod");

    // =====================================================================
    //  0. Sanity: the class + the methods/fields we drive all resolve.
    // =====================================================================
    ctx.check("throwfix_class_registered_static_field_resolves", throwfix::static_field("go").has_value());
    // trigger()/boom()/safeAdd() are INSTANCE methods; their resolution is
    // verified against the live SINGLETON inside the detour (g_boom_resolved /
    // g_safe_add_resolved below).  Here just confirm the static handshake fields
    // the module reads/writes all exist.
    ctx.check("throwfix_done_field_resolves",  throwfix::static_field("done").has_value());
    ctx.check("throwfix_boomEntered_field_resolves", throwfix::static_field("boomEntered").has_value());
    ctx.check("throwfix_staticHealth_field_resolves", throwfix::static_field("staticHealthField").has_value());

    // Record the characterization contract up front so it is in the results even
    // if the probe never completes on some exotic JDK.
    ctx.record("[INFO] method_throwing_call_site: a Java method invoked via "
               "method_proxy::call() that THROWS must (a) unwind back to native "
               "with NO access-violation, (b) leave the thread clearable so the "
               "next call()/field-read succeeds.  vmhook's JNI-fallback path "
               "(JDK 21+/CI) auto-clears via check_callee_exception()/"
               "ExceptionDescribe and returns the dispatcher default (int 0); the "
               "call-stub path (JDK 8..20) does NOT auto-clear, so this module's "
               "defensive jni_exception_clear() guarantees cleanliness either way. "
               "The throwing call's return VALUE is NOT asserted (it is the "
               "dispatcher's default cell, not a vmhook contract).");

    // =====================================================================
    //  1. Install the hook on trigger() and drive the probe.  The detour does
    //     the throwing call() and the post-throw health checks.
    // =====================================================================
    const bool hook_installed{ vmhook::hook<throwfix>("trigger", &on_trigger) };
    ctx.check("trigger_hook_installed", hook_installed);

    if (!hook_installed)
    {
        ctx.record("[INFO] method_throwing_call_site: hook on trigger() failed to "
                   "install; skipping the live throwing-call drive (no JVM left "
                   "dirty — nothing was armed).");
        vmhook::shutdown_hooks();
        return;
    }

    const bool probe_done{ drive(ctx) };

    // PRIMARY: the probe action completed => no native crash/truncation wedged
    // the Java loop, and trigger()'s detour ran to its end.
    ctx.check("throw_probe_completed", probe_done);
    ctx.check("detour_ran_once", g_detour_calls.load() == 1);

    // =====================================================================
    //  2. PRIMARY assertions — call site completed + method genuinely ran.
    // =====================================================================
    if (probe_done)
    {
        // Receiver + resolution survived the guards inside the detour.
        ctx.check("detour_self_valid",        g_self_valid.load());
        ctx.check("detour_boom_resolved",     g_boom_resolved.load());
        ctx.check("detour_boom_identity_ok",  g_boom_identity_ok.load());

        // *** THE headline proof: the line after the throwing call() executed. ***
        ctx.check("reached_line_after_throwing_call", g_reached_after_boom.load());

        // The throwing method genuinely ran with our marshalled argument
        // (recorded BEFORE the throw, so the throw cannot hide it).
        ctx.check("boom_body_entered",       throwfix::get_boom_entered() >= 1);
        ctx.check("boom_received_neg_one",   throwfix::get_boom_last_arg() == -1);

        // trigger() itself returned/ran exactly once (handshake sanity).
        ctx.check("trigger_count_is_one",    throwfix::get_trigger_count() == 1);

        // =================================================================
        //  3. PRIMARY — thread left CLEAN (not in ExceptionOccurred state).
        // =================================================================
        ctx.check("defensive_clear_invoked",     g_clear_invoked.load());
        ctx.check("no_pending_exception_after_clear", g_clean_after_clear.load());
        // The post-clear ExceptionCheck must be a definite 0 (not -1 unknown).
        ctx.check("post_clear_exception_check_is_zero", g_pending_post_clear.load() == 0);

        // =================================================================
        //  4. PRIMARY — JVM healthy AFTER the throw.
        // =================================================================
        // (a) a subsequent benign call() succeeds and returns 41 + 1 == 42.
        ctx.check("post_throw_safeAdd_resolved",      g_safe_add_resolved.load());
        ctx.check("post_throw_safeAdd_call_returned", g_safe_add_reached_after.load());
        ctx.check("post_throw_safeAdd_value_42",      g_safe_add_value.load() == 42);
        // The benign call actually executed its Java body (side-effect counter).
        ctx.check("post_throw_safeAdd_body_ran",      throwfix::get_safe_add_calls() >= 1);

        // (b) an instance field read succeeds with the expected value.
        ctx.check("post_throw_instance_field_read_ok", g_health_field_read_ok.load());
        ctx.check("post_throw_instance_field_value",
                  g_health_field_value.load() == static_cast<std::int64_t>(0x600DC0DE));

        // (c) a static field read succeeds with the expected value (two ways:
        //     the value captured inside the detour, and a fresh read now).
        ctx.check("post_throw_static_field_read_ok", g_static_health_read_ok.load());
        ctx.check("post_throw_static_field_value",
                  g_static_health_value.load() == static_cast<std::int64_t>(0x5AFE5AFE));
        ctx.check("post_throw_static_field_reread_now",
                  throwfix::get_static_health() == static_cast<std::int32_t>(0x5AFE5AFE));

        // =================================================================
        //  5. CHARACTERIZATION ([INFO], not asserted) — HOW the throw surfaced.
        //     This is the key finding the task asks us to record.
        // =================================================================
        {
            const int variant{ g_boom_ret_variant.load() };
            const bool is_void{ g_boom_ret_is_void.load() };
            const bool is_int32{ g_boom_ret_is_int32.load() };
            const std::int64_t retv{ g_boom_ret_int.load() };
            const int pre{ g_pending_pre_clear.load() };

            std::string line{ "[INFO] method_throwing_call_site: throwing boom(-1) via "
                              "method_proxy::call() surfaced as -> value_t.is_void()=" };
            line += (is_void ? "true" : "false");
            line += " variant_index=" + std::to_string(variant);
            line += " is_int32=" + std::string(is_int32 ? "true" : "false");
            if (is_int32)
            {
                line += " int_value=" + std::to_string(retv)
                      + " (the dispatcher's default return cell for a throwing "
                        "(I) method; NOT asserted)";
            }
            ctx.record(line);

            std::string exc{ "[INFO] method_throwing_call_site: pending-exception "
                             "state IMMEDIATELY AFTER call() (pre-defensive-clear) "
                             "ExceptionCheck=" };
            exc += (pre == 1 ? "1 (pending -> CALL-STUB path: vmhook did NOT "
                               "auto-clear; our jni_exception_clear() handled it)"
                  : pre == 0 ? "0 (already clear -> JNI-FALLBACK path: vmhook's "
                               "check_callee_exception()/ExceptionDescribe cleared it)"
                  : "-1 (could not determine: no JNIEnv / slot)");
            ctx.record(exc);

            // The benign RECOVERY call after the throw decoded normally — record
            // its variant so the "JVM healthy after" proof is fully visible
            // (int32 alternative == index 4 in method_proxy::value_t).
            ctx.record(std::string("[INFO] method_throwing_call_site: post-throw "
                                   "recovery safeAdd(41) returned variant_index=")
                       + std::to_string(g_safe_add_variant.load())
                       + " value=" + std::to_string(g_safe_add_value.load())
                       + " (a clean int 42 proves the thread fully recovered).");
        }
    }
    else
    {
        ctx.record("[INFO] method_throwing_call_site: probe did not complete; the "
                   "throwing-call observations were not captured this run.");
    }

    // =====================================================================
    //  6. Leave NOTHING armed for later modules sharing the JVM.
    // =====================================================================
    vmhook::shutdown_hooks();
}
