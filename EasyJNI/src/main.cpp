#include <windows.h>

#include <EasyJNI/EasyJNI.hpp>

namespace Wrapper
{
    class EntityPlayerSP final : public EasyJNI::Object
    {
    public:
        EntityPlayerSP(const jobject instance)
            : EasyJNI::Object{ instance }
        {

        }

        auto GetServerSprintState() -> jboolean
        {
            return GetField<jboolean, EntityPlayerSP>("serverSprintState");
        }

        auto SetServerSprintState(const jboolean value) -> void
        {
            SetField<jboolean, EntityPlayerSP>("serverSprintState", value);
        }
    };

    class Minecraft final : public EasyJNI::Object
    {
    public:
        Minecraft(const jobject instance)
            : EasyJNI::Object{ instance }
        {

        }

        static auto GetMinecraft() -> std::unique_ptr<Minecraft>
        {
            return GetStaticField<Minecraft, Minecraft>("theMinecraft");
        }

        auto GetThePlayer() -> std::unique_ptr<EntityPlayerSP>
        {
            return GetField<EntityPlayerSP, Minecraft>("thePlayer");
        }
    };
}

static DWORD WINAPI ThreadEntry(const HMODULE module)
{
    FILE* outputBuffer{ nullptr };

    AllocConsole();
    freopen_s(&outputBuffer, "CONOUT$", "w", stdout);

    /*
    
    */

    if (EasyJNI::Init())
    {
        {
            EasyJNI::RegisterClass<Wrapper::Minecraft>("net/minecraft/client/Minecraft");
            EasyJNI::RegisterClass<Wrapper::EntityPlayerSP>("net/minecraft/client/entity/EntityPlayerSP");

            const std::unique_ptr<Wrapper::Minecraft> theMinecraft{ Wrapper::Minecraft::GetMinecraft() };

            while (true)
            {
                if (theMinecraft->GetThePlayer()->GetInstance())
                {
					theMinecraft->GetThePlayer()->SetServerSprintState(true);

                    std::println("Server Sprint State: {}", theMinecraft->GetThePlayer()->GetServerSprintState());
                }

                std::this_thread::sleep_for(std::chrono::milliseconds{ 50 });

                if (GetAsyncKeyState(VK_END) bitand 0x8000)
                {
                    break;
                }
            }
        }

        EasyJNI::Shutdown();
    }
    
    /*

    */

    FreeConsole();
    FreeLibraryAndExitThread(module, 0ul);

    return 0l;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved)
{
    switch (ul_reason_for_call)
    {
    case DLL_PROCESS_ATTACH:
    {
        DisableThreadLibraryCalls(hModule);
        HANDLE threadHandle{ CreateThread(nullptr, 0ull, reinterpret_cast<LPTHREAD_START_ROUTINE>(ThreadEntry), hModule, 0ul, nullptr) };
        if (threadHandle)
        {
            CloseHandle(threadHandle);
        }
        break;
    }
    case DLL_PROCESS_DETACH:
        break;
    }
    return TRUE;
}