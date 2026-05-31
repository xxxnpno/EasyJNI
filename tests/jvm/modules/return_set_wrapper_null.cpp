// JVM test module for return_value::set_arg(index, <wrapper-or-null>) — the
// OBJECT-WRAPPER and NULL-reference argument-injection branches of set_arg.
// Area: return_value / argument mutation.
//
// The canonical set_arg module (return_set_arg.cpp) covers primitives and the
// String-from-C++-source-type branches.  THIS module covers the two remaining
// set_arg input branches, the object/null ones (vmhook.hpp ~7826-7837):
//
//   * is_unique_object_ptr branch — set_arg(slot, unique_ptr<wrapper>):
//       store_oop(value ? value->get_instance() : nullptr).  A non-empty
//       unique_ptr injects the wrapper's OOP into the slot; an EMPTY unique_ptr
//       injects a null reference.
//   * object_base-by-value branch — set_arg(slot, wrapper):
//       store_oop(value.get_instance()).
//
// ── WHY THIS MODULE WAS QUARANTINED, AND THE CRASH-PROOF DESIGN ───────────────
// A live JDK-21 / Windows build+inject reproduced an EXCEPTION_ACCESS_VIOLATION:
// the JVM died on System.identityHashCode inside an INTERPRETED takeObject body
// (caller Harness.tickAll() C2-compiled — hence the "under C2" report), reading
// address 0x00000000c1e3e3cd.  HotSpot's own hs_err decoded RAX/RBX/RDX = that
// value as "a COMPRESSED pointer to ReturnSetWrapperNull$Donor (tag=3332)".  So
// the injected oop was the RIGHT object, but it was stored COMPRESSED (narrow,
// 4-byte) in the interpreter local slot, and the body dereferenced the 4-byte
// narrow value as a raw 64-bit pointer.
//
// ROOT CAUSE (a genuine vmhook bug, characterized in the REPORT at the bottom —
// vmhook.hpp is NOT edited here): set_arg's store_oop (vmhook.hpp ~7803-7824)
// chooses narrow-vs-wide storage from the PREVIOUS slot value's magnitude:
//   if (previous_bits > 0xFFFFFFFF) store wide; else store encode_oop_pointer(oop).
// A HotSpot interpreter local slot for an object reference holds a FULL 64-bit
// (uncompressed) oop REGARDLESS of heap compression — only the heap stores narrow
// oops.  When the slot being overwritten previously held null (previous_bits==0)
// or any sub-4GB value, store_oop WRONGLY compresses, writing a narrow oop into a
// slot that must hold a wide oop -> the next aload/getfield/native-call AVs.
//
// CONSEQUENCE FOR THE TEST: it is safe to allow the Java body through ONLY when
// the overwritten slot already holds a wide oop (so store_oop's raw-store branch
// fires) — i.e. the ORIGINAL arg in that slot is NON-NULL — or when we inject a
// literal null (which store_oop writes verbatim).  Injecting an object over a
// genuinely-NULL slot, or injecting a wrong-klass oop, is characterized with
// NATIVE, is_valid_pointer-gated slot reads and the body is CANCELLED so Java
// never dereferences a possibly-narrow / wrong-typed slot.
//
// This split keeps the module EXHAUSTIVE (correct-type wrapper inject + body
// read-through, static slot 0, by-value branch, make_unique fresh object, slot-2
// targeting among consecutive refs / after a primitive, real String object
// inject + null String inject, empty-uptr null on instance+static, the
// bug-triggering null-original case, the wrong-type case) while being CRASH-PROOF
// on every JDK/compiler, including under C2.
//
// HARD RULES honoured: never crash the JVM (every raw OOP deref is gated by
// is_valid_pointer; the dangerous cases are cancelled + characterized natively);
// a real vmhook bug is CHARACTERIZED + recorded, never used to fail CI, and
// vmhook.hpp is not edited.  Harness API only; scoped_hook (never shutdown_hooks)
// so no hook is left armed for later modules.  Self-registers via VMHOOK_JVM_MODULE.
#include <vmhook/vmhook.hpp>

#include "../harness.hpp"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>

namespace
{
    // Wrapper for vmhook.fixtures.ReturnSetWrapperNull$Donor — the reference
    // object injected into the hooked methods' argument slots.
    class donor_object : public vmhook::object<donor_object>
    {
    public:
        explicit donor_object(vmhook::oop_t instance) noexcept
            : vmhook::object<donor_object>{ instance }
        {
        }

        auto tag() -> std::int32_t { std::int32_t v = get_field("tag")->get(); return v; }
    };

    // Wrapper for vmhook.fixtures.ReturnSetWrapperNull$Decoy — an UNRELATED Java
    // class.  Used for the cross-type-injection characterization (set_arg accepts
    // it into a Donor-typed slot; no klass check).
    class decoy_object : public vmhook::object<decoy_object>
    {
    public:
        explicit decoy_object(vmhook::oop_t instance) noexcept
            : vmhook::object<decoy_object>{ instance }
        {
        }
    };

    // Wrapper for vmhook.fixtures.ReturnSetWrapperNull.  All observables are
    // static fields, read into a concretely-typed local first (the field_proxy
    // value_t conversion is templated, so a bare comparison would be ambiguous).
    class rsw_fixture : public vmhook::object<rsw_fixture>
    {
    public:
        explicit rsw_fixture(vmhook::oop_t instance) noexcept
            : vmhook::object<rsw_fixture>{ instance }
        {
        }

        // ── go/done/mode handshake ─────────────────────────────────────────
        static auto set_go(bool value)   -> void { static_field("go")->set(value); }
        static auto get_done()           -> bool { bool v = static_field("done")->get(); return v; }
        static auto set_done(bool value) -> void { static_field("done")->set(value); }
        static auto set_mode(std::int32_t m) -> void { static_field("mode")->set(m); }

        static auto get_probe_ticks() -> std::int32_t { std::int32_t v = static_field("probeTicks")->get(); return v; }

        // ── published donor objects + identities ───────────────────────────
        static auto donor()        -> std::unique_ptr<donor_object> { return static_field("DONOR")->get(); }
        static auto byval_donor()  -> std::unique_ptr<donor_object> { return static_field("BYVAL_DONOR")->get(); }
        static auto decoy()        -> std::unique_ptr<decoy_object> { return static_field("DECOY")->get(); }
        static auto string_donor() -> std::unique_ptr<vmhook::object_base> { return static_field("STRING_DONOR_REF")->get(); }

        static auto donor_identity()       -> std::int32_t { std::int32_t v = static_field("donorIdentity")->get();      return v; }
        static auto byval_donor_identity() -> std::int32_t { std::int32_t v = static_field("byvalDonorIdentity")->get(); return v; }
        static auto decoy_identity()       -> std::int32_t { std::int32_t v = static_field("decoyIdentity")->get();      return v; }

        // ── allow-through observations ─────────────────────────────────────
        static auto obj_was_null() -> bool        { bool v = static_field("objWasNull")->get(); return v; }
        static auto obj_identity() -> std::int32_t { std::int32_t v = static_field("objIdentity")->get(); return v; }
        static auto obj_tag()      -> std::int32_t { std::int32_t v = static_field("objTag")->get(); return v; }

        static auto sobj_was_null() -> bool        { bool v = static_field("sObjWasNull")->get(); return v; }
        static auto sobj_identity() -> std::int32_t { std::int32_t v = static_field("sObjIdentity")->get(); return v; }
        static auto sobj_tag()      -> std::int32_t { std::int32_t v = static_field("sObjTag")->get(); return v; }

        static auto null_obj_was_null()  -> bool { bool v = static_field("nullObjWasNull")->get();  return v; }
        static auto null_obj_tag()       -> std::int32_t { std::int32_t v = static_field("nullObjTag")->get(); return v; }
        static auto snull_obj_was_null() -> bool { bool v = static_field("sNullObjWasNull")->get(); return v; }

        static auto byval_was_null() -> bool        { bool v = static_field("byvalWasNull")->get(); return v; }
        static auto byval_identity() -> std::int32_t { std::int32_t v = static_field("byvalIdentity")->get(); return v; }
        static auto byval_tag()      -> std::int32_t { std::int32_t v = static_field("byvalTag")->get(); return v; }

        static auto fresh_was_null() -> bool        { bool v = static_field("freshWasNull")->get(); return v; }
        static auto fresh_tag()      -> std::int32_t { std::int32_t v = static_field("freshTag")->get(); return v; }

        static auto two_first_tag()       -> std::int32_t { std::int32_t v = static_field("twoFirstTag")->get();  return v; }
        static auto two_second_tag()      -> std::int32_t { std::int32_t v = static_field("twoSecondTag")->get(); return v; }
        static auto two_first_was_null()  -> bool { bool v = static_field("twoFirstWasNull")->get();  return v; }
        static auto two_second_was_null() -> bool { bool v = static_field("twoSecondWasNull")->get(); return v; }

        static auto mixed_n()           -> std::int32_t { std::int32_t v = static_field("mixedN")->get();      return v; }
        static auto mixed_obj_tag()     -> std::int32_t { std::int32_t v = static_field("mixedObjTag")->get(); return v; }
        static auto mixed_obj_was_null()-> bool { bool v = static_field("mixedObjWasNull")->get(); return v; }

        static auto str_was_null() -> bool        { bool v = static_field("strWasNull")->get(); return v; }
        static auto str_len()      -> std::int32_t { std::int32_t v = static_field("strLen")->get(); return v; }
        static auto str_seen()     -> std::string { std::string s = static_field("strSeen")->get(); return s; }

        // ── cancel-only witnesses (must remain false — body never ran) ─────
        static auto over_null_body_ran() -> bool { bool v = static_field("overNullBodyRan")->get(); return v; }
        static auto wrong_body_ran()     -> bool { bool v = static_field("wrongBodyRan")->get(); return v; }
    };

    // Mirrored fixture constants (kept in lockstep with ReturnSetWrapperNull.java).
    constexpr std::int32_t DONOR_TAG       = 0x0D04;    // 3332
    constexpr std::int32_t BYVAL_DONOR_TAG = 0x0BABE;   // 48318
    constexpr std::int32_t FRESH_DONOR_TAG = 0x7E57;    // 32343
    const std::string      STRING_DONOR    = "injected-object-string";

    // ── per-hook observation state (captured in the detours) ────────────────

    // Allow-through: takeObject (unique_ptr, slot 1).
    std::atomic<bool>           g_obj_set_ok{ false };
    std::atomic<bool>           g_obj_donor_resolved{ false };
    std::atomic<bool>           g_obj_self_ok{ false };

    // takeObjectStatic (unique_ptr, slot 0)
    std::atomic<bool>           g_sobj_set_ok{ false };

    // takeObjectNull / static (empty uptr)
    std::atomic<bool> g_nullobj_set_ok{ false };
    std::atomic<bool> g_snullobj_set_ok{ false };

    // takeByVal (object_base-by-value)
    std::atomic<bool>           g_byval_set_ok{ false };

    // takeFresh (make_unique)
    std::atomic<bool> g_fresh_made{ false };
    std::atomic<bool> g_fresh_set_ok{ false };

    // takeTwoObjects / takeMixedObject
    std::atomic<bool> g_two_b_set_ok{ false };
    std::atomic<bool> g_mixed_set_ok{ false };

    // takeString (object inject) — round 1
    std::atomic<bool> g_strobj_set_ok{ false };
    std::atomic<bool> g_strobj_donor_resolved{ false };

    // takeString (null inject) — round 2
    std::atomic<bool> g_strnull_set_ok{ false };

    // Cancel-only: takeOverNull (the bug-triggering null-original case).  The
    // detour reads the slot NATIVELY after injection (gated), then cancels.
    std::atomic<bool>           g_overnull_set_ok{ false };
    std::atomic<bool>           g_overnull_donor_resolved{ false };
    std::atomic<std::uintptr_t> g_overnull_expected_oop{ 0 };   // donor->get_instance()
    std::atomic<std::uintptr_t> g_overnull_slot_decoded{ 0 };   // native gated decode of slot 1
    std::atomic<bool>           g_overnull_slot_valid{ false }; // decoded oop passed is_valid_pointer
    std::atomic<bool>           g_overnull_cancelled{ false };

    // Cancel-only: takeWrongType (Decoy into a Donor slot).
    std::atomic<bool>           g_wrong_set_ok{ false };
    std::atomic<std::uintptr_t> g_wrong_expected_oop{ 0 };      // decoy->get_instance()
    std::atomic<std::uintptr_t> g_wrong_slot_decoded{ 0 };
    std::atomic<bool>           g_wrong_slot_valid{ false };
    std::atomic<bool>           g_wrong_cancelled{ false };

    auto as_uptr(void* p) -> std::uintptr_t { return reinterpret_cast<std::uintptr_t>(p); }

    // Native, fully-gated decode of an object reference held in interpreter local
    // slot `base` (locals[-base]), using the SAME width convention the library's
    // read path uses (frame::get_argument): a narrow (<= 0xFFFFFFFF) slot is run
    // through decode_oop_pointer; a wider value is a direct pointer.  Returns
    // nullptr (never dereferences) on any invalid pointer along the way.  This is
    // exactly the safe surface return_frame_raw_access.cpp uses; it lets us
    // characterize a slot WITHOUT a Java aload/getfield (which would crash on a
    // narrow-oop slot).
    auto decode_slot_oop_gated(vmhook::hotspot::frame* const frame, const std::int32_t base) noexcept -> void*
    {
        if (!frame || !vmhook::hotspot::is_valid_pointer(frame))
        {
            return nullptr;
        }
        void** const locals{ frame->get_locals() };
        if (!locals || !vmhook::hotspot::is_valid_pointer(static_cast<void*>(locals - base)))
        {
            return nullptr;
        }
        void* const raw{ locals[-base] };
        const std::uintptr_t bits{ reinterpret_cast<std::uintptr_t>(raw) };
        void* decoded{ nullptr };
        if (bits == 0)
        {
            return nullptr;
        }
        else if (bits <= 0xFFFFFFFFull)
        {
            decoded = vmhook::hotspot::decode_oop_pointer(static_cast<std::uint32_t>(bits));
        }
        else
        {
            decoded = raw;
        }
        if (decoded == nullptr || !vmhook::hotspot::is_valid_pointer(decoded))
        {
            return nullptr;
        }
        return decoded;
    }
}

VMHOOK_JVM_MODULE(return_set_wrapper_null)
{
    vmhook::register_class<rsw_fixture>("vmhook/fixtures/ReturnSetWrapperNull");
    vmhook::register_class<donor_object>("vmhook/fixtures/ReturnSetWrapperNull$Donor");
    vmhook::register_class<decoy_object>("vmhook/fixtures/ReturnSetWrapperNull$Decoy");

    // =====================================================================
    // ROUND 1 — every object/null injection except the takeString NULL case
    // (round 2), in one probe (one bytecode dispatch per method).  All hooks
    // are scoped to this block and uninstall on exit, so this module never
    // tears down another module's hooks (scoped_hook, never shutdown_hooks).
    // =====================================================================
    {
        // ── ALLOW-THROUGH: takeObject(Donor sentinel) slot 1 -> DONOR ──────
        // The probe passes a NON-NULL sentinel, so slot 1 already holds a wide
        // OOP; store_oop takes its raw-store branch and writes the DONOR's wide
        // OOP -> the body safely reads tag + identity through the injection.
        auto h_obj{ vmhook::scoped_hook<rsw_fixture>(
            "takeObject", "(Lvmhook/fixtures/ReturnSetWrapperNull$Donor;)V",
            [](vmhook::return_value& ret,
               const std::unique_ptr<rsw_fixture>& self,
               const std::unique_ptr<donor_object>& /*incoming(sentinel)*/)
            {
                g_obj_self_ok.store(self != nullptr, std::memory_order_relaxed);
                std::unique_ptr<donor_object> d{ rsw_fixture::donor() };
                g_obj_donor_resolved.store(d != nullptr, std::memory_order_relaxed);
                if (d)
                {
                    g_obj_set_ok.store(ret.set_arg(1, std::move(d)), std::memory_order_relaxed);
                }
            }) };
        ctx.check("rsw_obj_hook_installed", h_obj.installed());

        // ── ALLOW-THROUGH: takeObjectStatic(Donor sentinel) slot 0 -> DONOR ─
        auto h_sobj{ vmhook::scoped_hook<rsw_fixture>(
            "takeObjectStatic", "(Lvmhook/fixtures/ReturnSetWrapperNull$Donor;)V",
            [](vmhook::return_value& ret,
               const std::unique_ptr<donor_object>& /*incoming(sentinel)*/)
            {
                std::unique_ptr<donor_object> d{ rsw_fixture::donor() };
                if (d)
                {
                    g_sobj_set_ok.store(ret.set_arg(0, std::move(d)), std::memory_order_relaxed);
                }
            }) };
        ctx.check("rsw_sobj_hook_installed", h_sobj.installed());

        // ── ALLOW-THROUGH: takeByVal(Donor sentinel) slot 1, by-value branch ─
        auto h_byval{ vmhook::scoped_hook<rsw_fixture>(
            "takeByVal", "(Lvmhook/fixtures/ReturnSetWrapperNull$Donor;)V",
            [](vmhook::return_value& ret,
               const std::unique_ptr<rsw_fixture>&,
               const std::unique_ptr<donor_object>& /*incoming(sentinel)*/)
            {
                std::unique_ptr<donor_object> bd{ rsw_fixture::byval_donor() };
                if (bd && bd->get_instance())
                {
                    donor_object by_value{ bd->get_instance() };   // wrapper by value
                    g_byval_set_ok.store(ret.set_arg(1, by_value), std::memory_order_relaxed);
                }
            }) };
        ctx.check("rsw_byval_hook_installed", h_byval.installed());

        // ── ALLOW-THROUGH: takeFresh(Donor sentinel) slot 1, make_unique ───
        auto h_fresh{ vmhook::scoped_hook<rsw_fixture>(
            "takeFresh", "(Lvmhook/fixtures/ReturnSetWrapperNull$Donor;)V",
            [](vmhook::return_value& ret,
               const std::unique_ptr<rsw_fixture>&,
               const std::unique_ptr<donor_object>& /*incoming(sentinel)*/)
            {
                std::unique_ptr<donor_object> fresh{
                    vmhook::make_unique<donor_object>(static_cast<std::int32_t>(FRESH_DONOR_TAG)) };
                g_fresh_made.store(fresh != nullptr, std::memory_order_relaxed);
                if (fresh)
                {
                    g_fresh_set_ok.store(ret.set_arg(1, std::move(fresh)), std::memory_order_relaxed);
                }
            }) };
        ctx.check("rsw_fresh_hook_installed", h_fresh.installed());

        // ── ALLOW-THROUGH: takeTwoObjects(Donor a, Donor sentinel) -> DONOR@2 ─
        auto h_two{ vmhook::scoped_hook<rsw_fixture>(
            "takeTwoObjects",
            "(Lvmhook/fixtures/ReturnSetWrapperNull$Donor;Lvmhook/fixtures/ReturnSetWrapperNull$Donor;)V",
            [](vmhook::return_value& ret,
               const std::unique_ptr<rsw_fixture>&,
               const std::unique_ptr<donor_object>& /*a(Donor 11)*/,
               const std::unique_ptr<donor_object>& /*b(sentinel)*/)
            {
                std::unique_ptr<donor_object> d{ rsw_fixture::donor() };
                if (d)
                {
                    g_two_b_set_ok.store(ret.set_arg(2, std::move(d)), std::memory_order_relaxed);
                }
            }) };
        ctx.check("rsw_two_hook_installed", h_two.installed());

        // ── ALLOW-THROUGH: takeMixedObject(int n, Donor sentinel) -> DONOR@2 ─
        auto h_mixed{ vmhook::scoped_hook<rsw_fixture>(
            "takeMixedObject", "(ILvmhook/fixtures/ReturnSetWrapperNull$Donor;)V",
            [](vmhook::return_value& ret,
               const std::unique_ptr<rsw_fixture>&,
               std::int32_t /*n*/,
               const std::unique_ptr<donor_object>& /*x(sentinel)*/)
            {
                std::unique_ptr<donor_object> d{ rsw_fixture::donor() };
                if (d)
                {
                    g_mixed_set_ok.store(ret.set_arg(2, std::move(d)), std::memory_order_relaxed);
                }
            }) };
        ctx.check("rsw_mixed_hook_installed", h_mixed.installed());

        // ── ALLOW-THROUGH: takeString(String "before") slot 1 -> String donor ─
        // The published String donor is a 'L java/lang/String' reference; the
        // original arg is the non-null "before" (wide OOP), so the inject is safe.
        auto h_strobj{ vmhook::scoped_hook<rsw_fixture>(
            "takeString", "(Ljava/lang/String;)V",
            [](vmhook::return_value& ret,
               const std::unique_ptr<rsw_fixture>&,
               const std::string& /*incoming "before"*/)
            {
                std::unique_ptr<vmhook::object_base> s{ rsw_fixture::string_donor() };
                g_strobj_donor_resolved.store(s != nullptr, std::memory_order_relaxed);
                if (s)
                {
                    g_strobj_set_ok.store(ret.set_arg(1, std::move(s)), std::memory_order_relaxed);
                }
            }) };
        ctx.check("rsw_strobj_hook_installed", h_strobj.installed());

        // ── ALLOW-THROUGH (null inject): takeObjectNull(Donor DONOR) slot 1 ─
        // Empty unique_ptr drives store_oop(nullptr) -> literal null (always safe).
        auto h_nullobj{ vmhook::scoped_hook<rsw_fixture>(
            "takeObjectNull", "(Lvmhook/fixtures/ReturnSetWrapperNull$Donor;)V",
            [](vmhook::return_value& ret,
               const std::unique_ptr<rsw_fixture>&,
               const std::unique_ptr<donor_object>& /*incoming(DONOR)*/)
            {
                std::unique_ptr<donor_object> empty{};
                g_nullobj_set_ok.store(ret.set_arg(1, std::move(empty)), std::memory_order_relaxed);
            }) };
        ctx.check("rsw_nullobj_hook_installed", h_nullobj.installed());

        // ── ALLOW-THROUGH (null inject): takeObjectNullStatic(Donor) slot 0 ─
        auto h_snullobj{ vmhook::scoped_hook<rsw_fixture>(
            "takeObjectNullStatic", "(Lvmhook/fixtures/ReturnSetWrapperNull$Donor;)V",
            [](vmhook::return_value& ret,
               const std::unique_ptr<donor_object>& /*incoming(DONOR)*/)
            {
                std::unique_ptr<donor_object> empty{};
                g_snullobj_set_ok.store(ret.set_arg(0, std::move(empty)), std::memory_order_relaxed);
            }) };
        ctx.check("rsw_snullobj_hook_installed", h_snullobj.installed());

        // ── CANCEL-ONLY: takeOverNull(Donor null) — THE BUG-TRIGGERING CASE ─
        // The probe passes null, so set_arg overwrites a NULL slot: store_oop
        // encodes the DONOR as a NARROW oop and stores it (the corruption).  We
        // (1) record set_arg's return, (2) read the slot back NATIVELY (gated)
        // and decode it the way the library's read path would, to prove the
        // injected DONOR really landed (slot decode == donor oop), then (3)
        // CANCEL so the body never dereferences the narrow-oop slot.
        auto h_overnull{ vmhook::scoped_hook<rsw_fixture>(
            "takeOverNull", "(Lvmhook/fixtures/ReturnSetWrapperNull$Donor;)V",
            [](vmhook::return_value& ret,
               const std::unique_ptr<rsw_fixture>&,
               const std::unique_ptr<donor_object>& /*incoming(null)*/)
            {
                std::unique_ptr<donor_object> d{ rsw_fixture::donor() };
                g_overnull_donor_resolved.store(d != nullptr, std::memory_order_relaxed);
                if (d)
                {
                    g_overnull_expected_oop.store(as_uptr(d->get_instance()), std::memory_order_relaxed);
                    g_overnull_set_ok.store(ret.set_arg(1, std::move(d)), std::memory_order_relaxed);
                    // Native gated read-back of slot 1 — NEVER a Java deref.
                    void* const decoded{ decode_slot_oop_gated(ret.frame(), 1) };
                    g_overnull_slot_decoded.store(as_uptr(decoded), std::memory_order_relaxed);
                    g_overnull_slot_valid.store(
                        decoded != nullptr && vmhook::hotspot::is_valid_pointer(decoded),
                        std::memory_order_relaxed);
                }
                // CRASH-PROOF: suppress the body so Java never touches slot 1.
                ret.cancel();
                g_overnull_cancelled.store(true, std::memory_order_relaxed);
            }) };
        ctx.check("rsw_overnull_hook_installed", h_overnull.installed());

        // ── CANCEL-ONLY: takeWrongType(Donor sentinel) -> DECOY (wrong type) ─
        // set_arg has no klass check, so the Decoy oop is accepted into the
        // Donor slot.  We characterize the cross-type acceptance NATIVELY (the
        // slot decodes to the Decoy oop) and CANCEL — a getfield/invokevirtual
        // on a Decoy-as-Donor would be UB.
        auto h_wrong{ vmhook::scoped_hook<rsw_fixture>(
            "takeWrongType", "(Lvmhook/fixtures/ReturnSetWrapperNull$Donor;)V",
            [](vmhook::return_value& ret,
               const std::unique_ptr<rsw_fixture>&,
               const std::unique_ptr<donor_object>& /*incoming(sentinel)*/)
            {
                std::unique_ptr<decoy_object> dc{ rsw_fixture::decoy() };
                if (dc && dc->get_instance())
                {
                    g_wrong_expected_oop.store(as_uptr(dc->get_instance()), std::memory_order_relaxed);
                    g_wrong_set_ok.store(ret.set_arg(1, std::move(dc)), std::memory_order_relaxed);
                    void* const decoded{ decode_slot_oop_gated(ret.frame(), 1) };
                    g_wrong_slot_decoded.store(as_uptr(decoded), std::memory_order_relaxed);
                    g_wrong_slot_valid.store(
                        decoded != nullptr && vmhook::hotspot::is_valid_pointer(decoded),
                        std::memory_order_relaxed);
                }
                ret.cancel();
                g_wrong_cancelled.store(true, std::memory_order_relaxed);
            }) };
        ctx.check("rsw_wrong_hook_installed", h_wrong.installed());

        // ── Drive every method once (single real bytecode dispatch each) ───
        const bool done{ ctx.run_probe(
            [](bool value) { if (value) { rsw_fixture::set_done(false); rsw_fixture::set_mode(0); } rsw_fixture::set_go(value); },
            []() { return rsw_fixture::get_done(); }) };
        ctx.check("rsw_probe_completed", done);
        ctx.check("rsw_probe_ticked", rsw_fixture::get_probe_ticks() >= 1);

        // The published donor identities are non-zero (Java actually ran run()).
        ctx.check("rsw_donor_identity_published", rsw_fixture::donor_identity() != 0);
        ctx.check("rsw_byval_donor_identity_published", rsw_fixture::byval_donor_identity() != 0);
        ctx.check("rsw_decoy_identity_published", rsw_fixture::decoy_identity() != 0);

        ctx.record("[INFO] return_set_wrapper_null: allow-through object injections write OVER a "
                   "non-null sentinel Donor (a wide OOP), so store_oop's raw-store branch fires and "
                   "the body safely reads the injected, type-matching object. The bug-triggering "
                   "null-original case (takeOverNull) and the wrong-type case (takeWrongType) are "
                   "characterized NATIVELY and CANCELLED so Java never dereferences a possibly-narrow "
                   "/ wrong-typed slot (see the module's CRASH-PROOF DESIGN header + REPORT).");

        // ── takeObject (unique_ptr, instance slot 1) ───────────────────────
        ctx.check("rsw_obj_detour_saw_self", g_obj_self_ok.load(std::memory_order_relaxed));
        ctx.check("rsw_obj_donor_resolved", g_obj_donor_resolved.load(std::memory_order_relaxed));
        ctx.check("rsw_obj_set_arg_returned_true", g_obj_set_ok.load(std::memory_order_relaxed));
        ctx.check("rsw_obj_body_not_null", !rsw_fixture::obj_was_null());
        ctx.check("rsw_obj_body_tag_is_donor", rsw_fixture::obj_tag() == DONOR_TAG);
        ctx.check("rsw_obj_body_identity_is_donor",
                  rsw_fixture::obj_identity() == rsw_fixture::donor_identity()
                  && rsw_fixture::obj_identity() != 0);

        // ── takeObjectStatic (unique_ptr, STATIC slot 0) ───────────────────
        ctx.check("rsw_sobj_set_arg_returned_true", g_sobj_set_ok.load(std::memory_order_relaxed));
        ctx.check("rsw_sobj_body_not_null", !rsw_fixture::sobj_was_null());
        ctx.check("rsw_sobj_body_tag_is_donor", rsw_fixture::sobj_tag() == DONOR_TAG);
        ctx.check("rsw_sobj_body_identity_is_donor",
                  rsw_fixture::sobj_identity() == rsw_fixture::donor_identity()
                  && rsw_fixture::sobj_identity() != 0);

        // ── takeByVal (object_base-by-value branch, instance slot 1) ───────
        ctx.check("rsw_byval_set_arg_returned_true", g_byval_set_ok.load(std::memory_order_relaxed));
        ctx.check("rsw_byval_body_not_null", !rsw_fixture::byval_was_null());
        ctx.check("rsw_byval_body_tag_is_byval_donor", rsw_fixture::byval_tag() == BYVAL_DONOR_TAG);
        ctx.check("rsw_byval_body_identity_is_byval_donor",
                  rsw_fixture::byval_identity() == rsw_fixture::byval_donor_identity()
                  && rsw_fixture::byval_identity() != 0);

        // ── takeFresh (make_unique-allocated object, instance slot 1) ──────
        ctx.check("rsw_fresh_allocated", g_fresh_made.load(std::memory_order_relaxed));
        ctx.check("rsw_fresh_set_arg_returned_true", g_fresh_set_ok.load(std::memory_order_relaxed));
        ctx.check("rsw_fresh_body_not_null", !rsw_fixture::fresh_was_null());
        ctx.check("rsw_fresh_body_tag_is_fresh", rsw_fixture::fresh_tag() == FRESH_DONOR_TAG);

        // ── takeTwoObjects (inject into slot 2 only; slot 1 survives) ──────
        ctx.check("rsw_two_b_set_arg_returned_true", g_two_b_set_ok.load(std::memory_order_relaxed));
        ctx.check("rsw_two_b_body_is_donor",
                  !rsw_fixture::two_second_was_null()
                  && rsw_fixture::two_second_tag() == DONOR_TAG);
        ctx.check("rsw_two_a_body_untouched_11",
                  !rsw_fixture::two_first_was_null()
                  && rsw_fixture::two_first_tag() == 11);

        // ── takeMixedObject (object after a primitive; inject slot 2) ──────
        ctx.check("rsw_mixed_set_arg_returned_true", g_mixed_set_ok.load(std::memory_order_relaxed));
        ctx.check("rsw_mixed_x_body_is_donor",
                  !rsw_fixture::mixed_obj_was_null()
                  && rsw_fixture::mixed_obj_tag() == DONOR_TAG);
        ctx.check("rsw_mixed_n_body_untouched_4242", rsw_fixture::mixed_n() == 4242);

        // ── takeString: a real String OBJECT injected into a String slot ───
        ctx.check("rsw_strobj_donor_resolved", g_strobj_donor_resolved.load(std::memory_order_relaxed));
        ctx.check("rsw_strobj_set_arg_returned_true", g_strobj_set_ok.load(std::memory_order_relaxed));
        ctx.check("rsw_strobj_body_not_null", !rsw_fixture::str_was_null());
        ctx.check("rsw_strobj_body_content_is_donor", rsw_fixture::str_seen() == STRING_DONOR);
        ctx.check("rsw_strobj_body_len_matches",
                  rsw_fixture::str_len() == static_cast<std::int32_t>(STRING_DONOR.size()));

        // ── takeObjectNull (empty uptr -> Java null, instance slot 1) ──────
        ctx.check("rsw_nullobj_set_arg_returned_true", g_nullobj_set_ok.load(std::memory_order_relaxed));
        ctx.check("rsw_nullobj_body_is_null", rsw_fixture::null_obj_was_null());
        ctx.check("rsw_nullobj_body_tag_minus1", rsw_fixture::null_obj_tag() == -1);

        // ── takeObjectNullStatic (empty uptr -> null, STATIC slot 0) ───────
        ctx.check("rsw_snullobj_set_arg_returned_true", g_snullobj_set_ok.load(std::memory_order_relaxed));
        ctx.check("rsw_snullobj_body_is_null", rsw_fixture::snull_obj_was_null());

        // ==================================================================
        // CANCEL-ONLY: takeOverNull — the bug-triggering null-original slot.
        // set_arg returned true and the NATIVE gated read-back proves the
        // injected DONOR landed in slot 1 (decode(slot) == donor oop), yet the
        // body NEVER ran (cancelled), so the JVM never dereferenced the
        // narrow-oop slot.  This is the EXACT scenario that crashed the JVM
        // when the body was allowed through — now characterized crash-proof.
        // ==================================================================
        ctx.check("rsw_overnull_donor_resolved", g_overnull_donor_resolved.load(std::memory_order_relaxed));
        ctx.check("rsw_overnull_set_arg_returned_true", g_overnull_set_ok.load(std::memory_order_relaxed));
        ctx.check("rsw_overnull_cancelled", g_overnull_cancelled.load(std::memory_order_relaxed));
        // The body must NOT have run (cancel suppressed it) — the crash-proof gate.
        ctx.check("rsw_overnull_body_did_not_run", !rsw_fixture::over_null_body_ran());
        // Native gated decode of slot 1 == the donor oop we injected.
        ctx.check("rsw_overnull_slot_decoded_valid", g_overnull_slot_valid.load(std::memory_order_relaxed));
        ctx.check("rsw_overnull_slot_is_donor_oop",
                  g_overnull_slot_decoded.load(std::memory_order_relaxed)
                      == g_overnull_expected_oop.load(std::memory_order_relaxed)
                  && g_overnull_expected_oop.load(std::memory_order_relaxed) != 0);
        ctx.record("[INFO] return_set_wrapper_null: set_arg(slot, unique_ptr<wrapper>) over a "
                   "genuinely-NULL interpreter local slot stores the wrapper's oop as a NARROW "
                   "(compressed, 4-byte) value, but that slot must hold a wide 64-bit oop. The native "
                   "gated read-back confirms the DONOR landed; the body is CANCELLED because letting "
                   "Java aload/getfield/identity that narrow-oop slot AVs the JVM (reproduced on "
                   "JDK 21 / Windows: System.identityHashCode -> EXCEPTION_ACCESS_VIOLATION). "
                   "Genuine vmhook store_oop bug — see the module REPORT for the proposed fix.");

        // ==================================================================
        // CANCEL-ONLY: takeWrongType — cross-type injection (no klass check).
        // The Decoy oop is accepted into the Donor slot; the native gated read
        // proves the Decoy oop landed.  The body is cancelled (a wrong-klass
        // getfield/invokevirtual would be UB).  We inject OVER a non-null
        // sentinel, so the slot is a wide oop and the native decode is a plain
        // raw read — but we still never let Java touch it.
        // ==================================================================
        ctx.check("rsw_wrong_set_arg_returned_true", g_wrong_set_ok.load(std::memory_order_relaxed));
        ctx.check("rsw_wrong_cancelled", g_wrong_cancelled.load(std::memory_order_relaxed));
        ctx.check("rsw_wrong_body_did_not_run", !rsw_fixture::wrong_body_ran());
        ctx.check("rsw_wrong_slot_decoded_valid", g_wrong_slot_valid.load(std::memory_order_relaxed));
        ctx.check("rsw_wrong_slot_is_decoy_oop",
                  g_wrong_slot_decoded.load(std::memory_order_relaxed)
                      == g_wrong_expected_oop.load(std::memory_order_relaxed)
                  && g_wrong_expected_oop.load(std::memory_order_relaxed) != 0);
        ctx.record("[INFO] return_set_wrapper_null cross-type injection: a Decoy wrapper (unrelated "
                   "Java class) injected into a Donor-typed slot is ACCEPTED (set_arg has NO klass-match "
                   "check). The native gated read-back confirms the Decoy oop landed in the slot. The "
                   "body is CANCELLED — a getfield/invokevirtual against a Decoy-as-Donor is UB. Same "
                   "no-klass-check flaw class field_object_ref.cpp documents for the field-decode path; "
                   "characterized, not a CI failure.");
    }

    // =====================================================================
    // ROUND 2 — takeString NULL injection.  Re-fire the probe (mode 1) with a
    // single hook that injects an EMPTY unique_ptr<object_base> into the String
    // slot.  The probe passes the non-null "before"; the body must observe Java
    // null afterwards.  Null injection writes a literal null pointer (always
    // safe), so allow-through is fine here.  String-slot twin of takeObjectNull.
    // =====================================================================
    {
        auto h_strnull{ vmhook::scoped_hook<rsw_fixture>(
            "takeString", "(Ljava/lang/String;)V",
            [](vmhook::return_value& ret,
               const std::unique_ptr<rsw_fixture>&,
               const std::string& /*incoming "before"*/)
            {
                std::unique_ptr<vmhook::object_base> empty{};
                g_strnull_set_ok.store(ret.set_arg(1, std::move(empty)), std::memory_order_relaxed);
            }) };
        ctx.check("rsw_strnull_hook_installed", h_strnull.installed());

        const bool done{ ctx.run_probe(
            [](bool value) { if (value) { rsw_fixture::set_done(false); rsw_fixture::set_mode(1); } rsw_fixture::set_go(value); },
            []() { return rsw_fixture::get_done(); }) };
        ctx.check("rsw_strnull_probe_completed", done);
        ctx.check("rsw_strnull_set_arg_returned_true", g_strnull_set_ok.load(std::memory_order_relaxed));
        ctx.check("rsw_strnull_body_is_null", rsw_fixture::str_was_null());
        ctx.check("rsw_strnull_body_len_minus1", rsw_fixture::str_len() == -1);
    }

    ctx.record("[INFO] return_set_wrapper_null: proved set_arg object/null injection — "
               "unique_ptr<wrapper> (published donor + make_unique fresh) and object_base-by-value "
               "into a reference slot make the body observe the INJECTED object (identity + field read "
               "through it) when the overwritten slot held a wide OOP; empty unique_ptr makes the body "
               "observe Java null; verified instance (slot 1) AND static (slot 0), slot-2 targeting "
               "among consecutive object args, an object arg following a primitive, a real String "
               "object injected into a String slot, and a null String injection. The narrow-over-null "
               "store_oop bug and the wrong-type (Decoy into Donor slot) acceptance are characterized "
               "NATIVELY + CANCELLED (crash-proof under C2).");
}
