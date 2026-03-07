#include <windows.h>

#include <easy_jni/easy_jni.hpp>

class entity_player : public jni::object
{
public:
    entity_player(jobject instance)
        : jni::object{ instance }
    {

    }

    auto get_name() 
        -> std::string
    {
        return get_method<std::string>("getName")->call();
    }
};

class entity_player_sp : public entity_player
{
public:
    entity_player_sp(jobject instance)
        : entity_player{ instance }
    {

    }

    auto is_sprinting() 
        -> bool
    {
        return get_method<bool>("isSprinting")->call();
    }

    auto set_sprinting(const bool value) 
        -> void
    {
        get_method<void, bool>("setSprinting")->call(value);
    }

    auto send_chat_message(const std::string& value) 
        -> void
    {
        get_method<void, std::string>("sendChatMessage")->call(value);
    }
};

class world_client : public jni::object
{
public:
    world_client(jobject instance)
        : jni::object{ instance }
    {

    }

    auto get_player_entities()
        -> std::vector<std::unique_ptr<entity_player>>
    {
        return get_field<jni::list>("playerEntities")->get()->to_vector<entity_player>();
    }
};

class minecraft : public jni::object
{
public:
    minecraft(jobject instance)
        : jni::object{ instance }
    {

    }

    auto get_minecraft() 
        -> std::unique_ptr<minecraft>
    {
        return get_field<minecraft>("theMinecraft", jni::field_type::STATIC)->get();
    }

    auto get_the_player() 
        -> std::unique_ptr<entity_player_sp>
    {
		return get_field<entity_player_sp>("thePlayer")->get();
    }

    auto get_the_world()
        -> std::unique_ptr<world_client>
    {
        return get_field<world_client>("theWorld")->get();
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

            jni::register_class<entity_player>("net/minecraft/entity/player/EntityPlayer");
            jni::register_class<entity_player_sp>("net/minecraft/client/entity/EntityPlayerSP");

            jni::register_class<world_client>("net/minecraft/client/multiplayer/WorldClient");

			const std::unique_ptr<minecraft> the_minecraft{ std::make_unique<minecraft>(nullptr)->get_minecraft() };

            while (true)
            {
                const std::unique_ptr<entity_player_sp> the_player{ the_minecraft->get_the_player() };
                const std::unique_ptr<world_client> the_world{ the_minecraft->get_the_world() };

                if (the_player->get_instance() and the_world->get_instance())
                {
                    for (const std::unique_ptr<entity_player>& player : the_world->get_player_entities())
                    {
                        std::println("name {}", player->get_name());
                    }
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