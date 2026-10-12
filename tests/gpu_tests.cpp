// SPDX-License-Identifier: GPL-3.0-only
#include "ps/color.hpp"
#include "ps/capture_transform.hpp"
#include "ps/plugin.h"
#include "ps/windows.hpp"
#include <cmath>
#include <iostream>
int main() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    try {
        using namespace ps;
        using namespace ps::win;
        auto device = create_device(true);
        Renderer renderer(device.get());
        auto o = example_project().outputs[0];
        auto white = [](const std::string&) { return SourceTexture{nullptr, 1}; };
        auto pixels =
            read_pixels(device.get(), render_offscreen(device.get(), renderer, o, white, 128, 64).get());
        int failed = 0;
        auto expect = [&](bool ok, const char* m) {
            if (!ok) {
                std::cerr << "FAIL " << m << '\n';
                ++failed;
            }
        };
        expect(pixels[(32 * 128 + 64) * 4] > 250, "identity white");
        auto before = renderer.counters();
        render_offscreen(device.get(), renderer, o, white, 128, 64);
        auto after = renderer.counters();
        expect(after.mesh_uploads == before.mesh_uploads && after.curve_uploads == before.curve_uploads &&
                   after.target_allocations == before.target_allocations,
               "unchanged frame reuses GPU resources");
        o.color.gamma = 1.1;
        render_offscreen(device.get(), renderer, o, white, 128, 64);
        expect(renderer.counters().mesh_uploads == before.mesh_uploads, "color edit keeps mesh cache");
        o.color.gamma = 1;
        o.surfaces[0].mesh.points[0].x += .0000001;
        render_offscreen(device.get(), renderer, o, white, 128, 64);
        expect(renderer.counters().mesh_uploads == before.mesh_uploads + 1,
               "sub-micro mesh edit invalidates cache");
        for (auto& p : o.surfaces[0].mesh.points) {
            p.x = .25 + .5 * p.x;
            p.y = .25 + .5 * p.y;
        }
        pixels = read_pixels(device.get(), render_offscreen(device.get(), renderer, o, white, 128, 64).get());
        expect(pixels[0] < 2, "warped border black");
        expect(pixels[(32 * 128 + 64) * 4] > 250, "warped center white");
        o = example_project().outputs[0];
        o.blend.left = .5;
        pixels = read_pixels(device.get(), render_offscreen(device.get(), renderer, o, white, 128, 64).get());
        double linear = srgb_to_linear(pixels[(32 * 128 + 32) * 4] / 255.);
        expect(std::abs(linear - .5078) < .02, "blend performed in linear light");
        o.blend.left = 0;
        o.surfaces[0].mask.enabled = true;
        o.surfaces[0].mask.polygon = {{.25, .25}, {.75, .25}, {.75, .75}, {.25, .75}};
        pixels = read_pixels(device.get(), render_offscreen(device.get(), renderer, o, white, 128, 64).get());
        expect(pixels[0] < 2, "mask outside black");
        expect(pixels[(32 * 128 + 64) * 4] > 250, "mask center white");
        o.enabled = false;
        pixels = read_pixels(device.get(), render_offscreen(device.get(), renderer, o, white, 128, 64).get());
        expect(pixels[(32 * 128 + 64) * 4] < 2, "blackout");
        o = example_project().outputs[0];
        o.color.gain = {.8, .6, .4};
        o.color.gamma = 1.3;
        o.color.brightness = .03;
        o.blend.left = .8;
        o.blend.black_level = .02;
        auto mid = [](const std::string&) { return SourceTexture{nullptr, 6}; };
        pixels = read_pixels(device.get(), render_offscreen(device.get(), renderer, o, mid, 128, 64).get());
        double u = 32.5 / 128.;
        auto reference =
            correct_color({srgb_to_linear(u), srgb_to_linear(u), srgb_to_linear(u), 1}, o.color, u / .8, .02);
        expect(std::abs(pixels[(32 * 128 + 32) * 4] / 255. - linear_to_srgb(reference.r)) < .02,
               "CPU/GPU color and black-floor agreement");
        o.color.offset = {1, 1, 1};
        o.enabled = false;
        pixels = read_pixels(device.get(), render_offscreen(device.get(), renderer, o, white, 64, 32).get());
        expect(pixels[0] == 0, "blackout overrides color offset");
        o.enabled = true;
        o.surfaces.clear();
        pixels = read_pixels(device.get(), render_offscreen(device.get(), renderer, o, white, 64, 32).get());
        expect(pixels[0] == 0, "empty output stays black after color correction");
        auto large = example_project(16);
        for (auto& output : large.outputs) {
            for (int i = 1; i < 16; ++i) {
                auto surface = output.surfaces[0];
                surface.id = make_id();
                output.surfaces.push_back(surface);
            }
            pixels = read_pixels(device.get(),
                                 render_offscreen(device.get(), renderer, output, white, 64, 32).get());
            expect(pixels[(16 * 64 + 32) * 4] > 250, "16 output / 256 surface synthetic render");
        }
        {
            CaptureTransform transform(device.get());
            std::vector<unsigned char> raw(4 * 2 * 4);
            for (unsigned i = 0; i < 8; ++i) {
                raw[i * 4] = static_cast<unsigned char>(i * 25);
                raw[i * 4 + 1] = 100;
                raw[i * 4 + 2] = 50;
                raw[i * 4 + 3] = 255;
            }
            D3D11_TEXTURE2D_DESC description{};
            description.Width = 4;
            description.Height = 2;
            description.MipLevels = description.ArraySize = description.SampleDesc.Count = 1;
            description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            D3D11_SUBRESOURCE_DATA data{raw.data(), 16, 0};
            Com<ID3D11Texture2D> source;
            check(device->CreateTexture2D(&description, &data, source.put()), "Rotation test source");
            auto view = renderer.texture_view(source.get());
            for (auto rotation : {DXGI_MODE_ROTATION_IDENTITY, DXGI_MODE_ROTATION_ROTATE90,
                                  DXGI_MODE_ROTATION_ROTATE180, DXGI_MODE_ROTATION_ROTATE270}) {
                transform.render(view.get(), 4, 2, rotation, {0, 0}, false);
                auto result = read_pixels(device.get(), transform.texture());
                unsigned w =
                    (rotation == DXGI_MODE_ROTATION_ROTATE90 || rotation == DXGI_MODE_ROTATION_ROTATE270) ? 2
                                                                                                          : 4;
                unsigned h = 8 / w;
                bool matches = true;
                for (unsigned y = 0; y < h; ++y)
                    for (unsigned x = 0; x < w; ++x) {
                        unsigned sx = x, sy = y;
                        if (rotation == DXGI_MODE_ROTATION_ROTATE90) {
                            sx = y;
                            sy = 1 - x;
                        }
                        if (rotation == DXGI_MODE_ROTATION_ROTATE180) {
                            sx = 3 - x;
                            sy = 1 - y;
                        }
                        if (rotation == DXGI_MODE_ROTATION_ROTATE270) {
                            sx = 3 - y;
                            sy = x;
                        }
                        matches &= result[(y * w + x) * 4] == raw[(sy * 4 + sx) * 4];
                    }
                expect(matches, "capture rotation pixel coordinates");
            }
            DXGI_OUTDUPL_POINTER_SHAPE_INFO info{};
            info.Type = DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR;
            info.Width = info.Height = 1;
            info.Pitch = 4;
            transform.pointer(info, std::vector<unsigned char>{0, 0, 255, 255});
            transform.render(view.get(), 4, 2, DXGI_MODE_ROTATION_IDENTITY, {0, 0}, true);
            auto result = read_pixels(device.get(), transform.texture());
            expect(result[0] == 255 && result[1] == 0, "color cursor composition");
            info.Type = DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR;
            transform.pointer(info, std::vector<unsigned char>{0, 255, 0, 255});
            transform.render(view.get(), 4, 2, DXGI_MODE_ROTATION_IDENTITY, {0, 0}, true);
            result = read_pixels(device.get(), transform.texture());
            expect(result[1] == 155, "masked cursor XOR composition");
            info.Type = DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME;
            info.Width = 2;
            info.Height = 2;
            info.Pitch = 1;
            transform.pointer(info, std::vector<unsigned char>{0x40, 0x40});
            transform.render(view.get(), 4, 2, DXGI_MODE_ROTATION_IDENTITY, {0, 0}, true);
            result = read_pixels(device.get(), transform.texture());
            expect(result[0] == 0 && result[1] == 0 && result[4] == 230 && result[5] == 155,
                   "monochrome cursor AND/XOR composition");
        }
        ps_renderer handle{};
        expect(ps_renderer_create(999, device.get(), &handle) != 0 && !handle,
               "reject incompatible renderer ABI");
        expect(ps_renderer_create(PS_ABI_VERSION, device.get(), &handle) == 0, "C ABI create");
        auto config = example_project();
        auto serialized = project_json(config).dump();
        expect(ps_renderer_configure(handle, serialized.c_str(), config.outputs[0].id.c_str(), nullptr) == 0,
               "C ABI configure");
        auto input = render_offscreen(device.get(), renderer, config.outputs[0], white, 64, 32);
        auto target = render_offscreen(device.get(), renderer, config.outputs[0], mid, 64, 32);
        Com<ID3D11RenderTargetView> rt;
        device->CreateRenderTargetView(target.get(), nullptr, rt.put());
        expect(ps_renderer_draw(handle, input.get(), rt.get(), 64, 32) == 0, "C ABI render");
        pixels = read_pixels(device.get(), target.get());
        expect(pixels[(16 * 64 + 32) * 4] > 250, "C ABI pixels");
        expect(ps_renderer_draw(handle, target.get(), rt.get(), 64, 32) != 0, "reject aliased C ABI frame");
        ps_renderer_destroy(handle);
        std::cout << "D3D11 WARP render checks: " << failed << " failures\n";
        return failed ? 1 : 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
