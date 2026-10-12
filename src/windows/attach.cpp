// SPDX-License-Identifier: GPL-3.0-only
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <windows.h>
#include <tlhelp32.h>
namespace {
struct Handle {
    HANDLE value{};
    ~Handle() {
        if (value && value != INVALID_HANDLE_VALUE)
            CloseHandle(value);
    }
    operator HANDLE() const {
        return value;
    }
};
std::uintptr_t module_base(DWORD pid, const std::wstring& name) {
    Handle snapshot{CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid)};
    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Module32FirstW(snapshot, &entry))
        do {
            if (_wcsicmp(entry.szModule, name.c_str()) == 0)
                return reinterpret_cast<std::uintptr_t>(entry.modBaseAddr);
        } while (Module32NextW(snapshot, &entry));
    return 0;
}
DWORD call(HANDLE process, std::uintptr_t function, const std::wstring& argument) {
    size_t bytes = (argument.size() + 1) * sizeof(wchar_t);
    auto memory = VirtualAllocEx(process, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!memory)
        throw std::runtime_error("Allocate target argument failed");
    if (!WriteProcessMemory(process, memory, argument.c_str(), bytes, nullptr)) {
        VirtualFreeEx(process, memory, 0, MEM_RELEASE);
        throw std::runtime_error("Write target argument failed");
    }
    Handle thread{CreateRemoteThread(process, nullptr, 0, reinterpret_cast<LPTHREAD_START_ROUTINE>(function),
                                     memory, 0, nullptr)};
    if (!thread.value) {
        VirtualFreeEx(process, memory, 0, MEM_RELEASE);
        throw std::runtime_error("Target does not permit loading the adapter");
    }
    auto wait = WaitForSingleObject(thread, 10000);
    if (wait != WAIT_OBJECT_0)
        throw std::runtime_error(
            "Adapter initialization timed out; target argument retained until process exit");
    DWORD code;
    GetExitCodeThread(thread, &code);
    VirtualFreeEx(process, memory, 0, MEM_RELEASE);
    return code;
}
} // namespace
int wmain(int argc, wchar_t** argv) {
    try {
        if (argc != 4) {
            std::wcerr << L"Usage: projection-attach <PID> <projection-dx11.dll|projection-dx12.dll> "
                          L"<manifest.json>\n";
            return 2;
        }
        DWORD pid = std::stoul(argv[1]);
        if (pid <= 4 || pid == GetCurrentProcessId())
            throw std::runtime_error("Select a running user application");
        auto dll = std::filesystem::absolute(argv[2]);
        auto manifest = std::filesystem::absolute(argv[3]);
        if (!std::filesystem::is_regular_file(dll) || !std::filesystem::is_regular_file(manifest))
            throw std::runtime_error("DLL or manifest does not exist");
        Handle process{OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION |
                                       PROCESS_VM_WRITE | PROCESS_VM_READ,
                                   FALSE, pid)};
        if (!process.value)
            throw std::runtime_error("Cannot open selected process");
        wchar_t executable[32768]{};
        DWORD executable_length = 32768;
        if (!QueryFullProcessImageNameW(process, 0, executable, &executable_length))
            throw std::runtime_error("Cannot identify selected application");
        auto basename = std::filesystem::path(executable).filename().wstring();
        if (_wcsicmp(basename.c_str(), L"dwm.exe") == 0)
            throw std::runtime_error("DWM integration is not supported; select a user application");
        BOOL targetWow, selfWow;
        IsWow64Process(process, &targetWow);
        IsWow64Process(GetCurrentProcess(), &selfWow);
        if (targetWow != selfWow)
            throw std::runtime_error("Adapter and target must have the same architecture");
        auto other = _wcsicmp(dll.filename().c_str(), L"projection-dx12.dll") == 0 ? L"projection-dx11.dll"
                                                                                   : L"projection-dx12.dll";
        if (module_base(pid, other))
            throw std::runtime_error("Restart the application before switching graphics adapters; the "
                                     "previous hook stays loaded until exit");
        auto remote = module_base(pid, dll.filename().wstring());
        if (!remote) {
            auto kernel = GetModuleHandleW(L"kernel32.dll");
            auto load = GetProcAddress(kernel, "LoadLibraryW");
            auto remoteKernel = module_base(pid, L"kernel32.dll");
            if (!remoteKernel)
                throw std::runtime_error("Cannot find target Windows loader");
            call(process,
                 remoteKernel +
                     (reinterpret_cast<std::uintptr_t>(load) - reinterpret_cast<std::uintptr_t>(kernel)),
                 dll.wstring());
            remote = module_base(pid, dll.filename().wstring());
            if (!remote)
                throw std::runtime_error("Adapter did not load; ensure its runtime DLL dependencies are "
                                         "installed next to the target or on PATH");
        }
        HMODULE local = LoadLibraryExW(dll.c_str(), nullptr, DONT_RESOLVE_DLL_REFERENCES);
        if (!local)
            throw std::runtime_error("Cannot inspect adapter exports");
        auto configure = GetProcAddress(local, "PSConfigure");
        if (!configure) {
            FreeLibrary(local);
            throw std::runtime_error("Missing PSConfigure export");
        }
        auto offset = reinterpret_cast<std::uintptr_t>(configure) - reinterpret_cast<std::uintptr_t>(local);
        FreeLibrary(local);
        auto code = call(process, remote + offset, manifest.wstring());
        if (code)
            throw std::runtime_error("Adapter rejected configuration; inspect manifest.status.json");
        std::cout << "Graphics adapter loaded to selected process. Disable by setting enabled=false in its "
                     "manifest.\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
