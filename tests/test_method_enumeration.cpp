// Standalone (no-JVM) unit test for the method-enumeration API:
//   vmhook::get_class_methods(class_name)        (by internal name)
//   vmhook::get_class_methods<T>()               (by registered wrapper)
//   vmhook::find_methods_by_signature<T>(desc)   (descriptor selector)
//   vmhook::log_class_methods<T>()               (debug convenience)
//
// These read InstanceKlass::_methods directly.  With no JVM in this process,
// find_class resolves no klass, so every entry point must return an EMPTY
// result WITHOUT throwing or dereferencing — that is the contract this file
// pins down.  The "real list of methods on a loaded class" behaviour needs a
// live JVM and is covered by test_method_enumeration in example.cpp.
#include <vmhook/vmhook.hpp>

#include <cstdio>
#include <string>
#include <vector>
#include <utility>

static int failures{ 0 };
static auto check(const char* name, bool ok) -> void
{
    std::printf("%s %s\n", ok ? "[PASS]" : "[FAIL]", name);
    if (!ok) { ++failures; }
}

namespace
{
    // A wrapper that IS registered, and one that is NOT.
    class registered_wrapper : public vmhook::object<registered_wrapper>
    {
    public:
        explicit registered_wrapper(vmhook::oop_t oop) noexcept
            : vmhook::object<registered_wrapper>{ oop }
        {
        }
    };

    class unregistered_wrapper : public vmhook::object<unregistered_wrapper>
    {
    public:
        explicit unregistered_wrapper(vmhook::oop_t oop) noexcept
            : vmhook::object<unregistered_wrapper>{ oop }
        {
        }
    };

    // A detour with the shape hook_by_signature expects (return_value& + self).
    auto dummy_detour(vmhook::return_value&,
                      const std::unique_ptr<registered_wrapper>&) -> void
    {
    }
}

int main()
{
    // Without a JVM, register_class can't verify the class, but the type map
    // is still consulted by get_class_methods<T>(); either way the result must
    // be empty (no klass to walk).
    vmhook::register_class<registered_wrapper>("test/Registered");

    // -- by registered wrapper: no JVM -> empty, never throws ----------------
    {
        const auto methods{ vmhook::get_class_methods<registered_wrapper>() };
        check("registered_wrapper_methods_empty_no_jvm", methods.empty());
        check("registered_wrapper_methods_size0", methods.size() == 0);
    }

    // -- by UNregistered wrapper: empty (type-map miss), never throws --------
    {
        const auto methods{ vmhook::get_class_methods<unregistered_wrapper>() };
        check("unregistered_wrapper_methods_empty", methods.empty());
    }

    // -- by class name: no JVM -> find_class null -> empty -------------------
    {
        const auto methods{ vmhook::get_class_methods("java/lang/Object") };
        check("by_name_methods_empty_no_jvm", methods.empty());

        const auto missing{ vmhook::get_class_methods("definitely/Not/A/Class") };
        check("by_name_missing_class_empty", missing.empty());
    }

    // -- find_methods_by_signature: empty input -> empty output -------------
    {
        const auto names{
            vmhook::find_methods_by_signature<registered_wrapper>("()Ljava/util/Collection;") };
        check("find_by_signature_empty_no_jvm", names.empty());

        const auto names_unreg{
            vmhook::find_methods_by_signature<unregistered_wrapper>("()V") };
        check("find_by_signature_unregistered_empty", names_unreg.empty());
    }

    // -- hook_by_signature: no matching method -> returns false, no crash ----
    {
        const bool installed{
            vmhook::hook_by_signature<registered_wrapper>("()V", &dummy_detour) };
        check("hook_by_signature_no_match_returns_false", installed == false);
    }

    // -- log_class_methods compiles and is safe to call with no JVM ----------
    {
        vmhook::log_class_methods<registered_wrapper>();   // no-op data-wise; must not crash
        check("log_class_methods_no_jvm_safe", true);
    }

    // -- the return type is exactly vector<pair<string,string>> --------------
    {
        using ret_t = decltype(vmhook::get_class_methods<registered_wrapper>());
        static_assert(std::is_same_v<ret_t,
                          std::vector<std::pair<std::string, std::string>>>,
                      "get_class_methods<T>() must return vector<pair<name,descriptor>>");
        check("get_class_methods_return_type", true);
    }

    return failures == 0 ? 0 : 1;
}
