#include <vmhook/vmhook.hpp>
#include <chrono>
#include <ctime>
#include <fstream>
#include <iostream>

// ---- Logger -----------------------------------------------------------------

static std::ofstream g_log{};

static void log(const std::string_view msg)
{
    const auto now{ std::chrono::system_clock::now() };
    const std::time_t t{ std::chrono::system_clock::to_time_t(now) };
    char ts[16]{};
    std::tm tm_buf{};
    localtime_s(&tm_buf, &t);
    std::strftime(ts, sizeof(ts), "%H:%M:%S", &tm_buf);

    const std::string line{ std::format("[{}]  {}\n", ts, msg) };
    std::cout << line;
    if (g_log.is_open()) { g_log << line; g_log.flush(); }
}

// ---- Worker thread ----------------------------------------------------------

static DWORD WINAPI thread_entry(HMODULE module)
{
    FILE* con{ nullptr };
    AllocConsole();
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleTitleA("VMHook  --  Minecraft 1.8.9 test");
    freopen_s(&con, "CONOUT$", "w", stdout);
    freopen_s(&con, "CONOUT$", "w", stderr);

    g_log.open(R"(C:\repos\cpp\EasyJNI\log.txt)", std::ios::out | std::ios::trunc);

    std::println("================================================");
    std::println("  VMHook  --  Minecraft 1.8.9  HotSpot test");
    std::println("================================================");
    std::println("");

    // ---- Verify jvm.dll -----------------------------------------------------
    log("DLL injected.");

    const HMODULE jvm{ jni::hotspot::get_jvm_module() };
    if (!jvm) { log("[FAIL] jvm.dll not found."); goto wait_for_delete; }
    log(std::format("jvm.dll base : 0x{:016X}", reinterpret_cast<std::uintptr_t>(jvm)));

    // ---- VMStructs ----------------------------------------------------------
    {
        const auto* vs{ jni::hotspot::get_vm_structs() };
        const auto* vt{ jni::hotspot::get_vm_types()   };
        if (!vs || !vt) { log("[FAIL] VMStructs not resolved."); goto wait_for_delete; }
        log("gHotSpotVMStructs / gHotSpotVMTypes resolved OK.");

        // Print offsets of key fields so we can verify the layout
        auto show = [](const char* t, const char* f)
        {
            const auto* e = jni::hotspot::iterate_struct_entries(t, f);
            return e ? std::to_string(e->offset) : std::string{ "NOT FOUND" };
        };
        log(std::format("  ClassLoaderData._dictionary : {}", show("ClassLoaderData","_dictionary")));
        log(std::format("  ClassLoaderData._klasses    : {}", show("ClassLoaderData","_klasses")));
        log(std::format("  ClassLoaderData._next       : {}", show("ClassLoaderData","_next")));
        log(std::format("  Klass._name                 : {}", show("Klass","_name")));
        log(std::format("  Klass._next_link            : {}", show("Klass","_next_link")));
    }

    // ---- Class enumeration via _klasses linked list -------------------------
    {
        log("Walking ClassLoaderDataGraph via _klasses linked list...");

        const jni::hotspot::class_loader_data_graph graph{};
        jni::hotspot::class_loader_data* cld{ graph.get_head() };

        int cld_count{ 0 };
        int class_count{ 0 };
        int cld_with_klasses{ 0 };

        while (cld && jni::hotspot::is_valid_ptr(cld))
        {
            ++cld_count;

            jni::hotspot::klass* k{ cld->get_klasses() };
            if (k && jni::hotspot::is_valid_ptr(k)) ++cld_with_klasses;

            while (k && jni::hotspot::is_valid_ptr(k))
            {
                const jni::hotspot::symbol* sym{ k->get_name() };
                if (sym && jni::hotspot::is_valid_ptr(sym))
                {
                    const std::string name{ sym->to_string() };
                    if (!name.empty())
                    {
                        ++class_count;
                        if (g_log.is_open()) g_log << std::format("CLASS  {}\n", name);
                    }
                }
                k = k->get_next_link();
            }

            cld = cld->get_next();
        }

        if (g_log.is_open()) g_log.flush();

        log(std::format("Enumeration: {} CLDs, {} with klasses, {} classes total.",
            cld_count, cld_with_klasses, class_count));
    }

    // ---- find_class + field lookup ------------------------------------------
    {
        std::println("");
        std::println("================================================");
        std::println("  Class + field lookup");
        std::println("================================================");

        struct test_case { std::string cls; std::string field; std::string sig; };
        const std::vector<test_case> tests
        {
            { "java/lang/String",  "hash",     "I" },
            { "java/lang/Thread",  "priority", "I" },
            { "java/lang/Object",  "",         ""  },
            { "net/minecraft/client/main/Main", "", "" },
        };

        for (const auto& tc : tests)
        {
            jni::hotspot::klass* const k{ jni::find_class(tc.cls) };
            if (!k) { log(std::format("  [NOT FOUND]  {}", tc.cls)); continue; }

            log(std::format("  [FOUND]  {}  ({} methods)", tc.cls, k->get_methods_count()));

            if (!tc.field.empty())
            {
                const auto e{ k->find_field(tc.field) };
                if (e) log(std::format("    field '{}' sig='{}' offset={} static={}",
                    tc.field, e->signature, e->offset, e->is_static));
                else   log(std::format("    field '{}' NOT FOUND", tc.field));
            }
        }
    }

    std::println("");
    std::println("================================================");
    std::println("  Full class list -> log.txt");
    std::println("  Press DELETE to unload.");
    std::println("================================================");

wait_for_delete:
    while (!(GetAsyncKeyState(VK_DELETE) & 0x8000))
        std::this_thread::sleep_for(std::chrono::milliseconds{ 5 });

    log("Unloading.");
    if (g_log.is_open()) g_log.close();
    FreeConsole();
    FreeLibraryAndExitThread(module, 0);
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(hModule);
        HANDLE t{ CreateThread(nullptr, 0,
            reinterpret_cast<LPTHREAD_START_ROUTINE>(thread_entry), hModule, 0, nullptr) };
        if (t) CloseHandle(t);
    }
    return TRUE;
}
