// SPDX-License-Identifier: GPL-3.0-only
#include "ps/windows.hpp"
#include <chrono>
#include <shellapi.h>
#include <thread>
using namespace ps;
using namespace ps::win;
namespace {
bool running = true;
bool resize_requested = false;
LRESULT CALLBACK proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_APP + 1) {
        resize_requested = true;
        return 0;
    }
    if (m == WM_CLOSE) {
        running = false;
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}
} // namespace
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    try {
        int argc;
        auto argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        int seconds = 20, pattern = 0;
        std::filesystem::path info;
        for (int i = 1; i < argc; ++i) {
            if (std::wstring(argv[i]) == L"--info" && i + 1 < argc)
                info = argv[++i];
            else if (std::wstring(argv[i]) == L"--seconds" && i + 1 < argc)
                seconds = _wtoi(argv[++i]);
            else if (std::wstring(argv[i]) == L"--white")
                pattern = 1;
            else if (std::wstring(argv[i]) == L"--motion")
                pattern = 8;
        }
        LocalFree(argv);
        WNDCLASSW wc{};
        wc.hInstance = instance;
        wc.lpfnWndProc = proc;
        wc.lpszClassName = L"ProjectionStudioDX11Test";
        RegisterClassW(&wc);
        HWND hwnd =
            CreateWindowExW(WS_EX_TOPMOST, wc.lpszClassName, L"Projection Studio DX11 Test",
                            WS_OVERLAPPEDWINDOW, 100, 100, 800, 500, nullptr, nullptr, instance, nullptr);
        ShowWindow(hwnd, SW_SHOWNOACTIVATE);
        UpdateWindow(hwnd);
        auto device = create_device();
        Com<IDXGIDevice> dx;
        device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(dx.put()));
        Com<IDXGIAdapter> adapter;
        dx->GetAdapter(adapter.put());
        Com<IDXGIFactory> factory;
        adapter->GetParent(__uuidof(IDXGIFactory), reinterpret_cast<void**>(factory.put()));
        DXGI_SWAP_CHAIN_DESC s{};
        s.BufferCount = 2;
        s.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        s.BufferUsage = D3D11_USAGE_DEFAULT | DXGI_USAGE_RENDER_TARGET_OUTPUT;
        s.OutputWindow = hwnd;
        s.SampleDesc.Count = 1;
        s.Windowed = TRUE;
        s.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        Com<IDXGISwapChain> swap;
        check(factory->CreateSwapChain(device.get(), &s, swap.put()), "Test swapchain");
        Renderer renderer(device.get());
        auto o = example_project().outputs[0];
        auto start = std::chrono::steady_clock::now();
        unsigned frames = 0, state_errors = 0, resizes = 0, occluded = 0;
        Com<ID3D11DeviceContext> context;
        device->GetImmediateContext(context.put());
        if (!info.empty())
            atomic_write(
                info,
                Json{{"pid", GetCurrentProcessId()}, {"hwnd", reinterpret_cast<std::uint64_t>(hwnd)}}.dump());
        while (running && std::chrono::steady_clock::now() - start < std::chrono::seconds(seconds)) {
            MSG m;
            while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&m);
                DispatchMessageW(&m);
            }
            if ((frames == 180 || resize_requested) && resizes == 0) {
                context->OMSetRenderTargets(0, nullptr, nullptr);
                check(swap->ResizeBuffers(0, 960, 600, DXGI_FORMAT_UNKNOWN, 0), "Resize test swapchain");
                ++resizes;
            }
            Com<ID3D11Texture2D> back;
            check(swap->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(back.put())),
                  "Test buffer");
            D3D11_TEXTURE2D_DESC d{};
            back->GetDesc(&d);
            Com<ID3D11RenderTargetView> rt;
            device->CreateRenderTargetView(back.get(), nullptr, rt.put());
            renderer.render(
                o, [&](const std::string&) { return SourceTexture{nullptr, pattern}; }, rt.get(), d.Width,
                d.Height, std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
            // Sentinels must survive presentation interception. The next frame resets them.
            D3D11_VIEWPORT sentinel{3, 7, 120, 90, .2f, .8f};
            context->RSSetViewports(1, &sentinel);
            context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
            auto presented = swap->Present(1, 0);
            check(presented, "Test Present");
            if (presented == DXGI_STATUS_OCCLUDED) {
                ++occluded;
                std::this_thread::sleep_for(std::chrono::milliseconds(16));
            }
            D3D11_VIEWPORT after{};
            UINT count = 1;
            context->RSGetViewports(&count, &after);
            D3D11_PRIMITIVE_TOPOLOGY topology;
            context->IAGetPrimitiveTopology(&topology);
            if (after.TopLeftX != 3 || after.TopLeftY != 7 || after.Width != 120 || after.MinDepth != .2f ||
                topology != D3D11_PRIMITIVE_TOPOLOGY_LINELIST)
                ++state_errors;
            ++frames;
        }
        if (!info.empty())
            atomic_write(
                info.wstring() + L".done",
                Json{{"frames", frames},
                     {"state_errors", state_errors},
                     {"resizes", resizes},
                     {"requested_seconds", seconds},
                     {"elapsed_seconds",
                      std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count()},
                     {"closed_by_message", !running},
                     {"occluded_frames", occluded}}
                    .dump());
        DestroyWindow(hwnd);
        return state_errors ? 2 : 0;
    } catch (const std::exception& e) {
        OutputDebugStringA(e.what());
        return 1;
    }
}
