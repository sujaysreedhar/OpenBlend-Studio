// SPDX-License-Identifier: GPL-3.0-only
#include "ps/setup_check.hpp"
#include "ps/qualification.hpp"
#include "ps/gpu_timing.hpp"
#include <d3d12.h>
#include <tlhelp32.h>
#include <winver.h>
#include <thread>
#include <cmath>
#include <cstdio>
namespace ps::win {
namespace {
std::string luid(LUID id) {
    return std::to_string(id.HighPart) + ":" + std::to_string(id.LowPart);
}
std::string version(DWORD high, DWORD low) {
    return std::to_string(HIWORD(high)) + "." + std::to_string(LOWORD(high)) + "." +
           std::to_string(HIWORD(low)) + "." + std::to_string(LOWORD(low));
}
Json file_version(const std::filesystem::path& path) {
    DWORD unused{}, size = GetFileVersionInfoSizeW(path.c_str(), &unused);
    if (!size || size > 1024 * 1024)
        return nullptr;
    std::vector<unsigned char> data(size);
    if (!GetFileVersionInfoW(path.c_str(), 0, size, data.data()))
        return nullptr;
    VS_FIXEDFILEINFO* info{};
    UINT length{};
    if (!VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&info), &length) ||
        length < sizeof(*info))
        return nullptr;
    return version(info->dwFileVersionMS, info->dwFileVersionLS);
}
Json runtime_modules(DWORD pid) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snapshot == INVALID_HANDLE_VALUE)
        return nullptr;
    Json result = Json::array();
    MODULEENTRY32W module{};
    module.dwSize = sizeof(module);
    if (Module32FirstW(snapshot, &module))
        do {
            for (auto name : {L"d3d11.dll", L"d3d12.dll", L"vulkan-1.dll", L"opengl32.dll"})
                if (!_wcsicmp(module.szModule, name))
                    result.push_back(utf8(module.szModule));
        } while (Module32NextW(snapshot, &module));
    CloseHandle(snapshot);
    return result;
}
std::string registry_string(const wchar_t* name) {
    wchar_t text[256]{};
    DWORD size = sizeof(text);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", name,
                     RRF_RT_REG_SZ, nullptr, text, &size) != ERROR_SUCCESS)
        return "unavailable";
    return utf8(text);
}
} // namespace
Json check_setup(const Project& project, const std::filesystem::path& path, bool probe, double seconds,
                 const std::function<bool()>& cancelled) {
    if (!std::isfinite(seconds) || seconds < .1 || seconds > 30)
        throw std::invalid_argument("Capture probe must last 0.1–30 seconds");
    SYSTEMTIME utc{};
    GetSystemTime(&utc);
    char stamp[32];
    snprintf(stamp, sizeof(stamp), "%04u-%02u-%02uT%02u:%02u:%02uZ", utc.wYear, utc.wMonth, utc.wDay,
             utc.wHour, utc.wMinute, utc.wSecond);
    Json report = {{"kind", "setup-check"},
                   {"schema_version", 1},
                   {"application_version", "0.3.0"},
                   {"created_utc", stamp},
                   {"project_id", project.id},
                   {"os",
                    {{"build", registry_string(L"CurrentBuildNumber")},
                     {"display_version", registry_string(L"DisplayVersion")},
                     {"remote_session", GetSystemMetrics(SM_REMOTESESSION) != 0}}},
                   {"adapters", Json::array()},
                   {"displays", Json::array()},
                   {"applications", Json::array()},
                   {"capture", Json::object()},
                   {"stores_captured_pixels", false}};
    auto errors = validate(project);
    add_check(report, "project", errors.empty() ? "pass" : "fail",
              errors.empty() ? "Project structure and routes are valid" : errors.front());
    Com<IDXGIFactory1> factory;
    check(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(factory.put())),
          "Inspect graphics adapters");
    for (unsigned index = 0;; ++index) {
        Com<IDXGIAdapter1> adapter;
        auto hr = factory->EnumAdapters1(index, adapter.put());
        if (hr == DXGI_ERROR_NOT_FOUND)
            break;
        check(hr, "Enumerate adapters");
        DXGI_ADAPTER_DESC1 desc{};
        check(adapter->GetDesc1(&desc), "Adapter description");
        LARGE_INTEGER driver{};
        Json driver_version = nullptr;
        if (SUCCEEDED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &driver)))
            driver_version = version(driver.HighPart, driver.LowPart);
        Com<ID3D12Device> dx12;
        bool supports12 =
            SUCCEEDED(D3D12CreateDevice(adapter.get(), D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device),
                                        reinterpret_cast<void**>(dx12.put())));
        bool supports11 = false;
        try {
            auto device = create_device(false, &desc.AdapterLuid);
            supports11 = bool(device);
        } catch (...) {
        }
        report["adapters"].push_back({{"name", utf8(desc.Description)},
                                      {"luid", luid(desc.AdapterLuid)},
                                      {"vendor_id", desc.VendorId},
                                      {"device_id", desc.DeviceId},
                                      {"dedicated_memory_bytes", desc.DedicatedVideoMemory},
                                      {"software", bool(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)},
                                      {"umd_driver_version", driver_version},
                                      {"dx11_device", supports11},
                                      {"dx12_device", supports12}});
    }
    auto monitors = displays();
    add_check(report, "desktop", monitors.empty() ? "blocked" : "pass",
              monitors.empty() ? "No DXGI display attached to this execution desktop"
                               : "Attached displays enumerated",
              monitors.empty() ? "Run from a visible, unlocked Windows session on the display PC" : "");
    for (const auto& display : monitors) {
        DEVMODEW mode{};
        mode.dmSize = sizeof(mode);
        bool known = EnumDisplaySettingsW(wide(display.id).c_str(), ENUM_CURRENT_SETTINGS, &mode);
        report["displays"].push_back(
            {{"id", display.id},
             {"name", display.name},
             {"adapter_luid", luid(display.adapter)},
             {"x", display.bounds.left},
             {"y", display.bounds.top},
             {"width", display.bounds.right - display.bounds.left},
             {"height", display.bounds.bottom - display.bounds.top},
             {"rotation", display.rotation},
             {"refresh_hz",
              known && mode.dmDisplayFrequency > 1 ? Json(mode.dmDisplayFrequency) : Json(nullptr)}});
    }
    for (const auto& output : project.outputs) {
        auto display = std::find_if(monitors.begin(), monitors.end(),
                                    [&](const auto& d) { return d.id == output.display; });
        add_check(report, "output/" + output.id,
                  display == monitors.end() ? "blocked"
                  : output.enabled          ? "pass"
                                            : "blocked",
                  display == monitors.end() ? "Projector has no connected display assignment"
                  : output.enabled          ? "Assigned display is available"
                                            : "Output is disabled",
                  "Assign and enable the projector before running a physical output test");
        for (const auto& surface : output.surfaces)
            if (!surface.mask.bitmap.empty()) {
                bool exists = std::filesystem::is_regular_file(resolve_asset(path, surface.mask.bitmap));
                add_check(report, "mask/" + surface.id, exists ? "pass" : "fail",
                          exists ? "Mask asset exists" : "Mask asset missing", "Relink the mask file");
            }
    }
    for (const auto& source : project.sources) {
        if (cancelled && cancelled()) {
            add_check(report, "cancelled", "blocked", "Capture checks cancelled");
            break;
        }
        if (source.type == "image") {
            bool exists = std::filesystem::is_regular_file(resolve_asset(path, source.image));
            add_check(report, "source/" + source.id, exists ? "pass" : "fail",
                      exists ? "Image asset exists" : "Image asset missing", "Relink the image");
            continue;
        }
        if (source.type != "desktop" && source.type != "window")
            continue;
        if (!probe) {
            add_check(report, "capture/" + source.id, "not_run", "Capture probe was not requested",
                      "Enable live capture probes");
            continue;
        }
        Json sample = {{"type", source.type},
                       {"frames", 0},
                       {"first_timestamp_100ns", nullptr},
                       {"last_timestamp_100ns", nullptr}};
        try {
            const Display* input = nullptr;
            if (source.type == "desktop") {
                for (const auto& d : monitors)
                    if (d.id == source.display)
                        input = &d;
                if (!input)
                    throw std::runtime_error("Capture display is disconnected");
            }
            const Display* consumer = nullptr;
            for (const auto& o : project.outputs)
                for (const auto& s : o.surfaces)
                    if (s.source_id == source.id)
                        for (const auto& d : monitors)
                            if (d.id == o.display) {
                                if (input && luid(input->adapter) != luid(d.adapter))
                                    throw std::runtime_error("Desktop source and output use different GPUs; "
                                                             "cross-GPU transport is unavailable");
                                if (!consumer)
                                    consumer = &d;
                            }
            auto device = create_device(false, input      ? &input->adapter
                                               : consumer ? &consumer->adapter
                                                          : nullptr);
            std::unique_ptr<DesktopCapture> desktop;
            std::unique_ptr<WindowCapture> window;
            if (input)
                desktop = std::make_unique<DesktopCapture>(device.get(), source.display, source.cursor);
            else
                window = std::make_unique<WindowCapture>(device.get(), resolve_window(source), source.cursor);
            Measurements age;
            std::uint64_t frames = 0;
            auto start = std::chrono::steady_clock::now();
            while (std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() <
                   seconds) {
                if (cancelled && cancelled())
                    break;
                MSG msg;
                while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                    TranslateMessage(&msg);
                    DispatchMessageW(&msg);
                }
                if (desktop ? desktop->update() : window->update()) {
                    auto timestamp = desktop ? desktop->timestamp() : window->timestamp();
                    if (!frames)
                        sample["first_timestamp_100ns"] = timestamp;
                    ++frames;
                    sample["last_timestamp_100ns"] = timestamp;
                    if (timestamp > 0)
                        age.add(double(qpc_100ns() - timestamp) / 10000.);
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            sample["frames"] = frames;
            sample["arrival_age_ms"] = age.json();
            bool stopped = cancelled && cancelled();
            add_check(report, "capture/" + source.id, frames && !stopped ? "pass" : "blocked",
                      stopped  ? "Capture probe cancelled"
                      : frames ? "GPU capture frames arrived; pixels were not saved"
                               : "No capture frames arrived",
                      "Keep the source visible and moving on an unlocked desktop; inspect permissions if no "
                      "frames arrive");
        } catch (const std::exception& e) {
            sample["error"] = e.what();
            add_check(report, "capture/" + source.id, "blocked", e.what(),
                      "Check source selection, session visibility, GPU routing and capture permissions");
        }
        report["capture"][source.id] = std::move(sample);
    }
    auto running = windows();
    for (const auto& profile : project.applications) {
        Json info = {{"name", profile.name},
                     {"api", profile.api},
                     {"simulator_version", profile.simulator_version},
                     {"enabled", profile.enabled},
                     {"excluded", profile.excluded},
                     {"instances", Json::array()},
                     {"qualification", "not_run"}};
        for (const auto& w : running)
            if (!profile.executable.empty() &&
                (!_wcsicmp(wide(w.executable).c_str(), wide(profile.executable).c_str()) ||
                 !_wcsicmp(std::filesystem::path(wide(w.executable)).filename().c_str(),
                           wide(profile.executable).c_str()))) {
                info["instances"].push_back(
                    {{"pid", w.pid},
                     {"executable_version", file_version(wide(w.executable))},
                     {"loaded_graphics_runtimes", runtime_modules(w.pid)},
                     {"note",
                      "Loaded modules do not prove the active swapchain API or DX12 queue association"}});
            }
        report["applications"].push_back(std::move(info));
    }
    finish_checks(report);
    return report;
}
} // namespace ps::win
