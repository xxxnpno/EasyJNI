// Translation unit A in the ODR test: includes the whole header and exposes
// a small symbol so the linker is forced to combine A and B.
#include <vmhook/vmhook.hpp>

namespace vmhook_odr_test
{
    extern auto from_b() -> int;

    auto from_a() -> int
    {
        // Touch a few inline objects so the compiler emits comdat copies
        // in this TU.  If a symbol is defined non-inline in the header, the
        // linker will reject the resulting duplicate definitions.
        (void)vmhook::klass_lookup_cache.size();
        (void)vmhook::type_to_class_map.size();
        (void)vmhook::g_type_factory_map.size();
        return 1 + from_b();
    }
}

int main()
{
    return vmhook_odr_test::from_a() == 3 ? 0 : 1;
}
