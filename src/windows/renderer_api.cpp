// SPDX-License-Identifier: GPL-3.0-only
#define PS_RENDERER_EXPORTS
#include "ps/plugin.h"
#include "ps/windows.hpp"
#include <d3d11_1.h>
namespace {
using namespace ps;
using namespace ps::win;
thread_local std::string last_error;
struct ApiRenderer {
    Com<ID3D11Device> device;
    Com<ID3D11DeviceContext1> context;
    Com<ID3DDeviceContextState> state;
    Renderer renderer;
    Output output;
    explicit ApiRenderer(ID3D11Device* d) : device(d), renderer(d), output(example_project().outputs[0]) {
        Com<ID3D11Device1> d1;
        check(d->QueryInterface(__uuidof(ID3D11Device1), reinterpret_cast<void**>(d1.put())),
              "D3D11.1 device required");
        Com<ID3D11DeviceContext> c;
        d->GetImmediateContext(c.put());
        check(c->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(context.put())),
              "D3D11.1 context required");
        auto level = d->GetFeatureLevel();
        check(d1->CreateDeviceContextState(0, &level, 1, D3D11_SDK_VERSION, __uuidof(ID3D11Device), nullptr,
                                           state.put()),
              "Renderer isolation state");
    }
};
template <class F> int guard(F f) {
    try {
        f();
        last_error.clear();
        return 0;
    } catch (const std::exception& e) {
        last_error = e.what();
        return -1;
    } catch (...) {
        last_error = "Unexpected renderer failure";
        return -1;
    }
}
} // namespace
extern "C" {
uint32_t ps_renderer_abi_version() {
    return PS_ABI_VERSION;
}
const char* ps_renderer_last_error() {
    return last_error.c_str();
}
int ps_renderer_create(uint32_t version, void* device, ps_renderer* result) {
    if (result)
        *result = nullptr;
    return guard([&] {
        if (version != PS_ABI_VERSION || !device || !result)
            throw std::invalid_argument("Invalid renderer ABI or device");
        *result = new ApiRenderer(static_cast<ID3D11Device*>(device));
    });
}
int ps_renderer_configure(ps_renderer handle, const char* json, const char* id, const char* assets) {
    return guard([&] {
        if (!handle || !json || !id)
            throw std::invalid_argument("Null renderer configuration");
        auto p = project_from_json(Json::parse(json));
        auto& r = *static_cast<ApiRenderer*>(handle);
        for (auto& output : p.outputs)
            if (output.id == id) {
                r.renderer.set_asset_directory(assets ? std::filesystem::u8path(assets)
                                                      : std::filesystem::current_path());
                r.output = std::move(output);
                return;
            }
        throw std::invalid_argument("Unknown output ID");
    });
}
int ps_renderer_draw(ps_renderer handle, void* texture, void* target, uint32_t width, uint32_t height) {
    return guard([&] {
        if (!handle || !texture || !target || !width || !height || width > 16384 || height > 16384)
            throw std::invalid_argument("Invalid frame");
        auto& r = *static_cast<ApiRenderer*>(handle);
        auto* source = static_cast<ID3D11Texture2D*>(texture);
        auto* rt = static_cast<ID3D11RenderTargetView*>(target);
        D3D11_TEXTURE2D_DESC source_desc{};
        source->GetDesc(&source_desc);
        D3D11_RENDER_TARGET_VIEW_DESC target_desc{};
        rt->GetDesc(&target_desc);
        auto sdr = [](DXGI_FORMAT f) {
            return f == DXGI_FORMAT_R8G8B8A8_UNORM || f == DXGI_FORMAT_B8G8R8A8_UNORM;
        };
        if (!sdr(source_desc.Format) || !sdr(target_desc.Format) || source_desc.SampleDesc.Count != 1)
            throw std::invalid_argument("Renderer ABI requires single-sample SDR UNORM textures and targets");
        Com<ID3D11Device> source_device, target_device;
        source->GetDevice(source_device.put());
        rt->GetDevice(target_device.put());
        if (source_device.get() != r.device.get() || target_device.get() != r.device.get())
            throw std::invalid_argument("Source and target must belong to the renderer device");
        Com<ID3D11Resource> target_resource;
        rt->GetResource(target_resource.put());
        if (target_resource.get() == source)
            throw std::invalid_argument("Source and target may not alias");
        auto view = r.renderer.texture_view(source);
        Com<ID3DDeviceContextState> previous;
        r.context->SwapDeviceContextState(r.state.get(), previous.put());
        struct Restore {
            ID3D11DeviceContext1* c;
            ID3DDeviceContextState* s;
            ~Restore() {
                c->SwapDeviceContextState(s, nullptr);
            }
        } restore{r.context.get(), previous.get()};
        r.renderer.render(
            r.output, [&](const std::string&) { return SourceTexture{view.get(), -1}; }, rt, width, height);
    });
}
void ps_renderer_destroy(ps_renderer r) {
    delete static_cast<ApiRenderer*>(r);
}
}
