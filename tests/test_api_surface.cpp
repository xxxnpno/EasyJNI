// Compile-only check that the public API surface (register_class, hook<>,
// vmhook::object<>, vmhook::field_proxy, return_value) is callable with the
// expected signatures.  No JVM is running, so every function returns its
// safe-default value (false, std::nullopt, etc.) — we just want the compiler
// to type-check everything that downstream code might use.
#include <vmhook/vmhook.hpp>

#include <cstdio>
#include <memory>
#include <string>

class my_class : public vmhook::object<my_class>
{
public:
    explicit my_class(vmhook::oop_t oop) noexcept
        : vmhook::object<my_class>{ oop }
    {
    }

    // Instance-style accessors must compile.
    auto get_health() -> int { return get_field("health")->get(); }
    auto set_health(int v) -> void { get_field("health")->set(v); }
    auto add_score(int x) -> int { return get_method("addScore")->call(x); }

    // Static-style accessors must compile.
    static auto get_count() -> int { return static_field("entityCount")->get(); }
    static auto set_count(int v) -> void { static_field("entityCount")->set(v); }
    static auto reset() -> void { static_method("reset")->call(); }
};

static auto exercise_hooks() -> void
{
    // The whole hook machinery should be callable; without a JVM it returns
    // false rather than crashing.  We just want it to type-check.
    const bool ok{ vmhook::hook<my_class>("addScore",
        [](vmhook::return_value& retval,
           const std::unique_ptr<my_class>& self,
           int amount)
        {
            if (self && amount > 9000)
            {
                retval.set(int{ 0 });
            }
        }) };
    (void)ok;

    vmhook::shutdown_hooks();
}

int main()
{
    vmhook::register_class<my_class>("my/Class");
    exercise_hooks();
    std::printf("vmhook API surface: OK\n");
    return 0;
}
