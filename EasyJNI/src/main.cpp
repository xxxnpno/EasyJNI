#include <windows.h>

#include <easy_jni/easy_jni.hpp>

using namespace jni;

class EntityPlayerSP final : public object
{
public:
    EntityPlayerSP(const jobject instance)
        : object{ instance }
    {

    }

    auto GetServerSprintState() -> jboolean
    {
        return get_field <bool>("serverSprintState")->get();
    }

    auto SetServerSprintState(const bool value) -> void
    {
        get_field<bool>("serverSprintState")->Set(value);
    }

    auto GetName() -> std::string
    {
		return get_method<std::string>("getName")->call();
    }
};

class Minecraft final : public object
{
public:
    Minecraft(const jobject instance)
        : object{ instance }
    {

    }

    auto GetMinecraft() -> std::unique_ptr<Minecraft>
    {
        return get_field<Minecraft>("theMinecraft", field_type::STATIC)->get();
    }

    auto GetThePlayer() -> std::unique_ptr<EntityPlayerSP>
    {
		return get_field<EntityPlayerSP>("thePlayer")->get();
    }
};

static DWORD WINAPI thread_entry(const HMODULE module)
{
    FILE* outputBuffer{ nullptr };

    AllocConsole();
    freopen_s(&outputBuffer, "CONOUT$", "w", stdout);

    /*
    
    */

    if (jni::init())
    {
        {
            jni::register_class<Minecraft>("net/minecraft/client/Minecraft");
            jni::register_class<EntityPlayerSP>("net/minecraft/client/entity/EntityPlayerSP");

			const std::unique_ptr<Minecraft> theMinecraft{ std::make_unique<Minecraft>()->GetMinecraft() };

            while (true)
            {
                if (theMinecraft->GetThePlayer()->get_instance())
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

        jni::shutdown();
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
        HANDLE threadHandle{ CreateThread(nullptr, 0ull, reinterpret_cast<LPTHREAD_START_ROUTINE>(thread_entry), hModule, 0ul, nullptr) };
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