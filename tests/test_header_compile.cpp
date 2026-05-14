// Smoke test: does <vmhook/vmhook.hpp> compile as a single TU with no
// other configuration, on every supported compiler/platform combination?
#include <vmhook/vmhook.hpp>

#include <cstdio>

int main()
{
    std::printf("vmhook header compile: OK\n");
    return 0;
}
