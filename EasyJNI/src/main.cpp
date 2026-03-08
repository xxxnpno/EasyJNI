#include <windows.h>

// include the header (insane comment)
#include <easy_jni/easy_jni.hpp>

// you need to recreate the java classes that you want to use in cpp

// for normal classes, abtract classes or interface, create a normal cpp class
// here is the wrapper for the interface IChatComponent
class i_chat_component : public jni::object // inherit your class from jni::object
{
public:
    i_chat_component(jobject instance) // then create a constructor that takes a jobject
        : jni::object{ instance } // and pass it to jni::object
    {

    }
};

// here is an example of inheritance, just change jni::object to the super class
class chat_component_text : public i_chat_component
{
public:
    chat_component_text(jobject instance)
        : i_chat_component{ instance } // don't forget to change here too
    {

    }
};

class entity_player : public jni::object
{
public:
    entity_player(jobject instance)
        : jni::object{ instance }
    {

    }

    // now for method, recreate the java method in the right class
    auto get_name() 
        -> std::string // types are always cpp ones never the jni ones
        // for primitive types we have:
        // void, short, int, long long, float, double, boolean, std::string
    {
        // this method returns something so return it
        // it also takes a String so use the right cpp type associate with it
        // then specify the name of the method
        // and use the call method
        return get_method<std::string>("getName")->call();
    }

    // this method takes an IChatComponent, with EasyJNI pass the unique ptr that holds the jobject,
    // not the jobject directly
    auto add_chat_message(const std::unique_ptr<i_chat_component> value)
        -> void
    {
        // return void so don't return anything
        // but you still need to specify that it is a void method
        // then specify the arg types, here don't say it's a jobject or an unique ptr,
        // pass the class
        // give call the arguments, no need to specify again the types
        get_method<void, i_chat_component>("addChatMessage")->call(value);
    }
};

class entity_player_sp : public entity_player
{
public:
    entity_player_sp(jobject instance)
        : entity_player{ instance }
    {

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

    // usage of List wrapper, playerEntities is in java a List<EntityPlayer>
    // get returns a jni::list that has a to_vector method
    auto get_player_entities()
        -> std::vector<std::unique_ptr<entity_player>> // to_vector return a std::vector
    {
        // specify in to_vector the type of the Object, you can't pass primitive types just like java
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

    // if your field (or method) is static, don't make the cpp method static,
    // you just need to specify jni::field_type::STATIC or jni::method_type::STATIC
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

    // don't forget the jni::init() function
    if (jni::init())
    {
        {
            /*
                here we register all the classes that we may use in the dll
            */
            jni::register_class<minecraft>("net/minecraft/client/Minecraft");

            jni::register_class<entity_player>("net/minecraft/entity/player/EntityPlayer");
            jni::register_class<entity_player_sp>("net/minecraft/client/entity/EntityPlayerSP");

            jni::register_class<world_client>("net/minecraft/client/multiplayer/WorldClient");

            jni::register_class<i_chat_component>("net/minecraft/util/IChatComponent");
            jni::register_class<chat_component_text>("net/minecraft/util/ChatComponentText");

            /*
            
            */

            // in this example I am using the Minecraft 1.8.9 deobfuscate source code
            // theMicraft field is static and will be the enrty point of our dll
            // since get_minecraft is not static, we will create a jni::object with no instance, from it, we will use static methods
			const std::unique_ptr<minecraft> the_minecraft{ std::make_unique<minecraft>(nullptr)->get_minecraft() };
            
            // keep the dll alive
            while (true)
            {
                // get these Object only onces per loop 
                const std::unique_ptr<entity_player_sp> the_player{ the_minecraft->get_the_player() };
                const std::unique_ptr<world_client> the_world{ the_minecraft->get_the_world() };

                // in minecraft, thePlayer or theWorld might be null if the player is in the main menu
                // don't check if (the_player), unique ptrs are never nullptr, it's the holded instance that might be nullptr
                if (the_player->get_instance() and the_world->get_instance())
                {
                    // cool usage of our wrapper
                    // jni::make_unique is used here not std::make_unique
                    the_player->add_chat_message(jni::make_unique<chat_component_text, std::string>("Hello World"));

                    // another example
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

namespace jni
{
    class entity_player_sp : public jni::object
    {
    public:
        entity_player_sp(jobject instance)
            : jni::object{ instance }
        {

        }

        auto send_chat_message(const std::string& value)
            -> void
        {
            get_method<void, std::string>("sendChatMessage")->call(value);
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
    };
}

static DWORD WINAPI thread_entry_test(const HMODULE module)
{
    FILE* outputBuffer{ nullptr };

    AllocConsole();
    freopen_s(&outputBuffer, "CONOUT$", "w", stdout);

    if (jni::init())
    {
        {
            jni::register_class<jni::minecraft>("net/minecraft/client/Minecraft");
            jni::register_class<jni::entity_player_sp>("net/minecraft/client/entity/EntityPlayerSP");

            const std::unique_ptr<jni::minecraft> the_minecraft{ std::make_unique<jni::minecraft>(nullptr)->get_minecraft() };

            const jclass clazz{ jni::get_class("net/minecraft/client/entity/EntityPlayerSP") };
            const jmethodID method_id{ jni::get_env()->GetMethodID(clazz, "sendChatMessage", "(Ljava/lang/String;)V") };

            auto sendChatMessage_hook = [](jni::hotspot::frame* f, jni::hotspot::java_thread* thread, bool* cancel)
                {
                    
                };

            jni::hook(method_id, sendChatMessage_hook);

            while (true)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds{ 250 });

                if (GetAsyncKeyState(VK_END) bitand 0x8000)
                {
                    break;
                }
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