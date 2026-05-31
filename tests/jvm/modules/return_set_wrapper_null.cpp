// JVM test module for return_value::set_arg(index, value) with OBJECT-WRAPPER
// and NULL reference arguments — area: return_value / argument mutation.
//
// The canonical set_arg module (return_set_arg.cpp) covers primitives and
// Strings.  THIS module covers the two remaining set_arg input branches, the
// object/null ones (vmhook.hpp:7826-7837):
//
//   * is_unique_object_ptr branch — set_arg(slot, unique_ptr<wrapper>):
//       store_oop(value ? value->get_instance() : nullptr).  A non-empty
//       unique_ptr injects the wrapper's encoded compressed OOP into the slot;
//       an EMPTY unique_ptr injects a null reference.
//   * object_base-by-value branch — set_arg(slot, wrapper):
//       store_oop(value.get_instance()).
//
// Every check installs an interpreter hook on a tiny fixture method, injects a
// reference into one of its argument slots from inside the hook, fires the
// single fixture probe (one real bytecode dispatch per method), and reads back
// the static fields the body published — i.e. exactly what the original method
// observed AFTER the injection (identity hash, a field read THROUGH the injected
// object, and a null flag).
//
// Coverage (see ReturnSetWrapperNull.java for the matching method bodies):
//   * unique_ptr<wrapper> injection from a PUBLISHED donor OOP — instance
//     (slot 1) and static (slot 0); body observes the donor's identity AND its
//     tag read through the injected object;
//   * empty unique_ptr<wrapper> -> Java null — instance (slot 1) and static
//     (slot 0); body observes null;
//   * object_base-by-value injection (the other object branch) — instance;
//   * make_unique-allocated fresh object injection — proves an object created
//     entirely in native code can be injected and is walkable in the body;
//   * a String OBJECT reference injected into a String-typed slot (slot 1), and
//     explicit null injected into the same slot, on re-fired probes;
//   * object injection into slot 2 (the LATER of two consecutive reference
//     args) with the slot-1 arg surviving — proves slot targeting;
//   * object injection into the slot that FOLLOWS a primitive (takeMixedObject:
//     this=0, n=1, x=2) with n untouched;
//   * set_arg return-value semantics (true on every successful object/null
//     injection).
//
// CHARACTERIZED (not asserted-correct — surfaced as [INFO], asserted against the
// ACTUAL behaviour so a regression still shows, never as a CI [FAIL]):
//   * cross-type injection: a Decoy wrapper (unrelated Java class) injected into
//     a Donor-typed slot is ACCEPTED — set_arg has no klass-match check
//     (vmhook.hpp store_oop / is_unique_object_ptr branch), the same flaw class
//     field_object_ref.cpp documents for the field decode path.  We assert the
//     ONE robust fact (a non-null oop yields a non-null, non-crashing reference)
//     and record the wrong-type acceptance.
//
// HARD RULES honoured: never crash the JVM (every OOP deref is gated on
// is_valid_pointer before use); a real vmhook bug is CHARACTERIZED + recorded,
// never used to fail CI, and vmhook.hpp is not edited.  Harness API only;
// self-registers via VMHOOK_JVM_MODULE.
#include <vmhook/vmhook.hpp>

#include "../harness.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

namespace
{
    // Wrapper for vmhook.fixtures.ReturnSetWrapperNull$Donor — the reference
    // object injected into the hooked methods' argument slots.  Exposes the int
    // field / method used to prove an injected wrapper is a live, walkable object.
    class donor_object : public vmhook::object<donor_object>
    {
    public:
        explicit donor_object(vmhook::oop_t instance) noexcept
            : vmhook::object<donor_object>{ instance }
        {
        }

        auto tag() -> std::int32_t { return get_field("tag")->get(); }
    };

    // Wrapper for vmhook.fixtures.ReturnSetWrapperNull$Decoy — an UNRELATED Java
    // class whose layout differs from Donor.  Used for the cross-type-injection
    // characterization (set_arg accepts it into a Donor-typed slot; no klass
    // check).
    class decoy_object : public vmhook::object<decoy_object>
    {
    public:
        explicit decoy_object(vmhook::oop_t instance) noexcept
            : vmhook::object<decoy_object>{ instance }
        {
        }

        auto poison() -> std::int32_t { return get_field("poison")->get(); }
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

        // ── go/done handshake ──────────────────────────────────────────────
        static auto set_go(bool value)   -> void { static_field("go")->set(value); }
        static auto get_done()           -> bool { bool v = static_field("done")->get(); return v; }
        static auto reset_done()         -> void { static_field("done")->set(false); }

        static auto get_probe_ticks() -> std::int32_t { std::int32_t v = static_field("probeTicks")->get(); return v; }

        // ── published donor objects + identities ───────────────────────────
        static auto donor()        -> std::unique_ptr<donor_object> { return static_field("DONOR")->get(); }
        static auto byval_donor()  -> std::unique_ptr<donor_object> { return static_field("BYVAL_DONOR")->get(); }
        static auto decoy()        -> std::unique_ptr<decoy_object> { return static_field("DECOY")->get(); }
        static auto string_donor() -> std::unique_ptr<vmhook::object_base> { return static_field("STRING_DONOR_REF")->get(); }

        static auto donor_identity()       -> std::int32_t { std::int32_t v = static_field("donorIdentity")->get();      return v; }
        static auto byval_donor_identity() -> std::int32_t { std::int32_t v = static_field("byvalDonorIdentity")->get(); return v; }
        static auto decoy_identity()       -> std::int32_t { std::int32_t v = static_field("decoyIdentity")->get();      return v; }

        // ── takeObject (unique_ptr, instance slot 1) ───────────────────────
        static auto obj_was_null() -> bool        { bool v = static_field("objWasNull")->get(); return v; }
        static auto obj_identity() -> std::int32_t { std::int32_t v = static_field("objIdentity")->get(); return v; }
        static auto obj_tag()      -> std::int32_t { std::int32_t v = static_field("objTag")->get(); return v; }

        // ── takeObjectStatic (unique_ptr, static slot 0) ───────────────────
        static auto sobj_was_null() -> bool        { bool v = static_field("sObjWasNull")->get(); return v; }
        static auto sobj_identity() -> std::int32_t { std::int32_t v = static_field("sObjIdentity")->get(); return v; }
        static auto sobj_tag()      -> std::int32_t { std::int32_t v = static_field("sObjTag")->get(); return v; }

        // ── takeObjectNull / takeObjectNullStatic (empty uptr -> null) ─────
        static auto null_obj_was_null()  -> bool { bool v = static_field("nullObjWasNull")->get();  return v; }
        static auto null_obj_tag()       -> std::int32_t { std::int32_t v = static_field("nullObjTag")->get(); return v; }
        static auto snull_obj_was_null() -> bool { bool v = static_field("sNullObjWasNull")->get(); return v; }

        // ── takeByVal (object_base-by-value, instance slot 1) ──────────────
        static auto byval_was_null() -> bool        { bool v = static_field("byvalWasNull")->get(); return v; }
        static auto byval_identity() -> std::int32_t { std::int32_t v = static_field("byvalIdentity")->get(); return v; }
        static auto byval_tag()      -> std::int32_t { std::int32_t v = static_field("byvalTag")->get(); return v; }

        // ── takeFresh (make_unique-allocated, instance slot 1) ─────────────
        static auto fresh_was_null() -> bool        { bool v = static_field("freshWasNull")->get(); return v; }
        static auto fresh_tag()      -> std::int32_t { std::int32_t v = static_field("freshTag")->get(); return v; }

        // ── takeTwoObjects (slots 1 and 2) ─────────────────────────────────
        static auto two_first_tag()       -> std::int32_t { std::int32_t v = static_field("twoFirstTag")->get();  return v; }
        static auto two_second_tag()      -> std::int32_t { std::int32_t v = static_field("twoSecondTag")->get(); return v; }
        static auto two_first_was_null()  -> bool { bool v = static_field("twoFirstWasNull")->get();  return v; }
        static auto two_second_was_null() -> bool { bool v = static_field("twoSecondWasNull")->get(); return v; }

        // ── takeMixedObject (this=0, n=1, x=2) ─────────────────────────────
        static auto mixed_n()           -> std::int32_t { std::int32_t v = static_field("mixedN")->get();      return v; }
        static auto mixed_obj_tag()     -> std::int32_t { std::int32_t v = static_field("mixedObjTag")->get(); return v; }
        static auto mixed_obj_was_null()-> bool { bool v = static_field("mixedObjWasNull")->get(); return v; }

        // ── takeString (real String object / null, instance slot 1) ────────
        static auto str_was_null() -> bool        { bool v = static_field("strWasNull")->get(); return v; }
        static auto str_len()      -> std::int32_t { std::int32_t v = static_field("strLen")->get(); return v; }
        static auto str_seen()     -> std::string { std::string s = static_field("strSeen")->get(); return s; }

        // ── takeWrongType (cross-type characterization) ────────────────────
        static auto wrong_was_null() -> bool        { bool v = static_field("wrongWasNull")->get(); return v; }
        static auto wrong_tag_read() -> std::int32_t { std::int32_t v = static_field("wrongTagRead")->get(); return v; }
        static auto wrong_identity() -> std::int32_t { std::int32_t v = static_field("wrongIdentity")->get(); return v; }
    };

    // Mirrored fixture constants (kept in lockstep with ReturnSetWrapperNull.java).
    constexpr std::int32_t DONOR_TAG       = 0x0D04;    // 3332
    constexpr std::int32_t BYVAL_DONOR_TAG = 0x0BABE;   // 48318
    constexpr std::int32_t FRESH_DONOR_TAG = 0x7E57;    // 32343
    const std::string      STRING_DONOR    = "injected-object-string";

    // ── per-hook observation state (captured in the detours) ────────────────

    // takeObject (unique_ptr): the detour's own view of the published donor.
    std::atomic<bool>           g_obj_set_ok{ false };
    std::atomic<bool>           g_obj_donor_resolved{ false };
    std::atomic<std::uintptr_t> g_obj_donor_oop{ 0 };
    std::atomic<bool>           g_obj_self_ok{ false };

    // takeObjectStatic (unique_ptr, slot 0)
    std::atomic<bool>           g_sobj_set_ok{ false };
    std::atomic<std::uintptr_t> g_sobj_donor_oop{ 0 };

    // takeObjectNull / static (empty uptr)
    std::atomic<bool> g_nullobj_set_ok{ false };
    std::atomic<bool> g_snullobj_set_ok{ false };

    // takeByVal (object_base-by-value)
    std::atomic<bool>           g_byval_set_ok{ false };
    std::atomic<std::uintptr_t> g_byval_donor_oop{ 0 };

    // takeFresh (make_unique)
    std::atomic<bool> g_fresh_made{ false };
    std::atomic<bool> g_fresh_set_ok{ false };

    // takeTwoObjects
    std::atomic<bool> g_two_b_set_ok{ false };

    // takeMixedObject
    std::atomic<bool> g_mixed_set_ok{ false };

    // takeWrongType (cross-type)
    std::atomic<bool>           g_wrong_set_ok{ false };
    std::atomic<std::uintptr_t> g_wrong_decoy_oop{ 0 };

    // takeString (object / null) — re-fired probes
    std::atomic<bool> g_strobj_set_ok{ false };
    std::atomic<bool> g_strobj_donor_resolved{ false };
    std::atomic<bool> g_strnull_set_ok{ false };

    auto as_uptr(void* p) -> std::uintptr_t { return reinterpret_cast<std::uintptr_t>(p); }
}

VMHOOK_JVM_MODULE(return_set_wrapper_null)
{
    vmhook::register_class<rsw_fixture>("vmhook/fixtures/ReturnSetWrapperNull");
    vmhook::register_class<donor_object>("vmhook/fixtures/ReturnSetWrapperNull$Donor");
    vmhook::register_class<decoy_object>("vmhook/fixtures/ReturnSetWrapperNull$Decoy");

    // =====================================================================
    // ROUND 1 — every object/null injection EXCEPT the two takeString
    // scenarios, in one probe (one bytecode dispatch per method).  All hooks
    // are scoped to this block and uninstall on exit, so this module never
    // tears down another module's hooks (scoped_hook, never shutdown_hooks).
    // =====================================================================
    {
        // ── takeObject(Donor) instance slot 1: inject the published DONOR ──
        // unique_ptr branch.  Read the donor from a static field INSIDE the
        // detour (a live JavaThread is guaranteed there), move it into set_arg.
        // Moving is safe: set_arg only copies get_instance()'s OOP bits, and the
        // donor object stays alive via the static field that also holds it.
        auto h_obj{ vmhook::scoped_hook<rsw_fixture>(
            "takeObject", "(Lvmhook/fixtures/ReturnSetWrapperNull$Donor;)V",
            [](vmhook::return_value& ret,
               const std::unique_ptr<rsw_fixture>& self,
               const std::unique_ptr<donor_object>& /*incoming(null)*/)
            {
                g_obj_self_ok.store(self != nullptr, std::memory_order_relaxed);
                std::unique_ptr<donor_object> d{ rsw_fixture::donor() };
                g_obj_donor_resolved.store(d != nullptr, std::memory_order_relaxed);
                if (d)
                {
                    g_obj_donor_oop.store(as_uptr(d->get_instance()), std::memory_order_relaxed);
                    g_obj_set_ok.store(ret.set_arg(1, std::move(d)), std::memory_order_relaxed);
                }
            }) };
        ctx.check("rsw_obj_hook_installed", h_obj.installed());

        // ── takeObjectStatic(Donor) static slot 0: inject DONOR at slot 0 ──
        auto h_sobj{ vmhook::scoped_hook<rsw_fixture>(
            "takeObjectStatic", "(Lvmhook/fixtures/ReturnSetWrapperNull$Donor;)V",
            [](vmhook::return_value& ret,
               const std::unique_ptr<donor_object>& /*incoming(null)*/)
            {
                std::unique_ptr<donor_object> d{ rsw_fixture::donor() };
                if (d)
                {
                    g_sobj_donor_oop.store(as_uptr(d->get_instance()), std::memory_order_relaxed);
                    g_sobj_set_ok.store(ret.set_arg(0, std::move(d)), std::memory_order_relaxed);
                }
            }) };
        ctx.check("rsw_sobj_hook_installed", h_sobj.installed());

        // ── takeObjectNull(Donor) instance slot 1: empty unique_ptr -> null ─
        auto h_nullobj{ vmhook::scoped_hook<rsw_fixture>(
            "takeObjectNull", "(Lvmhook/fixtures/ReturnSetWrapperNull$Donor;)V",
            [](vmhook::return_value& ret,
               const std::unique_ptr<rsw_fixture>&,
               const std::unique_ptr<donor_object>& /*incoming(DONOR)*/)
            {
                // An EMPTY unique_ptr drives store_oop(nullptr) -> writes null.
                std::unique_ptr<donor_object> empty{};
                g_nullobj_set_ok.store(ret.set_arg(1, std::move(empty)), std::memory_order_relaxed);
            }) };
        ctx.check("rsw_nullobj_hook_installed", h_nullobj.installed());

        // ── takeObjectNullStatic(Donor) static slot 0: null at slot 0 ──────
        auto h_snullobj{ vmhook::scoped_hook<rsw_fixture>(
            "takeObjectNullStatic", "(Lvmhook/fixtures/ReturnSetWrapperNull$Donor;)V",
            [](vmhook::return_value& ret,
               const std::unique_ptr<donor_object>& /*incoming(DONOR)*/)
            {
                std::unique_ptr<donor_object> empty{};
                g_snullobj_set_ok.store(ret.set_arg(0, std::move(empty)), std::memory_order_relaxed);
            }) };
        ctx.check("rsw_snullobj_hook_installed", h_snullobj.installed());

        // ── takeByVal(Donor) instance slot 1: object_base-by-value branch ──
        // Build a wrapper by VALUE from the donor OOP and pass it by value so
        // set_arg takes the std::is_base_of_v<object_base, T> branch (distinct
        // from the unique_ptr branch).
        auto h_byval{ vmhook::scoped_hook<rsw_fixture>(
            "takeByVal", "(Lvmhook/fixtures/ReturnSetWrapperNull$Donor;)V",
            [](vmhook::return_value& ret,
               const std::unique_ptr<rsw_fixture>&,
               const std::unique_ptr<donor_object>& /*incoming(null)*/)
            {
                std::unique_ptr<donor_object> bd{ rsw_fixture::byval_donor() };
                if (bd && bd->get_instance())
                {
                    donor_object by_value{ bd->get_instance() };   // wrapper by value
                    g_byval_donor_oop.store(as_uptr(by_value.get_instance()),
                                            std::memory_order_relaxed);
                    g_byval_set_ok.store(ret.set_arg(1, by_value), std::memory_order_relaxed);
                }
            }) };
        ctx.check("rsw_byval_hook_installed", h_byval.installed());

        // ── takeFresh(Donor) instance slot 1: make_unique-allocated object ──
        // Allocate a brand-new Donor entirely from native code and inject it.
        auto h_fresh{ vmhook::scoped_hook<rsw_fixture>(
            "takeFresh", "(Lvmhook/fixtures/ReturnSetWrapperNull$Donor;)V",
            [](vmhook::return_value& ret,
               const std::unique_ptr<rsw_fixture>&,
               const std::unique_ptr<donor_object>& /*incoming(null)*/)
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

        // ── takeTwoObjects(Donor a, Donor b): inject DONOR into b (slot 2) ──
        auto h_two{ vmhook::scoped_hook<rsw_fixture>(
            "takeTwoObjects",
            "(Lvmhook/fixtures/ReturnSetWrapperNull$Donor;Lvmhook/fixtures/ReturnSetWrapperNull$Donor;)V",
            [](vmhook::return_value& ret,
               const std::unique_ptr<rsw_fixture>&,
               const std::unique_ptr<donor_object>& /*a(Donor 11)*/,
               const std::unique_ptr<donor_object>& /*b(null)*/)
            {
                std::unique_ptr<donor_object> d{ rsw_fixture::donor() };
                if (d)
                {
                    g_two_b_set_ok.store(ret.set_arg(2, std::move(d)), std::memory_order_relaxed);
                }
            }) };
        ctx.check("rsw_two_hook_installed", h_two.installed());

        // ── takeMixedObject(int n, Donor x): inject DONOR into x (slot 2) ───
        auto h_mixed{ vmhook::scoped_hook<rsw_fixture>(
            "takeMixedObject", "(ILvmhook/fixtures/ReturnSetWrapperNull$Donor;)V",
            [](vmhook::return_value& ret,
               const std::unique_ptr<rsw_fixture>&,
               std::int32_t /*n*/,
               const std::unique_ptr<donor_object>& /*x(null)*/)
            {
                std::unique_ptr<donor_object> d{ rsw_fixture::donor() };
                if (d)
                {
                    g_mixed_set_ok.store(ret.set_arg(2, std::move(d)), std::memory_order_relaxed);
                }
            }) };
        ctx.check("rsw_mixed_hook_installed", h_mixed.installed());

        // ── takeWrongType(Donor): inject a DECOY (unrelated class) — flaw ──
        // Cross-type characterization.  set_arg has no klass check, so the Decoy
        // oop is accepted into the Donor-typed slot.
        auto h_wrong{ vmhook::scoped_hook<rsw_fixture>(
            "takeWrongType", "(Lvmhook/fixtures/ReturnSetWrapperNull$Donor;)V",
            [](vmhook::return_value& ret,
               const std::unique_ptr<rsw_fixture>&,
               const std::unique_ptr<donor_object>& /*incoming(null)*/)
            {
                std::unique_ptr<decoy_object> dc{ rsw_fixture::decoy() };
                if (dc)
                {
                    g_wrong_decoy_oop.store(as_uptr(dc->get_instance()), std::memory_order_relaxed);
                    // unique_ptr<decoy_object> is also is_unique_object_ptr (decoy
                    // derives from object_base) -> same object branch, no klass
                    // check.  Inject into the Donor slot.
                    g_wrong_set_ok.store(ret.set_arg(1, std::move(dc)), std::memory_order_relaxed);
                }
            }) };
        ctx.check("rsw_wrong_hook_installed", h_wrong.installed());

        // ── takeString(String): inject a real String OBJECT (the donor) ────
        // The published String donor is a 'L java/lang/String' reference.  Decode
        // it as a generic object_base wrapper and inject it through the unique_ptr
        // branch: store_oop encodes its OOP into the slot, so the body sees the
        // SAME String object (length + content match), NOT a freshly-built copy.
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

        // ── Drive every method once (single real bytecode dispatch each) ───
        const bool done{ ctx.run_probe(
            [](bool value) { rsw_fixture::set_go(value); },
            []() { return rsw_fixture::get_done(); }) };
        ctx.check("rsw_probe_completed", done);
        // The probe body ran (one bytecode dispatch per method occurred).
        ctx.check("rsw_probe_ticked", rsw_fixture::get_probe_ticks() >= 1);

        // The published donor identities are non-zero (Java actually ran run()).
        ctx.check("rsw_donor_identity_published", rsw_fixture::donor_identity() != 0);
        ctx.check("rsw_byval_donor_identity_published", rsw_fixture::byval_donor_identity() != 0);
        ctx.check("rsw_decoy_identity_published", rsw_fixture::decoy_identity() != 0);

        // ENVIRONMENTAL CAVEAT (audit return_value_set_arg_for_object_types.md
        // [high] "store_oop heuristic defaults to compressed when previous local
        // was null"): set_arg's store_oop picks compressed-vs-wide OOP storage
        // from the PREVIOUS slot value's magnitude.  Every injection below writes
        // over a slot whose incoming value is null (bits == 0) or a compressed
        // narrow OOP (bits <= 0xFFFFFFFF), so store_oop writes the COMPRESSED
        // form — correct on a +UseCompressedOops JVM (the default for heaps
        // <= 32 GB, which the CI matrix uses).  On a -XX:-UseCompressedOops JVM
        // (or heap > 32 GB) the slot expects a full 64-bit pointer and the body
        // would observe a corrupted reference; the body-observation asserts below
        // are therefore valid under the default JVM only.  This is a documented
        // library heuristic limitation, not a defect in this test; we record it
        // so a failure on a non-default JVM points at the right root cause.  We
        // do not branch on it because the suite's JVMs run with compressed OOPs.
        ctx.record("[INFO] set_arg object injection asserts assume +UseCompressedOops "
                   "(default, heap <= 32 GB): store_oop writes a compressed narrow OOP "
                   "when the incoming slot is null/compressed. On -XX:-UseCompressedOops "
                   "the body would see a corrupted reference (audit "
                   "return_value_set_arg_for_object_types.md [high]).");

        // ── takeObject (unique_ptr, instance slot 1) ───────────────────────
        ctx.check("rsw_obj_detour_saw_self", g_obj_self_ok.load(std::memory_order_relaxed));
        ctx.check("rsw_obj_donor_resolved", g_obj_donor_resolved.load(std::memory_order_relaxed));
        ctx.check("rsw_obj_set_arg_returned_true", g_obj_set_ok.load(std::memory_order_relaxed));
        // The body must now see a NON-null object (we injected over the null arg).
        ctx.check("rsw_obj_body_not_null", !rsw_fixture::obj_was_null());
        // The body's tag read THROUGH the injected object == the donor's tag.
        ctx.check("rsw_obj_body_tag_is_donor", rsw_fixture::obj_tag() == DONOR_TAG);
        // The body's identityHashCode of the injected object == the published
        // donor identity: the SAME heap object the native side resolved.
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

        // ── takeObjectNull (empty uptr -> Java null, instance slot 1) ──────
        ctx.check("rsw_nullobj_set_arg_returned_true", g_nullobj_set_ok.load(std::memory_order_relaxed));
        // The probe passed DONOR; the empty-uptr injection must overwrite it
        // with null, so the body observes null.
        ctx.check("rsw_nullobj_body_is_null", rsw_fixture::null_obj_was_null());
        ctx.check("rsw_nullobj_body_tag_minus1", rsw_fixture::null_obj_tag() == -1);

        // ── takeObjectNullStatic (empty uptr -> null, STATIC slot 0) ───────
        ctx.check("rsw_snullobj_set_arg_returned_true", g_snullobj_set_ok.load(std::memory_order_relaxed));
        ctx.check("rsw_snullobj_body_is_null", rsw_fixture::snull_obj_was_null());

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
        // The freshly-allocated Donor carries the tag we constructed it with.
        ctx.check("rsw_fresh_body_tag_is_fresh", rsw_fixture::fresh_tag() == FRESH_DONOR_TAG);

        // ── takeTwoObjects (inject into slot 2 only) ───────────────────────
        ctx.check("rsw_two_b_set_arg_returned_true", g_two_b_set_ok.load(std::memory_order_relaxed));
        // b (slot 2) was injected with the DONOR.
        ctx.check("rsw_two_b_body_is_donor",
                  !rsw_fixture::two_second_was_null()
                  && rsw_fixture::two_second_tag() == DONOR_TAG);
        // a (slot 1) was the original Donor(11) the probe passed — never touched.
        ctx.check("rsw_two_a_body_untouched_11",
                  !rsw_fixture::two_first_was_null()
                  && rsw_fixture::two_first_tag() == 11);

        // ── takeMixedObject (object after a primitive; inject slot 2) ──────
        ctx.check("rsw_mixed_set_arg_returned_true", g_mixed_set_ok.load(std::memory_order_relaxed));
        // x (slot 2) was injected with the DONOR.
        ctx.check("rsw_mixed_x_body_is_donor",
                  !rsw_fixture::mixed_obj_was_null()
                  && rsw_fixture::mixed_obj_tag() == DONOR_TAG);
        // n (slot 1) — the primitive that PRECEDES the object — is untouched.
        ctx.check("rsw_mixed_n_body_untouched_4242", rsw_fixture::mixed_n() == 4242);

        // ── takeString: a real String OBJECT injected into a String slot ───
        ctx.check("rsw_strobj_donor_resolved", g_strobj_donor_resolved.load(std::memory_order_relaxed));
        ctx.check("rsw_strobj_set_arg_returned_true", g_strobj_set_ok.load(std::memory_order_relaxed));
        ctx.check("rsw_strobj_body_not_null", !rsw_fixture::str_was_null());
        ctx.check("rsw_strobj_body_content_is_donor", rsw_fixture::str_seen() == STRING_DONOR);
        ctx.check("rsw_strobj_body_len_matches",
                  rsw_fixture::str_len() == static_cast<std::int32_t>(STRING_DONOR.size()));

        // ==================================================================
        // CHARACTERIZE — cross-type injection is ACCEPTED (no klass check).
        // We injected a Decoy oop into a Donor-typed slot.  set_arg performs no
        // klass-match validation, so the body received the Decoy AS a Donor.
        // Hard-assert only the robust facts (the real Decoy oop landed in the
        // slot, the body saw a non-null reference, nothing crashed); record the
        // wrong-type acceptance + the mis-typed field read as [INFO].  The .tag
        // read on a Decoy oop is offset-based heap pollution — its value is
        // whatever lives at Donor.tag's offset in the Decoy (HotSpot may reorder
        // Decoy's int/long fields for alignment), so we report it but never
        // assert a specific number.
        // ==================================================================
        ctx.check("rsw_wrong_set_arg_returned_true", g_wrong_set_ok.load(std::memory_order_relaxed));
        ctx.check("rsw_wrong_body_not_null", !rsw_fixture::wrong_was_null());
        // The injected object's identity == the published DECOY identity: proves
        // the Decoy oop really landed in the slot (wrong type, but the OOP we
        // chose).  This is the concrete "wrong type accepted" evidence and is
        // klass-independent (identityHashCode works for any oop).
        ctx.check("rsw_wrong_body_identity_is_decoy",
                  rsw_fixture::wrong_identity() == rsw_fixture::decoy_identity()
                  && rsw_fixture::wrong_identity() != 0);
        {
            const std::int32_t tag_read{ rsw_fixture::wrong_tag_read() };
            ctx.record("[INFO] set_arg cross-type injection: a Decoy wrapper "
                       "(unrelated Java class) injected into a Donor-typed slot was "
                       "ACCEPTED (set_arg has NO klass-match check). The body's .tag "
                       "FIELD read on the Decoy oop observed " + std::to_string(tag_read) +
                       " (offset-based heap-pollution read; value is whatever sits at "
                       "Donor.tag's offset in the Decoy). This is the same no-klass-check "
                       "flaw class field_object_ref.cpp documents for the field-decode "
                       "path — characterized, not a CI failure.");
        }
    }

    // =====================================================================
    // ROUND 2 — takeString NULL injection.  Re-fire the probe (the fixture's
    // `done` latches; reset it) with a single hook that injects an EMPTY
    // unique_ptr<object_base> into the String slot.  The probe passes
    // "before"; the body must observe Java null afterwards.  This is the
    // string-slot twin of the takeObjectNull case, proving the null path is
    // type-agnostic (works for String slots too, not just custom 'L' types).
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

        rsw_fixture::reset_done();
        const bool done{ ctx.run_probe(
            [](bool value) { rsw_fixture::set_go(value); },
            []() { return rsw_fixture::get_done(); }) };
        ctx.check("rsw_strnull_probe_completed", done);
        ctx.check("rsw_strnull_set_arg_returned_true", g_strnull_set_ok.load(std::memory_order_relaxed));
        // The probe passed "before"; the empty-uptr injection overwrote it with
        // null, so the body observes Java null on a String-typed slot.
        ctx.check("rsw_strnull_body_is_null", rsw_fixture::str_was_null());
        ctx.check("rsw_strnull_body_len_minus1", rsw_fixture::str_len() == -1);
    }

    ctx.record("[INFO] return_set_wrapper_null: proved set_arg object/null injection — "
               "unique_ptr<wrapper> (published donor + make_unique fresh) and "
               "object_base-by-value into a reference slot make the body observe the "
               "INJECTED object (identity + field read through it); empty unique_ptr "
               "makes the body observe Java null; verified instance (slot 1) AND static "
               "(slot 0), slot-2 targeting among consecutive object args, an object arg "
               "following a primitive, a real String object injected into a String slot, "
               "and a null String injection. Cross-type (Decoy into Donor slot) is "
               "ACCEPTED with no klass check (characterized).");
}
