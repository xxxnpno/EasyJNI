#include <easy_jni/easy_jni.hpp>

#include <windows.h>

/*
    Minecraft 1.8.9 example using MCP deobfuscated names.

    Covers:
        - Wrapping Java classes and registering them
        - Inheritance between wrappers
        - Reading instance and static fields
        - Writing fields
        - Calling instance and static methods
        - Using jni::make_unique to call Java constructors
        - Converting a Java List to a std::vector
        - Null checking with get_instance()
        - Hooking Java methods and reading their arguments
        - Cancelling a hooked method and returning a custom value
*/

namespace jni
{
    // net.minecraft.util.IChatComponent
    // Base interface for all chat components in Minecraft.
    class i_chat_component : public jni::object
    {
    public:
        explicit i_chat_component(jobject instance)
            : jni::object{ instance }
        {
        }

        // Returns the plain text of the component, with all formatting codes stripped.
        auto get_unformatted_text() 
            -> std::string
        {
            return get_method<std::string>("getUnformattedText")->call();
        }

        // Returns the full formatted text, including color and style codes.
        auto get_formatted_text() 
            -> std::string
        {
            return get_method<std::string>("getFormattedText")->call();
        }
    };

    // net.minecraft.util.ChatComponentText
    // A plain-text chat component. Inherits from i_chat_component.
    // Use jni::make_unique<chat_component_text, std::string>("...") to construct one.
    class chat_component_text : public i_chat_component
    {
    public:
        explicit chat_component_text(jobject instance)
            : i_chat_component{ instance }
        {
        }
    };

    // net.minecraft.entity.player.EntityPlayer
    // Base player class. We wrap it to access shared player fields and methods.
    class entity_player : public jni::object
    {
    public:
        explicit entity_player(jobject instance)
            : jni::object{ instance }
        {
        }

        // Returns the player's username.
        auto get_name() 
            -> std::string
        {
            return get_method<std::string>("getName")->call();
        }

        // Returns the player's current health as a float.
        auto get_health() 
            -> float
        {
            return get_method<float>("getHealth")->call();
        }

        // Returns the player's maximum health.
        auto get_max_health() 
            -> float
        {
            return get_method<float>("getMaxHealth")->call();
        }

        // Sends a chat message as this player.
        auto send_chat_message(const std::string& message) 
            -> void
        {
            get_method<void, std::string>("sendChatMessage")->call(message);
        }

        // Adds a chat component to the player's chat GUI.
        auto add_chat_message(const std::unique_ptr<i_chat_component>& component) 
            -> void
        {
            get_method<void, i_chat_component>("addChatMessage")->call(component);
        }

        // Reads the "onGround" boolean field directly.
        // Demonstrates reading a primitive instance field.
        auto is_on_ground() 
            -> bool
        {
            return get_field<bool>("onGround")->get();
        }

        // Reads the "experienceLevel" int field directly.
        auto get_experience_level() 
            -> int
        {
            return get_field<int>("experienceLevel")->get();
        }

        // Writes the "experienceLevel" int field directly.
        // Demonstrates writing a primitive instance field.
        auto set_experience_level(int level) 
            -> void
        {
            get_field<int>("experienceLevel")->set(level);
        }
    };

    // net.minecraft.client.entity.EntityPlayerSP
    // The local single-player entity. Inherits entity_player.
    class entity_player_sp : public entity_player
    {
    public:
        explicit entity_player_sp(jobject instance)
            : entity_player{ instance }
        {
        }
    };

    // net.minecraft.world.WorldClient
    // The client-side world. We use it to iterate over player entities.
    class world_client : public jni::object
    {
    public:
        explicit world_client(jobject instance)
            : jni::object{ instance }
        {
        }

        // Returns all player entities in the world as a std::vector.
        // "playerEntities" is a List<EntityPlayer> in Java.
        // Demonstrates converting a Java List to a std::vector<std::unique_ptr<T>>.
        auto get_player_entities() 
            -> std::vector<std::unique_ptr<entity_player>>
        {
            return get_field<jni::list>("playerEntities")->get()->to_vector<entity_player>();
        }
    };

    // net.minecraft.client.Minecraft
    // The main game singleton. All access to the game state starts here.
    class minecraft : public jni::object
    {
    public:
        explicit minecraft(jobject instance)
            : jni::object{ instance }
        {
        }

        // Returns the Minecraft singleton via the static "theMinecraft" field.
        // Demonstrates reading a static object field.
        auto get_the_minecraft() 
            -> std::unique_ptr<minecraft>
        {
            return get_field<minecraft>("theMinecraft", jni::field_type::STATIC)->get();
        }

        // Returns the local player (thePlayer instance field).
        auto get_the_player() 
            -> std::unique_ptr<entity_player_sp>
        {
            return get_field<entity_player_sp>("thePlayer")->get();
        }

        // Returns the current world (theWorld instance field).
        auto get_the_world() 
            -> std::unique_ptr<world_client>
        {
            return get_field<world_client>("theWorld")->get();
        }
    };
}

static auto register_classes() 
    -> void
{
    jni::register_class<jni::minecraft>("net/minecraft/client/Minecraft");
    jni::register_class<jni::entity_player>("net/minecraft/entity/player/EntityPlayer");
    jni::register_class<jni::entity_player_sp>("net/minecraft/client/entity/EntityPlayerSP");
    jni::register_class<jni::world_client>("net/minecraft/client/multiplayer/WorldClient");
    jni::register_class<jni::i_chat_component>("net/minecraft/util/IChatComponent");
    jni::register_class<jni::chat_component_text>("net/minecraft/util/ChatComponentText");
}

static auto register_hooks() 
    -> void
{
    // Hook EntityPlayerSP.sendChatMessage(String)
    // Observes every outgoing chat message without blocking it.
    static auto send_chat_hook = [](jni::hotspot::frame* frame, jni::hotspot::java_thread*, bool*)
    {
        auto [self, message] = frame->get_arguments<jni::entity_player_sp, std::string>();
        std::println("[HOOK] sendChatMessage > \"{}\"", message);
    };

    jni::hook<jni::entity_player_sp>("sendChatMessage", send_chat_hook);

    // Hook EntityPlayer.addChatMessage(IChatComponent)
    // Reads the incoming chat component and logs its plain text.
    static auto add_chat_hook = [](jni::hotspot::frame* frame, jni::hotspot::java_thread*, bool*)
    {
        auto [self, component] = frame->get_arguments<jni::entity_player_sp, jni::i_chat_component>();

        if (component->get_instance())
        {
            std::println("[HOOK] addChatMessage > \"{}\"", component->get_unformatted_text());
        }
    };

    jni::hook<jni::entity_player_sp>("addChatMessage", add_chat_hook);
}

static DWORD WINAPI thread_entry(HMODULE module)
{
    FILE* console{ nullptr };
    AllocConsole();
    freopen_s(&console, "CONOUT$", "w", stdout);

    std::println("[INFO] EasyJNI initializing...");

    if (not jni::init())
    {
        std::println("[ERROR] jni::init() failed.");
        FreeConsole();
        FreeLibraryAndExitThread(module, 0);
        return 0;
    }

    register_classes();
    register_hooks();

    // Get the Minecraft singleton via its static field.
    const std::unique_ptr<jni::minecraft>& the_minecraft{ std::make_unique<jni::minecraft>(nullptr)->get_the_minecraft() };

    while (true)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds{ 2000 });

        // Exit when DELETE is held
        if (GetAsyncKeyState(VK_DELETE) bitand 0x8000)
        {
            break;
        }

        // Re-fetch the player each tick since it can change (world reload, death, etc.)
        const std::unique_ptr<jni::entity_player_sp>& the_player{ the_minecraft->get_the_player() };
        const std::unique_ptr<jni::world_client>& the_world{ the_minecraft->get_the_world() };

        // Null check: player or world can be null on the main menu.
        if (not the_player->get_instance() or not the_world->get_instance())
        {
            std::println("[INFO] Player or world is null, skipping.");
            continue;
        }

        // Reading a method return value (health, max health)
        std::println("[INFO] Player: {} | Health: {:.1f}/{:.1f} | Level: {} | OnGround: {}",
            the_player->get_name(),
            the_player->get_health(),
            the_player->get_max_health(),
            the_player->get_experience_level(),
            the_player->is_on_ground()
        );

        // Writing a field: set the player's XP level to 30
        the_player->set_experience_level(30);
        std::println("[INFO] Set experience level to 30.");

        // Calling a method with a std::string argument
        the_player->send_chat_message("Hello from EasyJNI!");

        // Using jni::make_unique to construct a Java object (ChatComponentText)
        // and passing it to a method that expects an IChatComponent.
        const std::unique_ptr<jni::i_chat_component>& chat_msg{ 
            jni::make_unique<jni::chat_component_text, std::string>("[EasyJNI] Constructor and object argument test.")
        };
        the_player->add_chat_message(chat_msg);

        // Converting a Java List to a std::vector and iterating it.
        const std::vector<std::unique_ptr<jni::entity_player>>& players{ the_world->get_player_entities() };
        std::println("[INFO] Players in world: {}", players.size());
        for (const std::unique_ptr<jni::entity_player>& player : players)
        {
            if (player->get_instance())
            {
                std::println("  - {} (health: {:.1f})", player->get_name(), player->get_health());
            }
        }
    }

    // Must be called before unloading the DLL.
    // Removes all hooks, deletes global refs, and detaches the current thread.
    jni::shutdown();

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