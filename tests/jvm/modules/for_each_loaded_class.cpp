// for_each_loaded_class JVM test module  (feature area: class enumeration)
//
// Exhaustively exercises vmhook::for_each_loaded_class() on a LIVE JVM.  The
// function takes a SNAPSHOT of every Java class currently reachable through the
// global ClassLoaderDataGraph (JDK 21+ via ClassLoaderData::_klasses; JDK 8-17
// via the per-CLD Dictionary hashtables + the SystemDictionary _dictionary /
// _shared_dictionary fallback) and invokes
//
//     visitor(const std::string& internal_name, vmhook::hotspot::klass* k)
//
// once per Klass, where internal_name uses JVM '/'-separated form (e.g.
// "java/lang/String").  This migrates the legacy inline
// test_for_each_loaded_class from vmhook/src/example.cpp (Rework D).
//
// What the module proves, angle by angle (all PORTABLE across the supported JDK
// matrix — no exact count, no exact class set, because the loaded-class universe
// differs wildly between JDK 8 and JDK 21+ and between CDS-on / CDS-off):
//
//   - the snapshot is NON-TRIVIAL: a real JVM with the harness loaded holds far
//     more than 100 classes (bootstrap rt.jar / the java.base module alone is
//     thousands), so count > 100 is a robust liveness floor;
//   - the UNIVERSAL bootstrap classes are present: java/lang/Object,
//     java/lang/String, java/lang/Class, java/lang/Integer, java/lang/Thread —
//     these are loaded before any user code on every HotSpot, so their absence
//     would mean the walk missed the bootstrap loader entirely;
//   - APPLICATION-loaded classes are reached, not just bootstrap: this module's
//     OWN fixture class vmhook/fixtures/ForEachLoadedClass (Class.forName'd by
//     Main.loadFixtures at startup) MUST appear — the single strongest proof the
//     walk descends past the bootstrap CLD into the application class loader;
//   - EVERY klass pointer handed to the visitor is valid (passes the same
//     is_valid_pointer gate vmhook itself uses) — the enumeration never yields a
//     torn / sentinel / freed pointer the caller would crash on;
//   - the enumerated klass is genuinely USABLE, not merely non-null: for the OWN
//     fixture class the module derefs the supplied klass* (guarded) and confirms
//     klass->get_name()->to_string() ROUND-TRIPS to the very name the visitor was
//     handed — i.e. the pointer and the name describe the same live Klass;
//   - NO name is empty and EVERY name is well-formed (no leading '/', no NUL, no
//     embedded whitespace) — symbol decode never silently produced "" or garbage;
//   - the walk TERMINATES (the visitor stops being called; control returns) and
//     the count is bounded well below the internal 1M-per-CLD / 64K-CLD safety
//     caps, so we are observing a real finite graph, not a runaway loop;
//   - the snapshot is STABLE: a second independent enumeration agrees on every
//     robust invariant (count still > 100, the known classes still present, the
//     two counts within a small drift band) — enumeration is repeatable and
//     side-effect-free.
//
// Treated as BEST-EFFORT (recorded [INFO], never a hard FAIL — see the legacy
// test's note): the launcher-entry class vmhook/Main is NOT surfaced by HotSpot's
// JDK 8 SystemDictionary walk, and the nested / array anchors
// (ForEachLoadedClass$Inner, [I, [Ljava/lang/String;) live in Klass families the
// JDK-8 dictionary path may not list — their presence is informational only.
//
// Harness note: class enumeration is a pure HotSpot-internal read driven straight
// from the native worker thread — no go/done probe and no hook are required (the
// fixture registers a trivial no-op probe only to be a well-formed Harness
// participant).  The module installs NO hooks, so there is nothing to scope or
// shut down; it reads vmhook.hpp's public surface and never mutates JVM state.
#include <vmhook/vmhook.hpp>

#include "../harness.hpp"

#include <cstddef>
#include <set>
#include <string>

namespace
{
    // Internal name of this module's own fixture — Class.forName'd at startup by
    // Main.loadFixtures(), so it lives in the application class loader's CLD and
    // MUST appear in the enumeration (the app-class proof).
    constexpr char OWN_FIXTURE[]{ "vmhook/fixtures/ForEachLoadedClass" };

    // The result of one full enumeration pass.
    struct enumeration
    {
        std::set<std::string>      names{};               // every internal name seen
        std::size_t                count{ 0 };            // raw visit count (with dups)
        bool                       all_klass_valid{ true };// every klass* passed the gate
        bool                       any_empty_name{ false };// any visit yielded ""
        bool                       any_bad_name{ false };  // any malformed name
        // For the OWN fixture: the klass* the visitor handed us, plus whether its
        // get_name() round-trips back to OWN_FIXTURE (usability proof).
        bool                       own_seen{ false };
        bool                       own_klass_valid{ false };
        bool                       own_name_roundtrips{ false };
    };

    // A name is well-formed when it is non-empty, carries no leading '/', and has
    // no control / whitespace bytes.  (Array names like "[I" legitimately start
    // with '[', and inner-class names carry '$', so neither is rejected here.)
    auto name_is_wellformed(const std::string& name) -> bool
    {
        if (name.empty() || name.front() == '/')
        {
            return false;
        }
        for (const char ch : name)
        {
            const unsigned char byte{ static_cast<unsigned char>(ch) };
            if (byte <= 0x20u)   // NUL, control chars, space
            {
                return false;
            }
        }
        return true;
    }

    // Runs ONE full for_each_loaded_class pass and folds every observation into a
    // single `enumeration`.  Every klass dereference is guarded by is_valid_pointer
    // so a torn pointer in the snapshot can never crash the JVM from this test.
    auto enumerate_once() -> enumeration
    {
        enumeration result{};

        vmhook::for_each_loaded_class(
            [&](const std::string& name, vmhook::hotspot::klass* const k)
            {
                ++result.count;
                result.names.insert(name);

                if (name.empty())
                {
                    result.any_empty_name = true;
                }
                else if (!name_is_wellformed(name))
                {
                    result.any_bad_name = true;
                }

                // The visitor may legitimately be handed nullptr for an exotic
                // entry, but a NON-null pointer must be a valid, dereferenceable
                // Klass — never a sentinel / freed / torn value.
                if (k != nullptr && !vmhook::hotspot::is_valid_pointer(k))
                {
                    result.all_klass_valid = false;
                }

                // For our OWN fixture class, prove the supplied klass* is usable:
                // deref it (guarded) and confirm its own _name symbol round-trips
                // back to the exact internal name the visitor handed us.
                if (name == OWN_FIXTURE)
                {
                    result.own_seen = true;
                    result.own_klass_valid =
                        (k != nullptr) && vmhook::hotspot::is_valid_pointer(k);
                    if (result.own_klass_valid)
                    {
                        const vmhook::hotspot::symbol* const sym{ k->get_name() };
                        if (vmhook::hotspot::is_valid_pointer(sym))
                        {
                            result.own_name_roundtrips = (sym->to_string() == OWN_FIXTURE);
                        }
                    }
                }
            });

        return result;
    }
}

VMHOOK_JVM_MODULE(for_each_loaded_class)
{
    // =====================================================================
    // PASS 1 — a single full enumeration; every invariant folded in one walk.
    // =====================================================================
    const enumeration e1{ enumerate_once() };

    ctx.record(std::string{ "[INFO] for_each_loaded_class pass 1 visited " }
               + std::to_string(e1.count) + " klass(es), "
               + std::to_string(e1.names.size()) + " distinct name(s)");

    // ---- Liveness: the snapshot is non-trivial. -------------------------
    // A real JVM with the harness loaded holds thousands of classes; >100 is a
    // robust portable floor across JDK 8 .. 21+ and CDS-on / CDS-off.
    ctx.check("count_over_100", e1.count > 100);
    // The distinct-name set is likewise far over the floor (no massive dup skew).
    ctx.check("distinct_names_over_100", e1.names.size() > 100);
    // ...and the visit count terminated well under the internal safety caps
    // (1M klasses per CLD, 64K CLDs), so this is a real finite graph, not a loop.
    ctx.check("count_under_safety_cap", e1.count < 1'000'000);

    // ---- Universal bootstrap classes are present. -----------------------
    // These five load before any user code on every HotSpot build; missing any
    // one means the walk never reached the bootstrap loader.
    ctx.check("has_java_lang_Object",  e1.names.contains("java/lang/Object"));
    ctx.check("has_java_lang_String",  e1.names.contains("java/lang/String"));
    ctx.check("has_java_lang_Class",   e1.names.contains("java/lang/Class"));
    ctx.check("has_java_lang_Integer", e1.names.contains("java/lang/Integer"));
    ctx.check("has_java_lang_Thread",  e1.names.contains("java/lang/Thread"));

    // ---- Application-loaded class is reached (not just bootstrap). -------
    // The single strongest proof the walk descends into the application loader:
    // this module's OWN fixture, Class.forName'd at startup, MUST be enumerated.
    ctx.check("has_own_fixture_class", e1.names.contains(OWN_FIXTURE));
    ctx.check("own_fixture_seen_by_visitor", e1.own_seen);

    // ---- The enumerated klass pointer is valid AND usable. --------------
    // Every non-null klass* the visitor produced passed the is_valid_pointer gate.
    ctx.check("every_klass_pointer_valid", e1.all_klass_valid);
    // For the OWN fixture specifically: the supplied pointer is valid and its own
    // _name symbol round-trips to the same internal name — pointer and name agree.
    ctx.check("own_fixture_klass_pointer_valid", e1.own_klass_valid);
    ctx.check("own_fixture_klass_name_roundtrips", e1.own_name_roundtrips);

    // ---- Symbol decode never produced empty / malformed names. ----------
    ctx.check("no_empty_name", !e1.any_empty_name);
    ctx.check("no_malformed_name", !e1.any_bad_name);
    // Every distinct name is independently well-formed (second angle over the set,
    // not just the per-visit flag — catches a name that appeared only as a dup).
    bool all_names_wellformed{ true };
    for (const std::string& n : e1.names)
    {
        if (!name_is_wellformed(n))
        {
            all_names_wellformed = false;
            break;
        }
    }
    ctx.check("all_distinct_names_wellformed", all_names_wellformed);

    // ---- Best-effort, informational only (NEVER hard-fail). -------------
    // The launcher-entry class is omitted by HotSpot's JDK 8 SystemDictionary
    // walk, so record its presence rather than asserting it.
    const bool main_present{ e1.names.contains("vmhook/Main") };
    ctx.record(std::string{ "[INFO] for_each_loaded_class: vmhook/Main " }
               + (main_present ? "enumerated"
                               : "NOT enumerated (JDK 8 launcher quirk)"));
    // vmhook/Example IS reliably present on every JDK (the legacy test asserted
    // it); record it as a cross-check alongside Main without making the suite
    // depend on the example class being loaded under the modular harness.
    ctx.record(std::string{ "[INFO] for_each_loaded_class: vmhook/Example " }
               + (e1.names.contains("vmhook/Example") ? "enumerated"
                                                       : "NOT enumerated"));
    // Nested + array anchors the fixture force-loads live in Klass families the
    // JDK 8 dictionary path may not list — informational, never a FAIL.
    ctx.record(std::string{ "[INFO] for_each_loaded_class: nested $Inner " }
               + (e1.names.contains("vmhook/fixtures/ForEachLoadedClass$Inner")
                      ? "enumerated" : "NOT enumerated (dictionary-path quirk)"));
    ctx.record(std::string{ "[INFO] for_each_loaded_class: [I array klass " }
               + (e1.names.contains("[I") ? "enumerated"
                                          : "NOT enumerated (array-klass quirk)"));

    // =====================================================================
    // PASS 2 — snapshot stability: an independent enumeration agrees on every
    // robust invariant.  Enumeration is repeatable and side-effect-free; a
    // second pass must reproduce the floor, the known classes, and a count that
    // has not drifted wildly (a few classes may lazy-load between passes, so the
    // bound is generous rather than exact).
    // =====================================================================
    const enumeration e2{ enumerate_once() };

    ctx.record(std::string{ "[INFO] for_each_loaded_class pass 2 visited " }
               + std::to_string(e2.count) + " klass(es), "
               + std::to_string(e2.names.size()) + " distinct name(s)");

    ctx.check("pass2_count_over_100", e2.count > 100);
    ctx.check("pass2_has_java_lang_Object", e2.names.contains("java/lang/Object"));
    ctx.check("pass2_has_java_lang_String", e2.names.contains("java/lang/String"));
    ctx.check("pass2_has_own_fixture_class", e2.names.contains(OWN_FIXTURE));
    ctx.check("pass2_every_klass_pointer_valid", e2.all_klass_valid);
    ctx.check("pass2_no_empty_name", !e2.any_empty_name);

    // The two passes' distinct-name counts are close: |Δ| stays within a small
    // band (classes only ever ACCRETE between two back-to-back snapshots, and
    // only a handful could lazy-load in that window).  Computed without unsigned
    // wraparound.
    const std::size_t lo{ e1.names.size() < e2.names.size() ? e1.names.size() : e2.names.size() };
    const std::size_t hi{ e1.names.size() < e2.names.size() ? e2.names.size() : e1.names.size() };
    ctx.check("pass_to_pass_name_count_stable", (hi - lo) <= 64);

    // Every distinct name pass 1 saw that is a known-stable class is still present
    // in pass 2 (the core set never disappears between snapshots).
    ctx.check("stable_Object_across_passes",
              e1.names.contains("java/lang/Object") == e2.names.contains("java/lang/Object"));
    ctx.check("stable_own_fixture_across_passes",
              e1.names.contains(OWN_FIXTURE) == e2.names.contains(OWN_FIXTURE));
}
