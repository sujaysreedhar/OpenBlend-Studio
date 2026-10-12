// SPDX-License-Identifier: GPL-3.0-only
#include "ps/geometry.hpp"
#include "ps/windows.hpp"
#include <unordered_set>
#include "shaders.hpp"
#include <algorithm>
#include <array>
#include <cstring>
#include <d3dcompiler.h>
#include <wincodec.h>
namespace ps::win {
struct Constants {
    float crop[4], params[4], widths[4], response[4], gain[4], offset[4], adjustments[4];
};
struct Renderer::Impl {
    std::vector<std::string> surface_ids;
    Com<ID3D11Device> device;
    Com<ID3D11DeviceContext> context;
    Com<ID3D11VertexShader> warpVS, fullVS;
    Com<ID3D11PixelShader> warpPS, outputPS;
    Com<ID3D11InputLayout> layout;
    Com<ID3D11Buffer> constants;
    Com<ID3D11SamplerState> sampler;
    Com<ID3D11BlendState> alpha, opaque;
    Com<ID3D11RasterizerState> raster;
    Com<ID3D11DepthStencilState> depth;
    Com<ID3D11Texture2D> intermediate;
    Com<ID3D11RenderTargetView> intermediateRT;
    Com<ID3D11ShaderResourceView> intermediateSR;
    unsigned width{}, height{};
    RenderCounters counters;
    struct MeshBuffer {
        Mesh mesh;
        Com<ID3D11Buffer> buffer;
        UINT count{};
    };
    std::unordered_map<std::string, MeshBuffer> meshes;
    struct Cached {
        Mask mask;
        std::filesystem::file_time_type modified{};
        Com<ID3D11ShaderResourceView> view;
    };
    std::unordered_map<std::string, Cached> masks;
    Com<ID3D11ShaderResourceView> curve;
    std::vector<Vec2> curve_points;
    std::filesystem::path assets;
    explicit Impl(ID3D11Device* d) : device(d) {
        d->GetImmediateContext(context.put());
        auto compile = [](const char* entry, const char* type) {
            Com<ID3DBlob> code, error;
            auto hr = D3DCompile(shaders, sizeof(shaders) - 1, nullptr, nullptr, nullptr, entry, type,
                                 D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, code.put(), error.put());
            if (FAILED(hr))
                throw std::runtime_error(error ? static_cast<const char*>(error->GetBufferPointer())
                                               : "Shader compilation failed");
            return code;
        };
        auto vs = compile("warpVS", "vs_5_0"), fs = compile("fullVS", "vs_5_0"),
             ps = compile("warpPS", "ps_5_0"), out = compile("outputPS", "ps_5_0");
        check(d->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, warpVS.put()),
              "Warp vertex shader");
        check(d->CreateVertexShader(fs->GetBufferPointer(), fs->GetBufferSize(), nullptr, fullVS.put()),
              "Output vertex shader");
        check(d->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, warpPS.put()),
              "Warp pixel shader");
        check(d->CreatePixelShader(out->GetBufferPointer(), out->GetBufferSize(), nullptr, outputPS.put()),
              "Output pixel shader");
        D3D11_INPUT_ELEMENT_DESC elements[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0}};
        check(d->CreateInputLayout(elements, 2, vs->GetBufferPointer(), vs->GetBufferSize(), layout.put()),
              "Vertex layout");
        D3D11_BUFFER_DESC b{};
        b.ByteWidth = sizeof(Constants);
        b.Usage = D3D11_USAGE_DYNAMIC;
        b.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        b.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        check(d->CreateBuffer(&b, nullptr, constants.put()), "Constant buffer");
        D3D11_SAMPLER_DESC s{};
        s.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        s.AddressU = s.AddressV = s.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        s.MaxLOD = D3D11_FLOAT32_MAX;
        check(d->CreateSamplerState(&s, sampler.put()), "Sampler");
        D3D11_BLEND_DESC blend{};
        auto& r = blend.RenderTarget[0];
        r.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        r.BlendEnable = TRUE;
        r.SrcBlend = D3D11_BLEND_ONE;
        r.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        r.BlendOp = D3D11_BLEND_OP_ADD;
        r.SrcBlendAlpha = D3D11_BLEND_ONE;
        r.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
        r.BlendOpAlpha = D3D11_BLEND_OP_ADD;
        check(d->CreateBlendState(&blend, alpha.put()), "Alpha blend");
        r.BlendEnable = FALSE;
        check(d->CreateBlendState(&blend, opaque.put()), "Opaque blend");
        D3D11_RASTERIZER_DESC ra{};
        ra.FillMode = D3D11_FILL_SOLID;
        ra.CullMode = D3D11_CULL_NONE;
        ra.DepthClipEnable = TRUE;
        check(d->CreateRasterizerState(&ra, raster.put()), "Rasterizer");
        D3D11_DEPTH_STENCIL_DESC ds{};
        ds.DepthEnable = FALSE;
        ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        ds.DepthFunc = D3D11_COMPARISON_ALWAYS;
        check(d->CreateDepthStencilState(&ds, depth.put()), "Depth state");
    }
    void set_constants(const Constants& c) {
        D3D11_MAPPED_SUBRESOURCE m{};
        check(context->Map(constants.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m), "Map constants");
        std::memcpy(m.pData, &c, sizeof(c));
        context->Unmap(constants.get(), 0);
        auto* b = constants.get();
        context->VSSetConstantBuffers(0, 1, &b);
        context->PSSetConstantBuffers(0, 1, &b);
    }
    Com<ID3D11ShaderResourceView> gray_texture(unsigned w, unsigned h,
                                               const std::vector<unsigned char>& bytes) {
        D3D11_TEXTURE2D_DESC d{};
        d.Width = w;
        d.Height = h;
        d.MipLevels = d.ArraySize = 1;
        d.Format = DXGI_FORMAT_R8_UNORM;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_IMMUTABLE;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA data{bytes.data(), w, 0};
        Com<ID3D11Texture2D> t;
        check(device->CreateTexture2D(&d, &data, t.put()), "Mask texture");
        Com<ID3D11ShaderResourceView> view;
        check(device->CreateShaderResourceView(t.get(), nullptr, view.put()), "Mask view");
        return view;
    }
    ID3D11ShaderResourceView* mask(const Surface& f) {
        auto& entry = masks[f.id];
        // Compare exact values: decimal string signatures lost small but visible edits.
        if (entry.view && entry.mask == f.mask)
            return entry.view.get();
        constexpr unsigned size = 512;
        auto data = mask_pixels(f.mask, assets, size);
        entry.view = gray_texture(size, size, data);
        entry.mask = f.mask;
        ++counters.mask_uploads;
        return entry.view.get();
    }
    MeshBuffer& mesh(const Surface& f) {
        auto& entry = meshes[f.id];
        if (entry.buffer && entry.mesh == f.mesh)
            return entry;
        auto triangles = triangulate(f, f.mesh.curved ? 4 : 1);
        D3D11_BUFFER_DESC d{};
        d.ByteWidth = UINT(triangles.vertices.size() * sizeof(Vertex));
        d.Usage = D3D11_USAGE_IMMUTABLE;
        d.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        D3D11_SUBRESOURCE_DATA data{triangles.vertices.data(), 0, 0};
        check(device->CreateBuffer(&d, &data, entry.buffer.put()), "Warp mesh buffer");
        entry.mesh = f.mesh;
        entry.count = UINT(triangles.vertices.size());
        ++counters.mesh_uploads;
        return entry;
    }
    void ensure_target(unsigned w, unsigned h) {
        if (w == width && h == height && intermediate)
            return;
        width = w;
        height = h;
        D3D11_TEXTURE2D_DESC d{};
        d.Width = w;
        d.Height = h;
        d.MipLevels = d.ArraySize = 1;
        d.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        d.SampleDesc.Count = 1;
        d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        intermediateRT.reset();
        intermediateSR.reset();
        check(device->CreateTexture2D(&d, nullptr, intermediate.put()), "Linear composition texture");
        check(device->CreateRenderTargetView(intermediate.get(), nullptr, intermediateRT.put()),
              "Composition target");
        check(device->CreateShaderResourceView(intermediate.get(), nullptr, intermediateSR.put()),
              "Composition view");
        ++counters.target_allocations;
    }
};
Renderer::Renderer(ID3D11Device* d) : impl_(std::make_unique<Impl>(d)) {}
Renderer::~Renderer() = default;
RenderCounters Renderer::counters() const {
    return impl_->counters;
}
void Renderer::set_asset_directory(std::filesystem::path path) {
    if (impl_->assets != path) {
        impl_->assets = std::move(path);
        impl_->masks.clear();
    }
}
Com<ID3D11ShaderResourceView> Renderer::texture_view(ID3D11Texture2D* texture) {
    Com<ID3D11ShaderResourceView> v;
    check(impl_->device->CreateShaderResourceView(texture, nullptr, v.put()), "Source texture view");
    return v;
}
Com<ID3D11ShaderResourceView> Renderer::load_image(const std::filesystem::path& path) {
    unsigned w, h;
    auto bytes = load_image_pixels(path, w, h);
    D3D11_TEXTURE2D_DESC d{};
    d.Width = w;
    d.Height = h;
    d.MipLevels = d.ArraySize = 1;
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_IMMUTABLE;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA data{bytes.data(), w * 4, 0};
    Com<ID3D11Texture2D> t;
    check(impl_->device->CreateTexture2D(&d, &data, t.put()), "Image texture");
    return texture_view(t.get());
}
void Renderer::render(const Output& o, const SourceProvider& provider, ID3D11RenderTargetView* target,
                      unsigned w, unsigned h, double time, unsigned identifier) {
    auto& a = *impl_;
    // Compare IDs without allocating each frame; prune only when membership changes.
    bool membership_changed = a.surface_ids.size() != o.surfaces.size();
    if (!membership_changed)
        for (size_t i = 0; i < o.surfaces.size(); ++i)
            if (a.surface_ids[i] != o.surfaces[i].id) {
                membership_changed = true;
                break;
            }
    if (membership_changed) {
        a.surface_ids.clear();
        std::unordered_set<std::string> ids;
        for (const auto& f : o.surfaces) {
            a.surface_ids.push_back(f.id);
            ids.insert(f.id);
        }
        auto removed = [&](const auto& entry) { return !ids.contains(entry.first); };
        std::erase_if(a.meshes, removed);
        std::erase_if(a.masks, removed);
    }
    if (!w || !h)
        return;
    auto* c = a.context.get();
    if (!o.enabled) {
        float black[4] = {0, 0, 0, 1};
        c->ClearRenderTargetView(target, black);
        return;
    }
    a.ensure_target(w, h);
    ID3D11ShaderResourceView* none[3]{};
    c->PSSetShaderResources(0, 3, none);
    auto* rt = a.intermediateRT.get();
    c->OMSetRenderTargets(1, &rt, nullptr);
    float clear[4]{};
    c->ClearRenderTargetView(rt, clear);
    D3D11_VIEWPORT viewport{0, 0, float(w), float(h), 0, 1};
    c->RSSetViewports(1, &viewport);
    c->RSSetState(a.raster.get());
    c->OMSetDepthStencilState(a.depth.get(), 0);
    c->GSSetShader(nullptr, nullptr, 0);
    c->HSSetShader(nullptr, nullptr, 0);
    c->DSSetShader(nullptr, nullptr, 0);
    c->IASetInputLayout(a.layout.get());
    c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    c->VSSetShader(a.warpVS.get(), nullptr, 0);
    c->PSSetShader(a.warpPS.get(), nullptr, 0);
    auto* sampler = a.sampler.get();
    c->PSSetSamplers(0, 1, &sampler);
    c->OMSetBlendState(a.alpha.get(), nullptr, 0xffffffff);
    if (o.enabled)
        for (auto& f : o.surfaces) {
            if (!f.enabled)
                continue;
            auto source = provider(f.source_id);
            auto& mesh = a.mesh(f);
            auto* vb = mesh.buffer.get();
            UINT stride = sizeof(Vertex), offset = 0;
            c->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
            Constants p{};
            p.crop[0] = float(f.source_rect.x);
            p.crop[1] = float(f.source_rect.y);
            p.crop[2] = float(f.source_rect.w);
            p.crop[3] = float(f.source_rect.h);
            p.params[0] = float(f.opacity);
            p.params[1] = float(source.pattern);
            p.params[2] = f.mask.enabled ? 1.f : 0.f;
            p.params[3] = float(time);
            a.set_constants(p);
            ID3D11ShaderResourceView* views[] = {source.view, f.mask.enabled ? a.mask(f) : nullptr, nullptr};
            c->PSSetShaderResources(0, 3, views);
            c->Draw(mesh.count, 0);
        }
    c->PSSetShaderResources(0, 3, none);
    c->OMSetRenderTargets(1, &target, nullptr);
    c->OMSetBlendState(a.opaque.get(), nullptr, 0xffffffff);
    Constants p{};
    p.widths[0] = float(o.blend.left);
    p.widths[1] = float(o.blend.right);
    p.widths[2] = float(o.blend.top);
    p.widths[3] = float(o.blend.bottom);
    p.response[0] = float(o.blend.gamma);
    p.response[1] = float(o.enabled ? o.blend.black_level : 0);
    p.response[2] = float(o.color.gamma);
    for (int i = 0; i < 3; ++i) {
        p.gain[i] = float(o.color.gain[i]);
        p.offset[i] = float(o.color.offset[i]);
    }
    p.adjustments[0] = float(o.enabled ? o.color.brightness : 0);
    p.adjustments[1] = float(o.enabled ? o.color.contrast : 1);
    p.adjustments[2] = float(o.color.saturation);
    p.adjustments[3] = float(identifier);
    a.set_constants(p);
    if (!a.curve || a.curve_points != o.blend.curve) {
        std::vector<unsigned char> values(1024);
        for (unsigned i = 0; i < 1024; ++i)
            values[i] = static_cast<unsigned char>(
                std::clamp(curve_value(o.blend.curve, i / 1023.), 0., 1.) * 255 + .5);
        a.curve = a.gray_texture(1024, 1, values);
        a.curve_points = o.blend.curve;
        ++a.counters.curve_uploads;
    }
    ID3D11ShaderResourceView* views[] = {a.intermediateSR.get(), nullptr, a.curve.get()};
    c->PSSetShaderResources(0, 3, views);
    c->IASetInputLayout(nullptr);
    c->VSSetShader(a.fullVS.get(), nullptr, 0);
    c->PSSetShader(a.outputPS.get(), nullptr, 0);
    c->Draw(3, 0);
    c->PSSetShaderResources(0, 3, none);
}
int pattern_index(const std::string& name) {
    const std::array<const char*, 9> names = {"grid", "white",    "black",   "red",   "green",
                                              "blue", "gradient", "checker", "motion"};
    for (size_t i = 0; i < names.size(); ++i)
        if (name == names[i])
            return int(i);
    return 0;
}
Com<ID3D11Texture2D> render_offscreen(ID3D11Device* device, Renderer& renderer, const Output& output,
                                      const SourceProvider& source, unsigned w, unsigned h) {
    D3D11_TEXTURE2D_DESC d{};
    d.Width = w;
    d.Height = h;
    d.MipLevels = d.ArraySize = 1;
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    Com<ID3D11Texture2D> t;
    check(device->CreateTexture2D(&d, nullptr, t.put()), "Offscreen texture");
    Com<ID3D11RenderTargetView> rt;
    check(device->CreateRenderTargetView(t.get(), nullptr, rt.put()), "Offscreen view");
    renderer.render(output, source, rt.get(), w, h);
    return t;
}
std::vector<std::uint8_t> read_pixels(ID3D11Device* device, ID3D11Texture2D* texture) {
    D3D11_TEXTURE2D_DESC d{};
    texture->GetDesc(&d);
    if (d.Format != DXGI_FORMAT_R8G8B8A8_UNORM)
        throw std::runtime_error("Readback expects RGBA8");
    d.Usage = D3D11_USAGE_STAGING;
    d.BindFlags = 0;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    d.MiscFlags = 0;
    Com<ID3D11Texture2D> copy;
    check(device->CreateTexture2D(&d, nullptr, copy.put()), "Readback texture");
    Com<ID3D11DeviceContext> c;
    device->GetImmediateContext(c.put());
    c->CopyResource(copy.get(), texture);
    D3D11_MAPPED_SUBRESOURCE m{};
    check(c->Map(copy.get(), 0, D3D11_MAP_READ, 0, &m), "Readback mapping");
    std::vector<std::uint8_t> data(size_t(d.Width) * d.Height * 4);
    for (unsigned y = 0; y < d.Height; ++y)
        std::memcpy(data.data() + size_t(y) * d.Width * 4,
                    static_cast<const char*>(m.pData) + size_t(y) * m.RowPitch, size_t(d.Width) * 4);
    c->Unmap(copy.get(), 0);
    return data;
}
} // namespace ps::win
