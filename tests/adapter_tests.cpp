// SPDX-License-Identifier: GPL-3.0-only
#include "dx12_context.hpp"
#include <iostream>
#include <thread>
#include <fstream>
#include <d3d11_1.h>
using Configure = DWORD(WINAPI*)(LPVOID);
using Register12 = int (*)(IDXGISwapChain*, ID3D12CommandQueue*);
namespace {
std::function<void(IDXGISwapChain*)> observer_function;
void WINAPI observe_frame(IDXGISwapChain* chain) {
    if (observer_function)
        observer_function(chain);
}
using SetObserver = void (*)(void(WINAPI*)(IDXGISwapChain*));
struct Window {
    HWND handle{};
    Window(const wchar_t* title) {
        handle = CreateWindowExW(0, L"STATIC", title, WS_OVERLAPPEDWINDOW, 0, 0, 200, 120, nullptr, nullptr,
                                 GetModuleHandleW(nullptr), nullptr);
        if (!handle)
            throw std::runtime_error("Test window failed");
    }
    ~Window() {
        DestroyWindow(handle);
    }
};
bool pixels_match(const std::vector<unsigned char>& p, unsigned width, unsigned height, bool warped) {
    auto edge = p[(size_t(height / 2) * width + width / 8) * 4],
         center = p[(size_t(height / 2) * width + width / 2) * 4];
    bool ok = center > 250 && (warped ? edge < 3 : edge > 250);
    if (!ok)
        std::cerr << "pixels: edge=" << int(edge) << " center=" << int(center) << " expected warp=" << warped
                  << " size=" << width << "x" << height << "\n";
    return ok;
}
} // namespace
int main(int argc, char** argv) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool dx12 = argc > 1 && std::string(argv[1]) == "--dx12";
    int failures = 0;
    auto expect = [&](bool ok, const char* message) {
        if (!ok) {
            ++failures;
            std::cerr << "FAIL " << message << '\n';
        }
    };
    try {
        auto scratch = std::filesystem::current_path() / ("adapter-test-" + make_id());
        std::filesystem::create_directories(scratch);
        auto manifest = scratch / "manifest.json";
        Window target(L"Adapter test target"), other(L"Adapter test untouched");
        auto project = example_project();
        for (auto& p : project.outputs[0].surfaces[0].mesh.points) {
            p.x = .25 + .5 * p.x;
            p.y = .25 + .5 * p.y;
        }
        Json config = {{"project", project_json(project)},
                       {"output_id", project.outputs[0].id},
                       {"target_hwnd", reinterpret_cast<std::uint64_t>(target.handle)},
                       {"enabled", true}};
        auto write = [&] {
            atomic_write(manifest, config.dump());
            std::this_thread::sleep_for(std::chrono::milliseconds(120));
        };
        write();
        auto dll = std::filesystem::current_path() / (dx12 ? "projection-dx12.dll" : "projection-dx11.dll");
        HMODULE module = LoadLibraryW(dll.c_str());
        if (!module)
            throw std::runtime_error("Load test adapter failed: " + std::to_string(GetLastError()));
        auto configure = reinterpret_cast<Configure>(GetProcAddress(module, "PSConfigure"));
        if (!configure)
            throw std::runtime_error("Adapter configure export missing");
        auto set_observer = reinterpret_cast<SetObserver>(GetProcAddress(module, "PSSetDiagnosticObserver"));
        if (!set_observer)
            throw std::runtime_error("Build adapters with PS_BUILD_TESTS to observe pre-presentation pixels");
        set_observer(observe_frame);
        if (dx12) {
            Context c(true);
            Com<IDXGIFactory2> factory;
            check(CreateDXGIFactory1(__uuidof(IDXGIFactory2), reinterpret_cast<void**>(factory.put())),
                  "Test factory");
            auto create = [&](HWND hwnd) {
                DXGI_SWAP_CHAIN_DESC1 d{};
                d.Width = 128;
                d.Height = 64;
                d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                d.SampleDesc.Count = 1;
                d.BufferCount = 2;
                d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
                d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
                Com<IDXGISwapChain1> chain;
                check(factory->CreateSwapChainForHwnd(c.queue.get(), hwnd, &d, nullptr, nullptr, chain.put()),
                      "DX12 test chain");
                Com<IDXGISwapChain3> result;
                check(
                    chain->QueryInterface(__uuidof(IDXGISwapChain3), reinterpret_cast<void**>(result.put())),
                    "Test chain3");
                return result;
            };
            std::cout << "create initial DX12 chain" << std::endl;
            auto late = create(target.handle);
            expect(configure(const_cast<wchar_t*>(manifest.c_str())) == 0, "DX12 hooks installed");
            auto draw = [&](IDXGISwapChain3* swap, bool warped, bool present1) {
                Com<ID3D12Resource> back;
                check(swap->GetBuffer(swap->GetCurrentBackBufferIndex(), __uuidof(ID3D12Resource),
                                      reinterpret_cast<void**>(back.put())),
                      "Test DX12 backbuffer");
                auto d = back->GetDesc();
                std::vector<unsigned char> white(size_t(d.Width) * d.Height * 4, 255);
                c.transfer(back.get(), &white);
                bool observed = false;
                observer_function = [&](IDXGISwapChain*) {
                    auto pixels = c.transfer(back.get());
                    observed = true;
                    expect(pixels_match(pixels, unsigned(d.Width), d.Height, warped),
                           warped ? "DX12 intercepted pixels" : "DX12 original passthrough pixels");
                };
                DXGI_PRESENT_PARAMETERS params{};
                auto hr = present1 ? swap->Present1(0, 0, &params) : swap->Present(0, 0);
                observer_function = {};
                check(hr, "DX12 test present");
                expect(observed, "DX12 observer reached real present hook");
            };
            draw(late.get(), false, false); // Created before interception: must not guess a command queue.
            auto registration =
                reinterpret_cast<Register12>(GetProcAddress(module, "PSRegisterD3D12Swapchain"));
            expect(registration && registration(late.get(), c.queue.get()) == 0,
                   "Explicit DX12 queue registration");
            draw(late.get(), true, false);
            draw(late.get(), true, true);
            check(late->ResizeBuffers(0, 192, 96, DXGI_FORMAT_UNKNOWN, 0), "DX12 resize");
            draw(late.get(), true, true);
            std::cout << "create second DX12 chain" << std::endl;
            auto untouched = create(other.handle);
            draw(untouched.get(), false, false);
            late.reset(); // Release hook retires state before recreation.
            std::cout << "recreate DX12 chain" << std::endl;
            auto captured = create(target.handle);
            draw(captured.get(), true, false);
            config["enabled"] = false;
            write();
            draw(captured.get(), false, true);
            config["enabled"] = true;
            write();
            draw(captured.get(), true, false);
            config["enabled"] = false;
            write();
            draw(captured.get(), false, false);
        } else {
            auto device = create_device();
            Com<ID3D11DeviceContext> context;
            device->GetImmediateContext(context.put());
            Com<IDXGIFactory2> factory;
            check(CreateDXGIFactory1(__uuidof(IDXGIFactory2), reinterpret_cast<void**>(factory.put())),
                  "DX11 test factory");
            auto create = [&](HWND hwnd) {
                DXGI_SWAP_CHAIN_DESC1 d{};
                d.Width = 128;
                d.Height = 64;
                d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                d.SampleDesc.Count = 1;
                d.BufferCount = 2;
                d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
                d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
                Com<IDXGISwapChain1> chain;
                check(factory->CreateSwapChainForHwnd(device.get(), hwnd, &d, nullptr, nullptr, chain.put()),
                      "DX11 test chain");
                return chain;
            };
            auto selected = create(target.handle), untouched = create(other.handle);
            expect(configure(const_cast<wchar_t*>(manifest.c_str())) == 0, "DX11 hooks installed");
            auto draw = [&](IDXGISwapChain1* swap, bool warped, bool present1) {
                Com<ID3D11Texture2D> back;
                check(swap->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(back.put())),
                      "DX11 test backbuffer");
                D3D11_TEXTURE2D_DESC d{};
                back->GetDesc(&d);
                Com<ID3D11RenderTargetView> rt;
                check(device->CreateRenderTargetView(back.get(), nullptr, rt.put()), "DX11 test RTV");
                float white[4] = {1, 1, 1, 1};
                context->ClearRenderTargetView(rt.get(), white);
                D3D11_VIEWPORT viewport{3, 7, 70, 40, .2f, .8f};
                context->RSSetViewports(1, &viewport);
                context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
                bool observed = false;
                observer_function = [&](IDXGISwapChain*) {
                    auto pixels = read_pixels(device.get(), back.get());
                    observed = true;
                    expect(pixels_match(pixels, d.Width, d.Height, warped),
                           warped ? "DX11 intercepted pixels" : "DX11 original passthrough pixels");
                };
                DXGI_PRESENT_PARAMETERS params{};
                check(present1 ? swap->Present1(0, 0, &params) : swap->Present(0, 0), "DX11 test present");
                observer_function = {};
                expect(observed, "DX11 observer reached real present hook");
                D3D11_VIEWPORT actual{};
                UINT count = 1;
                context->RSGetViewports(&count, &actual);
                D3D11_PRIMITIVE_TOPOLOGY topology;
                context->IAGetPrimitiveTopology(&topology);
                expect(actual.TopLeftX == 3 && actual.MinDepth == .2f &&
                           topology == D3D11_PRIMITIVE_TOPOLOGY_LINELIST,
                       "DX11 state isolation");
            };
            draw(selected.get(), true, false);
            draw(selected.get(), true, true);
            draw(untouched.get(), false, true);
            for (int i = 0; i < 4; ++i) {
                context->OMSetRenderTargets(0, nullptr, nullptr);
                check(selected->ResizeBuffers(0, 128 + i * 16, 64 + i * 8, DXGI_FORMAT_UNKNOWN, 0),
                      "Repeated DX11 resize");
                draw(selected.get(), true, i % 2);
            }
            context->ClearState();
            selected.reset();
            selected = create(target.handle);
            draw(selected.get(), true, false);
            config["enabled"] = false;
            write();
            draw(selected.get(), false, true);
            config["enabled"] = true;
            write();
            draw(selected.get(), true, true);
            config["enabled"] = false;
            write();
            draw(selected.get(), false, false);
        }
        set_observer(nullptr);
        atomic_write(dx12 ? "adapter12-test-results.json" : "adapter11-test-results.json",
                     Json{{"passed", failures == 0},
                          {"failures", failures},
                          {"present", true},
                          {"present1", true},
                          {"resize", true},
                          {"multiple_swapchains", true},
                          {"disable_reenable", true},
                          {"physical_presentation_qualified", false}}
                         .dump(2));
        // The hook DLL intentionally stays loaded until process exit.
    } catch (const std::exception& e) {
        ++failures;
        std::cerr << e.what() << '\n';
    }
    std::cout << (dx12 ? "DX12" : "DX11") << " adapter failures: " << failures << '\n';
    return failures ? 1 : 0;
}
