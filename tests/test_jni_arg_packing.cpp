// Standalone unit test: detail::write_jni_arg_to_slot / append_jni_arg union-member
// writes + needs_release tagging (regression guard for the union-aliasing
// DeleteLocalRef bug). No JVM present -> jni_new_string_utf returns null, so the
// needs_release tag stays false on every path here. Anything requiring a live
// oop / running JVM (the actual DeleteLocalRef cleanup loop, Call*MethodA
// dispatch, result-handle release) is covered by JVM integration in example.cpp.
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

// Minimal object_base-derived wrapper so we can exercise the object-arg branch
// of write_jni_arg_to_slot without a JVM. get_instance() just returns the raw
// pointer we hand the base ctor; no oop is dereferenced.
struct fake_object : vmhook::object_base
{
    explicit fake_object(void* p) noexcept : vmhook::object_base{ p } {}
};

// Helper: pack a single arg into a fresh slot and report back the union value,
// the storage cell, and the needs_release tag the library decided on.
template<typename arg_t>
static auto pack_one(arg_t&& arg, vmhook::detail::jni_value& out_value, void*& out_storage)
    -> bool
{
    out_value = vmhook::detail::jni_value{};
    out_storage = nullptr;
    bool needs_release{ true }; // poison: library MUST overwrite this
    vmhook::detail::write_jni_arg_to_slot(out_value, out_storage, needs_release,
                                          std::forward<arg_t>(arg));
    return needs_release;
}

int main()
{
    // --- Precondition: confirm we really are running without a JVM ----------
    // write_jni_arg_to_slot's string branches call jni_new_string_utf, which
    // returns null when current_jni_env is null. The whole "needs_release stays
    // false for strings" cluster below depends on this being null.
    check("precondition_no_jvm_env_is_null",
          vmhook::hotspot::current_jni_env == nullptr);

    // --- union jni_value layout sanity --------------------------------------
    // All members alias the same storage; this is exactly why reading value.l
    // back to classify a slot is unsound and a dedicated needs_release tag is
    // required. Guard the assumptions the cleanup-loop bug report relies on.
    check("union_jni_value_is_pointer_sized",
          sizeof(vmhook::detail::jni_value) == sizeof(void*));
    {
        vmhook::detail::jni_value v{};
        v.j = static_cast<std::int64_t>(0x1234'5678'1234'5678LL);
        check("union_long_aliases_pointer_member",
              v.l == reinterpret_cast<void*>(static_cast<std::uintptr_t>(v.j)));
        v = vmhook::detail::jni_value{};
        v.z = true;
        check("union_bool_true_aliases_pointer_member",
              v.l == reinterpret_cast<void*>(static_cast<std::uintptr_t>(1)));
    }

    // --- needs_release tag: every primitive leaves it false -----------------
    // This is the core regression guard. A non-zero primitive must NOT be
    // tagged for release (otherwise the cleanup loop DeleteLocalRef's garbage).
    {
        vmhook::detail::jni_value v{};
        void* storage{ nullptr };

        check("needs_release_false_for_bool_true",
              pack_one(true, v, storage) == false);
        check("needs_release_false_for_bool_false",
              pack_one(false, v, storage) == false);
        check("needs_release_false_for_int32_minus_one",
              pack_one(std::int32_t{ -1 }, v, storage) == false);
        check("needs_release_false_for_int32_high_bit",
              pack_one(std::int32_t{ static_cast<std::int32_t>(0x8000'0000) }, v, storage) == false);
        check("needs_release_false_for_int64_sentinel",
              pack_one(std::int64_t{ static_cast<std::int64_t>(0xCAFE'BABE'DEAD'BEEFULL) }, v, storage) == false);
        check("needs_release_false_for_int16",
              pack_one(std::int16_t{ 0x1234 }, v, storage) == false);
        check("needs_release_false_for_uint16",
              pack_one(std::uint16_t{ 0xBEEF }, v, storage) == false);
        check("needs_release_false_for_int8",
              pack_one(std::int8_t{ -7 }, v, storage) == false);
        check("needs_release_false_for_float_one",
              pack_one(float{ 1.0f }, v, storage) == false);
        check("needs_release_false_for_double_one",
              pack_one(double{ 1.0 }, v, storage) == false);
    }

    // --- needs_release tag: object-reference args leave it false ------------
    // Synthetic stack handles (value.l points INTO `storage`) are not JNI local
    // refs and must not be tagged for release.
    {
        vmhook::detail::jni_value v{};
        void* storage{ nullptr };
        fake_object obj{ reinterpret_cast<void*>(static_cast<std::uintptr_t>(0xABCD'0000)) };
        const bool rel{ pack_one(obj, v, storage) };
        check("needs_release_false_for_object_arg", rel == false);
        // value.l must point at the caller's storage cell, and storage must hold
        // the object's instance pointer (the synthetic-handle indirection).
        check("object_arg_value_l_points_at_storage",
              v.l == static_cast<void*>(&storage));
        check("object_arg_storage_holds_instance",
              storage == obj.get_instance());
    }

    // --- needs_release tag: null c-string leaves it false -------------------
    // A null const char* never calls NewStringUTF, so value.l is null and the
    // tag is false. (A non-null c-string WOULD call NewStringUTF, but with no
    // JVM that returns null too -> still false; asserted below.)
    {
        vmhook::detail::jni_value v{};
        void* storage{ nullptr };
        const char* null_cstr{ nullptr };
        const bool rel{ pack_one(null_cstr, v, storage) };
        check("needs_release_false_for_null_cstring", rel == false);
        check("null_cstring_value_l_is_null", v.l == nullptr);
    }

    // --- needs_release tag: string args stay false WITHOUT a JVM ------------
    // jni_new_string_utf returns null (no env) so value.l == nullptr and the
    // library tags needs_release = (value.l != nullptr) == false. This is the
    // explicitly-requested "assert that path too". With a live JVM these would
    // flip to true and be released -- covered by JVM integration in example.cpp.
    {
        vmhook::detail::jni_value v{};
        void* storage{ nullptr };

        check("needs_release_false_for_std_string_no_jvm",
              pack_one(std::string{ "hi" }, v, storage) == false);
        check("std_string_value_l_null_no_jvm", v.l == nullptr);

        check("needs_release_false_for_string_view_no_jvm",
              pack_one(std::string_view{ "world" }, v, storage) == false);
        check("string_view_value_l_null_no_jvm", v.l == nullptr);

        const char* cstr{ "literal" };
        check("needs_release_false_for_nonnull_cstring_no_jvm",
              pack_one(cstr, v, storage) == false);
        check("nonnull_cstring_value_l_null_no_jvm", v.l == nullptr);
    }

    // --- union member writes land in the RIGHT member -----------------------
    // The cluster focus: each primitive must write the documented union field.
    // bool -> .z, integral<=4B -> .i (NOT .s/.b/.c), integral==8B -> .j,
    // float -> .f, double -> .d. We read .l back too to document the aliasing.
    {
        vmhook::detail::jni_value v{};
        void* storage{ nullptr };

        pack_one(true, v, storage);
        check("bool_true_writes_z_member", v.z == true);

        pack_one(false, v, storage);
        check("bool_false_writes_z_member", v.z == false);

        pack_one(std::int32_t{ 42 }, v, storage);
        check("int32_writes_i_member", v.i == 42);

        pack_one(std::int32_t{ -1 }, v, storage);
        check("int32_minus_one_writes_i_member", v.i == -1);

        // int16/uint16/int8 are promoted into the .i (int32) member, NOT .s/.c/.b.
        // This is the documented behaviour of write_jni_arg_to_slot's
        // `integral && sizeof<=int32` branch.
        pack_one(std::int16_t{ 0x1234 }, v, storage);
        check("int16_writes_i_member_widened", v.i == 0x1234);

        pack_one(std::uint16_t{ 0xBEEF }, v, storage);
        check("uint16_writes_i_member_widened", v.i == static_cast<std::int32_t>(0xBEEF));

        pack_one(std::int8_t{ -7 }, v, storage);
        check("int8_writes_i_member_widened", v.i == -7);

        pack_one(std::int64_t{ 0x1234'5678'90AB'CDEFLL }, v, storage);
        check("int64_writes_j_member", v.j == 0x1234'5678'90AB'CDEFLL);

        pack_one(float{ 3.5f }, v, storage);
        check("float_writes_f_member", v.f == 3.5f);

        pack_one(double{ 2.71828 }, v, storage);
        check("double_writes_d_member", v.d == 2.71828);
    }

    // --- union-aliasing footgun, demonstrated on the real packer ------------
    // A long sentinel written via .j re-reads through .l as a non-null, bogus
    // pointer. The cleanup loop would DeleteLocalRef THIS if it trusted .l --
    // which is precisely why needs_release stayed false above. Pin the hazard.
    {
        vmhook::detail::jni_value v{};
        void* storage{ nullptr };
        const std::int64_t sentinel{ static_cast<std::int64_t>(0xCAFE'BABE'DEAD'BEEFULL) };
        const bool rel{ pack_one(sentinel, v, storage) };
        check("long_sentinel_aliases_nonnull_l",
              v.l == reinterpret_cast<void*>(static_cast<std::uintptr_t>(sentinel)));
        check("long_sentinel_l_is_not_storage_cell",
              v.l != static_cast<void*>(&storage));
        check("long_sentinel_not_tagged_for_release", rel == false);
    }

    // --- value is zero-initialized before the member write ------------------
    // write_jni_arg_to_slot does `value = jni_value{}` first, so a bool false
    // leaves the entire pointer-sized cell zero (no stale upper bits).
    {
        vmhook::detail::jni_value v{};
        void* storage{ nullptr };
        // Pre-dirty the slot, then pack a 'false' and confirm full-width clear.
        v.j = static_cast<std::int64_t>(0xFFFF'FFFF'FFFF'FFFFULL);
        bool needs_release{ true };
        vmhook::detail::write_jni_arg_to_slot(v, storage, needs_release, false);
        check("bool_false_clears_full_union_width", v.l == nullptr);
    }

    // --- vector path: make_jni_args / append_jni_arg parity -----------------
    // jni_make_unique uses the std::vector<char> needs_release tag rather than a
    // single bool, but the classification logic is identical. Confirm the same
    // invariant: primitives tagged 0, no JVM means string args also tagged 0.
    {
        std::vector<void*> object_handles{};
        std::vector<char>  needs_release{};
        std::vector<vmhook::detail::jni_value> values{
            vmhook::detail::make_jni_args(
                object_handles, needs_release,
                std::int64_t{ static_cast<std::int64_t>(0xCAFE'BABE'DEAD'BEEFULL) },
                std::int32_t{ -1 },
                true,
                float{ 1.0f },
                double{ 2.0 },
                std::int16_t{ 0x1234 },
                std::string{ "no_jvm_so_null" })
        };

        check("make_jni_args_value_count_matches", values.size() == 7);
        check("make_jni_args_tag_count_matches", needs_release.size() == 7);

        // Every tag must be 0: six primitives + one string that NewStringUTF
        // could not build (no env). Not a single slot is releasable here.
        bool all_zero{ true };
        for (const char tag : needs_release)
        {
            if (tag != 0) { all_zero = false; }
        }
        check("make_jni_args_no_slot_tagged_for_release", all_zero);

        // Spot-check the union members landed correctly through the vector path.
        check("make_jni_args_long_in_j_member",
              values[0].j == static_cast<std::int64_t>(0xCAFE'BABE'DEAD'BEEFULL));
        check("make_jni_args_int_in_i_member", values[1].i == -1);
        check("make_jni_args_bool_in_z_member", values[2].z == true);
        check("make_jni_args_float_in_f_member", values[3].f == 1.0f);
        check("make_jni_args_double_in_d_member", values[4].d == 2.0);
        check("make_jni_args_int16_widened_in_i_member", values[5].i == 0x1234);
        check("make_jni_args_string_l_null_no_jvm", values[6].l == nullptr);
    }

    return failures == 0 ? 0 : 1;
}
