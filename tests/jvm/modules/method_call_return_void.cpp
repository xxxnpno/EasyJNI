// method_call_return_void JVM test module — area: methods.
//
// FEATURE: vmhook::method_proxy::call() invoking VOID-returning Java methods,
// and the value_t::is_void() introspection path.
//
// The void return path of call() (vmhook.hpp): the result-decode switch's
// case 'V' returns value_t{ std::monostate{} } WITHOUT reading the result
// slot, on BOTH the interpreter call_stub fast path (when the JDK exposes
// StubRoutines::_call_stub_entry) and the JNI fallback (CallVoidMethodA /
// CallStaticVoidMethodA on modern JDKs).  is_void() reports true for that
// monostate, distinguishing "returned void / failed" from a primitive zero.
//
// A void method returns nothing the native side can read, so this module proves
// each call TWO independent ways:
//   (1) the returned value_t.is_void() is true  (the discard contract), AND
//   (2) the Java body actually executed with the right arguments — observed by
//       reading the static fields the fixture records its invocation/args into.
//
// Both halves matter: (1) without (2) would pass even if call() silently no-op'd
// the dispatch; (2) without (1) would miss a value_t that wrongly carried a
// numeric alternative for a 'V' signature.
//
// Scenarios (every angle the audit finding lists):
//   * void INSTANCE method                       -> is_void + side effect,
//   * void STATIC method                         -> is_void + side effect
//                                                   (static slot; no receiver),
//   * void method with PRIMITIVE args (I,J,Z,D)  -> args observed verbatim,
//   * void method with a STRING arg              -> String observed,
//   * void method with an OBJECT arg             -> identity observed,
//   * NON-CORRUPTION: a value-returning call right after a void call still
//     returns the correct value (a void dispatch must not poison the thread),
//   * CONTRAST: is_void() is FALSE for an int-returning method (zero != void).
//
// call() must run where current_java_thread is set, i.e. inside a hook detour.
// So we hook MethodCallVoid.trigger(int); the probe calls trigger() on a real
// bytecode dispatch, and the detour performs every call() below on the live
// receiver + the static methods, recording observations into file-scope atomics.
#include <vmhook/vmhook.hpp>

#include "../harness.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

namespace
{
    // Wrapper for vmhook.fixtures.MethodCallVoid.  Static helpers read back the
    // side-effect / recorded-argument fields the void bodies write, so the
    // native side can prove each void dispatch reached a real Java body.
    class method_call_void : public vmhook::object<method_call_void>
    {
    public:
        explicit method_call_void(vmhook::oop_t instance) noexcept
            : vmhook::object<method_call_void>{ instance }
        {
        }

        // -- go/done handshake --
        static auto set_go(bool v) -> void { static_field("go")->set(v); }
        static auto get_done() -> bool     { return static_field("done")->get(); }

        // -- side-effect counters --
        static auto void_instance_hits() -> std::int32_t { return static_field("voidInstanceHits")->get(); }
        static auto void_static_hits()   -> std::int32_t { return static_field("voidStaticHits")->get(); }

        // -- recorded primitive args --
        static auto prim_args_called() -> bool        { return static_field("primArgsCalled")->get(); }
        static auto prim_arg_int()     -> std::int32_t { return static_field("primArgInt")->get(); }
        static auto prim_arg_long()    -> std::int64_t { return static_field("primArgLong")->get(); }
        static auto prim_arg_bool()    -> bool         { return static_field("primArgBool")->get(); }
        static auto prim_arg_double()  -> double       { return static_field("primArgDouble")->get(); }

        // -- recorded String arg --
        static auto string_arg_called() -> bool        { return static_field("stringArgCalled")->get(); }
        static auto string_arg()        -> std::string  { return static_field("stringArg")->get(); }
        static auto string_arg_len()    -> std::int32_t { return static_field("stringArgLen")->get(); }

        // -- recorded Object arg --
        static auto object_arg_called()   -> bool        { return static_field("objectArgCalled")->get(); }
        static auto object_arg_non_null() -> bool        { return static_field("objectArgNonNull")->get(); }
        static auto object_arg_identity() -> std::int32_t { return static_field("objectArgIdentity")->get(); }
        static auto self_identity()       -> std::int32_t { return static_field("selfIdentity")->get(); }

        // -- non-corruption breadcrumb --
        static auto last_echo_arg() -> std::int32_t { return static_field("lastEchoArg")->get(); }
    };

    // ------------------------------------------------------------------
    //  Captured observations.  The detour writes; the module body reads.
    // ------------------------------------------------------------------
    std::atomic<int>  g_detour_calls{ 0 };
    std::atomic<bool> g_detour_saw_self{ false };
    std::atomic<bool> g_call_stub_present{ false };

    // is_void() results for every void call (instance / static / arg variants).
    std::atomic<int> g_void_inst_is_void{ -1 };
    std::atomic<int> g_void_stat_is_void{ -1 };
    std::atomic<int> g_void_prim_is_void{ -1 };
    std::atomic<int> g_void_str_is_void{ -1 };
    std::atomic<int> g_void_obj_is_void{ -1 };

    // is_void() / is_string() on a NON-void int return must both be false, and
    // the int value must decode correctly (proves the contrast call really ran).
    std::atomic<int>          g_int_is_void{ -1 };
    std::atomic<int>          g_int_is_string{ -1 };
    std::atomic<std::int64_t> g_int_ret_value{ 0 };

    // Non-corruption: a value-returning call performed AFTER void calls.  Both
    // the echoed arg (echoIntAfterVoid) and a constant returner (retInt) are
    // captured so we prove the post-void dispatch path is intact.
    std::atomic<std::int64_t> g_post_void_echo{ 0 };
    std::atomic<std::int64_t> g_post_void_retint{ 0 };

    // Sentinel args the native side passes, mirrored by the Java assertions.
    constexpr std::int32_t k_prim_int    = 0x0BADF00D;            // 195948557
    constexpr std::int64_t k_prim_long   = static_cast<std::int64_t>(0x0123456789ABCDEFLL);
    constexpr bool         k_prim_bool   = true;
    constexpr double       k_prim_double = 3.141592653589793;
    // Pure ASCII so length/round-trip is path-independent: the OUTGOING string
    // bytes diverge between call_stub (LATIN1 raw copy) and call_jni (NewStringUTF)
    // only for >= 0x80 bytes, which this string has none of.  Exhaustive unicode
    // String coverage lives in method_call_string; here we only need to prove a
    // String arg reaches a VOID body, so ASCII keeps the assertion exact.
    const std::string      k_string_arg  = "void-string-arg-0123456789";
    constexpr std::int32_t k_echo_arg    = 0x5A5A5A5A;            // 1515870810
}

VMHOOK_JVM_MODULE(method_call_return_void)
{
    vmhook::register_class<method_call_void>("vmhook/fixtures/MethodCallVoid");

    g_call_stub_present.store(vmhook::detail::find_call_stub_entry() != nullptr,
                              std::memory_order_relaxed);

    {
        // Hook trigger(); inside the detour current_java_thread is live, so every
        // call() below dispatches a real Java method.  scoped_hook uninstalls
        // when the handle leaves scope.
        auto handle{ vmhook::scoped_hook<method_call_void>(
            "trigger",
            [](vmhook::return_value&,
               const std::unique_ptr<method_call_void>& self,
               std::int32_t /*delta*/)
            {
                g_detour_calls.fetch_add(1, std::memory_order_relaxed);
                g_detour_saw_self.store(self != nullptr, std::memory_order_relaxed);
                if (!self)
                {
                    return;
                }

                // ---- void INSTANCE method: is_void true + side effect ----
                {
                    auto proxy{ self->get_method("voidBumpInstance") };
                    if (proxy.has_value())
                    {
                        const vmhook::method_proxy::value_t v{ proxy->call() };
                        g_void_inst_is_void.store(v.is_void() ? 1 : 0, std::memory_order_relaxed);
                    }
                }

                // ---- void STATIC method: is_void true + side effect ----
                {
                    auto proxy{ method_call_void::static_method("voidBumpStatic") };
                    if (proxy.has_value())
                    {
                        const vmhook::method_proxy::value_t v{ proxy->call() };
                        g_void_stat_is_void.store(v.is_void() ? 1 : 0, std::memory_order_relaxed);
                    }
                }

                // ---- void method with PRIMITIVE args (I, J, Z, D) ----
                // is_void must be true AND every arg must arrive verbatim at the
                // Java body (read back from the recorded static fields later).
                {
                    auto proxy{ self->get_method("voidPrimArgs") };
                    if (proxy.has_value())
                    {
                        const vmhook::method_proxy::value_t v{
                            proxy->call(k_prim_int, k_prim_long, k_prim_bool, k_prim_double) };
                        g_void_prim_is_void.store(v.is_void() ? 1 : 0, std::memory_order_relaxed);
                    }
                }

                // ---- void method with a STRING arg ----
                {
                    auto proxy{ self->get_method("voidStringArg") };
                    if (proxy.has_value())
                    {
                        const vmhook::method_proxy::value_t v{ proxy->call(k_string_arg) };
                        g_void_str_is_void.store(v.is_void() ? 1 : 0, std::memory_order_relaxed);
                    }
                }

                // ---- void method with an OBJECT arg ----
                // Pass the live receiver itself (a wrapper -> object_base branch
                // marshals arg.get_instance()).  The body records the object's
                // identityHashCode; we cross-check it against selfIdentity, so
                // this proves the EXACT object reached the void body.
                {
                    auto proxy{ self->get_method("voidObjectArg") };
                    if (proxy.has_value())
                    {
                        const vmhook::method_proxy::value_t v{ proxy->call(*self) };
                        g_void_obj_is_void.store(v.is_void() ? 1 : 0, std::memory_order_relaxed);
                    }
                }

                // ---- CONTRAST: int returner — is_void() FALSE, value correct ----
                {
                    auto proxy{ self->get_method("retInt") };
                    if (proxy.has_value())
                    {
                        const vmhook::method_proxy::value_t v{ proxy->call() };
                        g_int_is_void.store(v.is_void() ? 1 : 0, std::memory_order_relaxed);
                        g_int_is_string.store(v.is_string() ? 1 : 0, std::memory_order_relaxed);
                        g_int_ret_value.store(static_cast<std::int32_t>(v), std::memory_order_relaxed);
                    }
                }

                // ---- NON-CORRUPTION: value-returning calls AFTER the void calls.
                // A void dispatch must leave the thread / call gate intact, so a
                // subsequent echo and constant return must still be correct.
                {
                    auto echo{ self->get_method("echoIntAfterVoid") };
                    if (echo.has_value())
                    {
                        const std::int32_t r{ echo->call(k_echo_arg) };
                        g_post_void_echo.store(r, std::memory_order_relaxed);
                    }
                    auto ret{ self->get_method("retInt") };
                    if (ret.has_value())
                    {
                        const std::int32_t r{ ret->call() };
                        g_post_void_retint.store(r, std::memory_order_relaxed);
                    }
                }
            }) };

        ctx.check("mcrv_hook_installed", handle.installed());

        const bool done{ ctx.run_probe(
            [](bool v) { method_call_void::set_go(v); },
            []() { return method_call_void::get_done(); }) };

        ctx.check("mcrv_probe_completed", done);
        ctx.check("mcrv_detour_fired", g_detour_calls.load(std::memory_order_relaxed) >= 1);
        ctx.check("mcrv_detour_saw_self", g_detour_saw_self.load(std::memory_order_relaxed));

        ctx.record(std::string{ "[INFO] method_call_return_void dispatch path: " }
                   + (g_call_stub_present.load(std::memory_order_relaxed)
                          ? "call_stub fast path (StubRoutines::_call_stub_entry present)"
                          : "JNI fallback (CallVoidMethodA / CallStaticVoidMethodA)"));

        // =====================================================================
        //  void INSTANCE method
        // =====================================================================
        // The returned value_t must report void...
        ctx.check("mcrv_void_instance_is_void", g_void_inst_is_void.load() == 1);
        // ...and the body must have actually run (exactly once).
        ctx.check("mcrv_void_instance_side_effect",
                  method_call_void::void_instance_hits() == 1);

        // =====================================================================
        //  void STATIC method (CallStaticVoidMethodA / static call_stub slot)
        // =====================================================================
        ctx.check("mcrv_void_static_is_void", g_void_stat_is_void.load() == 1);
        ctx.check("mcrv_void_static_side_effect",
                  method_call_void::void_static_hits() == 1);

        // =====================================================================
        //  void method with PRIMITIVE args (I, J, Z, D) — args delivered
        // =====================================================================
        ctx.check("mcrv_void_prim_is_void", g_void_prim_is_void.load() == 1);
        ctx.check("mcrv_void_prim_called", method_call_void::prim_args_called());
        // Each argument must have reached the void body verbatim — this is the
        // ONLY proof that arguments are marshalled for a no-return dispatch.
        ctx.check("mcrv_void_prim_arg_int",
                  method_call_void::prim_arg_int() == k_prim_int);
        ctx.check("mcrv_void_prim_arg_long",
                  method_call_void::prim_arg_long() == k_prim_long);
        ctx.check("mcrv_void_prim_arg_bool",
                  method_call_void::prim_arg_bool() == k_prim_bool);
        ctx.check("mcrv_void_prim_arg_double",
                  method_call_void::prim_arg_double() == k_prim_double);

        // =====================================================================
        //  void method with a STRING arg
        // =====================================================================
        ctx.check("mcrv_void_string_is_void", g_void_str_is_void.load() == 1);
        ctx.check("mcrv_void_string_called", method_call_void::string_arg_called());
        // The String must have been delivered non-null with the exact length.
        // (k_string_arg is pure ASCII, so its String.length() equals its byte
        // count identically on the call_stub and call_jni argument paths.)
        ctx.check("mcrv_void_string_non_null", method_call_void::string_arg_len() >= 0);
        ctx.check("mcrv_void_string_len_exact",
                  static_cast<std::size_t>(method_call_void::string_arg_len())
                      == k_string_arg.size());
        // And the round-tripped String value must match byte-for-byte (ASCII).
        ctx.check("mcrv_void_string_value_exact",
                  method_call_void::string_arg() == k_string_arg);

        // =====================================================================
        //  void method with an OBJECT arg — exact identity delivered
        // =====================================================================
        ctx.check("mcrv_void_object_is_void", g_void_obj_is_void.load() == 1);
        ctx.check("mcrv_void_object_called", method_call_void::object_arg_called());
        ctx.check("mcrv_void_object_non_null", method_call_void::object_arg_non_null());
        // The body's identityHashCode of the received object must equal the
        // receiver's published identity — proves the EXACT object was passed.
        ctx.check("mcrv_void_object_identity_matches_receiver",
                  method_call_void::object_arg_identity() != 0
                  && method_call_void::object_arg_identity()
                         == method_call_void::self_identity());

        // =====================================================================
        //  CONTRAST: an int return must NOT be reported as void or string
        // =====================================================================
        ctx.check("mcrv_int_return_is_not_void", g_int_is_void.load() == 0);
        ctx.check("mcrv_int_return_is_not_string", g_int_is_string.load() == 0);
        // And it must decode to the real value (proves "not void" isn't a fluke
        // from a failed call that also yields monostate -> would read as 0).
        ctx.check("mcrv_int_return_value_correct", g_int_ret_value.load() == 1337);

        // =====================================================================
        //  NON-CORRUPTION: value-returning calls after the void calls
        // =====================================================================
        // A value-returning call performed AFTER all the void dispatches must
        // still deliver its argument and return value intact.
        ctx.check("mcrv_post_void_echo_value", g_post_void_echo.load() == k_echo_arg);
        ctx.check("mcrv_post_void_echo_side_effect",
                  method_call_void::last_echo_arg() == k_echo_arg);
        ctx.check("mcrv_post_void_retint_value", g_post_void_retint.load() == 1337);
    }
}
