// SPDX-License-Identifier: GPL-3.0-only
#include "ps/d3d12_renderer.hpp"
#include "ps/plugin12.h"
#include <dxgi1_4.h>
#include <iostream>
#include <cstring>
#include <functional>
using namespace ps;
using namespace ps::win;
#include "dx12_context.hpp"

int main() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    int failures = 0;
    auto expect = [&](bool ok, const char* name) {
        if (!ok) {
            ++failures;
            std::cerr << "FAIL " << name << '\n';
        }
    };
    try {
        Context c;
        Renderer12 renderer(c.device.get(), c.queue.get());
        auto d11 = create_device(true);
        Renderer reference(d11.get());
        constexpr unsigned width = 128, height = 64;
        std::vector<unsigned char> input(width * height * 4);
        for (unsigned y = 0; y < height; ++y)
            for (unsigned x = 0; x < width; ++x) {
                auto k = (y * width + x) * 4;
                input[k] = static_cast<unsigned char>(x * 255 / (width - 1));
                input[k + 1] = static_cast<unsigned char>(y * 255 / (height - 1));
                input[k + 2] = 117;
                input[k + 3] = 255;
            }
        auto source = c.texture(width, height), target = c.texture(width, height);
        c.transfer(source.get(), &input);
        D3D11_TEXTURE2D_DESC d{};
        d.Width = width;
        d.Height = height;
        d.MipLevels = d.ArraySize = d.SampleDesc.Count = 1;
        d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        d.Usage = D3D11_USAGE_IMMUTABLE;
        D3D11_SUBRESOURCE_DATA initial{input.data(), width * 4, 0};
        Com<ID3D11Texture2D> texture11;
        check(d11->CreateTexture2D(&d, &initial, texture11.put()), "Reference source");
        auto view11 = reference.texture_view(texture11.get());
        auto provider = [&](const std::string&) { return SourceTexture{view11.get(), -1}; };
        auto output = example_project().outputs[0];
        auto compare = [&](const char* label) {
            auto done = renderer.render(output, source.get(), D3D12_RESOURCE_STATE_COMMON, target.get(),
                                        D3D12_RESOURCE_STATE_COMMON);
            expect(done && renderer.wait(done, 10000), "DX12 submitted and completed");
            auto result = c.transfer(target.get());
            auto expected = read_pixels(
                d11.get(), render_offscreen(d11.get(), reference, output, provider, width, height).get());
            int maximum = 0;
            for (size_t i = 0; i < result.size(); ++i)
                maximum = std::max(maximum, std::abs(int(result[i]) - int(expected[i])));
            std::cout << label << " max pixel error=" << maximum << '\n';
            expect(maximum <= 2, label);
        };
        compare("identity");
        auto counters = renderer.counters();
        compare("cache reuse");
        expect(renderer.counters().mesh_uploads == counters.mesh_uploads, "DX12 unchanged mesh cache");
        for (auto& p : output.surfaces[0].mesh.points) {
            p.x = .15 + .7 * p.x;
            p.y = .1 + .8 * p.y;
        }
        output.surfaces[0].mesh.curved = true;
        output.surfaces[0].mesh.points[12].x += .06;
        compare("curved warp");
        output.surfaces[0].mask.enabled = true;
        output.surfaces[0].mask.polygon = {{.1, .1}, {.9, .2}, {.7, .95}, {.2, .8}};
        output.surfaces[0].mask.feather = .025;
        compare("feathered mask");
        output.surfaces[0].mask.inverted = true;
        compare("inverted mask");
        output.blend.left = .4;
        output.blend.right = .2;
        output.blend.gamma = 1.2;
        output.blend.black_level = .02;
        output.color.gain = {.8, .9, 1.1};
        output.color.gamma = 1.3;
        output.color.brightness = .02;
        compare("linear blend and color");
        output.enabled = false;
        output.color.offset = {1, 1, 1};
        compare("blackout");
        output.enabled = true;
        output.surfaces.clear();
        compare("empty output");
        output = example_project().outputs[0];
        output.surfaces[0].mesh.points[0] = {.3, .2};
        auto done = renderer.render(output, source.get(), D3D12_RESOURCE_STATE_COMMON, source.get(),
                                    D3D12_RESOURCE_STATE_COMMON);
        expect(done && renderer.wait(done, 10000), "in-place frame complete");
        auto in_place = c.transfer(source.get());
        auto expected = read_pixels(
            d11.get(), render_offscreen(d11.get(), reference, output, provider, width, height).get());
        int maximum = 0;
        for (size_t i = 0; i < in_place.size(); ++i)
            maximum = std::max(maximum, std::abs(int(in_place[i]) - int(expected[i])));
        expect(maximum <= 2, "in-place copy before warp");
        // Old uploads and descriptors must stay alive while successive edited frames are in flight.
        c.transfer(source.get(), &input);
        for (int i = 0; i < 3; ++i) {
            output.surfaces[0].mesh.points[0].x = .1 + i * .05;
            done = renderer.render(output, source.get(), D3D12_RESOURCE_STATE_COMMON, target.get(),
                                   D3D12_RESOURCE_STATE_COMMON);
            expect(done != 0, "three in-flight frame slots");
        }
        expect(renderer.wait(done, 10000), "edited frames retire");
        ps_renderer12 handle{};
        expect(ps_renderer12_create(999, c.device.get(), c.queue.get(), &handle) == -1 && !handle,
               "reject incompatible DX12 ABI");
        expect(ps_renderer12_create(PS_DX12_ABI_VERSION, c.device.get(), c.queue.get(), &handle) == 0,
               "DX12 ABI create");
        auto project = example_project();
        auto json = project_json(project).dump();
        expect(ps_renderer12_configure(handle, json.c_str(), project.outputs[0].id.c_str(), nullptr) == 0,
               "DX12 ABI configure");
        uint64_t completion{};
        expect(ps_renderer12_draw(handle, source.get(), 0, target.get(), 0, &completion) == 0,
               "DX12 ABI draw");
        expect(ps_renderer12_wait(handle, completion, 10000) == 0, "DX12 ABI wait");
        expect(ps_renderer12_draw(handle, source.get(), 0, source.get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                                  &completion) == -1,
               "reject inconsistent alias states");
        expect(Json::parse(ps_renderer12_capabilities())["api"] == "dx12", "DX12 capability discovery");
        ps_renderer12_destroy(handle);
        atomic_write("dx12-test-results.json", Json{{"failures", failures},
                                                    {"software", true},
                                                    {"tolerance_channel_values", 2},
                                                    {"msfs_tested", false}}
                                                   .dump(2));
    } catch (const std::exception& e) {
        ++failures;
        std::cerr << e.what() << '\n';
    }
    std::cout << "DX12 regression failures: " << failures << '\n';
    return failures ? 1 : 0;
}
