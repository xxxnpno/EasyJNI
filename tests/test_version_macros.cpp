// Standalone (no-JVM) unit test for the VMHOOK version macros: proves the three
// component macros (MAJOR/MINOR/PATCH) are internally consistent with the packed
// VMHOOK_VERSION integer, that VMHOOK_MAKE_VERSION packs in the documented way,
// and that the packed value is monotonic with respect to its components.
//
// Source of truth is vmhook.hpp:
//   VMHOOK_MAKE_VERSION(major, minor, patch) ==
//       (((major) * 1000000) + ((minor) * 1000) + (patch))
//   VMHOOK_VERSION == VMHOOK_MAKE_VERSION(MAJOR, MINOR, PATCH)
// i.e. a *decimal* pack (major*1e6 + minor*1e3 + patch), NOT a bit shift/mask.
// Each component occupies a 3-decimal-digit field, so the pack is only lossless
// while MINOR < 1000 and PATCH < 1000 -- several checks below pin that invariant
// because the field-decompose identities depend on it.
//
// This file deliberately overlaps as little as possible with the lighter
// test_version_macros() already living in test_helpers.cpp; it goes deeper on
// the packing formula, monotonicity, and the string<->component agreement.
//
// Anything requiring a live oop / running JVM is out of scope here (there is no
// JVM in this process) -- covered by JVM integration in example.cpp.
#include <vmhook/vmhook.hpp>
#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>

static int failures{ 0 };
static auto check(const char* name, bool ok) -> void
{
    std::printf("%s %s\n", ok ? "[PASS]" : "[FAIL]", name);
    if (!ok) { ++failures; }
}

int main()
{
    // -----------------------------------------------------------------------
    // The three component macros must be defined and expand to integers.
    // (If any were missing the file would not compile, so reaching here with a
    // value already proves existence; these checks pin the documented domain.)
    // -----------------------------------------------------------------------
    constexpr int v_major{ VMHOOK_VERSION_MAJOR };
    constexpr int v_minor{ VMHOOK_VERSION_MINOR };
    constexpr int v_patch{ VMHOOK_VERSION_PATCH };

    check("major_is_nonnegative", v_major >= 0);
    check("minor_is_nonnegative", v_minor >= 0);
    check("patch_is_nonnegative", v_patch >= 0);

    // Each component must fit its 3-decimal-digit field or the decimal pack
    // would carry into the neighbouring field and silently corrupt the version.
    check("minor_within_field_range", v_minor >= 0 && v_minor < 1000);
    check("patch_within_field_range", v_patch >= 0 && v_patch < 1000);

    // -----------------------------------------------------------------------
    // VMHOOK_MAKE_VERSION field weights (the exact documented formula).
    // -----------------------------------------------------------------------
    static_assert(VMHOOK_MAKE_VERSION(0, 0, 0) == 0,
                  "MAKE(0,0,0) must be 0");
    static_assert(VMHOOK_MAKE_VERSION(1, 0, 0) == 1000000,
                  "major field weight must be 1'000'000");
    static_assert(VMHOOK_MAKE_VERSION(0, 1, 0) == 1000,
                  "minor field weight must be 1'000");
    static_assert(VMHOOK_MAKE_VERSION(0, 0, 1) == 1,
                  "patch field weight must be 1");
    check("make_version_zero", VMHOOK_MAKE_VERSION(0, 0, 0) == 0);
    check("make_version_major_weight", VMHOOK_MAKE_VERSION(1, 0, 0) == 1000000);
    check("make_version_minor_weight", VMHOOK_MAKE_VERSION(0, 1, 0) == 1000);
    check("make_version_patch_weight", VMHOOK_MAKE_VERSION(0, 0, 1) == 1);

    // Combined pack of an arbitrary triple equals the explicit decimal sum.
    static_assert(VMHOOK_MAKE_VERSION(1, 2, 3) == 1002003,
                  "MAKE(1,2,3) must pack to 1'002'003");
    check("make_version_combined_1_2_3", VMHOOK_MAKE_VERSION(1, 2, 3) == 1002003);
    check("make_version_combined_max_fields",
          VMHOOK_MAKE_VERSION(7, 999, 999) == (7 * 1000000) + (999 * 1000) + 999);

    // The pack must equal the open-coded formula for the live component values.
    check("make_version_matches_formula_for_components",
          VMHOOK_MAKE_VERSION(VMHOOK_VERSION_MAJOR, VMHOOK_VERSION_MINOR, VMHOOK_VERSION_PATCH)
              == (v_major * 1000000) + (v_minor * 1000) + v_patch);

    // -----------------------------------------------------------------------
    // VMHOOK_VERSION must be exactly the packed form of its three components.
    // -----------------------------------------------------------------------
    static_assert(VMHOOK_VERSION == VMHOOK_MAKE_VERSION(VMHOOK_VERSION_MAJOR,
                                                        VMHOOK_VERSION_MINOR,
                                                        VMHOOK_VERSION_PATCH),
                  "VMHOOK_VERSION must equal MAKE(MAJOR,MINOR,PATCH)");
    constexpr int packed{ VMHOOK_VERSION };
    check("version_equals_packed_components",
          packed == VMHOOK_MAKE_VERSION(VMHOOK_VERSION_MAJOR,
                                        VMHOOK_VERSION_MINOR,
                                        VMHOOK_VERSION_PATCH));
    check("version_equals_open_coded_sum",
          packed == (v_major * 1000000) + (v_minor * 1000) + v_patch);

    // Round-trip: recomputing from the parts reproduces the macro verbatim.
    check("version_roundtrip_through_make",
          VMHOOK_MAKE_VERSION(VMHOOK_VERSION_MAJOR,
                              VMHOOK_VERSION_MINOR,
                              VMHOOK_VERSION_PATCH) == VMHOOK_VERSION);

    // -----------------------------------------------------------------------
    // Decompose the packed integer back into fields.  These identities only
    // hold because minor/patch are < 1000 (asserted above); together they prove
    // no field bled into another during packing.
    // -----------------------------------------------------------------------
    check("version_decompose_major", (packed / 1000000) == v_major);
    check("version_decompose_minor", ((packed / 1000) % 1000) == v_minor);
    check("version_decompose_patch", (packed % 1000) == v_patch);

    // -----------------------------------------------------------------------
    // Monotonicity of the pack with respect to each component.  A larger triple
    // (lexicographically) must produce a strictly larger packed integer.
    // -----------------------------------------------------------------------
    static_assert(VMHOOK_MAKE_VERSION(0, 0, 1) > VMHOOK_MAKE_VERSION(0, 0, 0),
                  "bumping patch must increase the packed value");
    static_assert(VMHOOK_MAKE_VERSION(0, 1, 0) > VMHOOK_MAKE_VERSION(0, 0, 999),
                  "bumping minor must outweigh a maxed-out patch");
    static_assert(VMHOOK_MAKE_VERSION(1, 0, 0) > VMHOOK_MAKE_VERSION(0, 999, 999),
                  "bumping major must outweigh maxed-out minor.patch");

    check("monotonic_patch_step",
          VMHOOK_MAKE_VERSION(2, 3, 5) > VMHOOK_MAKE_VERSION(2, 3, 4));
    check("monotonic_minor_step",
          VMHOOK_MAKE_VERSION(2, 4, 0) > VMHOOK_MAKE_VERSION(2, 3, 0));
    check("monotonic_major_step",
          VMHOOK_MAKE_VERSION(3, 0, 0) > VMHOOK_MAKE_VERSION(2, 0, 0));

    // Field-dominance: one minor step always exceeds the entire patch field,
    // and one major step always exceeds the entire minor.patch range.  This is
    // the property that makes `#if VMHOOK_VERSION >= MAKE(x,y,z)` gating sound.
    check("minor_field_dominates_patch_field",
          VMHOOK_MAKE_VERSION(2, 4, 0) > VMHOOK_MAKE_VERSION(2, 3, 999));
    check("major_field_dominates_minor_field",
          VMHOOK_MAKE_VERSION(3, 0, 0) > VMHOOK_MAKE_VERSION(2, 999, 999));

    // The live packed value is non-negative and strictly ordered against its
    // immediate neighbours: the next patch up must exceed it, and the previous
    // patch down (when patch>0) must be below it.  This anchors the monotonicity
    // property to the actual shipped version, not just synthetic triples.
    check("version_nonnegative", packed >= 0);
    check("version_strictly_below_next_patch",
          packed < VMHOOK_MAKE_VERSION(v_major, v_minor, v_patch + 1));
    check("version_strictly_above_prev_patch",
          (v_patch == 0)
              || (packed > VMHOOK_MAKE_VERSION(v_major, v_minor, v_patch - 1)));

    // -----------------------------------------------------------------------
    // Version-gating idiom documented in the header comment:
    //   #if VMHOOK_VERSION >= VMHOOK_MAKE_VERSION(0,3,0)
    // The project is past 0.3.0 and still in the 0.x major series, so these are
    // genuine assertions about the current released state, not tautologies.
    // -----------------------------------------------------------------------
    check("version_at_least_0_3_0", packed >= VMHOOK_MAKE_VERSION(0, 3, 0));
    check("version_below_1_0_0_while_major_zero",
          (v_major != 0) || (packed < VMHOOK_MAKE_VERSION(1, 0, 0)));
    // A 0.x version must sit strictly inside the major-0 band [0, 1'000'000).
    check("version_inside_major_band",
          (v_major != 0) || (packed >= 0 && packed < 1000000));

    // -----------------------------------------------------------------------
    // VMHOOK_VERSION_STRING must read "MAJOR.MINOR.PATCH" with exactly two dots,
    // no stray whitespace from token-paste/stringize, and must agree digit-for-
    // digit with the numeric components.
    // -----------------------------------------------------------------------
    const std::string version_text{ VMHOOK_VERSION_STRING };
    check("version_string_not_empty", !version_text.empty());

    std::size_t dot_count{ 0 };
    std::size_t space_count{ 0 };
    for (const char c : version_text)
    {
        if (c == '.') { ++dot_count; }
        if (c == ' ' || c == '\t') { ++space_count; }
    }
    check("version_string_has_exactly_two_dots", dot_count == 2);
    check("version_string_has_no_whitespace", space_count == 0);
    check("version_string_first_char_is_digit",
          !version_text.empty() && version_text.front() >= '0' && version_text.front() <= '9');
    check("version_string_last_char_is_digit",
          !version_text.empty() && version_text.back() >= '0' && version_text.back() <= '9');

    // Rebuild the expected "M.m.p" from the numeric components and compare.
    char expected[64]{};
    const int written{ std::snprintf(expected, sizeof(expected), "%d.%d.%d",
                                     v_major, v_minor, v_patch) };
    check("version_string_snprintf_ok", written > 0 && written < static_cast<int>(sizeof(expected)));
    check("version_string_matches_components", version_text == std::string{ expected });

    // -----------------------------------------------------------------------
    // Cross-check against the CMake project version.  By default this standalone
    // test is NOT compiled with -DVMHOOK_CMAKE_VERSION_* (only the helpers test
    // target defines them in tests/CMakeLists.txt), so this block is normally
    // skipped -- when absent we only verify internal macro consistency, exactly
    // as the audit specifies.
    // -----------------------------------------------------------------------
#if defined(VMHOOK_CMAKE_VERSION_MAJOR) && defined(VMHOOK_CMAKE_VERSION_MINOR) \
    && defined(VMHOOK_CMAKE_VERSION_PATCH)
    check("cmake_version_matches_header_major",
          VMHOOK_CMAKE_VERSION_MAJOR == VMHOOK_VERSION_MAJOR);
    check("cmake_version_matches_header_minor",
          VMHOOK_CMAKE_VERSION_MINOR == VMHOOK_VERSION_MINOR);
    check("cmake_version_matches_header_patch",
          VMHOOK_CMAKE_VERSION_PATCH == VMHOOK_VERSION_PATCH);
    check("cmake_version_matches_packed",
          VMHOOK_MAKE_VERSION(VMHOOK_CMAKE_VERSION_MAJOR,
                              VMHOOK_CMAKE_VERSION_MINOR,
                              VMHOOK_CMAKE_VERSION_PATCH) == VMHOOK_VERSION);
#else
    std::printf("[SKIP] cmake_version_cross_check "
                "(VMHOOK_CMAKE_VERSION_* not defined for this target)\n");
#endif

    return failures == 0 ? 0 : 1;
}
