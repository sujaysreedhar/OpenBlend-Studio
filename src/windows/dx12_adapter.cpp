// SPDX-License-Identifier: GPL-3.0-only
#include "ps/d3d12_renderer.hpp"
#include <dxgi1_4.h>
#include <MinHook.h>
#include <mutex>
#include <atomic>
#include <fstream>
#include <chrono>
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
using Create = HRESULT(WINAPI*)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
using CreateHwnd = HRESULT(WINAPI*)(IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*,
                                    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1**);
Present original_present{};
Present1 original_present1{};
Resize original_resize{};
Release original_release{};
Create original_create{};
CreateHwnd original_create_hwnd{};
std::recursive_mutex guard;
bool installed{}, enabled{}, blackout{};
thread_local bool inside{};
HWND selected{};
Project project;
std::string output_id;
std::filesystem::path manifest;
std::uint64_t modified{};
std::chrono::steady_clock::time_point last_reload{}, last_status{};
std::uint64_t rendered{}, busy{}, unknown_queue{};
struct Entry {
    Com<ID3D12CommandQueue> queue;
    std::unique_ptr<Renderer12> renderer;
};
std::unordered_map<IDXGISwapChain*, Entry> entries;
void status(const std::string& message) {
    if (manifest.empty())
        return;
    try {
        atomic_write(manifest.wstring() + L".status.json", Json{{"api", "dx12"},
                                                                {"message", message},
                                                                {"enabled", enabled},
                                                                {"rendered_frames", rendered},
                                                                {"busy_frames", busy},
                                                                {"unknown_queue_frames", unknown_queue}}
                                                               .dump());
    } catch (...) {
    }
}
void associate(IDXGISwapChain* chain, IUnknown* device) {
    if (!chain || !device)
        return;
    Com<ID3D12CommandQueue> queue;
    if (FAILED(device->QueryInterface(__uuidof(ID3D12CommandQueue), reinterpret_cast<void**>(queue.put()))) ||
        queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT)
        return;
    Com<ID3D12Device> chain_device, queue_device;
    check(chain->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void**>(chain_device.put())),
          "Swapchain device");
    check(queue->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void**>(queue_device.put())),
          "Queue device");
    Com<IUnknown> chain_identity, queue_identity;
    check(chain_device->QueryInterface(__uuidof(IUnknown), reinterpret_cast<void**>(chain_identity.put())),
          "Swapchain identity");
    check(queue_device->QueryInterface(__uuidof(IUnknown), reinterpret_cast<void**>(queue_identity.put())),
          "Queue identity");
    if (chain_identity.get() != queue_identity.get())
        throw std::runtime_error("Queue belongs to a different device");
    std::lock_guard lock(guard);
    auto& entry = entries[chain];
    entry.renderer.reset();
    entry.queue = std::move(queue);
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
        auto j = Json::parse(std::ifstream(manifest));
        auto candidate = project_from_json(j.at("project"));
        project = std::move(candidate);
        selected = reinterpret_cast<HWND>(j.at("target_hwnd").get<std::uint64_t>());
        output_id = j.at("output_id");
        blackout = j.value("blackout", false);
        enabled = j.value("enabled", true);
        modified = stamp;
        status(enabled ? "DX12 enabled; waiting for a registered presentation queue"
                       : "DX12 disabled; original presentation restored");
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
        auto it = entries.find(swap);
        if (it == entries.end()) {
            ++unknown_queue;
            if (std::chrono::steady_clock::now() - last_status > std::chrono::seconds(1)) {
                status("Presentation queue unknown. Recreate the application's swapchain after attachment, "
                       "register its queue through the SDK, or use window capture.");
                last_status = std::chrono::steady_clock::now();
            }
            return false;
        }
        auto& entry = it->second;
        Com<ID3D12Device> device;
        check(swap->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void**>(device.put())),
              "Application DX12 device");
        if (!entry.renderer) {
            entry.renderer = std::make_unique<Renderer12>(device.get(), entry.queue.get());
            entry.renderer->set_asset_directory(manifest.parent_path());
        }
        Com<IDXGISwapChain3> swap3;
        check(swap->QueryInterface(__uuidof(IDXGISwapChain3), reinterpret_cast<void**>(swap3.put())),
              "DX12 swapchain3");
        Com<ID3D12Resource> back;
        check(swap->GetBuffer(swap3->GetCurrentBackBufferIndex(), __uuidof(ID3D12Resource),
                              reinterpret_cast<void**>(back.put())),
              "DX12 application backbuffer");
        auto output = std::find_if(project.outputs.begin(), project.outputs.end(),
                                   [](const auto& o) { return o.id == output_id; });
        if (output == project.outputs.end())
            throw std::runtime_error("DX12 output ID not found");
        auto config = *output;
        if (blackout)
            config.enabled = false;
        bool submitted = entry.renderer->render(config, back.get(), D3D12_RESOURCE_STATE_PRESENT, back.get(),
                                                D3D12_RESOURCE_STATE_PRESENT) != 0;
        if (submitted)
            ++rendered;
        else
            ++busy;
        if (std::chrono::steady_clock::now() - last_status > std::chrono::seconds(1)) {
            status("DX12 presentation active");
            last_status = std::chrono::steady_clock::now();
        }
        return submitted;
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
HRESULT WINAPI present(IDXGISwapChain* swap, UINT interval, UINT flags) {
    if (inside)
        return original_present(swap, interval, flags);
    Reentry reentry;
    warp(swap, flags);
    observe_for_test(swap);
    return original_present(swap, interval, flags);
}
HRESULT WINAPI present1(IDXGISwapChain1* swap, UINT interval, UINT flags, const DXGI_PRESENT_PARAMETERS* p) {
    if (inside)
        return original_present1(swap, interval, flags, p);
    Reentry reentry;
    bool warped = warp(swap, flags);
    observe_for_test(swap);
    // Warping changes the whole frame; dirty rectangles from the application no longer describe it.
    DXGI_PRESENT_PARAMETERS whole{};
    return original_present1(swap, interval, flags, warped ? &whole : p);
}
HRESULT WINAPI resize(IDXGISwapChain* swap, UINT n, UINT w, UINT h, DXGI_FORMAT f, UINT flags) {
    {
        std::lock_guard lock(guard);
        auto it = entries.find(swap);
        if (it != entries.end())
            it->second.renderer.reset();
    }
    return original_resize(swap, n, w, h, f, flags);
}
ULONG WINAPI release(IUnknown* object) {
    auto count = original_release(object);
    if (!count) {
        std::lock_guard lock(guard);
        entries.erase(reinterpret_cast<IDXGISwapChain*>(object));
    }
    return count;
}
HRESULT WINAPI create(IDXGIFactory* factory, IUnknown* device, DXGI_SWAP_CHAIN_DESC* desc,
                      IDXGISwapChain** swap) {
    auto hr = original_create(factory, device, desc, swap);
    if (SUCCEEDED(hr)) {
        try {
            associate(*swap, device);
        } catch (const std::exception& e) {
            std::lock_guard lock(guard);
            status(e.what());
        } catch (...) {
        } // Never unwind through the application's COM call.
    }
    return hr;
}
HRESULT WINAPI create_hwnd(IDXGIFactory2* factory, IUnknown* device, HWND hwnd,
                           const DXGI_SWAP_CHAIN_DESC1* desc, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* full,
                           IDXGIOutput* output, IDXGISwapChain1** swap) {
    auto hr = original_create_hwnd(factory, device, hwnd, desc, full, output, swap);
    if (SUCCEEDED(hr)) {
        try {
            associate(*swap, device);
        } catch (const std::exception& e) {
            std::lock_guard lock(guard);
            status(e.what());
        } catch (...) {
        } // Never unwind through the application's COM call.
    }
    return hr;
}
void install() {
    if (installed)
        return;
    Com<IDXGIFactory2> factory;
    check(CreateDXGIFactory1(__uuidof(IDXGIFactory2), reinterpret_cast<void**>(factory.put())),
          "DX12 hook factory");
    Com<ID3D12Device> device;
    check(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device),
                            reinterpret_cast<void**>(device.put())),
          "DX12 hook device");
    D3D12_COMMAND_QUEUE_DESC q{};
    Com<ID3D12CommandQueue> queue;
    check(device->CreateCommandQueue(&q, __uuidof(ID3D12CommandQueue), reinterpret_cast<void**>(queue.put())),
          "DX12 hook queue");
    HWND window = CreateWindowExW(0, L"STATIC", L"Projection DX12 initialization", WS_OVERLAPPED, 0, 0, 64,
                                  64, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!window)
        throw std::runtime_error("DX12 initialization window failed");
    struct Window {
        HWND handle;
        ~Window() {
            DestroyWindow(handle);
        }
    } cleanup{window};
    DXGI_SWAP_CHAIN_DESC1 d{};
    d.Width = d.Height = 64;
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    d.BufferCount = 2;
    d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    Com<IDXGISwapChain1> swap;
    check(factory->CreateSwapChainForHwnd(queue.get(), window, &d, nullptr, nullptr, swap.put()),
          "DX12 hook swapchain");
    auto initialized = MH_Initialize();
    if (initialized != MH_OK && initialized != MH_ERROR_ALREADY_INITIALIZED)
        throw std::runtime_error("DX12 MinHook initialization failed");
    std::vector<void*> hooks;
    auto hook = [&](void* address, void* replacement, void** original) {
        if (MH_CreateHook(address, replacement, original) != MH_OK)
            throw std::runtime_error("DX12 hook creation failed (another adapter may already be active)");
        hooks.push_back(address);
    };
    try {
        auto v = *reinterpret_cast<void***>(swap.get());
        auto f = *reinterpret_cast<void***>(factory.get());
        hook(v[8], reinterpret_cast<void*>(present), reinterpret_cast<void**>(&original_present));
        hook(v[22], reinterpret_cast<void*>(present1), reinterpret_cast<void**>(&original_present1));
        hook(v[13], reinterpret_cast<void*>(resize), reinterpret_cast<void**>(&original_resize));
        hook(v[2], reinterpret_cast<void*>(release), reinterpret_cast<void**>(&original_release));
        hook(f[10], reinterpret_cast<void*>(create), reinterpret_cast<void**>(&original_create));
        hook(f[15], reinterpret_cast<void*>(create_hwnd), reinterpret_cast<void**>(&original_create_hwnd));
        for (auto address : hooks)
            if (MH_EnableHook(address) != MH_OK)
                throw std::runtime_error("DX12 hook activation failed");
        installed = true;
    } catch (...) {
        for (auto address : hooks) {
            MH_DisableHook(address);
            MH_RemoveHook(address);
        }
        throw;
    }
}
} // namespace
extern "C" __declspec(dllexport) DWORD WINAPI PSConfigure(LPVOID argument) {
    std::lock_guard lock(guard);
    try {
        manifest = static_cast<const wchar_t*>(argument);
        modified = {};
        last_reload = {};
        reload();
        if (!enabled)
            return 2;
        install();
        status("DX12 integration armed; existing swapchains require queue registration or recreation");
        return 0;
    } catch (const std::exception& e) {
        enabled = false;
        status(e.what());
        return 1;
    }
}
extern "C" __declspec(dllexport) int PSRegisterD3D12Swapchain(IDXGISwapChain* swap,
                                                              ID3D12CommandQueue* queue) {
    try {
        associate(swap, queue);
        std::lock_guard lock(guard);
        return entries.contains(swap) ? 0 : 1;
    } catch (...) {
        return 1;
    }
}
extern "C" __declspec(dllexport) DWORD WINAPI PSDisable(LPVOID) {
    std::lock_guard lock(guard);
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
