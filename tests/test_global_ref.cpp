// Standalone (no-JVM) unit test for vmhook::jni::global_ref and vmhook::pin().
//
// global_ref is the GC-pin lifetime primitive ported from the NPNOQOL fork:
// it keeps a Java object alive across relocating GCs and re-derives the live
// (relocated) address via oop().  The cross-GC behaviour itself needs a live
// JVM (covered by the JVM integration suite in example.cpp); here we pin down
// the parts that are fully testable without a JVM:
//   * move-only semantics (copy is statically disabled),
//   * null / empty construction is safe and inert,
//   * a moved-from pin is empty, the moved-to pin owns the (empty) state,
//   * self-move-assign doesn't corrupt state,
//   * with no JVM, constructing from a non-null fake OOP stays empty (NewGlobalRef
//     can't run), and destruction / reset never crash,
//   * pin() free helpers compile and produce empty pins without a JVM.
//
// Anything requiring a real pinned object that survives System.gc() is out of
// scope here — see test_global_ref_survives_gc in the JVM integration driver.
#include <vmhook/vmhook.hpp>

#include <cstdio>
#include <cstdint>
#include <memory>
#include <type_traits>
#include <utility>

static int failures{ 0 };
static auto check(const char* name, bool ok) -> void
{
    std::printf("%s %s\n", ok ? "[PASS]" : "[FAIL]", name);
    if (!ok) { ++failures; }
}

// A minimal wrapper to exercise the pin(unique_ptr<T>) overload's compile path.
namespace
{
    class dummy_wrapper : public vmhook::object<dummy_wrapper>
    {
    public:
        explicit dummy_wrapper(vmhook::oop_t instance) noexcept
            : vmhook::object<dummy_wrapper>{ instance }
        {
        }
    };
}

int main()
{
    using vmhook::jni::global_ref;

    // -- Type properties: move-only ------------------------------------------
    static_assert(!std::is_copy_constructible_v<global_ref>,
                  "global_ref must not be copy-constructible (single ownership)");
    static_assert(!std::is_copy_assignable_v<global_ref>,
                  "global_ref must not be copy-assignable (single ownership)");
    static_assert(std::is_move_constructible_v<global_ref>,
                  "global_ref must be move-constructible");
    static_assert(std::is_move_assignable_v<global_ref>,
                  "global_ref must be move-assignable");
    static_assert(std::is_nothrow_move_constructible_v<global_ref>,
                  "global_ref move ctor must be noexcept");
    check("move_only_type_traits", true);

    // -- Default construction is empty/inert ---------------------------------
    {
        global_ref empty{};
        check("default_constructed_is_falsy", !static_cast<bool>(empty));
        check("default_constructed_oop_is_null", empty.oop() == nullptr);
        check("default_constructed_handle_is_null", empty.handle() == nullptr);
        empty.reset();  // reset on empty must be safe
        check("reset_on_empty_is_safe", empty.oop() == nullptr);
    }

    // -- Null-OOP construction is empty/inert (no NewGlobalRef issued) --------
    {
        global_ref from_null{ static_cast<vmhook::oop_t>(nullptr) };
        check("null_oop_construct_is_falsy", !static_cast<bool>(from_null));
        check("null_oop_construct_oop_is_null", from_null.oop() == nullptr);
    }

    // -- Non-null fake OOP with NO JVM: NewGlobalRef can't run -> stays empty -
    // (current_jni_env is null in this process, so jni_new_global_ref returns
    //  nullptr; the pin must end up empty, not holding a bogus handle.)
    {
        auto* const fake_oop{ reinterpret_cast<vmhook::oop_t>(
            static_cast<std::uintptr_t>(0x1000)) };
        global_ref no_jvm{ fake_oop };
        check("non_null_oop_without_jvm_is_empty", !static_cast<bool>(no_jvm));
        check("non_null_oop_without_jvm_oop_is_null", no_jvm.oop() == nullptr);
    }  // destructor runs on an empty handle -> no crash

    // -- Move construction transfers ownership; source becomes empty ---------
    {
        global_ref a{};                       // empty (no JVM anyway)
        global_ref b{ std::move(a) };
        check("moved_from_is_falsy", !static_cast<bool>(a));
        check("moved_from_oop_is_null", a.oop() == nullptr);
        check("moved_to_is_consistent", b.oop() == nullptr);  // both empty here
    }

    // -- Move assignment ------------------------------------------------------
    {
        global_ref a{};
        global_ref b{};
        b = std::move(a);
        check("move_assign_source_empty", !static_cast<bool>(a));
        check("move_assign_dest_consistent", b.oop() == nullptr);
    }

    // -- Self-move-assign must not corrupt or double-free --------------------
    {
        global_ref a{};
        global_ref& ref{ a };
        ref = std::move(a);   // guarded by this != &other
        check("self_move_assign_safe", a.oop() == nullptr);
    }

    // -- pin() free helpers compile and are inert without a JVM --------------
    {
        auto pinned = vmhook::pin(static_cast<vmhook::oop_t>(nullptr));
        check("pin_null_oop_is_empty", !static_cast<bool>(pinned));

        std::unique_ptr<dummy_wrapper> null_wrapper{};
        auto pinned_wrapper = vmhook::pin(null_wrapper);
        check("pin_null_wrapper_is_empty", !static_cast<bool>(pinned_wrapper));
    }

    // -- A vector of pins move-relocates cleanly (the unordered_map use case) -
    {
        std::vector<global_ref> pins;
        pins.emplace_back();
        pins.emplace_back(static_cast<vmhook::oop_t>(nullptr));
        pins.reserve(64);  // forces a move-relocation of existing elements
        check("vector_of_pins_relocates", pins.size() == 2 && pins[0].oop() == nullptr);
    }

    return failures == 0 ? 0 : 1;
}
