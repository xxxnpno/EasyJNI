#include <print>
#include <filesystem>
#include <string>

#include <windows.h>
#include <tlhelp32.h>

// std::println has no formatter<wchar_t> for narrow output; convert manually.
static auto wstr_to_str(const std::wstring& ws)
    -> std::string
{
    std::string result{};
    result.reserve(ws.size());
    for (const wchar_t c : ws) result += static_cast<char>(c);
    return result;
}

static auto resolve_dll_path()
    -> std::wstring
{
    wchar_t executable_path_buffer[MAX_PATH]{};

    GetModuleFileNameW(nullptr, executable_path_buffer, MAX_PATH);

    return (std::filesystem::path{ executable_path_buffer }.parent_path() / L"vmhook.dll").wstring();
}

struct process_entry
{
    DWORD pid;
    std::wstring name;
};

static auto find_processes(const std::wstring& exe_name)
    -> std::vector<process_entry>
{
    std::vector<process_entry> result{};

    const HANDLE snapshot_handle{ CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0) };

    if (snapshot_handle == INVALID_HANDLE_VALUE)
    {
        return result;
    }

    PROCESSENTRY32W entry{};

    entry.dwSize = sizeof(entry);

    if (Process32FirstW(snapshot_handle, &entry))
    {
        do
        {
            if (!_wcsicmp(entry.szExeFile, exe_name.c_str()))
            {
                result.push_back({ entry.th32ProcessID, entry.szExeFile });
            }

        } while (Process32NextW(snapshot_handle, &entry));
    }

    CloseHandle(snapshot_handle);

    return result;
}

static auto inject_dll(const DWORD pid, const std::wstring& dll_path)
    -> bool
{
    HANDLE process_handle{ OpenProcess(PROCESS_CREATE_THREAD | PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ, FALSE, pid) };

    if (!process_handle)
    {
        std::println("[ERROR] Failed to open process with PID {}, error {}.", pid, GetLastError());

        return false;
    }

    const std::size_t path_bytes{ (dll_path.size() + 1) * sizeof(wchar_t) };

    void* const remote_buffer{ VirtualAllocEx(process_handle, nullptr, path_bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE) };

    if (!remote_buffer)
    {
        std::println("[ERROR] VirtualAllocEx failed, error {}", GetLastError());

        CloseHandle(process_handle);

        return false;
    }

    if (!WriteProcessMemory(process_handle, remote_buffer, dll_path.c_str(), path_bytes, nullptr))
    {
        std::println("[ERROR] WriteProcessMemory failed, error {}", GetLastError());

        VirtualFreeEx(process_handle, remote_buffer, 0, MEM_RELEASE);

        CloseHandle(process_handle);

        return false;
    }

    const HMODULE kernel32{ GetModuleHandleW(L"kernel32.dll") };

    const auto load_library_address{ reinterpret_cast<LPTHREAD_START_ROUTINE>(GetProcAddress(kernel32, "LoadLibraryW")) };

    HANDLE remote_thread{ CreateRemoteThread(process_handle, nullptr, 0, load_library_address, remote_buffer, 0, nullptr) };

    if (!remote_thread)
    {
        std::println("[ERROR] CreateRemoteThread failed, error {}", GetLastError());

        VirtualFreeEx(process_handle, remote_buffer, 0, MEM_RELEASE);

        CloseHandle(process_handle);

        return false;
    }

    WaitForSingleObject(remote_thread, INFINITE);

    DWORD exit_code{ 0 };

    GetExitCodeThread(remote_thread, &exit_code);

    CloseHandle(remote_thread);

    VirtualFreeEx(process_handle, remote_buffer, 0, MEM_RELEASE);

    CloseHandle(process_handle);

    return exit_code != 0;
}

auto main(std::int32_t argc, char** argv)
    -> std::int32_t
{
    const std::wstring dll_path{ resolve_dll_path() };

    std::println("[INFO] dll_path : {}.", wstr_to_str(dll_path));

    if (!std::filesystem::exists(dll_path))
    {
        std::println("[ERROR] vmhook.dll not found.");

        return EXIT_FAILURE;
    }

    std::println("[OK] dll found.");

    DWORD target_pid{ 0 };

    if (argc >= 2)
    {
        try
        {
            target_pid = static_cast<DWORD>(std::stoul(argv[1]));
        }
        catch (...)
        {
            std::println("[ERROR] Invalid PID provided.");

            return EXIT_FAILURE;
        }

        std::println("[INFO] Using provided PID {}.", target_pid);
    }
    else
    {
        std::println("[INFO] No PID provided. Scanning for JVM processes...");
    }

    std::vector<process_entry> processes{ find_processes(L"javaw.exe") };

    if (processes.empty())
    {
        processes = find_processes(L"java.exe");
    }

    if (processes.empty())
    {
        std::println("[ERROR] No running JVM processes found.");

        return EXIT_FAILURE;
    }

    if (processes.size() == 1)
    {
        target_pid = processes[0].pid;

        std::println("[INFO] Found 1 process: {}, injecting automatically.", target_pid);
    }
    else
    {
        std::println("[ERROR] Multiple JVM processes found, aborting automatic injection.");

        return EXIT_FAILURE;
    }

    std::println("[INFO] Injecting into process {}", target_pid);

    if (inject_dll(target_pid, dll_path))
    {
        std::println("[OK] Injection successful.");

        return EXIT_SUCCESS;
    }
    else
    {
        std::println("[ERROR] Injection failed.");

        return EXIT_FAILURE;
    }
}
