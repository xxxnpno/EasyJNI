// Translation unit B in the ODR test.
#include <vmhook/vmhook.hpp>

namespace vmhook_odr_test
{
    auto from_b() -> int
    {
        (void)vmhook::klass_lookup_cache.size();
        (void)vmhook::type_to_class_map.size();
        (void)vmhook::g_type_factory_map.size();
        return 2;
    }
}
