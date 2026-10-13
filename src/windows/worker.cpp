// SPDX-License-Identifier: GPL-3.0-only
#include "ps/windows.hpp"
#include "ps/source_registry.hpp"
#include <dxgi1_4.h>
#include <psapi.h>
#include <chrono>
#include <fstream>
#include <shellapi.h>
#include <thread>
#include <roapi.h>
#include <unordered_map>
using namespace ps;
using namespace ps::win;
namespace {
bool running = true, blackout = false;
LRESULT CALLBACK proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_CLOSE) {
        running = false;
        return 0;
    }
    if (m == WM_KEYDOWN && w == VK_ESCAPE) {
        running = false;
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}
struct Group {
    Com<ID3D11Device> device;
    std::unique_ptr<SourceRegistry> sources;
    std::string name;
};
struct Target {
    HWND window{};
    std::string id, signature;
    Group* group{};
    Com<IDXGISwapChain2> swap;
    Com<ID3D11RenderTargetView> rt;
    std::unique_ptr<Renderer> renderer;
    std::unique_ptr<GpuTiming> timing;
    HANDLE ready{};
    unsigned width{}, height{};
    unsigned flags{};
    std::uint64_t presents{}, occluded{}, late_submissions{}, failures{};
    std::uint64_t generation{};
    double refresh_ms{1000. / 60.};
    Measurements submission, intervals, source_age;
    std::chrono::steady_clock::time_point next_probe{}, last_present{};
    std::string error;
    bool is_occluded{};
    ~Target() {
        rt.reset();
        swap.reset();
        if (ready)
            CloseHandle(ready);
        if (window)
            DestroyWindow(window);
    }
    void resize() {
        RECT r{};
        GetClientRect(window, &r);
        auto w = unsigned(r.right), h = unsigned(r.bottom);
        if (!w || !h || (w == width && h == height))
            return;
        Com<ID3D11DeviceContext> c;
        group->device->GetImmediateContext(c.put());
        c->OMSetRenderTargets(0, nullptr, nullptr);
        rt.reset();
        check(swap->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, flags), "Resize output");
        Com<ID3D11Texture2D> back;
        check(swap->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(back.put())),
              "Output buffer");
        check(group->device->CreateRenderTargetView(back.get(), nullptr, rt.put()), "Output target");
        width = w;
        height = h;
    }
};
struct Pipe {
    HANDLE handle{INVALID_HANDLE_VALUE};
    bool connected{};
    std::string pending;
    std::string outgoing;
    explicit Pipe(const std::wstring& name) {
        if (name.empty())
            return;
        handle = CreateNamedPipeW(
            (L"\\\\.\\pipe\\" + name).c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_NOWAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 65536, 65536,
            0, nullptr);
        if (handle == INVALID_HANDLE_VALUE)
            throw std::runtime_error("Cannot create renderer control pipe");
    }
    ~Pipe() {
        if (handle != INVALID_HANDLE_VALUE)
            CloseHandle(handle);
    }
    std::vector<Json> read() {
        std::vector<Json> messages;
        if (handle == INVALID_HANDLE_VALUE)
            return messages;
        if (!connected) {
            if (ConnectNamedPipe(handle, nullptr) || GetLastError() == ERROR_PIPE_CONNECTED)
                connected = true;
            else
                return messages;
        }
        flush();
        char buffer[65536];
        DWORD n{};
        while (ReadFile(handle, buffer, sizeof(buffer), &n, nullptr) && n) {
            pending.append(buffer, n);
            if (pending.size() > 32 * 1024 * 1024)
                throw std::runtime_error("Oversized control message");
        }
        auto read_error = GetLastError();
        if (read_error == ERROR_BROKEN_PIPE) {
            disconnect();
            return messages;
        }
        size_t pos;
        while ((pos = pending.find('\n')) != std::string::npos) {
            auto parsed = Json::parse(pending.substr(0, pos), nullptr, false);
            if (!parsed.is_discarded())
                messages.push_back(std::move(parsed));
            else
                send({{"type", "error"}, {"message", "Malformed JSON command"}});
            pending.erase(0, pos + 1);
        }
        return messages;
    }
    void disconnect() {
        DisconnectNamedPipe(handle);
        connected = false;
        pending.clear();
        outgoing.clear();
    }
    void send(const Json& j) {
        if (!connected)
            return;
        if (j.value("type", "") == "stats" && outgoing.size() > 262144)
            return;
        outgoing += j.dump() + "\n";
        flush();
    }
    void flush() {
        while (connected && !outgoing.empty()) {
            DWORD n{};
            if (!WriteFile(handle, outgoing.data(), DWORD(std::min<size_t>(outgoing.size(), 65536)), &n,
                           nullptr)) {
                if (GetLastError() == ERROR_BROKEN_PIPE || GetLastError() == ERROR_NO_DATA) {
                    DisconnectNamedPipe(handle);
                    connected = false;
                    pending.clear();
                    outgoing.clear();
                }
                break;
            }
            if (!n)
                break;
            outgoing.erase(0, n);
        }
    }
};
} // namespace
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    RoInitialize(RO_INIT_MULTITHREADED);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    int argc;
    auto argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::filesystem::path project_path, status_path;
    std::wstring pipe_name;
    bool fullscreen = false, software = false, native_size = false;
    double duration_limit = 0;
    int frame_limit = 0;
    for (int i = 1; i < argc; ++i) {
        std::wstring a = argv[i];
        if (a == L"--project" && i + 1 < argc)
            project_path = argv[++i];
        else if (a == L"--pipe" && i + 1 < argc)
            pipe_name = argv[++i];
        else if (a == L"--status" && i + 1 < argc)
            status_path = argv[++i];
        else if (a == L"--fullscreen")
            fullscreen = true;
        else if (a == L"--duration" && i + 1 < argc)
            duration_limit = _wtof(argv[++i]);
        else if (a == L"--native-size")
            native_size = true;
        else if (a == L"--blackout")
            blackout = true;
        else if (a == L"--software")
            software = true;
        else if (a == L"--frames" && i + 1 < argc)
            frame_limit = _wtoi(argv[++i]);
    }
    LocalFree(argv);
    auto report = [&](const Json& j) {
        if (!status_path.empty())
            try {
                atomic_write(status_path, j.dump(2));
            } catch (...) {
            };
    };
    try {
        if (!std::isfinite(duration_limit) || duration_limit < 0 || duration_limit > 90000)
            throw std::runtime_error("Invalid duration limit");
        if (project_path.empty())
            throw std::runtime_error("Use --project path.json [--fullscreen] [--pipe name]");
        auto project = load_project(project_path);
        ProjectReplica replica(project);
        Pipe pipe(pipe_name);
        WNDCLASSW wc{};
        wc.lpfnWndProc = proc;
        wc.hInstance = instance;
        wc.lpszClassName = L"ProjectionStudioOutput";
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        RegisterClassW(&wc);
        std::unordered_map<std::string, std::unique_ptr<Group>> groups;
        std::unordered_map<std::string, std::unique_ptr<Target>> targets;
        Json output_errors = Json::object();
        std::uint64_t recoveries = 0;
        std::uint64_t target_generation = 0;
        std::chrono::steady_clock::time_point identify_until{};
        auto synchronize = [&] {
            auto monitors = displays();
            bool headless = monitors.empty();
            if (headless && !fullscreen)
                monitors.push_back({"", "Simulated display", {0, 0, 1920, 1080}, 0, {0, 0}, 0});
            std::erase_if(targets, [&](const auto& entry) {
                return std::none_of(project.outputs.begin(), project.outputs.end(),
                                    [&](const Output& o) { return o.id == entry.first; });
            });
            for (auto it = groups.begin(); it != groups.end();) {
                if (FAILED(it->second->device->GetDeviceRemovedReason())) {
                    auto* failed = it->second.get();
                    std::erase_if(targets, [&](const auto& e) { return e.second->group == failed; });
                    it = groups.erase(it);
                    ++recoveries;
                } else
                    ++it;
            }
            output_errors = Json::object();
            for (size_t index = 0; index < project.outputs.size(); ++index) {
                auto& o = project.outputs[index];
                try {
                    const Display* display = nullptr;
                    if (!o.display.empty()) {
                        for (auto& d : monitors)
                            if (d.id == o.display)
                                display = &d;
                    } else if (!fullscreen && !monitors.empty())
                        display = &monitors[0];
                    if (!display)
                        throw std::runtime_error(
                            o.display.empty() ? "Assign a display before fullscreen presentation"
                                              : "Assigned display disconnected; waiting for reconnect");
                    auto key = std::to_string(display->adapter.HighPart) + ":" +
                               std::to_string(display->adapter.LowPart);
                    Json identity = {key,
                                     o.display,
                                     o.viewport,
                                     display->bounds.left,
                                     display->bounds.top,
                                     display->bounds.right,
                                     display->bounds.bottom};
                    auto signature = identity.dump();
                    auto old = targets.find(o.id);
                    if (old != targets.end() && old->second->signature == signature)
                        continue;
                    targets.erase(o.id);
                    auto& gp = groups[key];
                    if (!gp) {
                        auto candidate = std::make_unique<Group>();
                        candidate->device = create_device(software, headless ? nullptr : &display->adapter);
                        candidate->sources = std::make_unique<SourceRegistry>(candidate->device.get(),
                                                                              project_path.parent_path());
                        candidate->sources->configure(project);
                        Com<IDXGIDevice> dx;
                        candidate->device->QueryInterface(__uuidof(IDXGIDevice),
                                                          reinterpret_cast<void**>(dx.put()));
                        Com<IDXGIAdapter> adapter;
                        dx->GetAdapter(adapter.put());
                        DXGI_ADAPTER_DESC desc{};
                        adapter->GetDesc(&desc);
                        candidate->name = utf8(desc.Description);
                        gp = std::move(candidate);
                    }
                    auto t = std::make_unique<Target>();
                    t->group = gp.get();
                    t->id = o.id;
                    t->signature = signature;
                    t->generation = ++target_generation;
                    t->renderer = std::make_unique<Renderer>(gp->device.get());
                    t->renderer->set_asset_directory(project_path.parent_path());
                    t->timing = std::make_unique<GpuTiming>(gp->device.get());
                    int x = 60 + int(index % 3) * 400, y = 80 + int(index / 3) * 270, w = 384, h = 216;
                    if (native_size) {
                        w = o.width;
                        h = o.height;
                    }
                    if (fullscreen) {
                        int dw = display->bounds.right - display->bounds.left,
                            dh = display->bounds.bottom - display->bounds.top;
                        x = display->bounds.left + int(o.viewport.x * dw);
                        y = display->bounds.top + int(o.viewport.y * dh);
                        w = int(o.viewport.w * dw);
                        h = int(o.viewport.h * dh);
                    }
                    DEVMODEW mode{};
                    mode.dmSize = sizeof(mode);
                    if (EnumDisplaySettingsW(wide(display->id).c_str(), ENUM_CURRENT_SETTINGS, &mode) &&
                        mode.dmDisplayFrequency > 1)
                        t->refresh_ms = 1000. / mode.dmDisplayFrequency;
                    DWORD style = fullscreen ? WS_POPUP : WS_OVERLAPPEDWINDOW;
                    RECT client{0, 0, w, h};
                    if (!fullscreen)
                        AdjustWindowRectEx(&client, style, FALSE, 0);
                    t->window = CreateWindowExW(
                        0, wc.lpszClassName, wide(o.name + " - Projection Studio [Esc to stop]").c_str(),
                        style, x, y, client.right - client.left, client.bottom - client.top, nullptr, nullptr,
                        instance, nullptr);
                    if (!t->window)
                        throw std::runtime_error("Cannot create output window");
                    SetWindowDisplayAffinity(t->window, 0x11);
                    ShowWindow(t->window, SW_SHOW);
                    Com<IDXGIDevice> dx;
                    gp->device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(dx.put()));
                    Com<IDXGIAdapter> adapter;
                    dx->GetAdapter(adapter.put());
                    Com<IDXGIFactory2> factory;
                    check(
                        adapter->GetParent(__uuidof(IDXGIFactory2), reinterpret_cast<void**>(factory.put())),
                        "DXGI factory2");
                    DXGI_SWAP_CHAIN_DESC1 d{};
                    d.Width = w;
                    d.Height = h;
                    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                    d.SampleDesc.Count = 1;
                    d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
                    d.BufferCount = 2;
                    d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
                    d.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
                    Com<IDXGISwapChain1> chain;
                    auto hr = factory->CreateSwapChainForHwnd(gp->device.get(), t->window, &d, nullptr,
                                                              nullptr, chain.put());
                    if (FAILED(hr)) {
                        d.Flags = 0;
                        check(factory->CreateSwapChainForHwnd(gp->device.get(), t->window, &d, nullptr,
                                                              nullptr, chain.put()),
                              "Output swapchain");
                    }
                    t->flags = d.Flags;
                    check(chain->QueryInterface(__uuidof(IDXGISwapChain2),
                                                reinterpret_cast<void**>(t->swap.put())),
                          "Swapchain2");
                    factory->MakeWindowAssociation(t->window, DXGI_MWA_NO_ALT_ENTER);
                    if (d.Flags) {
                        t->swap->SetMaximumFrameLatency(1);
                        t->ready = t->swap->GetFrameLatencyWaitableObject();
                    }
                    t->resize();
                    targets.emplace(o.id, std::move(t));
                } catch (const std::exception& e) {
                    targets.erase(o.id);
                    output_errors[o.id] = e.what();
                }
            }
            std::erase_if(groups, [&](const auto& e) {
                return !e.second || std::none_of(targets.begin(), targets.end(), [&](const auto& t) {
                    return t.second->group == e.second.get();
                });
            });
        };
        synchronize();
        auto start = std::chrono::steady_clock::now(), last_report = start, last_displays = start;
        unsigned frames = 0;
        double render_ms = 0;
        Json latest;
        auto telemetry = [&] {
            Json sources = Json::object(), errors = Json::object(), outputs = Json::object(),
                 adapters = Json::object();
            for (auto& [key, g] : groups) {
                auto state = g->sources->telemetry();
                for (auto it = state.begin(); it != state.end(); ++it)
                    sources[key + "/" + it.key()] = it.value();
                errors.update(g->sources->errors());
                Json info = {{"name", g->name}};
                Com<IDXGIDevice> dx;
                g->device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(dx.put()));
                Com<IDXGIAdapter> adapter;
                dx->GetAdapter(adapter.put());
                Com<IDXGIAdapter3> a3;
                if (SUCCEEDED(adapter->QueryInterface(__uuidof(IDXGIAdapter3),
                                                      reinterpret_cast<void**>(a3.put())))) {
                    DXGI_QUERY_VIDEO_MEMORY_INFO m{};
                    if (SUCCEEDED(a3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &m)))
                        info["memory"] = {{"used_bytes", m.CurrentUsage}, {"budget_bytes", m.Budget}};
                }
                adapters[key] = std::move(info);
            }
            for (auto& [id, t] : targets) {
                auto counters = t->renderer->counters();
                DXGI_FRAME_STATISTICS statistics{};
                Json presentation = nullptr;
                if (SUCCEEDED(t->swap->GetFrameStatistics(&statistics)))
                    presentation = {{"present_count", statistics.PresentCount},
                                    {"present_refresh_count", statistics.PresentRefreshCount},
                                    {"sync_refresh_count", statistics.SyncRefreshCount}};
                outputs[id] = {{"presents", t->presents},
                               {"occluded", t->occluded},
                               {"late_submissions", t->late_submissions},
                               {"resource_generation", t->generation},
                               {"width", t->width},
                               {"height", t->height},
                               {"failures", t->failures},
                               {"refresh_hz", 1000. / t->refresh_ms},
                               {"display", std::find_if(project.outputs.begin(), project.outputs.end(),
                                                        [&](const auto& o) { return o.id == id; })
                                               ->display},
                               {"gpu_ms", t->timing->milliseconds.json()},
                               {"cpu_submission_ms", t->submission.json()},
                               {"present_interval_ms", t->intervals.json()},
                               {"source_age_at_submit_ms", t->source_age.json()},
                               {"presentation_statistics", presentation},
                               {"waitable", t->ready != nullptr},
                               {"error", t->error},
                               {"mesh_uploads", counters.mesh_uploads},
                               {"mask_uploads", counters.mask_uploads},
                               {"curve_uploads", counters.curve_uploads}};
            }
            PROCESS_MEMORY_COUNTERS m{};
            GetProcessMemoryInfo(GetCurrentProcess(), &m, sizeof(m));
            return Json{{"type", "stats"},
                        {"protocol", 2},
                        {"elapsed_seconds",
                         std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count()},
                        {"revision", replica.revision()},
                        {"frames", frames},
                        {"fps", frames / std::max(.001, std::chrono::duration<double>(
                                                            std::chrono::steady_clock::now() - start)
                                                            .count())},
                        {"render_ms", render_ms},
                        {"outputs", targets.size()},
                        {"blackout", blackout},
                        {"source_errors", errors},
                        {"source_stats", sources},
                        {"output_stats", outputs},
                        {"output_errors", output_errors},
                        {"adapters", adapters},
                        {"working_set_bytes", m.WorkingSetSize},
                        {"device_recoveries", recoveries},
                        {"physical_latency_measured", false},
                        {"dropped_frames", nullptr}};
        };
        while (running) {
            auto frame_start = std::chrono::steady_clock::now();
            MSG msg;
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_QUIT)
                    running = false;
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            for (auto& command : pipe.read()) {
                try {
                    std::string type = command.at("type");
                    if (type == "stop")
                        running = false;
                    else if (type == "blackout")
                        blackout = command.at("enabled").get<bool>();
                    else if (type == "identify")
                        identify_until =
                            std::chrono::steady_clock::now() +
                            std::chrono::seconds(std::clamp(command.value("seconds", 10), 1, 30));
                    else if (type == "project" || type == "patch") {
                        if (command.contains("revision"))
                            project = replica.apply(command);
                        else
                            project = replica.apply({{"type", "project"},
                                                     {"revision", replica.revision() + 1},
                                                     {"project", command.at("project")}});
                        for (auto& [key, g] : groups)
                            g->sources->configure(project);
                        synchronize();
                    } else
                        throw std::runtime_error("Unknown renderer command");
                    pipe.send({{"type", "ack"}, {"command", type}, {"revision", replica.revision()}});
                } catch (const std::exception& e) {
                    pipe.send({{"type", "error"},
                               {"message", e.what()},
                               {"revision", replica.revision()},
                               {"resync_required", true}});
                }
            }
            if (!running)
                break;
            if (frame_start - last_displays > std::chrono::seconds(1)) {
                synchronize();
                last_displays = frame_start;
            }
            for (auto& [key, g] : groups)
                g->sources->begin_frame();
            bool drew = false;
            for (auto& o : project.outputs) {
                auto it = targets.find(o.id);
                if (it == targets.end())
                    continue;
                auto& t = *it->second;
                if (frame_start < t.next_probe)
                    continue;
                if (t.ready && !t.is_occluded && !frame_limit &&
                    WaitForSingleObject(t.ready, 0) != WAIT_OBJECT_0)
                    continue;
                try {
                    t.resize();
                    if (!t.rt || IsIconic(t.window)) {
                        t.next_probe = frame_start + std::chrono::milliseconds(100);
                        continue;
                    }
                    auto began = std::chrono::steady_clock::now();
                    auto output = o;
                    if (blackout)
                        output.enabled = false;
                    t.timing->begin();
                    t.renderer->render(
                        output, [&](const std::string& id) { return t.group->sources->get(id); }, t.rt.get(),
                        t.width, t.height, std::chrono::duration<double>(frame_start - start).count(),
                        frame_start < identify_until ? unsigned(&o - project.outputs.data() + 1) : 0);
                    t.timing->end();
                    auto age = t.group->sources->latest_age_ms();
                    if (age > 0)
                        t.source_age.add(age);
                    t.submission.add(
                        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - began)
                            .count());
                    auto hr = t.swap->Present(t.ready ? 1 : 0, t.ready ? 0 : DXGI_PRESENT_DO_NOT_WAIT);
                    if (hr == DXGI_STATUS_OCCLUDED) {
                        t.is_occluded = true;
                        ++t.occluded;
                        t.next_probe = frame_start + std::chrono::milliseconds(frame_limit ? 16 : 250);
                    } else if (hr == DXGI_ERROR_WAS_STILL_DRAWING) {
                        t.next_probe = frame_start + std::chrono::milliseconds(1);
                    } else {
                        check(hr, "Present output");
                        t.is_occluded = false;
                        ++t.presents;
                        if (t.last_present.time_since_epoch().count()) {
                            double interval =
                                std::chrono::duration<double, std::milli>(frame_start - t.last_present)
                                    .count();
                            t.intervals.add(interval);
                            if (interval > t.refresh_ms * 1.5)
                                ++t.late_submissions;
                        }
                        t.last_present = frame_start;
                        t.next_probe =
                            t.ready
                                ? frame_start
                                : frame_start + std::chrono::microseconds(std::int64_t(t.refresh_ms * 1000));
                    }
                    t.error.clear();
                    drew = true;
                } catch (const std::exception& e) {
                    t.error = e.what();
                    ++t.failures;
                    t.next_probe = frame_start + std::chrono::seconds(1);
                    // Recreate only this target. Its device is checked during synchronization.
                    t.signature.clear();
                }
            }
            if (drew)
                ++frames;
            auto now = std::chrono::steady_clock::now();
            render_ms = std::chrono::duration<double, std::milli>(now - frame_start).count();
            if (now - last_report > std::chrono::seconds(1)) {
                latest = telemetry();
                pipe.send(latest);
                report(latest);
                last_report = now;
            }
            if (duration_limit > 0 && std::chrono::duration<double>(now - start).count() >= duration_limit)
                break;
            if (frame_limit > 0 && int(frames) >= frame_limit)
                break;
            // Bounded wait keeps control IPC and every output responsive; never waits for all outputs.
            MsgWaitForMultipleObjectsEx(0, nullptr, drew ? 1 : 4, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        }
        latest = telemetry();
        latest["type"] = "stopped";
        report(latest);
        return 0;
    } catch (const std::exception& e) {
        report({{"type", "error"}, {"message", e.what()}});
        OutputDebugStringA(e.what());
        return 1;
    }
}
