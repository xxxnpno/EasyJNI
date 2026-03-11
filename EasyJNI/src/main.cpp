#include <windows.h>

#include <easy_jni/easy_jni.hpp>

namespace jni
{
    class i_chat_component : public jni::object
    {
    public:
        i_chat_component(jobject instance)
            : jni::object{ instance }
        {

        }

        auto get_unformatted_text() -> std::string
        {
            return get_method<std::string>("getUnformattedText")->call();
        }
    };

    class chat_component_text : public i_chat_component
    {
    public:
        chat_component_text(jobject instance)
            : i_chat_component{ instance }
        {

        }
    };

    class entity_player_sp : public jni::object
    {
    public:
        entity_player_sp(jobject instance)
            : jni::object{ instance }
        {

        }

        auto send_chat_message(const std::string& value) -> void
        {
            get_method<void, std::string>("sendChatMessage")->call(value);
        }

        auto add_chat_message(const std::unique_ptr<i_chat_component>& value)
            -> void
        {
            get_method<void, i_chat_component>("addChatMessage")->call(value);
        }
    };

    class minecraft : public jni::object
    {
    public:
        minecraft(jobject instance)
            : jni::object{ instance }
        {

        }
        
        auto get_the_minecraft() -> std::unique_ptr<jni::minecraft>
        {
            return get_field<jni::minecraft>("theMinecraft", jni::field_type::STATIC)->get();
        }

        auto get_the_player() -> std::unique_ptr<jni::entity_player_sp>
        {
            return get_field<jni::entity_player_sp>("thePlayer")->get();
        }
    };
}

static DWORD WINAPI thread_entry_test(const HMODULE module)
{
    FILE* outputBuffer{ nullptr };

    AllocConsole();
    freopen_s(&outputBuffer, "CONOUT$", "w", stdout);

    if (jni::init())
    {
        jni::register_class<jni::minecraft>("net/minecraft/client/Minecraft");

        jni::register_class<jni::entity_player_sp>("net/minecraft/client/entity/EntityPlayerSP");

        jni::register_class<jni::i_chat_component>("net/minecraft/util/IChatComponent");
        jni::register_class<jni::chat_component_text>("net/minecraft/util/ChatComponentText");

        const std::unique_ptr<jni::minecraft> the_minecraft{ std::make_unique<jni::minecraft>(nullptr)->get_the_minecraft() };

        auto sendChatMessage_hook = [](jni::hotspot::frame* frame, jni::hotspot::java_thread* thread, bool* cancel)
        {
            auto [self, message] = frame->get_arguments<jni::entity_player_sp, std::string>();

            std::println("[HOOK] sendChatMessage called, message: {}", message);
        };

        auto addChatMessage_hook = [](jni::hotspot::frame* frame, jni::hotspot::java_thread* thread, bool* cancel)
            {
                auto [self, chat_component] = frame->get_arguments<jni::entity_player_sp, jni::i_chat_component>();

                std::println("[HOOK] addChatMessage called, message: {}", chat_component->get_unformatted_text());
            };

        jni::hook<jni::entity_player_sp>("sendChatMessage", sendChatMessage_hook);
        jni::hook<jni::entity_player_sp>("addChatMessage", addChatMessage_hook);

        while (true)
        {
            const std::unique_ptr<jni::entity_player_sp> the_player{ the_minecraft->get_the_player() };
            the_player->add_chat_message(jni::make_unique<jni::chat_component_text, std::string>("Hello World"));

            std::this_thread::sleep_for(std::chrono::milliseconds{ 250 });

            if (GetAsyncKeyState(VK_END) bitand 0x8000)
            {
                break;
            }
        }

        jni::shutdown();
    }

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
        HANDLE thread_handle{ CreateThread(nullptr, 0ull, reinterpret_cast<LPTHREAD_START_ROUTINE>(thread_entry_test), hModule, 0ul, nullptr) };
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