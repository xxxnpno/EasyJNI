// global_ref JVM test module — exhaustive coverage of vmhook::jni::global_ref.
//
// global_ref (vmhook.hpp:16707) is the move-only RAII pin that keeps a Java
// object alive across a relocating garbage collection.  Its constructor promotes
// a raw decoded OOP to a JNI global reference (NewGlobalRef, slot 21); its
// destructor / reset() release it exactly once (DeleteGlobalRef, slot 22); and
// .oop() re-derives the object's CURRENT (post-relocation) heap address out of
// the handle slot on every call — masking the JDK 9+ JNI handle tag bits
// (vmhook.hpp:16768) so the deref is well-aligned on modern JDKs.  This module
// drives the full lifetime on a live JVM:
//
//   * BUILD + PIN (phase 1) — make_unique a GlobalRefProbe with a known sentinel,
//     pin it with vmhook::pin(unique_ptr), prove the pin is held and that the
//     sentinel reads back THROUGH .oop() (a FUNCTIONAL proof the pin points at
//     OUR object — never a raw-address identity assert, because a wrapper's bare
//     OOP goes stale after GC while .oop() tracks relocation), then DROP the
//     wrapper so the global ref is the object's only keep-alive.
//
//   * SURVIVE GC (phase 2) — the Java probe forces System.gc() several times
//     (mode 2), a relocating collector may MOVE the still-pinned object, and the
//     native detour then re-reads the sentinel through the SAME pin's .oop():
//     it must still be non-null and still read the sentinel.  The numeric
//     address from .oop() is ALLOWED to differ pre/post GC (that is relocation
//     being tracked, recorded as [INFO], never asserted).
//
//   * MOVE-ONLY SEMANTICS — move-construct / move-assign transfer ownership and
//     empty the source (no double DeleteGlobalRef); self-move leaves the handle
//     intact; copy is statically disabled (compile-time static_assert).
//
//   * NULL / EMPTY are safe — a default pin and pin(nullptr) are falsy, .oop()
//     is null, and reset() is a no-op (no NewGlobalRef/DeleteGlobalRef issued).
//
// Every JNI-touching step (make_unique, the pin's NewGlobalRef, reset()'s
// DeleteGlobalRef) needs a live JavaThread with an attached JNIEnv.  The
// test-suite worker runs on a detached native thread that has neither, so ALL of
// it happens inside a scoped_hook detour on trigger() — the same shape the
// make_unique module uses.  oop() is a pure slot dereference and is additionally
// GUARDED with hotspot::is_valid_pointer before any field read: if oop() ever
// returned garbage we record a FAIL rather than access-violate and take the
// whole suite down (NEVER crash the JVM).  The surviving pin lives in a
// file-scope global_ref so it persists across the phase-1 / phase-2 probe
// boundary; it is released explicitly inside the phase-2 detour (on a live
// JNIEnv) so the real DeleteGlobalRef path is exercised.
#include <vmhook/vmhook.hpp>

#include "../harness.hpp"

#include <atomic>
#include <cstdint>
#include <sstream>
#include <string>
#include <type_traits>

namespace
{
    // ── Compile-time contract: global_ref is move-only and trivially releasable.
    using gref = vmhook::jni::global_ref;
    static_assert(!std::is_copy_constructible_v<gref>,
                  "global_ref must NOT be copy-constructible (a global ref is owned exactly once).");
    static_assert(!std::is_copy_assignable_v<gref>,
                  "global_ref must NOT be copy-assignable (double DeleteGlobalRef corrupts the handle table).");
    static_assert(std::is_move_constructible_v<gref>,
                  "global_ref must be move-constructible (ownership transfers into snapshots/maps).");
    static_assert(std::is_move_assignable_v<gref>,
                  "global_ref must be move-assignable (build-outside-lock, then move-into-map).");
    static_assert(std::is_nothrow_destructible_v<gref>,
                  "global_ref destructor must be noexcept.");
    static_assert(std::is_nothrow_move_constructible_v<gref>,
                  "global_ref move ctor must be noexcept (lives inside std::unordered_map values).");

    // Sentinel value stamped into the pinned object's (I)V constructor and read
    // back through .oop() before and after GC.  Distinct, non-zero bit pattern so
    // a default-zeroed / freed slot is obviously wrong.
    constexpr std::int32_t k_sentinel{ 0x5A5A };

    // Wrapper for vmhook.fixtures.GlobalRefProbe.
    class global_ref_fixture : public vmhook::object<global_ref_fixture>
    {
    public:
        explicit global_ref_fixture(vmhook::oop_t instance) noexcept
            : vmhook::object<global_ref_fixture>{ instance }
        {
        }

        // ── Handshake ──────────────────────────────────────────────────────────
        static auto set_go(bool value) -> void        { static_field("go")->set(value); }
        static auto set_done(bool value) -> void       { static_field("done")->set(value); }
        static auto get_done() -> bool                 { return static_field("done")->get(); }
        static auto set_mode(std::int32_t m) -> void   { static_field("mode")->set(m); }
        static auto get_gc_rounds() -> std::int32_t    { return static_field("gcRounds")->get(); }

        // ── Sentinel read-back through a wrapper rebuilt from .oop() ────────────
        auto get_sentinel() -> std::int32_t { return get_field("sentinel")->get(); }
    };

    // ── Hook observation ─────────────────────────────────────────────────────────
    std::atomic<int>  g_trigger_calls{ 0 };
    std::atomic<bool> g_hook_saw_self{ false };

    // The phase the next trigger() detour should run.  Set by the module body
    // before each run_probe; read inside the detour.
    std::atomic<int>  g_phase{ 0 };

    // The single pin under test, surviving from phase 1 into phase 2.  Built and
    // released inside the trigger() detour (where a JNIEnv is live).
    vmhook::jni::global_ref g_pinned{};

    // ── Phase-1 (build + pin) observations ───────────────────────────────────────
    std::atomic<bool> g_made_ok{ false };
    std::atomic<std::int32_t> g_made_sentinel{ -1 };
    std::atomic<bool> g_pin_held{ false };
    std::atomic<bool> g_pin_oop_nonnull{ false };
    std::atomic<bool> g_pin_read_initial_ok{ false };
    std::atomic<std::int32_t> g_pin_read_initial_val{ -1 };

    // Pre-drop pointer relationship (for the [INFO] diagnostic / relocation cross-check).
    std::atomic<std::uintptr_t> g_oop_pre_gc{ 0 };
    std::atomic<std::uintptr_t> g_handle_bits{ 0 };
    std::atomic<std::uintptr_t> g_instance_bits{ 0 };

    // ── Move-only semantics observations ─────────────────────────────────────────
    std::atomic<bool> g_mc_built{ false };          // throwaway pin for move-construct built
    std::atomic<bool> g_mc_src_emptied{ false };     // moved-from is falsy + oop()==null
    std::atomic<bool> g_mc_dst_holds{ false };        // moved-to holds the pin
    std::atomic<bool> g_mc_dst_reads_ok{ false };      // moved-to reads the sentinel
    std::atomic<std::int32_t> g_mc_dst_val{ -1 };

    std::atomic<bool> g_ma_src_emptied{ false };       // move-assign emptied the source
    std::atomic<bool> g_ma_dst_holds{ false };
    std::atomic<bool> g_ma_dst_reads_ok{ false };
    std::atomic<std::int32_t> g_ma_dst_val{ -1 };

    std::atomic<bool> g_selfmove_intact{ false };      // self-move left the handle usable
    std::atomic<std::int32_t> g_selfmove_val{ -1 };

    // ── Null / empty safety observations ─────────────────────────────────────────
    std::atomic<bool> g_empty_falsy{ false };
    std::atomic<bool> g_empty_oop_null{ false };
    std::atomic<bool> g_empty_reset_safe{ false };
    std::atomic<bool> g_pin_nulloop_falsy{ false };
    std::atomic<bool> g_pin_null_wrapper_falsy{ false };

    // ── Phase-2 (survive GC) observations ────────────────────────────────────────
    std::atomic<bool> g_survive_oop_nonnull{ false };
    std::atomic<bool> g_survive_read_ok{ false };
    std::atomic<std::int32_t> g_survive_read_val{ -1 };
    std::atomic<std::uintptr_t> g_oop_post_gc{ 0 };
    std::atomic<bool> g_reset_clears_oop{ false };
    std::atomic<bool> g_double_reset_safe{ false };

    // Reads `fixture->sentinel` through a wrapper rebuilt from `live` — but ONLY
    // after is_valid_pointer clears the address.  Returns true on a guarded,
    // successful read (writing the value out); false if the address is unusable
    // (the caller records a FAIL, never an AV).
    auto read_sentinel_guarded(vmhook::oop_t live, std::int32_t& out) -> bool
    {
        if (!live || !vmhook::hotspot::is_valid_pointer(live))
        {
            return false;
        }
        global_ref_fixture via{ live };
        out = via.get_sentinel();
        return true;
    }
}

VMHOOK_JVM_MODULE(global_ref)
{
    vmhook::register_class<global_ref_fixture>("vmhook/fixtures/GlobalRefProbe");

    {
        // scoped_hook on trigger(): every JNI-touching global_ref operation below
        // runs INSIDE this detour, so a JavaThread (and an attached JNIEnv) is
        // live for make_unique / NewGlobalRef / DeleteGlobalRef.  Never call
        // shutdown_hooks() — the handle uninstalls on scope exit, isolating this
        // module.
        auto handle{ vmhook::scoped_hook<global_ref_fixture>(
            "trigger",
            [](vmhook::return_value&,
               const std::unique_ptr<global_ref_fixture>& self)
            {
                g_trigger_calls.fetch_add(1, std::memory_order_relaxed);
                g_hook_saw_self.store(self != nullptr, std::memory_order_relaxed);

                const int phase{ g_phase.load(std::memory_order_relaxed) };

                // ════════════════════════════════════════════════════════════════
                //  PHASE 1 — build, pin, move-only checks, drop the wrapper.
                // ════════════════════════════════════════════════════════════════
                if (phase == 1)
                {
                    // ── Allocate a fresh GlobalRefProbe with a known sentinel ────
                    auto made{ vmhook::make_unique<global_ref_fixture>(k_sentinel) };
                    if (!made)
                    {
                        return;  // module body records the FAIL via g_made_ok==false
                    }
                    g_made_ok.store(true, std::memory_order_relaxed);
                    g_made_sentinel.store(made->get_sentinel(), std::memory_order_relaxed);

                    // ── Pin it.  pin(unique_ptr) promotes the wrapper's OOP. ─────
                    g_pinned = vmhook::pin(made);
                    g_pin_held.store(static_cast<bool>(g_pinned), std::memory_order_relaxed);

                    const vmhook::oop_t pin_oop{ g_pinned.oop() };
                    g_pin_oop_nonnull.store(pin_oop != nullptr, std::memory_order_relaxed);

                    // Record the pre-drop pointer relationship for the diagnostic.
                    g_oop_pre_gc.store(reinterpret_cast<std::uintptr_t>(pin_oop),
                                       std::memory_order_relaxed);
                    g_handle_bits.store(reinterpret_cast<std::uintptr_t>(g_pinned.handle()),
                                        std::memory_order_relaxed);
                    g_instance_bits.store(
                        reinterpret_cast<std::uintptr_t>(made->vmhook::object_base::get_instance()),
                        std::memory_order_relaxed);

                    // ── FUNCTIONAL proof: read the sentinel THROUGH .oop() ───────
                    std::int32_t initial{ -1 };
                    if (read_sentinel_guarded(pin_oop, initial))
                    {
                        g_pin_read_initial_ok.store(true, std::memory_order_relaxed);
                        g_pin_read_initial_val.store(initial, std::memory_order_relaxed);
                    }

                    // ── Move-only semantics (genuine NewGlobalRef-backed pins) ───
                    // Use a SEPARATE freshly-pinned object so the move tests never
                    // disturb g_pinned (the one that must survive into phase 2).
                    if (auto made_mc{ vmhook::make_unique<global_ref_fixture>(0x1111) })
                    {
                        vmhook::jni::global_ref src{ vmhook::pin(made_mc) };
                        g_mc_built.store(static_cast<bool>(src), std::memory_order_relaxed);

                        // move-CONSTRUCT
                        vmhook::jni::global_ref dst{ std::move(src) };
                        g_mc_src_emptied.store(
                            !static_cast<bool>(src) && src.oop() == nullptr,
                            std::memory_order_relaxed);
                        g_mc_dst_holds.store(static_cast<bool>(dst), std::memory_order_relaxed);
                        std::int32_t mc_val{ -1 };
                        if (read_sentinel_guarded(dst.oop(), mc_val))
                        {
                            g_mc_dst_reads_ok.store(true, std::memory_order_relaxed);
                            g_mc_dst_val.store(mc_val, std::memory_order_relaxed);
                        }

                        // move-ASSIGN over a DIFFERENT held pin: the assignment
                        // must release the target's old handle (no leak) and take
                        // over dst's.  Build a third pin to be the assignment
                        // target so its prior handle's DeleteGlobalRef runs here
                        // on the live JNIEnv.
                        if (auto made_ma_target{ vmhook::make_unique<global_ref_fixture>(0x2222) })
                        {
                            vmhook::jni::global_ref target{ vmhook::pin(made_ma_target) };
                            target = std::move(dst);  // releases target's 0x2222 pin, adopts dst's 0x1111
                            g_ma_src_emptied.store(
                                !static_cast<bool>(dst) && dst.oop() == nullptr,
                                std::memory_order_relaxed);
                            g_ma_dst_holds.store(static_cast<bool>(target), std::memory_order_relaxed);
                            std::int32_t ma_val{ -1 };
                            if (read_sentinel_guarded(target.oop(), ma_val))
                            {
                                g_ma_dst_reads_ok.store(true, std::memory_order_relaxed);
                                g_ma_dst_val.store(ma_val, std::memory_order_relaxed);
                            }

                            // self-move-assign: impl guards `this != &other`, so
                            // the handle must remain intact and still readable.
                            // (Routed through a reference to dodge a -Wself-move
                            // diagnostic on a literal `x = std::move(x)`.)
                            vmhook::jni::global_ref& alias{ target };
                            target = std::move(alias);
                            std::int32_t self_val{ -1 };
                            if (static_cast<bool>(target)
                                && read_sentinel_guarded(target.oop(), self_val))
                            {
                                g_selfmove_intact.store(true, std::memory_order_relaxed);
                                g_selfmove_val.store(self_val, std::memory_order_relaxed);
                            }
                        }
                        // src / dst / target all destruct here on the live JNIEnv —
                        // each non-empty one issues exactly one DeleteGlobalRef.
                    }

                    // ── Null / empty pins are safe (no JNI calls issued) ─────────
                    {
                        vmhook::jni::global_ref empty{};
                        g_empty_falsy.store(!static_cast<bool>(empty), std::memory_order_relaxed);
                        g_empty_oop_null.store(empty.oop() == nullptr, std::memory_order_relaxed);
                        empty.reset();  // no-op on an empty pin
                        g_empty_reset_safe.store(
                            !static_cast<bool>(empty) && empty.oop() == nullptr,
                            std::memory_order_relaxed);
                    }
                    {
                        vmhook::jni::global_ref null_pin{ vmhook::pin(vmhook::oop_t{ nullptr }) };
                        g_pin_nulloop_falsy.store(!static_cast<bool>(null_pin), std::memory_order_relaxed);
                    }
                    {
                        const std::unique_ptr<global_ref_fixture> null_wrapper{};
                        vmhook::jni::global_ref from_null{ vmhook::pin(null_wrapper) };
                        g_pin_null_wrapper_falsy.store(!static_cast<bool>(from_null), std::memory_order_relaxed);
                    }

                    // ── Drop the wrapper: g_pinned is now the ONLY keep-alive ────
                    made.reset();
                    return;
                }

                // ════════════════════════════════════════════════════════════════
                //  PHASE 2 — post-GC: re-read through the SAME pin, then release.
                // ════════════════════════════════════════════════════════════════
                if (phase == 2)
                {
                    const vmhook::oop_t live{ g_pinned.oop() };
                    g_survive_oop_nonnull.store(live != nullptr, std::memory_order_relaxed);
                    g_oop_post_gc.store(reinterpret_cast<std::uintptr_t>(live),
                                        std::memory_order_relaxed);

                    std::int32_t survived{ -1 };
                    if (read_sentinel_guarded(live, survived))
                    {
                        g_survive_read_ok.store(true, std::memory_order_relaxed);
                        g_survive_read_val.store(survived, std::memory_order_relaxed);
                    }

                    // Release the pin on the live JNIEnv (real DeleteGlobalRef),
                    // then prove reset() is idempotent.
                    g_pinned.reset();
                    g_reset_clears_oop.store(g_pinned.oop() == nullptr, std::memory_order_relaxed);
                    g_pinned.reset();
                    g_double_reset_safe.store(!static_cast<bool>(g_pinned), std::memory_order_relaxed);
                    return;
                }
            }) };

        ctx.check("global_ref_hook_installed", handle.installed());

        // ── PHASE 1: build + pin + move-only + null/empty ────────────────────────
        g_phase.store(1, std::memory_order_relaxed);
        const bool done1{ ctx.run_probe(
            [](bool value)
            {
                if (value)
                {
                    global_ref_fixture::set_done(false);
                    global_ref_fixture::set_mode(1);
                }
                global_ref_fixture::set_go(value);
            },
            []() { return global_ref_fixture::get_done(); }) };

        ctx.check("global_ref_phase1_probe_completed", done1);
        ctx.check("global_ref_hook_fired",
                  g_trigger_calls.load(std::memory_order_relaxed) >= 1);
        ctx.check("global_ref_hook_saw_self",
                  g_hook_saw_self.load(std::memory_order_relaxed));

        // make_unique + initial sentinel
        ctx.check("global_ref_object_allocated", g_made_ok.load(std::memory_order_relaxed));
        ctx.check("global_ref_initial_sentinel_is_5A5A",
                  g_made_sentinel.load(std::memory_order_relaxed) == k_sentinel);

        // pin held + functional read through oop() BEFORE any GC
        ctx.check("global_ref_pin_is_held", g_pin_held.load(std::memory_order_relaxed));
        ctx.check("global_ref_pin_oop_nonnull", g_pin_oop_nonnull.load(std::memory_order_relaxed));
        ctx.check("global_ref_pin_reads_field_initially",
                  g_pin_read_initial_ok.load(std::memory_order_relaxed));
        ctx.check("global_ref_pin_initial_field_is_5A5A",
                  g_pin_read_initial_val.load(std::memory_order_relaxed) == k_sentinel);

        // move-construct
        ctx.check("global_ref_move_src_built", g_mc_built.load(std::memory_order_relaxed));
        ctx.check("global_ref_move_construct_empties_source",
                  g_mc_src_emptied.load(std::memory_order_relaxed));
        ctx.check("global_ref_move_construct_dest_holds",
                  g_mc_dst_holds.load(std::memory_order_relaxed));
        ctx.check("global_ref_move_construct_dest_reads_field",
                  g_mc_dst_reads_ok.load(std::memory_order_relaxed));
        ctx.check("global_ref_move_construct_dest_field_is_1111",
                  g_mc_dst_val.load(std::memory_order_relaxed) == 0x1111);

        // move-assign
        ctx.check("global_ref_move_assign_empties_source",
                  g_ma_src_emptied.load(std::memory_order_relaxed));
        ctx.check("global_ref_move_assign_dest_holds",
                  g_ma_dst_holds.load(std::memory_order_relaxed));
        ctx.check("global_ref_move_assign_dest_reads_field",
                  g_ma_dst_reads_ok.load(std::memory_order_relaxed));
        ctx.check("global_ref_move_assign_dest_field_is_1111",
                  g_ma_dst_val.load(std::memory_order_relaxed) == 0x1111);

        // self-move
        ctx.check("global_ref_self_move_keeps_handle",
                  g_selfmove_intact.load(std::memory_order_relaxed));
        ctx.check("global_ref_self_move_field_is_1111",
                  g_selfmove_val.load(std::memory_order_relaxed) == 0x1111);

        // null / empty safety
        ctx.check("global_ref_default_is_falsy", g_empty_falsy.load(std::memory_order_relaxed));
        ctx.check("global_ref_default_oop_is_null", g_empty_oop_null.load(std::memory_order_relaxed));
        ctx.check("global_ref_empty_reset_is_safe", g_empty_reset_safe.load(std::memory_order_relaxed));
        ctx.check("global_ref_pin_nullptr_is_falsy", g_pin_nulloop_falsy.load(std::memory_order_relaxed));
        ctx.check("global_ref_pin_null_wrapper_is_falsy",
                  g_pin_null_wrapper_falsy.load(std::memory_order_relaxed));

        // ── PHASE 2: force GC on the Java thread, then re-read through the pin ────
        g_phase.store(2, std::memory_order_relaxed);
        const bool done2{ ctx.run_probe(
            [](bool value)
            {
                if (value)
                {
                    global_ref_fixture::set_done(false);
                    global_ref_fixture::set_mode(2);
                }
                global_ref_fixture::set_go(value);
            },
            []() { return global_ref_fixture::get_done(); }) };

        ctx.check("global_ref_phase2_probe_completed", done2);
        ctx.check("global_ref_gc_actually_ran",
                  global_ref_fixture::get_gc_rounds() >= 1);

        // The pin survived GC: oop() still resolves and the sentinel still reads.
        ctx.check("global_ref_survives_gc_oop_nonnull",
                  g_survive_oop_nonnull.load(std::memory_order_relaxed));
        ctx.check("global_ref_field_survives_gc",
                  g_survive_read_ok.load(std::memory_order_relaxed));
        ctx.check("global_ref_field_survives_gc_value_is_5A5A",
                  g_survive_read_val.load(std::memory_order_relaxed) == k_sentinel);

        // reset() releases and is idempotent.
        ctx.check("global_ref_reset_clears_oop", g_reset_clears_oop.load(std::memory_order_relaxed));
        ctx.check("global_ref_double_reset_safe", g_double_reset_safe.load(std::memory_order_relaxed));

        // ── Diagnostic: pre/post-GC address relationship (relocation tracking) ───
        {
            const std::uintptr_t pre{ g_oop_pre_gc.load(std::memory_order_relaxed) };
            const std::uintptr_t post{ g_oop_post_gc.load(std::memory_order_relaxed) };
            const std::uintptr_t hbits{ g_handle_bits.load(std::memory_order_relaxed) };
            const std::uintptr_t ibits{ g_instance_bits.load(std::memory_order_relaxed) };
            std::ostringstream oss{};
            oss << "[INFO] global_ref diag: handle=0x" << std::hex << hbits
                << " handle_lowbits=0x" << (hbits & 0xF)
                << " oop_pre_gc=0x" << pre
                << " oop_post_gc=0x" << post
                << " wrapper_instance=0x" << ibits
                << " relocated=" << std::dec << (pre != 0 && post != 0 && pre != post)
                << " oop_eq_instance_pre=" << (pre == ibits);
            ctx.record(oss.str());
        }
    }
}
