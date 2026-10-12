// SPDX-License-Identifier: GPL-3.0-only
#include "ps/windows.hpp"
#include <chrono>
#include <iostream>
#include <thread>
#include <algorithm>
using namespace ps;
using namespace ps::win;
int main(int argc, char** argv) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    try {
        int outputs = 3, width = 1920, height = 1080, frames = 120;
        bool software = false;
        std::filesystem::path report = "benchmark.json";
        for (int i = 1; i < argc; ++i) {
            std::string a = argv[i];
            if (a == "--software")
                software = true;
            else if (i + 1 < argc) {
                if (a == "--outputs")
                    outputs = std::stoi(argv[++i]);
                else if (a == "--width")
                    width = std::stoi(argv[++i]);
                else if (a == "--height")
                    height = std::stoi(argv[++i]);
                else if (a == "--frames")
                    frames = std::stoi(argv[++i]);
                else if (a == "--report")
                    report = argv[++i];
                else
                    throw std::runtime_error("Unknown option");
            } else
                throw std::runtime_error("Missing option value");
        }
        if (outputs < 1 || outputs > 16 || width < 64 || height < 64 || width > 8192 || height > 8192 ||
            frames < 1 || frames > 10000)
            throw std::runtime_error("Benchmark configuration out of range");
        auto device = create_device(software);
        Com<ID3D11DeviceContext> context;
        device->GetImmediateContext(context.put());
        Com<IDXGIDevice> dx;
        device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(dx.put()));
        Com<IDXGIAdapter> adapter;
        dx->GetAdapter(adapter.put());
        DXGI_ADAPTER_DESC descriptor{};
        adapter->GetDesc(&descriptor);
        auto project = example_project(outputs);
        std::vector<std::unique_ptr<Renderer>> renderers;
        std::vector<Com<ID3D11RenderTargetView>> targets;
        for (int i = 0; i < outputs; ++i) {
            renderers.push_back(std::make_unique<Renderer>(device.get()));
            D3D11_TEXTURE2D_DESC d{};
            d.Width = width;
            d.Height = height;
            d.MipLevels = d.ArraySize = 1;
            d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            d.SampleDesc.Count = 1;
            d.BindFlags = D3D11_BIND_RENDER_TARGET;
            Com<ID3D11Texture2D> texture;
            check(device->CreateTexture2D(&d, nullptr, texture.put()), "Benchmark texture");
            Com<ID3D11RenderTargetView> rt;
            check(device->CreateRenderTargetView(texture.get(), nullptr, rt.put()), "Benchmark target");
            targets.push_back(std::move(rt));
        }
        Com<ID3D11Query> disjoint, start, end;
        D3D11_QUERY_DESC q{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
        check(device->CreateQuery(&q, disjoint.put()), "GPU frequency query");
        q.Query = D3D11_QUERY_TIMESTAMP;
        check(device->CreateQuery(&q, start.put()), "GPU start timestamp");
        check(device->CreateQuery(&q, end.put()), "GPU end timestamp");
        std::vector<double> gpu, cpu;
        for (int frame = -10; frame < frames; ++frame) {
            auto began = std::chrono::steady_clock::now();
            context->Begin(disjoint.get());
            context->End(start.get());
            for (int i = 0; i < outputs; ++i)
                renderers[i]->render(
                    project.outputs[i], [](const std::string&) { return SourceTexture{nullptr, 8}; },
                    targets[i].get(), width, height, frame / 60.);
            context->End(end.get());
            context->End(disjoint.get());
            auto submitted = std::chrono::steady_clock::now();
            D3D11_QUERY_DATA_TIMESTAMP_DISJOINT data{};
            for (;;) {
                auto hr = context->GetData(disjoint.get(), &data, sizeof(data), 0);
                check(hr, "Read GPU frequency");
                if (hr == S_OK)
                    break;
                if (std::chrono::steady_clock::now() - submitted > std::chrono::seconds(10))
                    throw std::runtime_error("GPU timing timeout");
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            UINT64 a{}, b{};
            check(context->GetData(start.get(), &a, sizeof(a), 0), "GPU start result");
            check(context->GetData(end.get(), &b, sizeof(b), 0), "GPU end result");
            if (frame >= 0 && !data.Disjoint) {
                gpu.push_back(double(b - a) * 1000 / data.Frequency);
                cpu.push_back(std::chrono::duration<double, std::milli>(submitted - began).count());
            }
        }
        if (gpu.empty())
            throw std::runtime_error("All GPU timings were disjoint");
        auto stats = [](std::vector<double> values) {
            std::sort(values.begin(), values.end());
            double sum = 0;
            for (auto v : values)
                sum += v;
            return Json{{"mean", sum / values.size()},
                        {"p95", values[std::min(values.size() - 1, size_t(values.size() * .95))]},
                        {"max", values.back()}};
        };
        Json result = {{"adapter", utf8(descriptor.Description)},
                       {"software", software},
                       {"outputs", outputs},
                       {"width", width},
                       {"height", height},
                       {"frames", gpu.size()},
                       {"gpu_ms", stats(gpu)},
                       {"cpu_submission_ms", stats(cpu)},
                       {"includes_capture", false},
                       {"includes_presentation", false},
                       {"physical_projectors", false}};
        result["resource_uploads"] = Json::array();
        for (auto& renderer : renderers) {
            auto c = renderer->counters();
            result["resource_uploads"].push_back({{"meshes", c.mesh_uploads},
                                                  {"masks", c.mask_uploads},
                                                  {"curves", c.curve_uploads},
                                                  {"composition_targets", c.target_allocations}});
        }
        atomic_write(report, result.dump(2));
        std::cout << result.dump(2) << '\n';
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
