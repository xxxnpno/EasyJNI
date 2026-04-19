#include <easy_jni/easy_jni.hpp>

namespace jni
{
    
}

static DWORD WINAPI thread_entry(HMODULE module)
{
    FILE* console{ nullptr };
    AllocConsole();
    SetConsoleOutputCP(CP_UTF8);
    freopen_s(&console, "CONOUT$", "w", stdout);

    
	

    while (not (GetAsyncKeyState(VK_DELETE) bitand 0x8000))
    {
        std::this_thread::sleep_for(std::chrono::milliseconds{ 5 });
    }

    FreeConsole();
    FreeLibraryAndExitThread(module, 0);
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(hModule);

        HANDLE thread{ CreateThread(nullptr, 0, reinterpret_cast<LPTHREAD_START_ROUTINE>(thread_entry), hModule, 0, nullptr) };
        if (thread)
        {
            CloseHandle(thread);
        }
    }

    return TRUE;
}