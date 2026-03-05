#include <windows.h>

#include <easy_jni/easy_jni.hpp>

class entity_player_sp final : public jni::object
{
public:
    entity_player_sp(const jobject instance)
        : jni::object{ instance }
    {

    }

    auto get_server_sprinting_state() -> bool
    {
        return get_field<bool>("serverSprintState")->get();
    }

    auto set_server_sprinting_state(const bool value) -> void
    {
        get_field<bool>("serverSprintState")->set(value);
    }

    auto get_name() -> std::string
    {
		return get_method<std::string>("getName")->call();
    }
};

class minecraft final : public jni::object
{
public:
    minecraft(const jobject instance)
        : jni::object{ instance }
    {

    }

    auto get_minecraft() -> std::unique_ptr<minecraft>
    {
        return get_field<minecraft>("theMinecraft", jni::field_type::STATIC)->get();
    }

    auto get_the_player() -> std::unique_ptr<entity_player_sp>
    {
		return get_field<entity_player_sp>("thePlayer")->get();
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
            jni::register_class<minecraft>("net/minecraft/client/Minecraft");
            jni::register_class<entity_player_sp>("net/minecraft/client/entity/EntityPlayerSP");

			const std::unique_ptr<minecraft> the_minecraft{ std::make_unique<minecraft>(nullptr)->get_minecraft() };

            while (true)
            {
                if (the_minecraft->get_the_player()->get_instance())
                {
                    the_minecraft->get_the_player()->set_server_sprinting_state(true);

                    std::println("Server Sprint State: {}", the_minecraft->get_the_player()->get_server_sprinting_state());
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
        HANDLE thread_handle{ CreateThread(nullptr, 0ull, reinterpret_cast<LPTHREAD_START_ROUTINE>(thread_entry), hModule, 0ul, nullptr) };
        if (thread_handle)
        {
            CloseHandle(thread_handle);
        }
        break;
    }
    case DLL_PROCESS_DETACH:
        break;
    }
    return TRUE;
}