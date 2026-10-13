// SPDX-License-Identifier: GPL-3.0-only
#include "ps/windows.hpp"
#include "ps/source_registry.hpp"
#include <chrono>
#include <fstream>
#include <iostream>
#include <roapi.h>
#include <thread>
using namespace ps;
using namespace ps::win;
namespace {
struct Process {
    PROCESS_INFORMATION p{};
    Process(const std::filesystem::path& exe, const std::wstring& args) {
        auto line = L"\"" + exe.wstring() + L"\" " + args;
        STARTUPINFOW s{};
        s.cb = sizeof(s);
        if (!CreateProcessW(exe.c_str(), line.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                            nullptr, &s, &p))
            throw std::runtime_error("Start test child failed");
    }
    ~Process() {
        CloseHandle(p.hThread);
        CloseHandle(p.hProcess);
    }
    DWORD wait(DWORD timeout) {
        if (WaitForSingleObject(p.hProcess, timeout) != WAIT_OBJECT_0)
            throw std::runtime_error("Test child timeout");
        DWORD code{};
        GetExitCodeProcess(p.hProcess, &code);
        return code;
    }
};
Json read(const std::filesystem::path& path) {
    std::ifstream in(path);
    return Json::parse(in);
}
} // namespace
int main(int argc, char** argv) {
    bool adapter_only = argc > 1 && std::string(argv[1]) == "--adapter-only";
    RoInitialize(RO_INIT_MULTITHREADED);
    try {
        auto dir = std::filesystem::current_path();
        auto scratch = dir / ("integration-" + make_id());
        std::filesystem::create_directories(scratch);
        auto info = scratch / "sample.json";
        Process sample(dir / "projection-dx11-sample.exe",
                       L"--white --seconds 60 --info \"" + info.wstring() + L"\"");
        for (int i = 0; i < 100 && !std::filesystem::exists(info); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        auto app = read(info);
        auto hwnd = reinterpret_cast<HWND>(app["hwnd"].get<std::uint64_t>());
        std::cout << "sample hwnd=" << hwnd << " visible=" << IsWindowVisible(hwnd)
                  << " pid=" << sample.p.dwProcessId << std::endl;
        struct Stop {
            HWND h;
            ~Stop() {
                PostMessageW(h, WM_CLOSE, 0, 0);
            }
        } stop{hwnd};
        auto device = create_device();
        std::unique_ptr<WindowCapture> capture;
        if (!adapter_only)
            capture = std::make_unique<WindowCapture>(device.get(), hwnd, false);
        Renderer renderer(device.get());
        auto output = example_project().outputs[0];
        auto observe = [&](bool warped) {
            for (int i = 0; i < 180; ++i) {
                MSG message;
                while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                    TranslateMessage(&message);
                    DispatchMessageW(&message);
                }
                if (WaitForSingleObject(sample.p.hProcess, 0) == WAIT_OBJECT_0)
                    throw std::runtime_error("Sample application exited early");
                if (capture->update() && capture->view()) {
                    auto texture = render_offscreen(
                        device.get(), renderer, output,
                        [&](const std::string&) { return SourceTexture{capture->view(), -1}; }, 128, 64);
                    auto pixels = read_pixels(device.get(), texture.get());
                    int edge = pixels[(32 * 128 + 16) * 4], center = pixels[(32 * 128 + 64) * 4];
                    if (i % 30 == 0)
                        std::cout << "capture " << i << " edge=" << edge << " center=" << center
                                  << " timestamp=" << capture->timestamp() << '\n';
                    if (center > 245 && (warped ? edge < 5 : edge > 245))
                        return true;
                    if (i == 150) {
                        std::ofstream image(scratch / "capture.ppm", std::ios::binary);
                        image << "P6\n128 64\n255\n";
                        for (size_t k = 0; k < pixels.size(); k += 4)
                            image.write(reinterpret_cast<const char*>(pixels.data() + k), 3);
                    }
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            return false;
        };
        if (!adapter_only && !observe(false))
            throw std::runtime_error("Windows Graphics Capture did not reproduce the white test application");
        auto project = example_project();
        for (auto& p : project.outputs[0].surfaces[0].mesh.points) {
            p.x = .25 + .5 * p.x;
            p.y = .25 + .5 * p.y;
        }
        auto manifest = scratch / "adapter.json";
        Json config = {{"project", project_json(project)},
                       {"target_hwnd", app["hwnd"]},
                       {"output_id", project.outputs[0].id},
                       {"enabled", true},
                       {"diagnostic_readback", true}};
        atomic_write(manifest, config.dump());
        Process attach(dir / "projection-attach.exe", std::to_wstring(app["pid"].get<unsigned>()) + L" \"" +
                                                          (dir / "projection-dx11.dll").wstring() + L"\" \"" +
                                                          manifest.wstring() + L"\"");
        if (attach.wait(15000))
            throw std::runtime_error("DX11 attach helper failed");
        auto diagnostics = [&](bool active) {
            for (int i = 0; i < 150; ++i) {
                try {
                    auto state = read(manifest.wstring() + L".status.json");
                    if (!active && !state.value("enabled", true))
                        return true;
                    if (active && state.value("edge_red", 255) < 5 && state.value("center_red", 0) > 245)
                        return true;
                } catch (...) {
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            return false;
        };
        if (!diagnostics(true) || (!adapter_only && !observe(true)))
            throw std::runtime_error("DX11 hook did not warp the application");
        // Explicitly request resize: frame-count timing varies on remote/occluded desktops.
        PostMessageW(hwnd, WM_APP + 1, 0, 0);
        Json after_resize;
        for (int i = 0; i < 300; ++i) {
            try {
                // A replacement can briefly deny a reader on Windows; retry within the deadline.
                after_resize = read(manifest.wstring() + L".status.json");
                if (after_resize.value("width", 0) == 960)
                    break;
            } catch (const std::exception&) {
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        if (!after_resize.is_object() || after_resize.value("width", 0) != 960)
            throw std::runtime_error("Adapter did not recreate resized resources");
        if (!diagnostics(true) || (!adapter_only && !observe(true)))
            throw std::runtime_error("Warp failed after swapchain resize");
        config["enabled"] = false;
        atomic_write(manifest, config.dump());
        if (!diagnostics(false) || (!adapter_only && !observe(false)))
            throw std::runtime_error("Disabling warp did not restore original presentation");
        auto capture_project = example_project();
        auto& source = capture_project.sources[0];
        source.type = "window";
        source.window_handle = reinterpret_cast<std::uint64_t>(hwnd);
        source.window_title = "Projection Studio DX11 Test";
        // Imported JSON may use forward slashes for an executable selected with native separators.
        source.executable = (dir / "projection-dx11-sample.exe").generic_string();
        source.window_class = "ProjectionStudioDX11Test";
        std::unique_ptr<SourceRegistry> registry;
        auto source_visible = [&] {
            for (int i = 0; i < 240; ++i) {
                MSG m;
                while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
                    TranslateMessage(&m);
                    DispatchMessageW(&m);
                }
                registry->begin_frame();
                auto frame = registry->get(source.id);
                if (frame.view) {
                    auto texture = render_offscreen(
                        device.get(), renderer, output, [&](const auto&) { return frame; }, 128, 64);
                    auto pixels = read_pixels(device.get(), texture.get());
                    if (pixels[(32 * 128 + 64) * 4] > 245)
                        return true;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            return false;
        };
        if (!adapter_only) {
            capture.reset();
            registry = std::make_unique<SourceRegistry>(device.get(), dir);
            registry->configure(capture_project);
            if (!source_visible())
                throw std::runtime_error("Registry did not capture the source");
            ShowWindow(hwnd, SW_MINIMIZE);
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
            registry->begin_frame();
            if (registry->get(source.id).view || registry->errors().empty())
                throw std::runtime_error("Minimized source retained stale visible pixels");
            ShowWindow(hwnd, SW_SHOWNOACTIVATE);
            if (!source_visible())
                throw std::runtime_error("Capture did not recover after restoration");
        }
        PostMessageW(hwnd, WM_CLOSE, 0, 0);
        if (sample.wait(5000))
            throw std::runtime_error("Sample reported state corruption");
        auto done = read(info.wstring() + L".done");
        if (done["state_errors"] != 0 || done["resizes"] != 1)
            throw std::runtime_error("State preservation / resize assertions failed");
        if (!adapter_only) {
            registry->begin_frame();
            if (registry->get(source.id).view)
                throw std::runtime_error("Closed source retained visible pixels");
            auto new_info = scratch / "restarted.json";
            Process restarted(dir / "projection-dx11-sample.exe",
                              L"--white --seconds 15 --info \"" + new_info.wstring() + L"\"");
            for (int i = 0; i < 100 && !std::filesystem::exists(new_info); ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(30));
            auto new_window = reinterpret_cast<HWND>(read(new_info).at("hwnd").get<std::uint64_t>());
            Stop stop_restart{new_window};
            if (resolve_window(source) != new_window || !source_visible())
                throw std::runtime_error("Capture did not reconnect to restarted application identity");
            PostMessageW(new_window, WM_CLOSE, 0, 0);
            restarted.wait(5000);
        }
        atomic_write(dir / "integration-results.json", Json{{"wgc", !adapter_only},
                                                            {"dx11_warp", true},
                                                            {"disable_configuration", true},
                                                            {"minimize_restore", !adapter_only},
                                                            {"source_restart_reconnect", !adapter_only},
                                                            {"sample", done},
                                                            {"physical_projectors", false}}
                                                           .dump(2));
        std::cout << "DX11 interception, resize, state restoration and disable passed; capture validated="
                  << !adapter_only << '\n';
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
