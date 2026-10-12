// SPDX-License-Identifier: GPL-3.0-only
#include "ps/windows.hpp"
#include <psapi.h>
#include <sstream>
#include <stdexcept>
namespace ps::win {
void check(HRESULT hr, const char* message) {
    if (FAILED(hr)) {
        std::ostringstream s;
        s << message << " (0x" << std::hex << unsigned(hr) << ')';
        throw std::runtime_error(s.str());
    }
}
std::string utf8(const std::wstring& s) {
    if (s.empty())
        return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0, nullptr, nullptr);
    std::string r(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), int(s.size()), r.data(), n, nullptr, nullptr);
    return r;
}
std::wstring wide(const std::string& s) {
    if (s.empty())
        return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring r(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), r.data(), n);
    return r;
}
std::vector<Display> displays() {
    Com<IDXGIFactory1> factory;
    check(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(factory.put())),
          "Create DXGI factory");
    std::vector<Display> out;
    for (unsigned a = 0;; ++a) {
        Com<IDXGIAdapter1> adapter;
        if (factory->EnumAdapters1(a, adapter.put()) == DXGI_ERROR_NOT_FOUND)
            break;
        DXGI_ADAPTER_DESC1 ad{};
        adapter->GetDesc1(&ad);
        for (unsigned i = 0;; ++i) {
            Com<IDXGIOutput> output;
            if (adapter->EnumOutputs(i, output.put()) == DXGI_ERROR_NOT_FOUND)
                break;
            DXGI_OUTPUT_DESC d{};
            output->GetDesc(&d);
            if (d.AttachedToDesktop)
                out.push_back({utf8(d.DeviceName), utf8(d.DeviceName) + " / " + utf8(ad.Description),
                               d.DesktopCoordinates, int(d.Rotation), ad.AdapterLuid, i});
        }
    }
    return out;
}
std::vector<WindowInfo> windows() {
    std::vector<WindowInfo> out;
    EnumWindows(
        [](HWND hwnd, LPARAM data) -> BOOL {
            if (!IsWindowVisible(hwnd) || GetWindowTextLengthW(hwnd) == 0 || GetWindow(hwnd, GW_OWNER))
                return TRUE;
            DWORD pid;
            GetWindowThreadProcessId(hwnd, &pid);
            if (pid == GetCurrentProcessId())
                return TRUE;
            std::wstring title(size_t(GetWindowTextLengthW(hwnd)) + 1, L'\0');
            GetWindowTextW(hwnd, title.data(), int(title.size()));
            title.resize(wcslen(title.c_str()));
            std::string executable;
            HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
            if (p) {
                wchar_t path[32768];
                DWORD n = 32768;
                if (QueryFullProcessImageNameW(p, 0, path, &n))
                    executable = utf8({path, n});
                CloseHandle(p);
            }
            wchar_t window_class[256]{};
            GetClassNameW(hwnd, window_class, 256);
            reinterpret_cast<std::vector<WindowInfo>*>(data)->push_back(
                {utf8(title), executable, hwnd, pid, utf8(window_class)});
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&out));
    return out;
}
HWND resolve_window(const Source& source) {
    const auto candidates = windows();
    auto equal_path = [](const std::string& a, const std::string& b) {
        return _wcsicmp(wide(a).c_str(), wide(b).c_str()) == 0;
    };
    auto matches = [&](const WindowInfo& w) {
        return (source.executable.empty() || equal_path(w.executable, source.executable)) &&
               (source.window_class.empty() || w.window_class == source.window_class);
    };
    for (const auto& w : candidates)
        if (reinterpret_cast<std::uint64_t>(w.hwnd) == source.window_handle && matches(w) &&
            (!source.executable.empty() || w.title == source.window_title))
            return w.hwnd;
    if (source.executable.empty())
        throw std::runtime_error("Window unavailable; reselect it to save its identity");
    std::vector<HWND> exact, compatible;
    for (const auto& w : candidates)
        if (matches(w)) {
            compatible.push_back(w.hwnd);
            if (w.title == source.window_title)
                exact.push_back(w.hwnd);
        }
    if (exact.size() == 1)
        return exact[0];
    if (exact.empty() && compatible.size() == 1)
        return compatible[0];
    throw std::runtime_error(compatible.empty()
                                 ? "Source application unavailable; waiting for restart"
                                 : "Multiple source windows match; reselect the intended window");
}
Com<ID3D11Device> create_device(bool software, const LUID* wanted) {
    Com<IDXGIAdapter1> adapter;
    if (wanted && !software) {
        Com<IDXGIFactory1> f;
        check(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(f.put())), "DXGI factory");
        for (unsigned i = 0;; ++i) {
            Com<IDXGIAdapter1> a;
            if (f->EnumAdapters1(i, a.put()) == DXGI_ERROR_NOT_FOUND)
                break;
            DXGI_ADAPTER_DESC1 d{};
            a->GetDesc1(&d);
            if (d.AdapterLuid.HighPart == wanted->HighPart && d.AdapterLuid.LowPart == wanted->LowPart) {
                adapter = std::move(a);
                break;
            }
        }
        if (!adapter)
            throw std::runtime_error("Requested graphics adapter is no longer present");
    }
    Com<ID3D11Device> d;
    D3D_FEATURE_LEVEL level;
    D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0};
    check(D3D11CreateDevice(adapter.get(),
                            adapter    ? D3D_DRIVER_TYPE_UNKNOWN
                            : software ? D3D_DRIVER_TYPE_WARP
                                       : D3D_DRIVER_TYPE_HARDWARE,
                            nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 1, D3D11_SDK_VERSION, d.put(),
                            &level, nullptr),
          "Create Direct3D 11 device");
    return d;
}
} // namespace ps::win
