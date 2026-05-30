// Standalone (no-JVM) unit test for the multi-classloader resolution helpers:
//   vmhook::find_class_via_oop(anchor, name)
//   vmhook::override_class_lookup(name, klass)
//   vmhook::evict_class_lookup(name)
//   vmhook::reanchor_classes_via_oop(anchor, {names...})
//
// The real "resolve the right loader's copy" behaviour needs a JVM with two
// classloaders and is out of scope here (it belongs in JVM integration).  What
// IS testable without a JVM:
//   * null / no-JVM safety: find_class_via_oop returns nullptr for a null anchor
//     and (since current_jni_env is null in this process) for any anchor;
//   * reanchor_classes_via_oop returns false for a null anchor and never throws;
//   * override_class_lookup / evict_class_lookup correctly seed and remove
//     entries in the shared klass_lookup_cache, which find_class() consults
//     first — so we can prove the override is observed and the evict forgets it.
#include <vmhook/vmhook.hpp>

#include <cstdio>
#include <cstdint>
#include <string>

static int failures{ 0 };
static auto check(const char* name, bool ok) -> void
{
    std::printf("%s %s\n", ok ? "[PASS]" : "[FAIL]", name);
    if (!ok) { ++failures; }
}

int main()
{
    // -- find_class_via_oop: null anchor -> nullptr, never throws -------------
    check("find_class_via_oop_null_anchor",
          vmhook::find_class_via_oop(nullptr, "java/lang/Object") == nullptr);

    // Non-null anchor but no JVM (current_jni_env is null) -> nullptr, no crash.
    {
        auto* const fake_anchor{ reinterpret_cast<void*>(
            static_cast<std::uintptr_t>(0x2000)) };
        check("find_class_via_oop_no_jvm",
              vmhook::find_class_via_oop(fake_anchor, "java/lang/Object") == nullptr);
    }

    // -- reanchor_classes_via_oop: null anchor -> false ----------------------
    check("reanchor_null_anchor_false",
          vmhook::reanchor_classes_via_oop(nullptr, { "a/B", "c/D" }) == false);

    // Non-null anchor, no JVM: nothing resolves -> false, no throw.
    {
        auto* const fake_anchor{ reinterpret_cast<void*>(
            static_cast<std::uintptr_t>(0x3000)) };
        check("reanchor_no_jvm_false",
              vmhook::reanchor_classes_via_oop(fake_anchor, { "a/B" }) == false);
    }
    // Empty list with a non-null anchor: vacuously true (all zero names resolved).
    {
        auto* const fake_anchor{ reinterpret_cast<void*>(
            static_cast<std::uintptr_t>(0x4000)) };
        check("reanchor_empty_list_true",
              vmhook::reanchor_classes_via_oop(fake_anchor, {}) == true);
    }

    // -- override_class_lookup / evict_class_lookup round-trip on the cache ---
    // We inspect klass_lookup_cache directly rather than through find_class():
    // find_class() defensively VALIDATES a cache hit (is_valid_pointer + name
    // match) and evicts a stale/garbage klass, so a bogus sentinel would never
    // survive a find_class() round-trip.  That validation is correct behaviour;
    // here we just prove override/evict mutate the cache the way find_class
    // expects.
    {
        auto* const sentinel_klass{ reinterpret_cast<vmhook::hotspot::klass*>(
            static_cast<std::uintptr_t>(0xABCD000)) };
        const std::string_view name{ "test/OverriddenClass" };

        auto cache_value = [](const std::string& key) -> vmhook::hotspot::klass*
        {
            std::lock_guard<std::mutex> lock{ vmhook::klass_lookup_cache_mutex };
            const auto it{ vmhook::klass_lookup_cache.find(key) };
            return it == vmhook::klass_lookup_cache.end() ? nullptr : it->second;
        };
        auto cache_has = [](const std::string& key) -> bool
        {
            std::lock_guard<std::mutex> lock{ vmhook::klass_lookup_cache_mutex };
            return vmhook::klass_lookup_cache.contains(key);
        };

        check("override_absent_before", !cache_has(std::string{ name }));

        vmhook::override_class_lookup(name, sentinel_klass);
        check("override_seeds_cache", cache_value(std::string{ name }) == sentinel_klass);

        // Last-write-wins for the corrective API.
        auto* const sentinel_klass2{ reinterpret_cast<vmhook::hotspot::klass*>(
            static_cast<std::uintptr_t>(0xBEEF000)) };
        vmhook::override_class_lookup(name, sentinel_klass2);
        check("override_replaces_previous", cache_value(std::string{ name }) == sentinel_klass2);

        vmhook::evict_class_lookup(name);
        check("evict_removes_from_cache", !cache_has(std::string{ name }));
    }

    // Evicting an absent name is a safe no-op.
    vmhook::evict_class_lookup("never/Seen");
    check("evict_absent_name_safe", true);

    return failures == 0 ? 0 : 1;
}
