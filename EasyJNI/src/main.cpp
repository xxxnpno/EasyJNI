#include <windows.h>

#include <easy_jni/easy_jni.hpp>

class entity_player_sp : public jni::object
{
public:
    entity_player_sp(jobject instance)
        : jni::object{ instance }
    {

    }

    auto is_sprinting() -> bool
    {
        return get_method<bool>("isSprinting")->call();
    }

    auto set_sprinting(const bool value) -> void
    {
        get_method<void, bool>("setSprinting")->call(value);
    }

    auto send_chat_message(const std::string& value) -> void
    {
        get_method<void, std::string>("sendChatMessage")->call(value);
    }
    
    auto get_name() -> std::string
    {
        return get_method<std::string>("getName")->call();
    }
};

class minecraft : public jni::object
{
public:
    minecraft(jobject instance)
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
                const std::unique_ptr<entity_player_sp> the_player{ the_minecraft->get_the_player() };

                if (the_player->get_instance())
                {
                    the_player->set_sprinting(true);

                    the_player->send_chat_message(
                        std::format("name: {}, sprinting: {}", the_player->get_name(), the_player->is_sprinting())
                    );
                }

                std::this_thread::sleep_for(std::chrono::milliseconds{ 250 });

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