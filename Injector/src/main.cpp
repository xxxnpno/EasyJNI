#define NOMINMAX
#include <windows.h>
#include <tlhelp32.h>
#include <filesystem>
#include <format>
#include <iostream>
#include <string>
#include <vector>

// --- Process helpers ---------------------------------------------------------

struct process_entry
{
    DWORD        pid;
    std::wstring name;
};

static auto find_processes(const std::wstring& exe_name) -> std::vector<process_entry>
{
    std::vector<process_entry> result;

    HANDLE snap{ CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0) };
    if (snap == INVALID_HANDLE_VALUE)
    {
        return result;
    }

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);

    if (Process32FirstW(snap, &entry))
    {
        do
        {
            if (_wcsicmp(entry.szExeFile, exe_name.c_str()) == 0)
            {
                result.push_back({ entry.th32ProcessID, entry.szExeFile });
            }
        }
        while (Process32NextW(snap, &entry));
    }

    CloseHandle(snap);
    return result;
}

// --- DLL injection -----------------------------------------------------------

static auto inject_dll(const DWORD pid, const std::wstring& dll_path) -> bool
{
    if (!std::filesystem::exists(dll_path))
    {
        std::wcerr << L"[ERROR] DLL not found at: " << dll_path << L"\n";
        return false;
    }

    HANDLE proc{
        OpenProcess(
            PROCESS_CREATE_THREAD | PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
            FALSE,
            pid
        )
    };

    if (!proc)
    {
        std::cerr << std::format("[ERROR] OpenProcess(pid={}) failed - error {}\n",
            pid, GetLastError());
        return false;
    }

    const std::size_t path_bytes{ (dll_path.size() + 1) * sizeof(wchar_t) };

    void* const remote_buf{
        VirtualAllocEx(proc, nullptr, path_bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE)
    };

    if (!remote_buf)
    {
        std::cerr << std::format("[ERROR] VirtualAllocEx failed - error {}\n", GetLastError());
        CloseHandle(proc);
        return false;
    }

    if (!WriteProcessMemory(proc, remote_buf, dll_path.c_str(), path_bytes, nullptr))
    {
        std::cerr << std::format("[ERROR] WriteProcessMemory failed - error {}\n", GetLastError());
        VirtualFreeEx(proc, remote_buf, 0, MEM_RELEASE);
        CloseHandle(proc);
        return false;
    }

    const HMODULE kernel32{ GetModuleHandleW(L"kernel32.dll") };
    const auto load_library{
        reinterpret_cast<LPTHREAD_START_ROUTINE>(GetProcAddress(kernel32, "LoadLibraryW"))
    };

    HANDLE remote_thread{
        CreateRemoteThread(proc, nullptr, 0, load_library, remote_buf, 0, nullptr)
    };

    if (!remote_thread)
    {
        std::cerr << std::format("[ERROR] CreateRemoteThread failed - error {}\n", GetLastError());
        VirtualFreeEx(proc, remote_buf, 0, MEM_RELEASE);
        CloseHandle(proc);
        return false;
    }

    WaitForSingleObject(remote_thread, INFINITE);

    DWORD exit_code{ 0 };
    GetExitCodeThread(remote_thread, &exit_code);

    CloseHandle(remote_thread);
    VirtualFreeEx(proc, remote_buf, 0, MEM_RELEASE);
    CloseHandle(proc);

    // LoadLibraryW returns the module handle (non-zero) on success
    return exit_code != 0;
}

// --- DLL path resolution -----------------------------------------------------

/*
    Both Injector.exe and VMHook.dll share the same output directory (build\).
    The DLL is therefore always next to this executable.
*/
static auto resolve_dll_path() -> std::wstring
{
    wchar_t exe_buf[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe_buf, MAX_PATH);

    return (std::filesystem::path{ exe_buf }.parent_path() / L"VMHook.dll").wstring();
}

// --- Entry point -------------------------------------------------------------

int main(int argc, char* argv[])
{
    std::cout <<
        "================================================\n"
        "  VMHook Injector\n"
        "================================================\n\n";

    // 1. Verify the DLL exists
    const std::wstring dll_path{ resolve_dll_path() };
    std::wcout << L"[*] DLL path : " << dll_path << L"\n";

    if (!std::filesystem::exists(dll_path))
    {
        std::cerr <<
            "[ERROR] VMHook.dll not found.\n"
            "        Build the VMHook project (Release|x64) first.\n";
        return 1;
    }
    std::cout << "[OK]  DLL found.\n\n";

    // 2. Determine target PID — accept optional PID argument for CI/scripted use
    DWORD target_pid{ 0 };

    if (argc >= 2)
    {
        // PID provided on command line: skip process scan entirely
        try { target_pid = static_cast<DWORD>(std::stoul(argv[1])); }
        catch (...) { std::cerr << "[ERROR] Invalid PID argument.\n"; return 1; }
        std::cout << std::format("[*] Using provided PID {}.\n", target_pid);
    }
    else
    {
        // No PID: scan for javaw.exe then java.exe
        std::cout << "[*] Scanning for javaw.exe / java.exe...\n";
        auto processes{ find_processes(L"javaw.exe") };
        if (processes.empty()) processes = find_processes(L"java.exe");

        if (processes.empty())
        {
            std::cerr <<
                "[ERROR] No javaw.exe or java.exe found.\n"
                "        Launch the target first, then run this tool.\n";
            return 1;
        }

        if (processes.size() == 1)
        {
            target_pid = processes[0].pid;
            std::cout << std::format("[*] Found 1 process - PID {} - injecting automatically.\n", target_pid);
        }
        else
        {
            std::cout << std::format("[*] Found {} java processes:\n", processes.size());
            for (std::size_t i{ 0 }; i < processes.size(); ++i)
                std::cout << std::format("    [{}]  PID {}\n", i + 1, processes[i].pid);

            std::size_t choice{ 0 };
            do
            {
                std::cout << std::format("Choose (1-{}): ", processes.size());
                std::cin >> choice;
            }
            while (choice < 1 || choice > processes.size());

            target_pid = processes[choice - 1].pid;
        }
    }

    // 3. Inject
    std::cout << std::format("\n[*] Injecting into PID {}...\n", target_pid);

    if (inject_dll(target_pid, dll_path))
    {
        std::cout <<
            "[OK]  Injection successful!\n\n"
            "================================================\n"
            "  A console window should appear inside\n"
            "  the target process with instructions.\n\n"
            "  Logs: log.txt (next to VMHook.dll)\n"
            "================================================\n";
    }
    else
    {
        std::cerr << "[ERROR] Injection failed. Try running as Administrator.\n";
    }

    return 0;
}
