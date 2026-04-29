#include <vmhook/vmhook.hpp>
#include "test.hpp"

#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>

// ─── Logger ──────────────────────────────────────────────────────────────────

static std::ofstream g_log{};

static void log(const std::string_view msg)
{
    const auto now{ std::chrono::system_clock::now() };
    const std::time_t ts{ std::chrono::system_clock::to_time_t(now) };
    char time_buf[16]{};
    std::tm tm_buf{};
    localtime_s(&tm_buf, &ts);
    std::strftime(time_buf, sizeof(time_buf), "%H:%M:%S", &tm_buf);
    const std::string line{ std::format("[{}]  {}\n", time_buf, msg) };
    std::cout << line;
    if (g_log.is_open()) { g_log << line; g_log.flush(); }
}

// ─── Worker thread ───────────────────────────────────────────────────────────

static DWORD WINAPI thread_entry(HMODULE module)
{
    FILE* console_handle{ nullptr };
    AllocConsole();
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleTitleA("VMHook  --  Unit tests");
    freopen_s(&console_handle, "CONOUT$", "w", stdout);
    freopen_s(&console_handle, "CONOUT$", "w", stderr);

    {
        wchar_t dll_buf[MAX_PATH]{};
        GetModuleFileNameW(module, dll_buf, MAX_PATH);
        const auto log_path = std::filesystem::path{dll_buf}.parent_path().parent_path() / L"log.txt";
        g_log.open(log_path, std::ios::out | std::ios::trunc);
    }

    std::println("════════════════════════════════════════════════════════");
    std::println("  VMHook  —  injection successful");
    std::println("════════════════════════════════════════════════════════");
    std::println("");
    log("DLL injected.");

    // ── 1. jvm.dll + VMStructs ───────────────────────────────────────────────

    const HMODULE jvm_module{ vmhook::hotspot::get_jvm_module() };
    if (!jvm_module) { log("[FAIL] jvm.dll not found."); goto wait_for_delete; }
    log(std::format("jvm.dll base  : 0x{:016X}", reinterpret_cast<std::uintptr_t>(jvm_module)));

    if (!vmhook::hotspot::get_vm_structs() || !vmhook::hotspot::get_vm_types())
    { log("[FAIL] VMStructs not resolved."); goto wait_for_delete; }
    log("VMStructs     : OK");

    {
        const bool has_fields{ vmhook::hotspot::iterate_struct_entries("InstanceKlass", "_fields")           != nullptr };
        const bool has_fis   { vmhook::hotspot::iterate_struct_entries("InstanceKlass", "_fieldinfo_stream") != nullptr };
        log(std::format("Field storage : {} ({})",
            has_fis ? "_fieldinfo_stream (JDK 21+)" : "_fields (JDK 8-20)",
            has_fields ? "legacy present" : "legacy absent"));
    }

    // ── 2. Walk ClassLoaderDataGraph + log all classes ────────────────────────
    {
        log("Walking ClassLoaderDataGraph...");
        const bool use_klasses_list{
            vmhook::hotspot::iterate_struct_entries("ClassLoaderData", "_klasses") != nullptr };

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

        // JDK 8 fallback via SystemDictionary static fields
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
        log(std::format("{} CLDs, {} classes. Full list in log.txt.", cld_count, class_count));
    }

    // ── 3. Install hook on TestTarget::onTick ────────────────────────────────
    {
        if (!vmhook::register_class<TestTargetWrapper>("vmhook/example/TestTarget"))
        {
            log("[WARN] TestTarget not found — hook + instance tests will be skipped.");
        }
        else
        {
            if (vmhook::hook<TestTargetWrapper>("onTick", on_tick_detour))
                log("Hook installed: TestTarget::onTick");
            else
                log("[WARN] Failed to install hook on TestTarget::onTick.");
        }
    }

    // ── 4. Wait up to 3 s for the hook to fire at least once ─────────────────
    {
        log("Waiting for hook to fire...");
        const auto deadline{ std::chrono::steady_clock::now() + std::chrono::seconds{ 3 } };
        while (g_hook_call_count.load() == 0
            && std::chrono::steady_clock::now() < deadline)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds{ 20 });
        }
        log(std::format("Hook calls observed: {}", g_hook_call_count.load()));
    }

    // ── 5. Run full test suite ────────────────────────────────────────────────
    {
        const int failed_count{ run_all_tests(log) };
        log(std::format("Test run complete: {} failures.", failed_count));
    }

    // ── 6. VMStruct offset summary ────────────────────────────────────────────
    {
        std::println("");
        std::println("── VMStruct offsets ──");
        const auto* flds   = vmhook::hotspot::iterate_struct_entries("InstanceKlass", "_fields");
        const auto* consts = vmhook::hotspot::iterate_struct_entries("InstanceKlass", "_constants");
        const auto* fis    = vmhook::hotspot::iterate_struct_entries("InstanceKlass", "_fieldinfo_stream");
        const auto* cp_tp  = vmhook::hotspot::iterate_type_entries("ConstantPool");
        log(std::format("  InstanceKlass._fields            = {}", flds   ? std::to_string(flds->offset)   : "N/A"));
        log(std::format("  InstanceKlass._constants         = {}", consts ? std::to_string(consts->offset) : "N/A"));
        log(std::format("  InstanceKlass._fieldinfo_stream  = {}", fis    ? std::to_string(fis->offset)    : "N/A"));
        log(std::format("  sizeof(ConstantPool)             = {}", cp_tp  ? std::to_string(cp_tp->size)    : "N/A"));
    }

    std::println("");
    std::println("════════════════════════════════════════════════════════");
    std::println("  Full class list  →  log.txt");
    std::println("  Press DELETE to unload VMHook.");
    std::println("════════════════════════════════════════════════════════");

wait_for_delete:
    while (!(GetAsyncKeyState(VK_DELETE) & 0x8000))
        std::this_thread::sleep_for(std::chrono::milliseconds{ 5 });

    log("Unloading: removing hooks...");
    vmhook::shutdown_hooks();
    log("Unloaded.");

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
