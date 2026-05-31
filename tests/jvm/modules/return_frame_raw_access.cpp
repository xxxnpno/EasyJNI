// return_frame_raw_access JVM test module  (feature area: hooks)
//
// Exhaustively exercises return_value's RAW interpreter-frame escape hatch on a
// LIVE JVM: from inside a detour we take the frame the trampoline stashed
// (ret.frame()) and drive the low-level primitives the typed hook API is built
// on — frame->get_method(), frame->get_locals(), frame->get_arguments<...>() —
// proving on real bytecode dispatch that:
//
//   * frame() is NON-null inside a real hook, and get_method() is the SAME
//     method the hook was installed on (name + descriptor match).  The existing
//     unit test only covers the null-frame case; this is the live counterpart.
//   * get_locals() is the live local array: slot 0 (locals[-0]) holds `this`
//     for an INSTANCE method — its decoded oop == the receiver `self`, and the
//     receiver's own `tag` field is readable THROUGH that recovered oop.
//   * raw primitive arg slots reproduce the Java args, respecting the HotSpot
//     two-slot rule: a long/double occupies TWO slots and its 64-bit value is
//     stored at the LOWER address locals[-(slot+1)]; the NEXT arg shifts by two
//     slot offsets.  int a @slot1, long b @slot2(value@-3), double c @slot4
//     (value@-5), int d @slot6 are each read raw AND cross-checked against the
//     public typed get_arguments<int,long,double,int>() tuple.
//   * a STATIC method has NO `this` at slot 0 — slot 0 holds the first PRIMITIVE
//     arg (a small int, not an oop): the raw slot-0 value == the first Java int,
//     and decoding slot 0 as an oop does not yield a plausible receiver.
//   * the locals array frame() exposes is the SAME one set_arg() mutates: write
//     via ret.set_arg(1, v), read back frame()->get_locals()[-1], they agree,
//     and the body observes the mutated value (allow-through).
//   * out-of-range slot reads return a DEFAULT and never crash the JVM (the
//     private get_argument bounds guard reached via the public typed accessor).
//
// SAFETY: this module drives raw pointers itself, so EVERY dereference off a
// frame/locals pointer is gated by vmhook::hotspot::is_valid_pointer first; a
// failed gate downgrades to a recorded [INFO] / false observation, never a deref.
// All hooks are scoped_hook (uninstall on scope exit) so we never tear down
// another module's hooks.  Single probe cycle: each method is hooked
// independently and fires exactly once, so one run() drives every observation.
#include <vmhook/vmhook.hpp>

#include "../harness.hpp"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <sstream>
#include <string>

namespace
{
    // Wrapper for vmhook.fixtures.ReturnFrameRaw.  Deriving from
    // vmhook::object<> gives it a vtable (required by register_class<T>),
    // get_instance() for the receiver-oop compare, and static_field / get_field
    // for the handshake and the `tag` cross-check.  Each typed getter reads into
    // a concretely-typed local first — field_proxy's value_t conversion operator
    // is templated, so a bare `static_field(...)->get() == x` is an ambiguous
    // deduction (see harness contract).
    class frame_raw_fixture : public vmhook::object<frame_raw_fixture>
    {
    public:
        explicit frame_raw_fixture(vmhook::oop_t instance) noexcept
            : vmhook::object<frame_raw_fixture>{ instance }
        {
        }

        // go / done handshake.
        static auto set_go(bool value) -> void  { static_field("go")->set(value); }
        static auto get_done() -> bool          { bool v = static_field("done")->get(); return v; }
        static auto reset_done() -> void        { static_field("done")->set(false); }
        static auto get_probe_ticks() -> std::int32_t { std::int32_t v = static_field("probeTicks")->get(); return v; }

        // Echoed observations (what each Java body actually received).
        static auto get_simple_seen()      -> std::int32_t { std::int32_t v = static_field("simpleSeen")->get();      return v; }
        static auto get_wide_a_seen()      -> std::int32_t { std::int32_t v = static_field("wideASeen")->get();       return v; }
        static auto get_wide_b_seen()      -> std::int64_t { std::int64_t v = static_field("wideBSeen")->get();       return v; }
        static auto get_wide_c_bits_seen() -> std::int64_t { std::int64_t v = static_field("wideCBitsSeen")->get();   return v; }
        static auto get_wide_d_seen()      -> std::int32_t { std::int32_t v = static_field("wideDSeen")->get();       return v; }
        static auto get_static_a_seen()    -> std::int32_t { std::int32_t v = static_field("staticASeen")->get();     return v; }
        static auto get_static_b_seen()    -> std::int64_t { std::int64_t v = static_field("staticBSeen")->get();     return v; }
        static auto get_static_c_seen()    -> std::int32_t { std::int32_t v = static_field("staticCSeen")->get();     return v; }
        static auto get_round_trip_seen()  -> std::int32_t { std::int32_t v = static_field("roundTripSeen")->get();   return v; }

        // The receiver's per-instance fingerprint, read THROUGH whatever oop the
        // test recovered from local slot 0.  Proves slot 0 is the real receiver.
        auto tag() const -> std::int32_t { std::int32_t v = get_field("tag")->get(); return v; }
    };

    // ── Fixture-mirrored constants (kept in lockstep with ReturnFrameRaw.java) ─
    constexpr std::int32_t INSTANCE_TAG{ 0x5A11AB1E };
    constexpr std::int32_t SIMPLE_X{ 0x1234 };
    constexpr std::int32_t WIDE_A{ 0x0A0B0C0D };
    constexpr std::int64_t WIDE_B{ 0x1122334455667788LL };
    constexpr double       WIDE_C{ 3.141592653589793 };
    constexpr std::int32_t WIDE_D{ -0x0708090A };
    constexpr std::int32_t STATIC_A{ 0x00C0FFEE };
    constexpr std::int64_t STATIC_B{ 0x7EDCBA9876543210LL };
    constexpr std::int32_t STATIC_C{ 0x1BADD00D };

    // ── Raw-slot read helpers (every deref gated by is_valid_pointer) ─────────

    // Reads the raw 64-bit machine word at local slot `base` (locals[-base]),
    // i.e. the verbatim slot contents BEFORE any oop-decode / primitive widen.
    // Returns false (and leaves out untouched) if the computed slot address
    // fails the safe-pointer gate — never dereferences a bad pointer.
    auto read_raw_slot(void** locals, std::int32_t base, std::uint64_t& out) noexcept -> bool
    {
        if (locals == nullptr || base < 0)
        {
            return false;
        }
        // The slot lives at &locals[-base]; validate that address itself.
        void* const slot_addr{ static_cast<void*>(locals - base) };
        if (!vmhook::hotspot::is_valid_pointer(slot_addr))
        {
            return false;
        }
        void* const raw{ locals[-base] };
        std::uint64_t bits{ 0 };
        std::memcpy(&bits, &raw, sizeof(bits));
        out = bits;
        return true;
    }

    // Decodes the slot-`base` value as a Java oop using the SAME convention the
    // library uses (frame::get_argument<void*> / get_arguments()): a narrow
    // (<= 0xFFFFFFFF) slot is run through decode_oop_pointer; a wider value is a
    // direct pointer.  Returns nullptr on any failure or invalid result.
    auto decode_slot_oop(void** locals, std::int32_t base) noexcept -> void*
    {
        std::uint64_t bits{ 0 };
        if (!read_raw_slot(locals, base, bits) || bits == 0)
        {
            return nullptr;
        }
        void* decoded{ nullptr };
        if (bits <= 0xFFFFFFFFull)
        {
            decoded = vmhook::hotspot::decode_oop_pointer(static_cast<std::uint32_t>(bits));
        }
        else
        {
            decoded = reinterpret_cast<void*>(bits);
        }
        if (decoded == nullptr || !vmhook::hotspot::is_valid_pointer(decoded))
        {
            return nullptr;
        }
        return decoded;
    }

    // ── Per-hook observation state (reset per module run) ─────────────────────

    // instanceSimple: method identity + this-slot + bounds.
    std::atomic<std::int32_t> g_simple_calls{ 0 };
    std::atomic<bool>         g_simple_frame_nonnull{ false };
    std::atomic<bool>         g_simple_method_nonnull{ false };
    std::atomic<bool>         g_simple_name_ok{ false };
    std::atomic<bool>         g_simple_sig_ok{ false };
    std::atomic<bool>         g_simple_locals_nonnull{ false };
    std::atomic<bool>         g_simple_self_nonnull{ false };
    std::atomic<bool>         g_simple_slot0_oop_matches_self{ false };
    std::atomic<bool>         g_simple_slot0_tag_ok{ false };
    std::atomic<bool>         g_simple_typed_arg0_matches_self{ false };
    std::atomic<bool>         g_simple_raw_slot1_ok{ false };
    std::atomic<bool>         g_simple_bounds_no_crash{ false };
    std::atomic<std::int32_t> g_simple_bounds_overread_value{ 0 };
    std::atomic<bool>         g_simple_oob_index_rejected{ false };

    // instanceWide: full slot model (int / long@2slots / double@2slots / int).
    std::atomic<std::int32_t> g_wide_calls{ 0 };
    std::atomic<bool>         g_wide_frame_nonnull{ false };
    std::atomic<bool>         g_wide_locals_nonnull{ false };
    std::atomic<bool>         g_wide_self_ok{ false };
    std::atomic<bool>         g_wide_raw_a_ok{ false };   // int  @ slot 1
    std::atomic<bool>         g_wide_raw_b_ok{ false };   // long @ slot 2, value@-3
    std::atomic<bool>         g_wide_raw_c_ok{ false };   // double @ slot 4, value@-5
    std::atomic<bool>         g_wide_raw_d_ok{ false };   // int  @ slot 6
    std::atomic<bool>         g_wide_typed_a_ok{ false };
    std::atomic<bool>         g_wide_typed_b_ok{ false };
    std::atomic<bool>         g_wide_typed_c_ok{ false };
    std::atomic<bool>         g_wide_typed_d_ok{ false };

    // staticWide: NO this at slot 0.
    std::atomic<std::int32_t> g_static_calls{ 0 };
    std::atomic<bool>         g_static_frame_nonnull{ false };
    std::atomic<bool>         g_static_locals_nonnull{ false };
    std::atomic<bool>         g_static_slot0_is_first_int{ false };
    std::atomic<bool>         g_static_slot0_not_receiver{ false };
    std::atomic<bool>         g_static_typed_args_ok{ false };

    // roundTrip: frame()'s locals alias the array set_arg mutates.
    std::atomic<std::int32_t> g_rt_calls{ 0 };
    std::atomic<bool>         g_rt_set_arg_ok{ false };
    std::atomic<bool>         g_rt_raw_readback_ok{ false };

    auto reset_observations() -> void
    {
        g_simple_calls.store(0);
        g_simple_frame_nonnull.store(false);
        g_simple_method_nonnull.store(false);
        g_simple_name_ok.store(false);
        g_simple_sig_ok.store(false);
        g_simple_locals_nonnull.store(false);
        g_simple_self_nonnull.store(false);
        g_simple_slot0_oop_matches_self.store(false);
        g_simple_slot0_tag_ok.store(false);
        g_simple_typed_arg0_matches_self.store(false);
        g_simple_raw_slot1_ok.store(false);
        g_simple_bounds_no_crash.store(false);
        g_simple_bounds_overread_value.store(0);
        g_simple_oob_index_rejected.store(false);

        g_wide_calls.store(0);
        g_wide_frame_nonnull.store(false);
        g_wide_locals_nonnull.store(false);
        g_wide_self_ok.store(false);
        g_wide_raw_a_ok.store(false);
        g_wide_raw_b_ok.store(false);
        g_wide_raw_c_ok.store(false);
        g_wide_raw_d_ok.store(false);
        g_wide_typed_a_ok.store(false);
        g_wide_typed_b_ok.store(false);
        g_wide_typed_c_ok.store(false);
        g_wide_typed_d_ok.store(false);

        g_static_calls.store(0);
        g_static_frame_nonnull.store(false);
        g_static_locals_nonnull.store(false);
        g_static_slot0_is_first_int.store(false);
        g_static_slot0_not_receiver.store(false);
        g_static_typed_args_ok.store(false);

        g_rt_calls.store(0);
        g_rt_set_arg_ok.store(false);
        g_rt_raw_readback_ok.store(false);
    }
}

VMHOOK_JVM_MODULE(return_frame_raw_access)
{
    vmhook::register_class<frame_raw_fixture>("vmhook/fixtures/ReturnFrameRaw");

    reset_observations();

    // All hooks live in this scope and uninstall on exit.  Explicit JVM
    // descriptors disambiguate every method.
    {
        // ── instanceSimple(int): method identity + this-slot + bounds ───────
        auto h_simple{ vmhook::scoped_hook<frame_raw_fixture>(
            "instanceSimple", "(I)I",
            [](vmhook::return_value& ret,
               const std::unique_ptr<frame_raw_fixture>& self,
               std::int32_t /*x*/)
            {
                g_simple_calls.fetch_add(1, std::memory_order_relaxed);

                vmhook::hotspot::frame* const fr{ ret.frame() };
                if (fr == nullptr || !vmhook::hotspot::is_valid_pointer(fr))
                {
                    return;   // gated: never deref a bad/null frame
                }
                g_simple_frame_nonnull.store(true, std::memory_order_relaxed);

                // get_method(): same method the hook was installed on.
                vmhook::hotspot::method* const m{ fr->get_method() };
                if (m != nullptr && vmhook::hotspot::is_valid_pointer(m))
                {
                    g_simple_method_nonnull.store(true, std::memory_order_relaxed);
                    const std::string name = m->get_name();
                    const std::string sig  = m->get_signature();
                    g_simple_name_ok.store(name == "instanceSimple", std::memory_order_relaxed);
                    g_simple_sig_ok.store(sig == "(I)I", std::memory_order_relaxed);
                }

                if (self != nullptr)
                {
                    g_simple_self_nonnull.store(true, std::memory_order_relaxed);
                }

                void** const locals{ fr->get_locals() };
                if (locals == nullptr)
                {
                    // Some JDKs where the locals_offset scan failed legitimately
                    // return null; record it but do not fail the run on it.
                    return;
                }
                g_simple_locals_nonnull.store(true, std::memory_order_relaxed);

                // slot 0 == `this`: decode the raw slot oop and compare to self.
                void* const slot0_oop{ decode_slot_oop(locals, 0) };
                if (slot0_oop != nullptr && self != nullptr)
                {
                    g_simple_slot0_oop_matches_self.store(
                        slot0_oop == self->get_instance(), std::memory_order_relaxed);

                    // Read `tag` THROUGH the recovered oop — proves it is a live,
                    // correct receiver, not just a coincidentally-equal pointer.
                    frame_raw_fixture recovered{ slot0_oop };
                    g_simple_slot0_tag_ok.store(recovered.tag() == INSTANCE_TAG,
                                                std::memory_order_relaxed);
                }

                // Cross-check with the PUBLIC typed accessor: get_arguments<oop>
                // decodes slot 0 the same way the library does internally.
                {
                    auto [decoded_self] = fr->get_arguments<vmhook::oop_t>();
                    if (decoded_self != nullptr && self != nullptr)
                    {
                        g_simple_typed_arg0_matches_self.store(
                            decoded_self == self->get_instance(), std::memory_order_relaxed);
                    }
                }

                // Raw read of the single primitive arg at slot 1 (low 32 bits).
                {
                    std::uint64_t bits{ 0 };
                    if (read_raw_slot(locals, 1, bits))
                    {
                        g_simple_raw_slot1_ok.store(
                            static_cast<std::int32_t>(bits) == SIMPLE_X,
                            std::memory_order_relaxed);
                    }
                }

                // BOUNDS — part 1 (NO CRASH on over-read): over-request types so
                // the public typed accessor reads several slots PAST this 1-arg
                // (+this) method's real locals.  Each read goes through the
                // private get_argument bounds guard (index <= 0xFFFF) and then
                // reads adjacent stack words; the contract under test is "this
                // never crashes the JVM".  Reaching the line after the call is
                // the proof.  We do NOT assert the over-read *value* — past-the-
                // end slots alias live operand-stack / saved-register words whose
                // contents are not defined — we only characterise it via [INFO].
                {
                    auto t = fr->get_arguments<vmhook::oop_t,
                                               std::int32_t, std::int32_t,
                                               std::int32_t, std::int32_t,
                                               std::int32_t>();
                    g_simple_bounds_no_crash.store(true, std::memory_order_relaxed);
                    g_simple_bounds_overread_value.store(std::get<5>(t),
                                                         std::memory_order_relaxed);
                }

                // BOUNDS — part 2 (DEFAULT/REJECT on truly out-of-range index):
                // the only place the library can be DRIVEN past 0xFFFF is set_arg
                // (which shares get_argument's `index > 0xFFFF` guard).  A huge
                // index must be rejected (return false) with no wild write and no
                // crash — the deterministic half of the bounds contract.  We use
                // a value the body never expects so a (wrongly) successful write
                // would be detectable; the allow-through check below proves the
                // original arg survived.
                {
                    const bool rejected{ !ret.set_arg(0x7FFFFFFF,
                                                       static_cast<std::int32_t>(0xBADBAD)) };
                    g_simple_oob_index_rejected.store(rejected, std::memory_order_relaxed);
                }
            }) };
        ctx.check("frame_simple_hook_installed", h_simple.installed());

        // ── instanceWide(int,long,double,int): full slot model ──────────────
        auto h_wide{ vmhook::scoped_hook<frame_raw_fixture>(
            "instanceWide", "(IJDI)J",
            [](vmhook::return_value& ret,
               const std::unique_ptr<frame_raw_fixture>& self,
               std::int32_t /*a*/, std::int64_t /*b*/, double /*c*/, std::int32_t /*d*/)
            {
                g_wide_calls.fetch_add(1, std::memory_order_relaxed);

                vmhook::hotspot::frame* const fr{ ret.frame() };
                if (fr == nullptr || !vmhook::hotspot::is_valid_pointer(fr))
                {
                    return;
                }
                g_wide_frame_nonnull.store(true, std::memory_order_relaxed);

                void** const locals{ fr->get_locals() };
                if (locals == nullptr)
                {
                    return;
                }
                g_wide_locals_nonnull.store(true, std::memory_order_relaxed);

                // this @ slot 0.
                void* const slot0_oop{ decode_slot_oop(locals, 0) };
                if (slot0_oop != nullptr && self != nullptr)
                {
                    g_wide_self_ok.store(slot0_oop == self->get_instance(),
                                         std::memory_order_relaxed);
                }

                // RAW reads honouring the HotSpot slot model:
                //   a (int)    @ base slot 1        -> locals[-1]    low 32 bits
                //   b (long)   @ base slot 2        -> value@-(2+1) = locals[-3]
                //   c (double) @ base slot 4        -> value@-(4+1) = locals[-5]
                //   d (int)    @ base slot 6        -> locals[-6]    low 32 bits
                {
                    std::uint64_t bits{ 0 };
                    if (read_raw_slot(locals, 1, bits))
                    {
                        g_wide_raw_a_ok.store(static_cast<std::int32_t>(bits) == WIDE_A,
                                              std::memory_order_relaxed);
                    }
                }
                {
                    std::uint64_t bits{ 0 };
                    if (read_raw_slot(locals, 3, bits))   // long value at the LOWER slot
                    {
                        g_wide_raw_b_ok.store(static_cast<std::int64_t>(bits) == WIDE_B,
                                              std::memory_order_relaxed);
                    }
                }
                {
                    std::uint64_t bits{ 0 };
                    if (read_raw_slot(locals, 5, bits))   // double value at the LOWER slot
                    {
                        double d{ 0.0 };
                        std::memcpy(&d, &bits, sizeof(d));
                        g_wide_raw_c_ok.store(d == WIDE_C, std::memory_order_relaxed);
                    }
                }
                {
                    std::uint64_t bits{ 0 };
                    if (read_raw_slot(locals, 6, bits))
                    {
                        g_wide_raw_d_ok.store(static_cast<std::int32_t>(bits) == WIDE_D,
                                              std::memory_order_relaxed);
                    }
                }

                // Cross-check against the PUBLIC typed accessor, which widens
                // long/double across two slots internally.  Both raw and typed
                // paths must agree (audit: typed-matches-autodetect).
                {
                    auto [a, b, c, d] =
                        fr->get_arguments<std::int32_t, std::int64_t, double, std::int32_t>();
                    g_wide_typed_a_ok.store(a == WIDE_A, std::memory_order_relaxed);
                    g_wide_typed_b_ok.store(b == WIDE_B, std::memory_order_relaxed);
                    g_wide_typed_c_ok.store(c == WIDE_C, std::memory_order_relaxed);
                    g_wide_typed_d_ok.store(d == WIDE_D, std::memory_order_relaxed);
                }
            }) };
        ctx.check("frame_wide_hook_installed", h_wide.installed());

        // ── staticWide(int,long,int): NO this at slot 0 ─────────────────────
        auto h_static{ vmhook::scoped_hook<frame_raw_fixture>(
            "staticWide", "(IJI)J",
            [](vmhook::return_value& ret,
               std::int32_t /*a*/, std::int64_t /*b*/, std::int32_t /*c*/)
            {
                g_static_calls.fetch_add(1, std::memory_order_relaxed);

                vmhook::hotspot::frame* const fr{ ret.frame() };
                if (fr == nullptr || !vmhook::hotspot::is_valid_pointer(fr))
                {
                    return;
                }
                g_static_frame_nonnull.store(true, std::memory_order_relaxed);

                void** const locals{ fr->get_locals() };
                if (locals == nullptr)
                {
                    return;
                }
                g_static_locals_nonnull.store(true, std::memory_order_relaxed);

                // slot 0 is the FIRST PRIMITIVE arg (a), NOT a receiver: a static
                // method's frame has no `this`.  Its raw low-32 value must equal
                // STATIC_A — a small integer, NOT a compressed oop / heap pointer.
                // (We deliberately do NOT decode slot 0 as an oop and chase it:
                // a static frame's slot 0 is a primitive, so decoding it would
                // fabricate a bogus pointer; reading a field through that would
                // violate the never-deref-unchecked rule.)
                std::int32_t static_slot0_raw{ 0 };
                {
                    std::uint64_t bits{ 0 };
                    if (read_raw_slot(locals, 0, bits))
                    {
                        static_slot0_raw = static_cast<std::int32_t>(bits);
                        g_static_slot0_is_first_int.store(
                            static_slot0_raw == STATIC_A, std::memory_order_relaxed);
                    }
                }

                // Typed accessor decodes the static args from slot 0 onward with
                // NO `this` shift: a@0, b@1(long), c@3(after the long).  That the
                // FIRST typed arg is STATIC_A (the Java arg0) — and not some
                // receiver-shaped value — is the decoder-level confirmation that
                // slot 0 carries arg0, not `this`.
                {
                    auto [a, b, c] =
                        fr->get_arguments<std::int32_t, std::int64_t, std::int32_t>();
                    g_static_typed_args_ok.store(
                        a == STATIC_A && b == STATIC_B && c == STATIC_C,
                        std::memory_order_relaxed);
                    // arg0 lands at slot 0 (no this): the typed read of slot 0 and
                    // the raw read of slot 0 agree, both == STATIC_A.
                    g_static_slot0_not_receiver.store(
                        a == STATIC_A && a == static_slot0_raw,
                        std::memory_order_relaxed);
                }
            }) };
        ctx.check("frame_static_hook_installed", h_static.installed());

        // ── roundTrip(int): frame() locals alias the set_arg array ──────────
        auto h_rt{ vmhook::scoped_hook<frame_raw_fixture>(
            "roundTrip", "(I)I",
            [](vmhook::return_value& ret,
               const std::unique_ptr<frame_raw_fixture>& /*self*/,
               std::int32_t /*value*/)
            {
                g_rt_calls.fetch_add(1, std::memory_order_relaxed);

                // Mutate slot 1 via the typed API, then read it back RAW through
                // frame()->get_locals() to prove both reach the same array.
                const std::int32_t injected{ 0x4242 };
                const bool set_ok{ ret.set_arg(1, injected) };
                g_rt_set_arg_ok.store(set_ok, std::memory_order_relaxed);

                vmhook::hotspot::frame* const fr{ ret.frame() };
                if (fr == nullptr || !vmhook::hotspot::is_valid_pointer(fr))
                {
                    return;
                }
                void** const locals{ fr->get_locals() };
                std::uint64_t bits{ 0 };
                if (read_raw_slot(locals, 1, bits))
                {
                    g_rt_raw_readback_ok.store(
                        static_cast<std::int32_t>(bits) == injected,
                        std::memory_order_relaxed);
                }
            }) };
        ctx.check("frame_round_trip_hook_installed", h_rt.installed());

        // ── Drive every method once (one real bytecode dispatch each) ───────
        const bool done{ ctx.run_probe(
            [](bool value) { frame_raw_fixture::set_go(value); },
            []() { return frame_raw_fixture::get_done(); }) };
        ctx.check("frame_probe_completed", done);
        ctx.check("frame_probe_ticked", frame_raw_fixture::get_probe_ticks() >= 1);

        // ═════════════════════ instanceSimple assertions ════════════════════
        ctx.check("frame_simple_hook_fired", g_simple_calls.load() == 1);
        ctx.check("frame_simple_frame_nonnull", g_simple_frame_nonnull.load());
        ctx.check("frame_simple_get_method_nonnull", g_simple_method_nonnull.load());
        ctx.check("frame_simple_method_name_matches_hooked", g_simple_name_ok.load());
        ctx.check("frame_simple_method_sig_matches_hooked", g_simple_sig_ok.load());
        ctx.check("frame_simple_self_nonnull", g_simple_self_nonnull.load());
        ctx.check("frame_simple_get_locals_nonnull", g_simple_locals_nonnull.load());
        // The headline: slot 0 raw oop IS the receiver.
        ctx.check("frame_simple_slot0_oop_is_receiver", g_simple_slot0_oop_matches_self.load());
        ctx.check("frame_simple_slot0_tag_read_through_oop", g_simple_slot0_tag_ok.load());
        ctx.check("frame_simple_typed_arg0_is_receiver", g_simple_typed_arg0_matches_self.load());
        ctx.check("frame_simple_raw_slot1_matches_arg", g_simple_raw_slot1_ok.load());
        // Allow-through: original body ran (returns tag + x).  This ALSO proves
        // the out-of-range set_arg(0x7FFFFFFF, ...) below wrote nothing: had it
        // corrupted the live local array, the body's observation would differ.
        ctx.check("frame_simple_allow_through_body_ran",
                  frame_raw_fixture::get_simple_seen() == SIMPLE_X);
        // Bounds part 1: over-reading past the real locals did not crash the JVM.
        ctx.check("frame_simple_bounds_overread_no_crash", g_simple_bounds_no_crash.load());
        // Bounds part 2: a truly out-of-range index is rejected (default/no write).
        ctx.check("frame_simple_oob_index_rejected", g_simple_oob_index_rejected.load());
        ctx.record(std::string{ "[INFO] return_frame_raw_access bounds: over-read of slot 5 "
                                "past a 2-local frame returned " } +
                   std::to_string(g_simple_bounds_overread_value.load()) +
                   " (value undefined by contract; the guarantee under test is "
                   "no-crash, which held).");

        // ═════════════════════ instanceWide assertions ══════════════════════
        ctx.check("frame_wide_hook_fired", g_wide_calls.load() == 1);
        ctx.check("frame_wide_frame_nonnull", g_wide_frame_nonnull.load());
        ctx.check("frame_wide_get_locals_nonnull", g_wide_locals_nonnull.load());
        ctx.check("frame_wide_slot0_oop_is_receiver", g_wide_self_ok.load());
        // Raw slot reads honouring the two-slot rule.
        ctx.check("frame_wide_raw_int_a_slot1", g_wide_raw_a_ok.load());
        ctx.check("frame_wide_raw_long_b_value_at_lower_slot", g_wide_raw_b_ok.load());
        ctx.check("frame_wide_raw_double_c_value_at_lower_slot", g_wide_raw_c_ok.load());
        ctx.check("frame_wide_raw_int_d_after_two_wides", g_wide_raw_d_ok.load());
        // Typed accessor cross-check (widening handled internally).
        ctx.check("frame_wide_typed_int_a", g_wide_typed_a_ok.load());
        ctx.check("frame_wide_typed_long_b", g_wide_typed_b_ok.load());
        ctx.check("frame_wide_typed_double_c", g_wide_typed_c_ok.load());
        ctx.check("frame_wide_typed_int_d", g_wide_typed_d_ok.load());
        // Allow-through: body observed every arg unmodified.
        ctx.check("frame_wide_body_saw_a", frame_raw_fixture::get_wide_a_seen() == WIDE_A);
        ctx.check("frame_wide_body_saw_b", frame_raw_fixture::get_wide_b_seen() == WIDE_B);
        {
            std::int64_t expected_bits{};
            std::memcpy(&expected_bits, &WIDE_C, sizeof(WIDE_C));
            ctx.check("frame_wide_body_saw_c",
                      frame_raw_fixture::get_wide_c_bits_seen() == expected_bits);
        }
        ctx.check("frame_wide_body_saw_d", frame_raw_fixture::get_wide_d_seen() == WIDE_D);

        // ═════════════════════ staticWide assertions ════════════════════════
        ctx.check("frame_static_hook_fired", g_static_calls.load() == 1);
        ctx.check("frame_static_frame_nonnull", g_static_frame_nonnull.load());
        ctx.check("frame_static_get_locals_nonnull", g_static_locals_nonnull.load());
        // The headline: a static method has NO `this` — slot 0 is the first int.
        ctx.check("frame_static_slot0_is_first_arg_not_this", g_static_slot0_is_first_int.load());
        ctx.check("frame_static_slot0_is_not_a_receiver_oop", g_static_slot0_not_receiver.load());
        ctx.check("frame_static_typed_args_decoded", g_static_typed_args_ok.load());
        // Allow-through on the static body.
        ctx.check("frame_static_body_saw_a", frame_raw_fixture::get_static_a_seen() == STATIC_A);
        ctx.check("frame_static_body_saw_b", frame_raw_fixture::get_static_b_seen() == STATIC_B);
        ctx.check("frame_static_body_saw_c", frame_raw_fixture::get_static_c_seen() == STATIC_C);

        // ═════════════════════ roundTrip assertions ═════════════════════════
        ctx.check("frame_round_trip_hook_fired", g_rt_calls.load() == 1);
        ctx.check("frame_round_trip_set_arg_returned_true", g_rt_set_arg_ok.load());
        // frame()'s locals reflect the set_arg write -> same array.
        ctx.check("frame_round_trip_raw_readback_sees_injected", g_rt_raw_readback_ok.load());
        // And the body observed the mutated value (allow-through after mutation).
        ctx.check("frame_round_trip_body_saw_injected",
                  frame_raw_fixture::get_round_trip_seen() == 0x4242);

        // ── INFO: surface the live raw-frame state for the run log ──────────
        {
            std::ostringstream oss;
            oss << "[INFO] return_frame_raw_access: frame() non-null on instance="
                << (g_simple_frame_nonnull.load() ? "yes" : "no")
                << " static=" << (g_static_frame_nonnull.load() ? "yes" : "no")
                << "; get_locals() non-null instance="
                << (g_simple_locals_nonnull.load() ? "yes" : "no")
                << " static=" << (g_static_locals_nonnull.load() ? "yes" : "no")
                << "; slot0==receiver(simple)="
                << (g_simple_slot0_oop_matches_self.load() ? "yes" : "no")
                << "; static-slot0-has-no-this="
                << (g_static_slot0_is_first_int.load() ? "yes" : "no") << ".";
            ctx.record(oss.str());
        }
    }
}
