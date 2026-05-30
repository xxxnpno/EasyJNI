// Runtime null-safety / never-throw checks for vmhook public entry points
// when NO JVM is loaded in the process.  Every documented entry point must
// no-op safely (return its safe-default, invoke no visitor, never crash).
// Extends tests/test_api_surface.cpp, which only checks that the surface
// type-checks; this file actually *runs* main() with no JVM behind it.
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

// A minimal wrapper type used for register_class<T> / make_unique<T> /
// for_each_instance<T>.  Derives from vmhook::object<T> with the required
// explicit T(vmhook::oop_t) constructor, mirroring the pattern in
// test_api_surface.cpp.
class dummy_wrapper : public vmhook::object<dummy_wrapper>
{
public:
    explicit dummy_wrapper(vmhook::oop_t oop) noexcept
        : vmhook::object<dummy_wrapper>{ oop }
    {
    }
};

int main()
{
    // --- find_class: no JVM -> nullptr, never throws ---------------------
    {
        vmhook::hotspot::klass* k{ nullptr };
        bool threw{ false };
        try { k = vmhook::find_class("java/lang/String"); }
        catch (...) { threw = true; }
        check("find_class_string_returns_null_without_jvm", k == nullptr);
        check("find_class_does_not_throw_without_jvm", !threw);
    }
    {
        // A class that does not exist anywhere must also be null, not throw.
        vmhook::hotspot::klass* k{ vmhook::find_class("definitely/Not/A/Real/Class") };
        check("find_class_missing_class_returns_null", k == nullptr);
    }
    {
        // Empty class name is still a safe lookup that yields null.
        vmhook::hotspot::klass* k{ vmhook::find_class("") };
        check("find_class_empty_name_returns_null", k == nullptr);
    }

    // --- read_java_string: null oop -> empty string, never throws --------
    {
        std::string s{ "sentinel" };
        bool threw{ false };
        try { s = vmhook::read_java_string(nullptr); }
        catch (...) { threw = true; }
        check("read_java_string_null_returns_empty", s.empty());
        check("read_java_string_null_does_not_throw", !threw);
    }
    {
        // A bogus non-null pointer is rejected by is_valid_pointer and must
        // yield an empty string rather than dereferencing garbage.
        void* const bogus{ reinterpret_cast<void*>(static_cast<std::uintptr_t>(0x1)) };
        std::string s{ "sentinel" };
        bool threw{ false };
        try { s = vmhook::read_java_string(bogus); }
        catch (...) { threw = true; }
        check("read_java_string_bogus_ptr_returns_empty", s.empty());
        check("read_java_string_bogus_ptr_does_not_throw", !threw);
    }

    // --- shutdown_hooks: safe to call with no hooks installed ------------
    {
        bool threw{ false };
        try
        {
            vmhook::shutdown_hooks();
            // Idempotent: a second call with nothing installed is still safe.
            vmhook::shutdown_hooks();
        }
        catch (...) { threw = true; }
        check("shutdown_hooks_no_hooks_does_not_throw", !threw);
    }

    // --- for_each_loaded_class: no JVM -> visitor never invoked ----------
    {
        int count{ 0 };
        bool threw{ false };
        try
        {
            vmhook::for_each_loaded_class(
                [&count](const std::string&, vmhook::hotspot::klass*)
                {
                    ++count;
                });
        }
        catch (...) { threw = true; }
        check("for_each_loaded_class_visitor_not_invoked_without_jvm", count == 0);
        check("for_each_loaded_class_does_not_throw_without_jvm", !threw);
    }

    // --- for_each_thread: no JVM -> visitor never invoked ----------------
    {
        int count{ 0 };
        bool threw{ false };
        try
        {
            vmhook::for_each_thread(
                [&count](const vmhook::thread_info&)
                {
                    ++count;
                });
        }
        catch (...) { threw = true; }
        check("for_each_thread_visitor_not_invoked_without_jvm", count == 0);
        check("for_each_thread_does_not_throw_without_jvm", !threw);
    }

    // --- register_class<T>: no JVM -> returns false (find_class fails) ---
    bool registered{ true };
    {
        bool threw{ false };
        try { registered = vmhook::register_class<dummy_wrapper>("my/Dummy"); }
        catch (...) { threw = true; }
        check("register_class_returns_false_without_jvm", registered == false);
        check("register_class_does_not_throw_without_jvm", !threw);
    }

    // --- for_each_instance<T>: no JVM -> 0 instances, visitor not run ----
    // for_each_instance resolves T's registered klass first; with no JVM the
    // type was never registered (register_class returned false), so it must
    // bail out reporting zero and never touch the visitor.
    {
        int count{ 0 };
        std::size_t reported{ 123 };
        bool threw{ false };
        try
        {
            reported = vmhook::for_each_instance<dummy_wrapper>(
                [&count](std::unique_ptr<dummy_wrapper>)
                {
                    ++count;
                });
        }
        catch (...) { threw = true; }
        check("for_each_instance_visitor_not_invoked_without_jvm", count == 0);
        check("for_each_instance_reports_zero_without_jvm", reported == 0);
        check("for_each_instance_does_not_throw_without_jvm", !threw);
    }
    {
        // Same, but with an explicit max_visits cap argument exercised.
        int count{ 0 };
        std::size_t reported{ vmhook::for_each_instance<dummy_wrapper>(
            [&count](std::unique_ptr<dummy_wrapper>) { ++count; },
            8) };
        check("for_each_instance_with_max_visits_reports_zero", reported == 0);
        check("for_each_instance_with_max_visits_visitor_not_invoked", count == 0);
    }

    // --- make_unique<T>: no JVM -> nullptr, never throws -----------------
    {
        std::unique_ptr<dummy_wrapper> obj{ reinterpret_cast<dummy_wrapper*>(0) };
        bool threw{ false };
        try { obj = vmhook::make_unique<dummy_wrapper>(); }
        catch (...) { threw = true; }
        check("make_unique_returns_null_without_jvm", obj == nullptr);
        check("make_unique_does_not_throw_without_jvm", !threw);
    }

    // --- on_class_loaded: no JVM -> empty handle, running()==false -------
    // The class-load hook install requires resolving java.lang.ClassLoader,
    // which fails without a JVM; the returned watch_handle must therefore be
    // inert (running() == false) and must not fire the callback.
    {
        int fired{ 0 };
        bool threw{ false };
        bool running_true{ true };
        try
        {
            vmhook::watch_handle handle{ vmhook::on_class_loaded(
                [&fired](const std::string&) { ++fired; }) };
            running_true = handle.running();
        }
        catch (...) { threw = true; }
        check("on_class_loaded_handle_not_running_without_jvm", running_true == false);
        check("on_class_loaded_callback_not_fired_without_jvm", fired == 0);
        check("on_class_loaded_does_not_throw_without_jvm", !threw);
    }

    // --- on_exception: no JVM -> empty handle, running()==false ----------
    // Mirrors on_class_loaded: installing the Throwable.fillInStackTrace hook
    // needs a live JVM; without one the handle is inert.
    {
        int fired{ 0 };
        bool threw{ false };
        bool running_true{ true };
        try
        {
            vmhook::watch_handle handle{ vmhook::on_exception(
                [&fired](const std::string&) { ++fired; }) };
            running_true = handle.running();
        }
        catch (...) { threw = true; }
        check("on_exception_handle_not_running_without_jvm", running_true == false);
        check("on_exception_callback_not_fired_without_jvm", fired == 0);
        check("on_exception_does_not_throw_without_jvm", !threw);
    }

    // --- watch_handle default-construct is inert -------------------------
    {
        vmhook::watch_handle handle{};
        check("default_watch_handle_not_running", handle.running() == false);
    }

    return failures == 0 ? 0 : 1;
}
