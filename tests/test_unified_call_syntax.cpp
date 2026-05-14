// Verifies that the C++23 deducing-this overloads on vmhook::object<T> let
// `get_field("name")` / `get_method("name")` be used inside *static* member
// functions of a wrapper class.  The static-call branch is guarded by
// VMHOOK_HAS_DEDUCING_THIS (true on MSVC 19.32+, GCC 14+, Clang 18+).
// On older toolchains static-context calls must go through static_field /
// static_method (always available).

#include <vmhook/vmhook.hpp>

#include <cstdio>

class wrapper_class : public vmhook::object<wrapper_class>
{
public:
    explicit wrapper_class(vmhook::oop_t oop) noexcept
        : vmhook::object<wrapper_class>{ oop }
    {
    }

    // Instance call sites — these MUST work on every compiler.
    auto inst_get() -> int { return get_field("a")->get(); }
    auto inst_get_method() -> int { return get_method("m")->call(); }
    auto inst_get_method_sig() -> int { return get_method("m", "()I")->call(); }

#if VMHOOK_HAS_DEDUCING_THIS
    // Static call sites — only required to work on toolchains with the
    // C++23 deducing-this feature, where the overload set in static contexts
    // falls through to the string_view static fallback.  These would not
    // compile on older Clang/GCC; the equivalent is exercised below via
    // `static_field` / `static_method`.
    static auto stat_get_field()      -> int { return get_field("a")->get(); }
    static auto stat_get_method()     -> int { return get_method("m")->call(); }
    static auto stat_get_method_sig() -> int { return get_method("m", "()I")->call(); }
#endif

    // Portable static call sites — these MUST compile on every compiler.
    static auto portable_get_field()      -> int { return static_field("a")->get(); }
    static auto portable_get_method()     -> int { return static_method("m")->call(); }
    static auto portable_get_method_sig() -> int { return static_method("m", "()I")->call(); }
};

int main()
{
    // We can't run anything against a real JVM; the goal is just to confirm
    // that all the call sites above compiled successfully.
    std::printf("vmhook unified call-syntax: OK\n");
    return 0;
}
