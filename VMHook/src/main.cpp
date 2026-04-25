#include <vmhook/vmhook.hpp>
#include <chrono>
#include <ctime>
#include <fstream>
#include <iostream>

// ─── Logger ──────────────────────────────────────────────────────────────────

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

    if (g_log.is_open())
    {
        g_log << line;
        g_log.flush();
    }
}

// ─── Worker thread ───────────────────────────────────────────────────────────

static DWORD WINAPI thread_entry(HMODULE module)
{
    // ── Console ──────────────────────────────────────────────────────────────
    FILE* con{ nullptr };
    AllocConsole();
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleTitleA("EasyJNI  —  Minecraft 1.8.9 test");
    freopen_s(&con, "CONOUT$", "w", stdout);
    freopen_s(&con, "CONOUT$", "w", stderr);

    // ── Log file ─────────────────────────────────────────────────────────────
    g_log.open(R"(C:\repos\cpp\EasyJNI\log.txt)", std::ios::out | std::ios::trunc);

    std::println("================================================");
    std::println("  EasyJNI  —  Minecraft 1.8.9  HotSpot test");
    std::println("================================================");
    std::println("");

    // ── Verify jvm.dll ───────────────────────────────────────────────────────
    log("DLL injected.");

    const HMODULE jvm{ jni::hotspot::get_jvm_module() };
    if (!jvm)
    {
        log("[FAIL] jvm.dll not found in this process — wrong target?");
        goto wait_for_delete;
    }
    log(std::format("jvm.dll base : 0x{:016X}", reinterpret_cast<std::uintptr_t>(jvm)));

    // ── Verify VMStructs ─────────────────────────────────────────────────────
    {
        const auto* vmstructs{ jni::hotspot::get_vm_structs() };
        const auto* vmtypes { jni::hotspot::get_vm_types()   };

        if (!vmstructs || !vmtypes)
        {
            log("[FAIL] gHotSpotVMStructs / gHotSpotVMTypes not resolved.");
            goto wait_for_delete;
        }
        log("gHotSpotVMStructs and gHotSpotVMTypes resolved OK.");
    }

    // ── Class enumeration via ClassLoaderDataGraph ────────────────────────────
    {
        log("Walking ClassLoaderDataGraph...");

        const jni::hotspot::class_loader_data_graph graph{};
        jni::hotspot::class_loader_data* cld{ graph.get_head() };

        int cld_count{ 0 };
        int class_count{ 0 };

        while (cld && jni::hotspot::is_valid_ptr(cld))
        {
            ++cld_count;

            // Walk via dictionary (more reliable than _klasses linked list)
            const jni::hotspot::dictionary* dict{ cld->get_dictionary() };
            if (jni::hotspot::is_valid_ptr(dict))
            {
                const std::int32_t table_size{ dict->get_table_size() };
                const std::uint8_t* const buckets{ dict->get_buckets() };

                if (jni::hotspot::is_valid_ptr(buckets) && table_size > 0 && table_size <= 0x186A0)
                {
                    for (std::int32_t i{ 0 }; i < table_size; ++i)
                    {
                        const auto* entry{
                            reinterpret_cast<const std::uint8_t*>(
                                jni::hotspot::untag_ptr(
                                    jni::hotspot::safe_read_ptr(buckets + i * 8)))
                        };

                        while (jni::hotspot::is_valid_ptr(entry))
                        {
                            const void* const raw_klass{
                                jni::hotspot::safe_read_ptr(entry + 16)
                            };
                            const auto* k{
                                reinterpret_cast<const jni::hotspot::klass*>(
                                    jni::hotspot::untag_ptr(raw_klass))
                            };

                            if (jni::hotspot::is_valid_ptr(k))
                            {
                                const jni::hotspot::symbol* sym{ k->get_name() };
                                if (jni::hotspot::is_valid_ptr(sym))
                                {
                                    const std::string name{ sym->to_string() };
                                    if (!name.empty())
                                    {
                                        ++class_count;
                                        if (g_log.is_open())
                                        {
                                            g_log << std::format("CLASS  {}\n", name);
                                        }
                                    }
                                }
                            }

                            entry = reinterpret_cast<const std::uint8_t*>(
                                jni::hotspot::untag_ptr(
                                    jni::hotspot::safe_read_ptr(entry)));
                        }
                    }
                }
            }

            cld = cld->get_next();
        }

        if (g_log.is_open()) g_log.flush();

        log(std::format("Enumeration done — {} ClassLoaders, {} classes. See log.txt for full list.",
            cld_count, class_count));
    }

    // ── find_class + metadata verification ───────────────────────────────────
    {
        std::println("");
        std::println("================================================");
        std::println("  Class + field lookup verification");
        std::println("================================================");

        struct test_case
        {
            std::string class_name;
            std::string field_name;   // field to look up (empty = skip)
            std::string expected_sig; // expected JVM type descriptor
        };

        const std::vector<test_case> tests
        {
            { "java/lang/String",  "hash",     "I"  },
            { "java/lang/Thread",  "priority", "I"  },
            { "java/lang/Object",  "",         ""   },
            { "net/minecraft/client/main/Main", "", "" },
        };

        for (const auto& tc : tests)
        {
            jni::hotspot::klass* const k{ jni::find_class(tc.class_name) };

            if (!k)
            {
                log(std::format("  [NOT FOUND]  {}", tc.class_name));
                continue;
            }

            const std::int32_t method_count{ k->get_methods_count() };
            log(std::format("  [FOUND]  {}  ({} methods)", tc.class_name, method_count));

            if (!tc.field_name.empty())
            {
                const auto entry{ k->find_field(tc.field_name) };
                if (entry)
                {
                    log(std::format("           field '{}' — sig='{}' offset={} static={}",
                        tc.field_name,
                        entry->signature,
                        entry->offset,
                        entry->is_static));

                    if (entry->signature != tc.expected_sig)
                    {
                        log(std::format("           [WARN] expected sig '{}', got '{}'",
                            tc.expected_sig, entry->signature));
                    }
                }
                else
                {
                    log(std::format("           [WARN] field '{}' not found", tc.field_name));
                }
            }
        }
    }

    // ── Instructions ─────────────────────────────────────────────────────────
    {
        std::println("");
        std::println("================================================");
        std::println("  Next steps");
        std::println("================================================");
        std::println("  1. Check log.txt — search for obfuscated class");
        std::println("     names you want to hook (e.g. EntityPlayer).");
        std::println("  2. Open your inventory in-game.");
        std::println("  3. Move around / send a chat message.");
        std::println("  4. Press DELETE to unload the DLL.");
        std::println("================================================");
        std::println("  Logs: C:\\repos\\cpp\\EasyJNI\\log.txt");
        std::println("================================================");
    }

wait_for_delete:
    while (!(GetAsyncKeyState(VK_DELETE) & 0x8000))
    {
        std::this_thread::sleep_for(std::chrono::milliseconds{ 5 });
    }

    log("Unloading.");
    if (g_log.is_open()) g_log.close();
    FreeConsole();
    FreeLibraryAndExitThread(module, 0);
    return 0;
}

// ─── DllMain ─────────────────────────────────────────────────────────────────

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(hModule);

        HANDLE thread{
            CreateThread(nullptr, 0,
                reinterpret_cast<LPTHREAD_START_ROUTINE>(thread_entry),
                hModule, 0, nullptr)
        };

        if (thread)
        {
            CloseHandle(thread);
        }
    }

    return TRUE;
}
