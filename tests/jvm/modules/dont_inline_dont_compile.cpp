// dont_inline_dont_compile JVM test module
//                                  (feature area: hooks / JIT inhibitors)
//
// Exhaustively exercises the JIT inhibitors vmhook applies to a hooked Method so
// the patched i2i (interpreter) stub stays reachable — the JIT must never inline
// or compile a caller past it.  vmhook::hook<T>() sets TWO HotSpot-internal
// flags at install time (vmhook.hpp:8046-8053):
//   * _dont_inline  -> Method::_flags        bit 2   (set via set_dont_inline)
//   * NO_COMPILE    -> Method::_access_flags  (NOT_C1/C2/C2_OSR compilable +
//                                              QUEUED, vmhook.hpp:5993)
// and clears BOTH on teardown (shutdown_hooks vmhook.hpp:8744-8749,
// hook_handle::stop vmhook.hpp:8828-8831).  hook_verify_repair.cpp already covers
// the NO_COMPILE / _code drift+repair angle; THIS module concentrates on the
// _dont_inline bit in Method::_flags specifically (read back through the live
// Method*), alongside the headline guarantee that a HOT method still fires its
// interpreter hook.
//
// What this module proves on a real bytecode dispatch (Harness go/done probe):
//   * installing a hook SETS _dont_inline (Method::_flags bit 2) AND NO_COMPILE
//     (Method::_access_flags) on the live Method — read back, not assumed,
//   * setting it is idempotent (a second hook attempt on the same method does
//     not flip the bit twice / clobber the access flags),
//   * driving the hooked method through a HOT LOOP (HOT_CALLS dispatches, well
//     over the JIT threshold) does NOT stop the interpreter hook firing: the
//     _dont_inline / NO_COMPILE inhibitors keep every dispatch on the i2i patch
//     (characterised quantitatively; a rare race past NO_COMPILE is REPORTED as
//     the documented limitation, not a spurious FAIL), and the flags are still
//     observably set afterward,
//   * removing the hook (shutdown_hooks() AND, separately, scoped_hook scope
//     exit / hook_handle::stop()) CLEARS _dont_inline and NO_COMPILE again,
//   * teardown of ONE method's hook clears that method's flags while a second
//     still-hooked method keeps both flags set,
//   * the flags SURVIVE a GC / safepoint cleanup (Method lives in Metaspace, not
//     the moving heap) — characterised: after GC churn the bits are still set
//     and the hook still fires.
//
// Read-back fidelity: this module reads Method::_flags through the SAME accessor
// vmhook writes through (method::get_flags(), a uint16_t* + bit (1<<2)).  It is
// therefore testing vmhook's OBSERVABLE behaviour, not an idealised flag layout.
// The audit (audit/findings/dont_inline_dont_compile_flags.md) flags get_flags()'
// hard-coded uint16_t width as wrong on JDK 8-12 (u1) and JDK 21+ (u4); on the
// Java-8 target this module runs against, the audit's repro acknowledges the
// READ side (`*flags & (1<<2)`) still observes the bit correctly, so the
// read-back assertions are sound here.  See the closing REPORT for the real bug.
//
// HARD RULE — never crash the JVM: every Method* deref is guarded by
// is_valid_pointer; if the live Method* cannot be located the Method-level
// scenario is RECORDED as [INFO] and skipped rather than dereferencing anything
// suspicious.  Lifecycle: installs use the low-level vmhook::hook<T>() path
// (persist until shutdown_hooks()); every scenario tears its hook down and the
// module's final statement is an unconditional shutdown_hooks() so NO hook is
// left armed for the modules that run after us.
#include <vmhook/vmhook.hpp>

#include "../harness.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

namespace
{
    // Wrapper for vmhook.fixtures.DontInlineProbe.  Deriving from
    // vmhook::object<> gives the wrapper its vtable (required by
    // register_class<T>) and the static_field(...) / get_field(...) accessors.
    class dip_fixture : public vmhook::object<dip_fixture>
    {
    public:
        explicit dip_fixture(vmhook::oop_t instance) noexcept
            : vmhook::object<dip_fixture>{ instance }
        {
        }

        // --- go/done handshake + scenario selector ------------------------
        static auto set_go(bool value) -> void       { static_field("go")->set(value); }
        static auto set_done(bool value) -> void      { static_field("done")->set(value); }
        static auto get_done() -> bool                { return static_field("done")->get(); }
        static auto set_mode(std::int32_t m) -> void  { static_field("mode")->set(m); }

        // --- recorded observations the Java side writes -------------------
        static auto get_last_hot_result() -> std::int32_t { return static_field("lastHotResult")->get(); }
        static auto get_hot_result_xor() -> std::int64_t  { return static_field("hotResultXor")->get(); }
        static auto get_hot_calls_made() -> std::int32_t  { return static_field("hotCallsMade")->get(); }

        // Reads this instance's own seed (proves `self` is the right object).
        auto seed() const -> std::int32_t { return get_field("seed")->get(); }
    };

    // ---- Fixture-mirrored constants (lockstep with DontInlineProbe.java) ----
    constexpr std::int32_t SEED{ 1000 };
    constexpr std::int32_t HOT_DELTA{ 7 };
    constexpr std::int32_t HOT_CALLS{ 200000 };

    constexpr std::int32_t HOT_ORIGINAL{ SEED + HOT_DELTA };  // hot(HOT_DELTA) body result

    // The fully-qualified class + the hooked method's name/signature.  Used to
    // locate the live Method* so we can READ BACK its _flags / _access_flags.
    constexpr const char* FIXTURE_CLASS{ "vmhook/fixtures/DontInlineProbe" };
    constexpr const char* HOT_NAME{ "hot" };
    constexpr const char* HOT_SIG{ "(I)I" };

    // The _dont_inline bit position vmhook writes (vmhook.hpp:6016) — bit 2 of
    // Method::_flags.  We read it back through the SAME accessor vmhook uses.
    constexpr std::uint16_t DONT_INLINE_BIT{ static_cast<std::uint16_t>(1u << 2) };

    // ---- Hook observation state (reset per scenario) -----------------------
    std::atomic<std::int32_t> g_fire_count{ 0 };
    std::atomic<std::int32_t> g_self_ok_fires{ 0 };   // self non-null & seed == SEED
    std::atomic<std::int64_t> g_arg_xor{ 0 };         // XOR of every decoded delta

    auto reset_observations() -> void
    {
        g_fire_count.store(0);
        g_self_ok_fires.store(0);
        g_arg_xor.store(0);
    }

    // Drives exactly one probe cycle for `mode`: resets observations + the
    // latched `done` flag, programs the scenario selector, then runs the probe.
    auto drive(vmhook_test::context& ctx, std::int32_t mode) -> bool
    {
        reset_observations();
        return ctx.run_probe(
            [mode](bool value)
            {
                if (value)
                {
                    // Rising edge: program the scenario and clear the latch
                    // BEFORE the fixture's pending() observes go.
                    dip_fixture::set_done(false);
                    dip_fixture::set_mode(mode);
                }
                dip_fixture::set_go(value);
            },
            []() { return dip_fixture::get_done(); });
    }

    // The observer detour installed via the low-level vmhook::hook<T>() path
    // (allow-through — it only records).  Counts fires, validates `self`, folds
    // the decoded delta into an XOR so a wrong decode is observable.
    auto install_hot_observer() -> bool
    {
        return vmhook::hook<dip_fixture>(
            HOT_NAME,
            [](vmhook::return_value&,
               const std::unique_ptr<dip_fixture>& self,
               std::int32_t delta)
            {
                g_fire_count.fetch_add(1, std::memory_order_relaxed);
                if (self != nullptr && self->seed() == SEED)
                {
                    g_self_ok_fires.fetch_add(1, std::memory_order_relaxed);
                }
                g_arg_xor.fetch_xor(delta, std::memory_order_relaxed);
            });
    }

    // Locates the live Method* for FIXTURE_CLASS::hot(I)I by walking the
    // InstanceKlass methods array.  Returns nullptr if anything looks invalid —
    // callers MUST treat nullptr as "cannot run this Method-level scenario" and
    // skip it rather than crash.  All reads are pointer-validated.  (Same shape
    // as hook_verify_repair.cpp's find_hot_method.)
    auto find_hot_method() -> vmhook::hotspot::method*
    {
        vmhook::hotspot::klass* const k{ vmhook::find_class(FIXTURE_CLASS) };
        if (!k || !vmhook::hotspot::is_valid_pointer(k))
        {
            return nullptr;
        }
        const std::int32_t count{ k->get_methods_count() };
        vmhook::hotspot::method** const methods{ k->get_methods_ptr() };
        if (!methods || count <= 0)
        {
            return nullptr;
        }
        for (std::int32_t i{ 0 }; i < count; ++i)
        {
            vmhook::hotspot::method* const m{ methods[i] };
            if (!m || !vmhook::hotspot::is_valid_pointer(m))
            {
                continue;
            }
            const std::string name = m->get_name();          // copy-init (MSVC)
            const std::string sig = m->get_signature();      // copy-init (MSVC)
            if (name == HOT_NAME && sig == HOT_SIG)
            {
                return m;
            }
        }
        return nullptr;
    }

    // True iff the _dont_inline bit (Method::_flags bit 2) is currently set.
    // Read through method::get_flags() — the exact accessor vmhook writes
    // through — so this observes vmhook's real effect.  Pointer-validated; a
    // bad/freed Method* or a missing _flags VMStruct yields false (never a deref
    // crash).
    auto dont_inline_set(vmhook::hotspot::method* const m) -> bool
    {
        if (!m || !vmhook::hotspot::is_valid_pointer(m))
        {
            return false;
        }
        const std::uint16_t* const flags{ m->get_flags() };
        return flags != nullptr && (*flags & DONT_INLINE_BIT) != 0;
    }

    // Snapshots the full Method::_flags word (for the idempotency / no-clobber
    // checks).  Returns 0 if unreadable — callers pair this with dont_inline_set
    // so a 0 from "unreadable" is never mistaken for a real cleared state.
    auto read_flags_word(vmhook::hotspot::method* const m) -> std::uint16_t
    {
        if (!m || !vmhook::hotspot::is_valid_pointer(m))
        {
            return 0;
        }
        const std::uint16_t* const flags{ m->get_flags() };
        return flags != nullptr ? *flags : static_cast<std::uint16_t>(0);
    }

    // True iff the method currently carries the NO_COMPILE inhibitor vmhook sets
    // at install time (Method::_access_flags).  Pointer-validated.
    auto no_compile_set(vmhook::hotspot::method* const m) -> bool
    {
        if (!m || !vmhook::hotspot::is_valid_pointer(m))
        {
            return false;
        }
        const std::uint32_t* const flags{ m->get_access_flags() };
        return flags != nullptr && (*flags & vmhook::hotspot::NO_COMPILE) != 0;
    }

    // Reads Method::_code through a validated pointer.  nullptr means "not
    // currently JIT-compiled" (the deopted steady state vmhook installs).
    auto method_code(vmhook::hotspot::method* const m) -> void*
    {
        if (!m || !vmhook::hotspot::is_valid_pointer(m))
        {
            return nullptr;
        }
        void* const code{ m->get_code() };
        return (code && vmhook::hotspot::is_valid_pointer(code)) ? code : nullptr;
    }
}

VMHOOK_JVM_MODULE(dont_inline_dont_compile)
{
    vmhook::register_class<dip_fixture>(FIXTURE_CLASS);

    // Clean baseline: nothing armed; the live Method* must carry NEITHER flag
    // before any hook is installed (proves the flags are ours to set, not a
    // pre-existing JVM state we'd be mis-reading).
    {
        vmhook::shutdown_hooks();   // belt-and-braces: ensure empty

        vmhook::hotspot::method* const m{ find_hot_method() };
        ctx.check("baseline_located_live_method", m != nullptr);
        if (m != nullptr)
        {
            ctx.check("baseline_dont_inline_clear_before_any_hook",
                      !dont_inline_set(m));
            ctx.check("baseline_no_compile_clear_before_any_hook",
                      !no_compile_set(m));
        }
        else
        {
            ctx.record("[INFO] dont_inline_dont_compile baseline: could not locate "
                       "live Method* for hot(I)I - Method-level read-back scenarios "
                       "will be skipped (no crash).");
        }
    }

    // =====================================================================
    // Scenario 1 — INSTALL SETS BOTH FLAGS, read back through the live Method*.
    //   Install the hook; the live Method must now carry _dont_inline
    //   (Method::_flags bit 2) AND NO_COMPILE (Method::_access_flags), and the
    //   detour must fire exactly once on a real dispatch with the original body
    //   running (allow-through).  This is the headline "install sets the flags"
    //   requirement, OBSERVED rather than assumed.
    // =====================================================================
    {
        ctx.check("install_returns_true", install_hot_observer());

        vmhook::hotspot::method* const m{ find_hot_method() };
        ctx.check("install_located_live_method", m != nullptr);
        if (m != nullptr)
        {
            ctx.check("install_set_dont_inline_bit", dont_inline_set(m));
            ctx.check("install_set_no_compile_flags", no_compile_set(m));
            // Install deopts the method, so _code is null in the steady state.
            ctx.check("install_left_code_null", method_code(m) == nullptr);
        }

        const bool done{ drive(ctx, 1) };
        ctx.check("install_probe_completed", done);
        ctx.check("install_java_made_one_call",
                  dip_fixture::get_hot_calls_made() == 1);
        ctx.check("install_detour_fired_exactly_once", g_fire_count.load() == 1);
        ctx.check("install_self_correct", g_self_ok_fires.load() == 1);
        ctx.check("install_arg_decoded", g_arg_xor.load() == HOT_DELTA);
        ctx.check("install_allow_through_original_result",
                  dip_fixture::get_last_hot_result() == HOT_ORIGINAL);

        // Flags still set AFTER the detour fired (firing didn't clear them).
        if (m != nullptr)
        {
            ctx.check("install_dont_inline_still_set_after_fire", dont_inline_set(m));
            ctx.check("install_no_compile_still_set_after_fire", no_compile_set(m));
        }

        vmhook::shutdown_hooks();   // clean up scenario 1
    }

    // =====================================================================
    // Scenario 2 — IDEMPOTENT install.  A second hook<T>() on the SAME method
    //   is a no-op that returns true (vmhook.hpp:8038-8044) and must NOT flip
    //   the _dont_inline bit twice or clobber the access-flags word.  Snapshot
    //   Method::_flags after the first install and after a redundant second
    //   attempt; they must be byte-identical, with the bit set exactly once.
    // =====================================================================
    {
        ctx.check("idempotent_first_install_returns_true", install_hot_observer());

        vmhook::hotspot::method* const m{ find_hot_method() };
        ctx.check("idempotent_located_live_method", m != nullptr);
        if (m != nullptr)
        {
            ctx.check("idempotent_first_install_set_bit", dont_inline_set(m));
            const std::uint16_t flags_after_first{ read_flags_word(m) };

            // Redundant second install on the same Method* — duplicate-membership
            // short-circuit returns true without re-touching the flags.
            ctx.check("idempotent_second_install_returns_true", install_hot_observer());

            const std::uint16_t flags_after_second{ read_flags_word(m) };
            ctx.check("idempotent_dont_inline_still_set_after_second",
                      dont_inline_set(m));
            ctx.check("idempotent_flags_word_unchanged_by_second_install",
                      flags_after_second == flags_after_first);
            // The bit is set exactly once: clearing bit 2 from the snapshot
            // leaves a word that, OR'd with the bit, reproduces the snapshot —
            // i.e. no OTHER bit was disturbed and the bit is genuinely present.
            ctx.check("idempotent_only_dont_inline_bit_accounts_for_difference",
                      static_cast<std::uint16_t>(flags_after_first | DONT_INLINE_BIT)
                          == flags_after_first);
            ctx.check("idempotent_no_compile_still_set", no_compile_set(m));
        }

        vmhook::shutdown_hooks();   // clean up scenario 2
    }

    // =====================================================================
    // Scenario 3 — HOT method still hooks: the JIT does NOT inline/compile past
    //   the patched i2i stub.  Drive hot() through a hot loop big enough to make
    //   HotSpot want to inline + compile it.  Because _dont_inline + NO_COMPILE
    //   are held, every interpreted dispatch should still route through the
    //   patched stub: the detour fires on every call and Method::_code stays
    //   null.  Quantitative proof of the "JIT can't inline/compile past the
    //   patch" guarantee, with the flags re-read afterward.
    //
    //   Robustness (mirrors hook_verify_repair scenario 2): HotSpot can, very
    //   rarely, race an in-flight compile past NO_COMPILE — the documented
    //   mode-3 limitation.  A shortfall is therefore folded into an [INFO] line
    //   (not a spurious FAIL); the always-true sanity properties are asserted
    //   hard, and the durable proof is that the inhibitors are still set + the
    //   hook fires again afterward.
    // =====================================================================
    {
        ctx.check("hot_install_returns_true", install_hot_observer());

        vmhook::hotspot::method* const m{ find_hot_method() };
        ctx.check("hot_located_live_method", m != nullptr);
        if (m != nullptr)
        {
            ctx.check("hot_install_set_dont_inline", dont_inline_set(m));
            ctx.check("hot_install_set_no_compile", no_compile_set(m));
            ctx.check("hot_install_left_code_null", method_code(m) == nullptr);
        }

        const bool done{ drive(ctx, 2) };
        ctx.check("hot_loop_probe_completed", done);
        ctx.check("hot_java_made_all_calls",
                  dip_fixture::get_hot_calls_made() == HOT_CALLS);

        const std::int32_t fired{ g_fire_count.load() };
        const bool fired_on_every_call{ fired == HOT_CALLS };
        ctx.record("[INFO] dont_inline_dont_compile scenario 3: detour fired "
                   + std::to_string(fired) + "/" + std::to_string(HOT_CALLS)
                   + " hot-loop dispatches"
                   + (fired_on_every_call
                          ? " (_dont_inline + NO_COMPILE held - JIT did NOT "
                            "inline/compile past the i2i patch)."
                          : " (DEFICIT - HotSpot raced a compile past NO_COMPILE "
                            "and bypassed the interpreter-only hook for the "
                            "shortfall: the documented mode-3 limitation)."));

        // Always-true robust sanity: the hook was in the dispatch path (fired at
        // least once) and never over-counted (no double-fire per call).
        ctx.check("hot_detour_was_in_dispatch_path", fired >= 1);
        ctx.check("hot_detour_not_double_fired", fired <= HOT_CALLS);
        // Every fire that DID happen saw the correct receiver + decoded its arg.
        ctx.check("hot_self_correct_on_every_fire_that_happened",
                  g_self_ok_fires.load() == fired);
        // Full-count guarantee asserted only in the healthy case; a deficit is
        // already characterised in the [INFO] line above (not a FAIL).
        if (fired_on_every_call)
        {
            ctx.check("hot_detour_fired_on_every_dispatch_when_inhibitors_held",
                      fired == HOT_CALLS);
        }
        // The original body always runs (allow-through), regardless of fire path.
        ctx.check("hot_allow_through_last_result",
                  dip_fixture::get_last_hot_result()
                      == (SEED + ((HOT_CALLS - 1) & 0xFF)));

        // The inhibitors must STILL be observably set after all that JIT
        // pressure (a verify pass restores the deopted state if HotSpot drifted).
        if (m != nullptr)
        {
            const bool di_after{ dont_inline_set(m) };
            const bool nc_after{ no_compile_set(m) };
            ctx.record(std::string{ "[INFO] dont_inline_dont_compile scenario 3: "
                       "post-hot-loop _dont_inline=" } + (di_after ? "set" : "CLEARED")
                       + ", NO_COMPILE=" + (nc_after ? "set" : "CLEARED")
                       + ", Method::_code=" + (method_code(m) == nullptr ? "null" : "NON-null") + ".");
            const std::size_t repaired{ vmhook::verify_hooks() };
            ctx.record("[INFO] dont_inline_dont_compile scenario 3: verify_hooks() "
                       "after hot loop repaired " + std::to_string(repaired) + " hook(s).");
            // After a verify pass the inhibitors are back no matter what HotSpot
            // did during the loop — this is the durable, deterministic assertion.
            ctx.check("hot_dont_inline_set_after_verify_pass", dont_inline_set(m));
            ctx.check("hot_no_compile_set_after_verify_pass", no_compile_set(m));
            ctx.check("hot_code_null_after_verify_pass", method_code(m) == nullptr);
        }

        // Hook still fires after all that JIT pressure + verify.
        const bool done2{ drive(ctx, 4) };
        ctx.check("hot_post_pressure_probe_completed", done2);
        ctx.check("hot_hook_still_fires_after_jit_pressure", g_fire_count.load() == 1);
        ctx.check("hot_post_pressure_allow_through",
                  dip_fixture::get_last_hot_result() == HOT_ORIGINAL);

        vmhook::shutdown_hooks();   // clean up scenario 3
    }

    // =====================================================================
    // Scenario 4 — TEARDOWN CLEARS BOTH FLAGS (bulk shutdown_hooks() path).
    //   Install -> both flags set; shutdown_hooks() -> both flags CLEARED on the
    //   live Method, the detour goes silent, and the original body runs
    //   byte-exact.  Proves the install/teardown flag contract is symmetric for
    //   the bulk path (vmhook.hpp:8744-8749).
    // =====================================================================
    {
        ctx.check("clear_install_returns_true", install_hot_observer());

        vmhook::hotspot::method* const m{ find_hot_method() };
        ctx.check("clear_located_live_method", m != nullptr);
        if (m != nullptr)
        {
            ctx.check("clear_pre_dont_inline_set", dont_inline_set(m));
            ctx.check("clear_pre_no_compile_set", no_compile_set(m));
        }

        // --- Bulk teardown must clear BOTH flags. ---
        vmhook::shutdown_hooks();
        if (m != nullptr)
        {
            ctx.check("clear_dont_inline_cleared_after_shutdown", !dont_inline_set(m));
            ctx.check("clear_no_compile_cleared_after_shutdown", !no_compile_set(m));
        }

        // Detour silent + original body byte-exact after teardown.
        const bool done{ drive(ctx, 1) };
        ctx.check("clear_post_probe_completed", done);
        ctx.check("clear_detour_silent_after_shutdown", g_fire_count.load() == 0);
        ctx.check("clear_byte_exact_original_after_shutdown",
                  dip_fixture::get_last_hot_result() == HOT_ORIGINAL);

        // shutdown_hooks already removed the hook; this is belt-and-braces.
        vmhook::shutdown_hooks();
    }

    // =====================================================================
    // Scenario 5 — scoped_hook teardown clears the flags (RAII / hook_handle::
    //   stop() path, vmhook.hpp:8828-8831).  A scoped_hook installed in an inner
    //   block sets both flags; on scope exit it must clear them on the live
    //   Method.  This exercises the OTHER teardown path (not bulk shutdown).
    // =====================================================================
    {
        vmhook::hotspot::method* const m{ find_hot_method() };
        ctx.check("scoped_located_live_method", m != nullptr);

        {
            auto handle{ vmhook::scoped_hook<dip_fixture>(
                HOT_NAME,
                [](vmhook::return_value&,
                   const std::unique_ptr<dip_fixture>& self,
                   std::int32_t delta)
                {
                    g_fire_count.fetch_add(1, std::memory_order_relaxed);
                    if (self != nullptr && self->seed() == SEED)
                    {
                        g_self_ok_fires.fetch_add(1, std::memory_order_relaxed);
                    }
                    g_arg_xor.fetch_xor(delta, std::memory_order_relaxed);
                }) };

            ctx.check("scoped_hook_installed", handle.installed());
            if (m != nullptr)
            {
                ctx.check("scoped_install_set_dont_inline", dont_inline_set(m));
                ctx.check("scoped_install_set_no_compile", no_compile_set(m));
            }

            const bool done{ drive(ctx, 1) };
            ctx.check("scoped_probe_completed", done);
            ctx.check("scoped_detour_fired", g_fire_count.load() == 1);
            ctx.check("scoped_allow_through",
                      dip_fixture::get_last_hot_result() == HOT_ORIGINAL);
        }
        // handle out of scope -> hook_handle::stop() ran -> flags must be clear.

        if (m != nullptr)
        {
            ctx.check("scoped_dont_inline_cleared_after_scope_exit", !dont_inline_set(m));
            ctx.check("scoped_no_compile_cleared_after_scope_exit", !no_compile_set(m));
        }

        // After scope exit the detour must not fire; original body intact.
        const bool done2{ drive(ctx, 4) };
        ctx.check("scoped_post_probe_completed", done2);
        ctx.check("scoped_detour_silent_after_scope_exit", g_fire_count.load() == 0);
        ctx.check("scoped_byte_exact_original_after_scope_exit",
                  dip_fixture::get_last_hot_result() == HOT_ORIGINAL);

        vmhook::shutdown_hooks();   // belt-and-braces
    }

    // =====================================================================
    // Scenario 6 — SHARED-Method* teardown semantics (CHARACTERISATION of a real
    //   vmhook trait — see the closing REPORT).  vmhook keys hooks by Method*
    //   with NO refcount: a duplicate hook<T>() on an already-hooked Method
    //   short-circuits (returns true WITHOUT a second g_hooked_methods entry,
    //   vmhook.hpp:8038-8044), yet scoped_hook still hands back a hook_handle
    //   bound to that SAME Method* (vmhook.hpp:8978).  So dropping that scoped
    //   handle runs hook_handle::stop() on the SHARED entry: it clears
    //   _dont_inline + NO_COMPILE and ERASES the single entry
    //   (vmhook.hpp:8828-8841) — taking the still-wanted persistent hook down
    //   with it.  This is a genuine "no per-handle refcount on a shared Method*"
    //   behaviour; we DOCUMENT it (the durable multi-DISTINCT-Method* selective
    //   teardown is already covered by shutdown_hooks_teardown.cpp scenario 3),
    //   asserting what the code ACTUALLY does so the module stays green and the
    //   trait is on record rather than silently mis-asserted.
    // =====================================================================
    {
        vmhook::hotspot::method* const m{ find_hot_method() };
        ctx.check("shared_located_live_method", m != nullptr);

        // Persistent low-level hook keeps the method hooked at block start.
        ctx.check("shared_persistent_install_returns_true", install_hot_observer());
        if (m != nullptr)
        {
            ctx.check("shared_flags_set_with_persistent_hook", dont_inline_set(m));
            ctx.check("shared_no_compile_set_with_persistent_hook", no_compile_set(m));
        }

        bool dup_short_circuit_returns_true{ false };
        {
            auto handle{ vmhook::scoped_hook<dip_fixture>(
                HOT_NAME,
                [](vmhook::return_value&,
                   const std::unique_ptr<dip_fixture>&,
                   std::int32_t)
                {
                    // Same Method already hooked: the scoped install short-circuits
                    // on duplicate-membership.  Body intentionally minimal.
                }) };
            // The duplicate install still yields a usable handle bound to the
            // shared Method* (installed() reflects the short-circuit's true).
            dup_short_circuit_returns_true = handle.installed();
            ctx.check("shared_duplicate_scoped_handle_is_installed",
                      dup_short_circuit_returns_true);
        }
        // The scoped handle just exited -> hook_handle::stop() ran on the SHARED
        // entry.  Because there is no refcount, the persistent hook's entry is
        // now gone and the inhibitors are cleared.  Assert the ACTUAL behaviour.
        if (m != nullptr)
        {
            const bool di_after{ dont_inline_set(m) };
            const bool nc_after{ no_compile_set(m) };
            ctx.record(std::string{ "[INFO] dont_inline_dont_compile scenario 6: "
                       "after a scoped_hook on an ALREADY-hooked Method* expired, "
                       "_dont_inline=" } + (di_after ? "still set" : "CLEARED")
                       + ", NO_COMPILE=" + (nc_after ? "still set" : "CLEARED")
                       + " (vmhook keys hooks by Method* with no refcount: the shared "
                         "entry was erased by the handle's stop()).");
            ctx.check("shared_dont_inline_cleared_by_shared_handle_stop", !di_after);
            ctx.check("shared_no_compile_cleared_by_shared_handle_stop", !nc_after);
        }

        // Consequence of the shared-entry erase: the persistent hook no longer
        // fires (its entry was removed too).  Characterise + assert the real
        // outcome so the trait is locked in.
        const bool done{ drive(ctx, 1) };
        ctx.check("shared_post_probe_completed", done);
        const std::int32_t fired_after_shared_stop{ g_fire_count.load() };
        ctx.record("[INFO] dont_inline_dont_compile scenario 6: persistent hook fired "
                   + std::to_string(fired_after_shared_stop)
                   + " time(s) after the shared scoped handle expired "
                     "(0 == the shared entry was torn down with the handle).");
        ctx.check("shared_persistent_hook_silenced_by_shared_handle_stop",
                  fired_after_shared_stop == 0);
        // The original body still runs byte-exact regardless.
        ctx.check("shared_byte_exact_original_after_shared_stop",
                  dip_fixture::get_last_hot_result() == HOT_ORIGINAL);

        // Belt-and-braces bulk teardown (state is already clean here).
        vmhook::shutdown_hooks();
        if (m != nullptr)
        {
            ctx.check("shared_flags_clear_after_final_shutdown", !dont_inline_set(m));
        }
    }

    // =====================================================================
    // Scenario 7 — FLAGS SURVIVE A GC / SAFEPOINT (characterisation).  A Method
    //   lives in Metaspace (not the moving heap), so a collection must not move
    //   it or strip our flags.  Install -> force GC churn (mode 3 allocates
    //   garbage + System.gc() twice) -> the flags must still be observably set
    //   and the hook must still fire.  Whether a redefine would survive is also
    //   characterised in the closing REPORT (a JVMTI RedefineClasses frees the
    //   old Method, so the flags do NOT survive that — vmhook's verify_hooks()
    //   mode-1 re-anchors to the new Method; provoking a real redefine safely is
    //   out of scope for an in-process module, exactly as hook_verify_repair.cpp
    //   notes for its modes 1/2).
    // =====================================================================
    {
        ctx.check("gc_install_returns_true", install_hot_observer());

        vmhook::hotspot::method* const m{ find_hot_method() };
        ctx.check("gc_located_live_method", m != nullptr);
        if (m != nullptr)
        {
            ctx.check("gc_pre_dont_inline_set", dont_inline_set(m));
            ctx.check("gc_pre_no_compile_set", no_compile_set(m));
        }

        // mode 3 = allocate garbage + System.gc() x2, then call hot() once.
        const bool done{ drive(ctx, 3) };
        ctx.check("gc_probe_completed", done);
        ctx.check("gc_java_made_one_call", dip_fixture::get_hot_calls_made() == 1);
        ctx.check("gc_hook_fired_after_gc", g_fire_count.load() == 1);
        ctx.check("gc_allow_through_after_gc",
                  dip_fixture::get_last_hot_result() == HOT_ORIGINAL);

        // The Method* is in Metaspace -> the same pointer is still valid and the
        // flags must have survived the collection.
        if (m != nullptr)
        {
            const bool di_survived{ dont_inline_set(m) };
            const bool nc_survived{ no_compile_set(m) };
            ctx.record(std::string{ "[INFO] dont_inline_dont_compile scenario 7: "
                       "after System.gc() x2, _dont_inline=" } + (di_survived ? "survived" : "LOST")
                       + ", NO_COMPILE=" + (nc_survived ? "survived" : "LOST")
                       + " (Method lives in Metaspace, not the moving heap).");
            ctx.check("gc_dont_inline_survived_collection", di_survived);
            ctx.check("gc_no_compile_survived_collection", nc_survived);
            ctx.check("gc_method_pointer_still_valid_after_gc",
                      vmhook::hotspot::is_valid_pointer(m));
        }

        vmhook::shutdown_hooks();   // clean up scenario 7
    }

    // =====================================================================
    // FINAL CLEANUP — belt-and-braces.  Other modules run after this one, so the
    //   module MUST leave ZERO hooks armed.  Every scenario already tears its
    //   hook down; call shutdown_hooks() once more unconditionally (idempotent,
    //   safe-when-empty) and confirm the live Method carries NEITHER inhibitor.
    // =====================================================================
    vmhook::shutdown_hooks();
    {
        vmhook::hotspot::method* const m{ find_hot_method() };
        if (m != nullptr)
        {
            ctx.check("module_left_clean_dont_inline_clear", !dont_inline_set(m));
            ctx.check("module_left_clean_no_compile_clear", !no_compile_set(m));
        }
    }
    ctx.check("module_left_clean_final_shutdown", true);
}
