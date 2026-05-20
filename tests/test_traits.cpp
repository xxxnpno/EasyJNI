// Compile-time-only test for the header's type traits.  Every static_assert
// here proves a property of the API surface the rest of the library relies
// on; the executable just succeeds when produced.
#include <vmhook/vmhook.hpp>

#include <cstdio>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

// -----------------------------------------------------------------------------
// is_vector
// -----------------------------------------------------------------------------
static_assert(vmhook::detail::is_vector_v<std::vector<int>>,
              "is_vector_v must recognise std::vector<int>");
static_assert(vmhook::detail::is_vector_v<const std::vector<int>&>,
              "is_vector_v must strip cv-ref before testing");
static_assert(!vmhook::detail::is_vector_v<int>,
              "is_vector_v must reject non-vector types");

// is_vector<...>::value_type_t had the same template-parameter-shadowing bug
// is_unique_ptr had (see below): the std::true_type base inherits a
// `using value_type = bool` typedef which silently won over the template
// parameter, making value_type_t resolve to bool for every vector type.
static_assert(std::is_same_v<typename vmhook::detail::is_vector<std::vector<int>>::value_type_t, int>,
              "is_vector<vector<int>>::value_type_t must be int, NOT bool "
              "(template-parameter shadowing the std::true_type::value_type "
              "typedef would silently regress this)");
static_assert(std::is_same_v<typename vmhook::detail::is_vector<std::vector<double>>::value_type_t, double>,
              "is_vector<vector<double>>::value_type_t must be double");
static_assert(std::is_same_v<typename vmhook::detail::is_vector<std::vector<std::string>>::value_type_t, std::string>,
              "is_vector<vector<string>>::value_type_t must be string");

// -----------------------------------------------------------------------------
// is_unique_ptr
// -----------------------------------------------------------------------------
static_assert(vmhook::detail::is_unique_ptr_v<std::unique_ptr<int>>,
              "is_unique_ptr_v must recognise std::unique_ptr<int>");
static_assert(!vmhook::detail::is_unique_ptr_v<int*>,
              "is_unique_ptr_v must reject raw pointers");
static_assert(vmhook::detail::is_unique_ptr_v<const std::unique_ptr<int>&>,
              "is_unique_ptr_v must strip cv-ref before testing");

// THIS is the regression test for the bug commit 9466ca5 fixed.  Before the
// fix, the partial specialisation was written as:
//
//   template<typename value_type, typename deleter_type>
//   struct is_unique_ptr<std::unique_ptr<value_type, deleter_type>>
//       : std::true_type
//   { using value_type_t = value_type; };
//
// The std::true_type base (= std::integral_constant<bool, true>) brings in
// `using value_type = bool;`.  Inside the class body, unqualified lookup of
// `value_type` finds the INHERITED typedef before the template parameter of
// the same name, so value_type_t collapsed to bool for every wrapper type.
// Downstream `if constexpr (is_base_of_v<object_base, value_type_t>)` then
// silently skipped the JNI-arg-write branch, leaving values[i].l == 0 and
// dispatching null IChatComponent into Lunar / Forge / vanilla.
static_assert(std::is_same_v<typename vmhook::detail::is_unique_ptr<std::unique_ptr<int>>::value_type_t, int>,
              "is_unique_ptr<unique_ptr<int>>::value_type_t must be int, NOT bool "
              "(template-parameter shadow regression).");
static_assert(std::is_same_v<typename vmhook::detail::is_unique_ptr<std::unique_ptr<std::string>>::value_type_t, std::string>,
              "is_unique_ptr<unique_ptr<string>>::value_type_t must be string");
static_assert(std::is_same_v<typename vmhook::detail::is_unique_ptr<std::unique_ptr<vmhook::object_base>>::value_type_t, vmhook::object_base>,
              "is_unique_ptr<unique_ptr<object_base>>::value_type_t must be object_base "
              "(this is the exact trait usage that drives write_jni_arg_to_slot and "
              "would re-introduce the chat-not-sending bug if it broke).");

// And the indirect chain that makes the bug bite: a typical vmhook wrapper
// like `class my_wrapper : public vmhook::object<my_wrapper>` is what users
// pass through unique_ptr.  Verify the trait + is_base_of combination
// resolves correctly.
namespace {
    struct test_wrapper : public vmhook::object<test_wrapper> {
        using vmhook::object<test_wrapper>::object;
    };
}
static_assert(std::is_base_of_v<vmhook::object_base, test_wrapper>,
              "vmhook::object<T> -> object_base inheritance must hold for the "
              "static_assert in write_jni_arg_to_slot to accept user wrappers");
static_assert(std::is_base_of_v<
                  vmhook::object_base,
                  typename vmhook::detail::is_unique_ptr<std::unique_ptr<test_wrapper>>::value_type_t>,
              "End-to-end: is_unique_ptr<unique_ptr<MyWrapper>>::value_type_t must yield "
              "a type that derives from object_base.  Regression of this is what made "
              "every player->add_chat_message(...) call pass null to the JVM.");

// -----------------------------------------------------------------------------
// is_unique_object_ptr (sibling trait — has bool_constant base, same shadow risk)
// -----------------------------------------------------------------------------
static_assert(vmhook::detail::is_unique_object_ptr<std::unique_ptr<test_wrapper>>::value,
              "is_unique_object_ptr must report true for unique_ptr<MyWrapper>");
static_assert(!vmhook::detail::is_unique_object_ptr<std::unique_ptr<int>>::value,
              "is_unique_object_ptr must report false for unique_ptr<int> "
              "(int is not an object_base)");
static_assert(!vmhook::detail::is_unique_object_ptr<int>::value,
              "is_unique_object_ptr must report false for raw int");

// -----------------------------------------------------------------------------
// dependent_false_v — the lazy static_assert helper used by the new
// fall-through guards in write_jni_arg_to_slot / append_jni_arg.
// -----------------------------------------------------------------------------
static_assert(!vmhook::detail::dependent_false_v<int>,
              "dependent_false_v<T> must always be false (it is meant to be passed "
              "to static_assert in discarded if-constexpr branches and only fire "
              "when its branch is actually reached at instantiation)");
static_assert(!vmhook::detail::dependent_false_v<std::vector<int>>,
              "dependent_false_v<T> must be false for any T");

// -----------------------------------------------------------------------------
// vmhook::jni namespace — public surface
//
// New public spelling for the JNI helpers that used to require digging into
// vmhook::detail::jni_*.  These static_asserts pin down the type signatures
// of the wrappers so accidental drift between detail::jni_* and jni::* (or
// missing wrappers as the API grows) is caught at build time.
// -----------------------------------------------------------------------------
static_assert(std::is_same_v<vmhook::jni::value, vmhook::detail::jni_value>,
              "vmhook::jni::value must alias the underlying jni_value union");

// These are non-template, non-overloaded - take their address and check the type.
// If the wrapper signature ever drifts from the underlying detail function, the
// types won't match and this fires at compile time.
static_assert(std::is_invocable_r_v<void*, decltype(vmhook::jni::find_class), std::string_view>,
              "vmhook::jni::find_class must accept string_view and return void* (jclass handle)");
static_assert(std::is_invocable_r_v<void*, decltype(vmhook::jni::decode_object), void*>,
              "vmhook::jni::decode_object must take a jobject and return the decoded oop");
static_assert(std::is_invocable_r_v<void*, decltype(vmhook::jni::new_string_utf), std::string_view>,
              "vmhook::jni::new_string_utf must accept string_view");
static_assert(std::is_invocable_r_v<std::string, decltype(vmhook::jni::get_string_utf), void*>,
              "vmhook::jni::get_string_utf must return std::string");
static_assert(std::is_invocable_r_v<void, decltype(vmhook::jni::exception_clear)>,
              "vmhook::jni::exception_clear must take no args");
static_assert(std::is_invocable_r_v<void*, decltype(vmhook::jni::get_object_class), void*>,
              "vmhook::jni::get_object_class must take a jobject and return a jclass");

// signature_for_arg<T> returns std::string (non-constexpr) so the cross-check
// against the underlying detail::jni_signature_for_arg lives in
// test_helpers.cpp where we can call it at runtime.  Here we just confirm the
// wrapper exists and the return type matches.
static_assert(std::is_same_v<decltype(vmhook::jni::signature_for_arg<int>()), std::string>,
              "signature_for_arg<T> must return std::string");

// -----------------------------------------------------------------------------
// java_slot_offsets — JVM interpreter slot widths for long / double
//
// HotSpot stores Java `long` and `double` parameters in TWO adjacent locals
// slots; every other type takes one.  Before this trait existed, the wrapper
// in vmhook::hook<T>() handed extract_frame_arg the C++ tuple index directly
// as the slot index, which silently read garbage for every arg following a
// long or double.
// -----------------------------------------------------------------------------
// Slot widths
static_assert( vmhook::detail::is_java_double_slot_v<std::int64_t>,
               "Java long must take 2 slots");
static_assert( vmhook::detail::is_java_double_slot_v<std::uint64_t>,
               "uint64_t (also mapped to Java long) must take 2 slots");
static_assert( vmhook::detail::is_java_double_slot_v<double>,
               "Java double must take 2 slots");
static_assert(!vmhook::detail::is_java_double_slot_v<std::int32_t>,
              "Java int must take 1 slot");
static_assert(!vmhook::detail::is_java_double_slot_v<bool>,
              "Java boolean must take 1 slot");
static_assert(!vmhook::detail::is_java_double_slot_v<float>,
              "Java float must take 1 slot (NOT double - different type)");
static_assert(!vmhook::detail::is_java_double_slot_v<void*>,
              "Object refs take 1 slot");
static_assert(!vmhook::detail::is_java_double_slot_v<std::string>,
              "String args take 1 slot");

// Slot offset tables
static_assert(vmhook::detail::java_slot_offsets<std::tuple<>>::value.size() == 0,
              "Empty tuple yields empty offsets");

static_assert(
    vmhook::detail::java_slot_offsets<std::tuple<std::int32_t, std::int32_t, std::int32_t>>::value
    == std::array<std::int32_t, 3>{ 0, 1, 2 },
    "Three ints: tuple index == slot index");

// (int, long, int) — the classic regression case: previously the second int
// was read from slot 2 (the high half of the long) instead of slot 3.
static_assert(
    vmhook::detail::java_slot_offsets<std::tuple<std::int32_t, std::int64_t, std::int32_t>>::value
    == std::array<std::int32_t, 3>{ 0, 1, 3 },
    "(int, long, int): the trailing int must be slot 3, NOT slot 2 - "
    "long occupies slots 1 and 2");

// (long, long, int) — two longs in a row.
static_assert(
    vmhook::detail::java_slot_offsets<std::tuple<std::int64_t, std::int64_t, std::int32_t>>::value
    == std::array<std::int32_t, 3>{ 0, 2, 4 },
    "(long, long, int): trailing int at slot 4 - each long takes 2 slots");

// (double, int, double) — double has the same slot-width-2 semantics as long.
static_assert(
    vmhook::detail::java_slot_offsets<std::tuple<double, std::int32_t, double>>::value
    == std::array<std::int32_t, 3>{ 0, 2, 3 },
    "(double, int, double): doubles occupy 2 slots each");

// (this[object], long, int) — instance-method case.  `this` is a 1-slot oop.
static_assert(
    vmhook::detail::java_slot_offsets<std::tuple<void*, std::int64_t, std::int32_t>>::value
    == std::array<std::int32_t, 3>{ 0, 1, 3 },
    "(this, long, int): `this` is 1 slot, long takes 2, trailing int at slot 3");

// -----------------------------------------------------------------------------
// Platform / compiler / arch self-check (unchanged)
// -----------------------------------------------------------------------------
#if (VMHOOK_OS_WINDOWS + VMHOOK_OS_LINUX + VMHOOK_OS_MACOS \
   + VMHOOK_OS_IOS    + VMHOOK_OS_ANDROID) != 1
#  error "exactly one VMHOOK_OS_* macro should be 1"
#endif

// VMHOOK_OS_POSIX is the OR of all POSIX-flavored backends.
#if VMHOOK_OS_WINDOWS && VMHOOK_OS_POSIX
#  error "POSIX detection is inconsistent with Windows detection"
#endif

#if VMHOOK_COMPILER_MSVC + VMHOOK_COMPILER_GCC + VMHOOK_COMPILER_CLANG != 1
#  error "exactly one compiler macro should be 1"
#endif

#if (VMHOOK_ARCH_X86_64 + VMHOOK_ARCH_ARM64) != 1
#  error "exactly one arch macro should be 1"
#endif

int main()
{
    std::printf("vmhook traits: OK\n");
    return 0;
}
