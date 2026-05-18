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

// Compile-only coverage of the java.util container wrappers (collection,
// list, set, linked_list, map, hash_map) and the matching field_proxy
// value_t entry points.  No JVM is running, so every call returns an
// empty container; the point of the test is purely to type-check that
// all of the templated `to_vector` / `to_entries` instantiations compile
// on every supported toolchain.
class element_w : public vmhook::object<element_w>
{
public:
    explicit element_w(vmhook::oop_t oop) noexcept
        : vmhook::object<element_w>{ oop }
    {
    }
};

class key_w : public vmhook::object<key_w>
{
public:
    explicit key_w(vmhook::oop_t oop) noexcept
        : vmhook::object<key_w>{ oop }
    {
    }
};

class value_w : public vmhook::object<value_w>
{
public:
    explicit value_w(vmhook::oop_t oop) noexcept
        : vmhook::object<value_w>{ oop }
    {
    }
};

static auto exercise_collection_wrappers() -> void
{
    // Direct construction from a null OOP — every member must be callable
    // without dereferencing anything.
    vmhook::collection  c{ nullptr };
    vmhook::list        l{ nullptr };
    vmhook::set         s{ nullptr };
    vmhook::linked_list ll{ nullptr };
    vmhook::map         m{ nullptr };
    vmhook::hash_map    hm{ nullptr };

    // size() / is_empty() resolve through the live OOP's klass; with a null
    // OOP they short-circuit to 0/true.  Just want the call to type-check.
    (void)c.size();
    (void)l.size();
    (void)s.size();
    (void)ll.size();
    (void)m.size();
    (void)hm.size();
    (void)c.is_empty();
    (void)m.is_empty();

    // Every to_vector<T>() / to_entries<K,V>() instantiation must compile.
    auto v0 = c.to_vector<element_w>();
    auto v1 = l.to_vector<element_w>();
    auto v2 = s.to_vector<element_w>();
    auto v3 = ll.to_vector<element_w>();
    auto e0 = m.to_entries<key_w, value_w>();
    auto e1 = hm.to_entries<key_w, value_w>();
    (void)v0; (void)v1; (void)v2; (void)v3;
    (void)e0; (void)e1;
}

static auto exercise_field_proxy_entrypoints() -> void
{
    // value_t::to_vector / value_t::to_entries — the two entry points
    // users actually reach via `get_field("foo")->get().to_vector<T>()`
    // and `get_field("foo")->get().to_entries<K,V>()`.  No template arg
    // on get() — the right wrapper / fast path is picked from the live
    // OOP's field layout inside collection::to_vector and map::to_entries.
    // Construct a null-OOP field_proxy directly so the test does not
    // depend on static-vs-instance get_field resolution.
    vmhook::field_proxy field{ nullptr, "Ljava/util/List;", false };

    auto via_value_t_vec     = field.get().to_vector<element_w>();
    auto via_value_t_entries = field.get().to_entries<key_w, value_w>();
    (void)via_value_t_vec; (void)via_value_t_entries;
}

int main()
{
    vmhook::register_class<my_class>("my/Class");
    vmhook::register_class<element_w>("my/Element");
    vmhook::register_class<key_w>("my/Key");
    vmhook::register_class<value_w>("my/Value");
    exercise_hooks();
    exercise_collection_wrappers();
    exercise_field_proxy_entrypoints();
    std::printf("vmhook API surface: OK\n");
    return 0;
}
