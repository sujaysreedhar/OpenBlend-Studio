// SPDX-License-Identifier: GPL-3.0-only
#include "ps/windows.hpp"
#include <MinHook.h>
#include <atomic>
#include <chrono>
#include <d3d11_1.h>
#include <fstream>
#include <mutex>
using namespace ps;
using namespace ps::win;
namespace {
#ifdef PS_ADAPTER_DIAGNOSTICS
using DiagnosticObserver = void(WINAPI*)(IDXGISwapChain*);
std::atomic<DiagnosticObserver> diagnostic_observer{};
#endif
void observe_for_test(IDXGISwapChain* swap) {
#ifdef PS_ADAPTER_DIAGNOSTICS
    if (auto observer = diagnostic_observer.load())
        observer(swap);
#endif
}

using Present = HRESULT(WINAPI*)(IDXGISwapChain*, UINT, UINT);
using Present1 = HRESULT(WINAPI*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
using Resize = HRESULT(WINAPI*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
using Release = ULONG(WINAPI*)(IUnknown*);
Release original_release{};
Present original_present{};
Present1 original_present1{};
Resize original_resize{};
std::recursive_mutex guard;
std::atomic<bool> enabled{false};
bool installed{};
thread_local bool inside{};
Project project;
std::filesystem::path manifest;
HWND selected{};
std::string output_id;
bool blackout{}, diagnostic_readback{};
std::uint64_t rendered_frames{};
std::chrono::steady_clock::time_point last_diagnostic{};
std::chrono::steady_clock::time_point last_reload{};
std::uint64_t modified{};
struct Resources {
    Com<ID3D11Device> device;
    Com<ID3D11DeviceContext1> context;
    Com<ID3DDeviceContextState> state;
    std::unique_ptr<Renderer> renderer;
    Com<ID3D11Texture2D> copy;
    Com<ID3D11ShaderResourceView> source;
    unsigned width{}, height{};
    DXGI_FORMAT format{};
};
std::unordered_map<IDXGISwapChain*, std::unique_ptr<Resources>> resources;
void status(const std::string& message) {
    try {
        atomic_write(manifest.wstring() + L".status.json",
                     Json{{"message", message}, {"enabled", enabled.load()}}.dump());
    } catch (...) {
    };
}
void reload() {
    auto now = std::chrono::steady_clock::now();
    if (now - last_reload < std::chrono::milliseconds(100))
        return;
    last_reload = now;
    try {
        auto stamp = file_write_stamp(manifest);
        if (stamp == modified)
            return;
        std::ifstream in(manifest);
        Json j;
        in >> j;
        auto candidate = project_from_json(j.at("project"));
        selected = reinterpret_cast<HWND>(j.at("target_hwnd").get<std::uint64_t>());
        output_id = j.at("output_id");
        blackout = j.value("blackout", false);
        diagnostic_readback = j.value("diagnostic_readback", false);
        project = std::move(candidate);
        enabled = j.value("enabled", true);
        modified = stamp;
        status(enabled ? "DX11 configuration applied" : "DX11 disabled: original presentation restored");
    } catch (const std::exception& e) {
        enabled = false;
        status(e.what());
    }
}
bool warp(IDXGISwapChain* swap, UINT flags) {
    if (flags & DXGI_PRESENT_TEST)
        return false;
    std::lock_guard lock(guard);
    if (!manifest.empty())
        reload();
    if (!enabled)
        return false;
    DXGI_SWAP_CHAIN_DESC desc{};
    if (FAILED(swap->GetDesc(&desc)) || desc.OutputWindow != selected)
        return false;
    try {
        const Output* output = nullptr;
        for (auto& o : project.outputs)
            if (o.id == output_id)
                output = &o;
        if (!output)
            throw std::runtime_error("Adapter output ID not found");
        Com<ID3D11Texture2D> back;
        check(swap->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(back.put())),
              "Game backbuffer");
        D3D11_TEXTURE2D_DESC d{};
        back->GetDesc(&d);
        // HDR requires an explicitly color-managed path; do not corrupt the game's presentation.
        if (d.Format != DXGI_FORMAT_R8G8B8A8_UNORM && d.Format != DXGI_FORMAT_B8G8R8A8_UNORM)
            throw std::runtime_error("DX11 adapter currently supports SDR RGBA8/BGRA8 swapchains");
        auto& entry = resources[swap];
        if (!entry || entry->width != d.Width || entry->height != d.Height || entry->format != d.Format) {
            entry = std::make_unique<Resources>();
            auto& r = *entry;
            check(swap->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(r.device.put())),
                  "Game D3D11 device");
            Com<ID3D11Device1> device1;
            check(r.device->QueryInterface(__uuidof(ID3D11Device1), reinterpret_cast<void**>(device1.put())),
                  "D3D11.1 state isolation");
            Com<ID3D11DeviceContext> context;
            r.device->GetImmediateContext(context.put());
            check(context->QueryInterface(__uuidof(ID3D11DeviceContext1),
                                          reinterpret_cast<void**>(r.context.put())),
                  "Game immediate context");
            auto level = r.device->GetFeatureLevel();
            check(device1->CreateDeviceContextState(0, &level, 1, D3D11_SDK_VERSION, __uuidof(ID3D11Device),
                                                    nullptr, r.state.put()),
                  "Isolated rendering state");
            r.width = d.Width;
            r.height = d.Height;
            r.format = d.Format;
            r.renderer = std::make_unique<Renderer>(r.device.get());
            r.renderer->set_asset_directory(manifest.parent_path());
            d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            d.MiscFlags = 0;
            d.Usage = D3D11_USAGE_DEFAULT;
            d.CPUAccessFlags = 0;
            d.SampleDesc.Count = 1;
            d.SampleDesc.Quality = 0;
            check(r.device->CreateTexture2D(&d, nullptr, r.copy.put()), "Game frame copy");
            r.source = r.renderer->texture_view(r.copy.get());
        }
        auto& r = *entry;
        Com<ID3DDeviceContextState> previous;
        r.context->SwapDeviceContextState(r.state.get(), previous.put());
        struct Restore {
            ID3D11DeviceContext1* c;
            ID3DDeviceContextState* state;
            ~Restore() {
                c->SwapDeviceContextState(state, nullptr);
            }
        } restore{r.context.get(), previous.get()};
        r.context->CopyResource(r.copy.get(), back.get());
        Com<ID3D11RenderTargetView> target;
        check(r.device->CreateRenderTargetView(back.get(), nullptr, target.put()), "Game render target");
        auto config = *output;
        if (blackout)
            config.enabled = false;
        r.renderer->render(
            config, [&](const std::string&) { return SourceTexture{r.source.get(), -1}; }, target.get(),
            r.width, r.height);
        ++rendered_frames;
        auto now = std::chrono::steady_clock::now();
        if (diagnostic_readback && d.Format == DXGI_FORMAT_R8G8B8A8_UNORM &&
            now - last_diagnostic > std::chrono::milliseconds(250)) {
            auto pixels = read_pixels(r.device.get(), back.get());
            auto red = [&](unsigned x, unsigned y) { return pixels[(size_t(y) * d.Width + x) * 4]; };
            atomic_write(manifest.wstring() + L".status.json",
                         Json{{"enabled", true},
                              {"rendered_frames", rendered_frames},
                              {"width", d.Width},
                              {"height", d.Height},
                              {"edge_red", red(d.Width / 8, d.Height / 2)},
                              {"center_red", red(d.Width / 2, d.Height / 2)}}
                             .dump());
            last_diagnostic = now;
        }
        return true;
    } catch (const std::exception& e) {
        enabled = false;
        status(e.what());
        return false;
    }
}
struct Reentry {
    Reentry() {
        inside = true;
    }
    ~Reentry() {
        inside = false;
    }
};
HRESULT WINAPI hooked_present(IDXGISwapChain* swap, UINT interval, UINT flags) {
    if (inside)
        return original_present(swap, interval, flags);
    Reentry r;
    warp(swap, flags);
    observe_for_test(swap);
    return original_present(swap, interval, flags);
}
HRESULT WINAPI hooked_present1(IDXGISwapChain1* swap, UINT interval, UINT flags,
                               const DXGI_PRESENT_PARAMETERS* p) {
    if (inside)
        return original_present1(swap, interval, flags, p);
    Reentry r;
    bool warped = warp(swap, flags);
    observe_for_test(swap);
    DXGI_PRESENT_PARAMETERS whole{};
    return original_present1(swap, interval, flags, warped ? &whole : p);
}
HRESULT WINAPI hooked_resize(IDXGISwapChain* swap, UINT count, UINT width, UINT height, DXGI_FORMAT format,
                             UINT flags) {
    {
        std::lock_guard lock(guard);
        resources.erase(swap);
    }
    return original_resize(swap, count, width, height, format, flags);
}
ULONG WINAPI hooked_release(IUnknown* object) {
    auto count = original_release(object);
    if (!count) {
        std::lock_guard lock(guard);
        resources.erase(reinterpret_cast<IDXGISwapChain*>(object));
    }
    return count;
}
void install() {
    if (installed)
        return;
    HWND window = CreateWindowExW(0, L"STATIC", L"Projection adapter initialization", WS_OVERLAPPED, 0, 0, 64,
                                  64, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!window)
        throw std::runtime_error("Adapter initialization window failed");
    struct Close {
        HWND h;
        ~Close() {
            DestroyWindow(h);
        }
    } close{window};
    DXGI_SWAP_CHAIN_DESC desc{};
    desc.BufferCount = 2;
    desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.OutputWindow = window;
    desc.SampleDesc.Count = 1;
    desc.Windowed = TRUE;
    Com<IDXGISwapChain> swap;
    Com<ID3D11Device> device;
    check(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                                        D3D11_SDK_VERSION, &desc, swap.put(), device.put(), nullptr, nullptr),
          "Discover DX11 presentation entrypoints");
    if (MH_Initialize() != MH_OK)
        throw std::runtime_error("Initialize MinHook");
    auto table = *reinterpret_cast<void***>(swap.get());
    auto hook = [](void* target, void* replacement, void** original) {
        if (MH_CreateHook(target, replacement, original) != MH_OK)
            throw std::runtime_error("Create presentation hook");
    };
    try {
        hook(table[2], reinterpret_cast<void*>(hooked_release), reinterpret_cast<void**>(&original_release));
        hook(table[8], reinterpret_cast<void*>(hooked_present), reinterpret_cast<void**>(&original_present));
        hook(table[13], reinterpret_cast<void*>(hooked_resize), reinterpret_cast<void**>(&original_resize));
        Com<IDXGISwapChain1> swap1;
        if (SUCCEEDED(
                swap->QueryInterface(__uuidof(IDXGISwapChain1), reinterpret_cast<void**>(swap1.put())))) {
            auto v = *reinterpret_cast<void***>(swap1.get());
            hook(v[22], reinterpret_cast<void*>(hooked_present1),
                 reinterpret_cast<void**>(&original_present1));
        }
        if (MH_EnableHook(MH_ALL_HOOKS) != MH_OK)
            throw std::runtime_error("Enable presentation hook");
        installed = true;
    } catch (...) {
        MH_Uninitialize();
        throw;
    }
}
} // namespace
extern "C" __declspec(dllexport) DWORD WINAPI PSConfigure(LPVOID parameter) {
    std::lock_guard lock(guard);
    try {
        manifest = static_cast<const wchar_t*>(parameter);
        last_reload = {};
        modified = {};
        reload();
        if (!enabled)
            return 2;
        install();
        status("DX11 adapter active");
        return 0;
    } catch (const std::exception& e) {
        enabled = false;
        status(e.what());
        return 1;
    }
}
extern "C" __declspec(dllexport) DWORD WINAPI PSDisable(LPVOID) {
    enabled = false;
    return 0;
}
BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH)
        DisableThreadLibraryCalls(instance);
    return TRUE;
}

#ifdef PS_ADAPTER_DIAGNOSTICS
// Test builds only: observes actual pixels after interception and before DXGI rotates buffers.
extern "C" __declspec(dllexport) void PSSetDiagnosticObserver(DiagnosticObserver observer) {
    diagnostic_observer = observer;
}
#endif
