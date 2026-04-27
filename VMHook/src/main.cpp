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
    const std::time_t timestamp{ std::chrono::system_clock::to_time_t(now) };
    char time_string[16]{};
    std::tm tm_buf{};
    localtime_s(&tm_buf, &timestamp);
    std::strftime(time_string, sizeof(time_string), "%H:%M:%S", &tm_buf);
    const std::string line{ std::format("[{}]  {}\n", time_string, msg) };
    std::cout << line;
    if (g_log.is_open()) { g_log << line; g_log.flush(); }
}

// ---- Worker thread ----------------------------------------------------------

static DWORD WINAPI thread_entry(HMODULE module)
{
    FILE* console_handle{ nullptr };
    AllocConsole();
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleTitleA("VMHook  --  Example target");
    freopen_s(&console_handle, "CONOUT$", "w", stdout);
    freopen_s(&console_handle, "CONOUT$", "w", stderr);

    g_log.open(R"(C:\repos\cpp\VMHook\log.txt)", std::ios::out | std::ios::trunc);

    std::println("================================================");
    std::println("  VMHook  --  JDK compatibility test");
    std::println("  Target: vmhook.example (example\\build_and_run.bat)");
    std::println("================================================");
    std::println("");

    // ---- jvm.dll + VMStructs ------------------------------------------------

    log("DLL injected.");

    const HMODULE jvm_module{ vmhook::hotspot::get_jvm_module() };
    if (!jvm_module) { log("[FAIL] jvm.dll not found."); goto wait_for_delete; }
    log(std::format("jvm.dll base : 0x{:016X}", reinterpret_cast<std::uintptr_t>(jvm_module)));

    {
        if (!vmhook::hotspot::get_vm_structs() || !vmhook::hotspot::get_vm_types())
        { log("[FAIL] VMStructs not resolved."); goto wait_for_delete; }
        log("VMStructs OK.");

        // Report which field storage format this JDK uses.
        const bool has_legacy_fields{ vmhook::hotspot::iterate_struct_entries("InstanceKlass", "_fields") != nullptr };
        const bool has_field_stream { vmhook::hotspot::iterate_struct_entries("InstanceKlass", "_fieldinfo_stream") != nullptr };
        log(std::format("  _fields           : {}", has_legacy_fields ? "PRESENT (JDK 8-20 layout)"   : "absent"));
        log(std::format("  _fieldinfo_stream : {}", has_field_stream  ? "PRESENT (JDK 21+ UNSIGNED5)"  : "absent"));
    }

    // ---- Class enumeration --------------------------------------------------
    {
        log("Walking ClassLoaderDataGraph...");

        const bool use_klasses_list{
            vmhook::hotspot::iterate_struct_entries("ClassLoaderData", "_klasses") != nullptr };
        log(std::format("  Enumeration strategy: {}", use_klasses_list
            ? "_klasses list (JDK 21+)"
            : "Dictionary (JDK 8-20)"));

        const vmhook::hotspot::class_loader_data_graph graph{};
        vmhook::hotspot::class_loader_data* cld{ graph.get_head() };
        int cld_count{ 0 }, class_count{ 0 };

        while (cld && vmhook::hotspot::is_valid_ptr(cld))
        {
            ++cld_count;

            auto emit = [&](vmhook::hotspot::klass* current_klass)
            {
                if (!current_klass || !vmhook::hotspot::is_valid_ptr(current_klass)) return;
                const vmhook::hotspot::symbol* sym{ current_klass->get_name() };
                if (sym && vmhook::hotspot::is_valid_ptr(sym))
                {
                    const std::string class_name{ sym->to_string() };
                    if (!class_name.empty())
                    {
                        ++class_count;
                        if (g_log.is_open()) g_log << std::format("CLASS  {}\n", class_name);
                    }
                }
            };

            if (use_klasses_list)
            {
                vmhook::hotspot::klass* current_klass{ cld->get_klasses() };
                while (current_klass && vmhook::hotspot::is_valid_ptr(current_klass))
                {
                    emit(current_klass);
                    current_klass = current_klass->get_next_link();
                }
            }
            else
            {
                // JDK 8-20: search via per-CLD Dictionary hashtable.
                const vmhook::hotspot::dictionary* dict{ cld->get_dictionary() };
                if (vmhook::hotspot::is_valid_ptr(dict))
                {
                    const std::int32_t table_size{ dict->get_table_size() };
                    const std::uint8_t* const buckets{ dict->get_buckets() };
                    if (vmhook::hotspot::is_valid_ptr(buckets) && table_size > 0 && table_size <= 0x186A0)
                    {
                        for (std::int32_t bucket_index{ 0 }; bucket_index < table_size; ++bucket_index)
                        {
                            const auto* dict_entry{ reinterpret_cast<const std::uint8_t*>(
                                vmhook::hotspot::untag_ptr(
                                    vmhook::hotspot::safe_read_ptr(buckets + bucket_index * 8))) };
                            while (vmhook::hotspot::is_valid_ptr(dict_entry))
                            {
                                const void* raw_klass{ vmhook::hotspot::safe_read_ptr(dict_entry + 16) };
                                emit(reinterpret_cast<vmhook::hotspot::klass*>(
                                    const_cast<void*>(vmhook::hotspot::untag_ptr(raw_klass))));
                                dict_entry = reinterpret_cast<const std::uint8_t*>(
                                    vmhook::hotspot::untag_ptr(
                                        vmhook::hotspot::safe_read_ptr(dict_entry)));
                            }
                        }
                    }
                }
            }

            cld = cld->get_next();
        }

        // JDK 8 fallback: use SystemDictionary._dictionary if the CLD walk yielded nothing.
        if (class_count == 0 && !use_klasses_list)
        {
            auto walk_system_dictionary = [&](const char* entry_name)
            {
                const auto* sde{ vmhook::hotspot::iterate_struct_entries("SystemDictionary", entry_name) };
                if (!sde || !sde->address) return;
                const auto* dict{ *reinterpret_cast<const vmhook::hotspot::dictionary**>(sde->address) };
                if (!vmhook::hotspot::is_valid_ptr(dict)) return;
                const std::int32_t table_size{ dict->get_table_size() };
                const std::uint8_t* const buckets{ dict->get_buckets() };
                if (!vmhook::hotspot::is_valid_ptr(buckets) || table_size <= 0 || table_size > 0x186A0) return;
                for (std::int32_t bucket_index{ 0 }; bucket_index < table_size; ++bucket_index)
                {
                    const auto* dict_entry{ reinterpret_cast<const std::uint8_t*>(
                        vmhook::hotspot::untag_ptr(
                            vmhook::hotspot::safe_read_ptr(buckets + bucket_index * 8))) };
                    while (vmhook::hotspot::is_valid_ptr(dict_entry))
                    {
                        const void* raw_klass{ vmhook::hotspot::safe_read_ptr(dict_entry + 16) };
                        auto* candidate_klass{ reinterpret_cast<vmhook::hotspot::klass*>(
                            const_cast<void*>(vmhook::hotspot::untag_ptr(raw_klass))) };
                        if (candidate_klass && vmhook::hotspot::is_valid_ptr(candidate_klass))
                        {
                            const vmhook::hotspot::symbol* sym{ candidate_klass->get_name() };
                            if (sym && vmhook::hotspot::is_valid_ptr(sym))
                            {
                                const std::string class_name{ sym->to_string() };
                                if (!class_name.empty())
                                {
                                    ++class_count;
                                    if (g_log.is_open()) g_log << std::format("CLASS  {}\n", class_name);
                                }
                            }
                        }
                        dict_entry = reinterpret_cast<const std::uint8_t*>(
                            vmhook::hotspot::untag_ptr(
                                vmhook::hotspot::safe_read_ptr(dict_entry)));
                    }
                }
            };
            walk_system_dictionary("_dictionary");
            walk_system_dictionary("_shared_dictionary");
        }

        if (g_log.is_open()) g_log.flush();
        log(std::format("{} CLDs, {} classes total. Full list in log.txt.", cld_count, class_count));
    }

    // ---- Field lookup on standard JDK classes (works on all JDK versions) ---
    {
        std::println("");
        std::println("================================================");
        std::println("  find_class + find_field verification");
        std::println("================================================");

        struct test_case
        {
            std::string class_name;
            std::string field_name;
            std::string expected_signature;
            bool        expected_static;
        };

        const std::vector<test_case> test_cases
        {
            // Standard JDK classes — present on every Java version.
            { "java/lang/String",  "hash",     "I",                  false },
            { "java/lang/Thread",  "priority", "I",                  false },
            { "java/lang/Integer", "TYPE",     "Ljava/lang/Class;",  true  },
            // Example application classes — present when example\build_and_run.bat is running.
            { "vmhook/example/Player", "health", "F",                 false },
            { "vmhook/example/Player", "x",      "D",                 false },
            { "vmhook/example/Player", "name",   "Ljava/lang/String;",false },
            { "vmhook/example/Player", "count",  "I",                 true  },
        };

        for (const auto& test_case : test_cases)
        {
            vmhook::hotspot::klass* const target_klass{ vmhook::find_class(test_case.class_name) };
            if (!target_klass) { log(std::format("  [NOT FOUND]  {}", test_case.class_name)); continue; }
            log(std::format("  [FOUND]  {}  ({} methods)", test_case.class_name, target_klass->get_methods_count()));

            if (test_case.field_name.empty()) continue;

            const auto field_entry{ target_klass->find_field(test_case.field_name) };
            if (!field_entry)
            {
                log(std::format("    field '{}' -- NOT FOUND", test_case.field_name));
                continue;
            }

            const bool signature_ok = field_entry->signature == test_case.expected_signature
                                   || test_case.expected_signature.empty();
            const bool static_ok    = field_entry->is_static == test_case.expected_static;
            const char* verdict     = (signature_ok && static_ok) ? "OK" : "MISMATCH";

            log(std::format("    field '{}' -- offset={} sig='{}' static={}  [{}]",
                test_case.field_name,
                field_entry->offset,
                field_entry->signature,
                field_entry->is_static,
                verdict));
        }
    }

    // ---- VMStruct offset summary --------------------------------------------
    {
        std::println("");
        std::println("================================================");
        std::println("  VMStruct offsets");
        std::println("================================================");

        const auto* flds   = vmhook::hotspot::iterate_struct_entries("InstanceKlass", "_fields");
        const auto* consts = vmhook::hotspot::iterate_struct_entries("InstanceKlass", "_constants");
        const auto* fis    = vmhook::hotspot::iterate_struct_entries("InstanceKlass", "_fieldinfo_stream");
        const auto* cp_type_entry = vmhook::hotspot::iterate_type_entries("ConstantPool");

        log(std::format("  InstanceKlass._fields offset            = {}",
            flds   ? std::to_string(flds->offset)   : "N/A (JDK 21+: replaced by _fieldinfo_stream)"));
        log(std::format("  InstanceKlass._constants offset         = {}",
            consts ? std::to_string(consts->offset) : "N/A"));
        log(std::format("  InstanceKlass._fieldinfo_stream offset  = {}",
            fis    ? std::to_string(fis->offset)    : "N/A (JDK 8-20: uses _fields instead)"));
        log(std::format("  sizeof(ConstantPool)                    = {}",
            cp_type_entry ? std::to_string(cp_type_entry->size) : "N/A"));
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
        HANDLE worker_thread{ CreateThread(nullptr, 0,
            reinterpret_cast<LPTHREAD_START_ROUTINE>(thread_entry), hModule, 0, nullptr) };
        if (worker_thread) CloseHandle(worker_thread);
    }
    return TRUE;
}
