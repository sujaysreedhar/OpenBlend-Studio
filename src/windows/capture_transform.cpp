// SPDX-License-Identifier: GPL-3.0-only
#include "ps/capture_transform.hpp"
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <cstring>
namespace ps::win {
std::vector<unsigned char> pointer_pixels(const DXGI_OUTDUPL_POINTER_SHAPE_INFO& info,
                                          std::span<const unsigned char> bytes) {
    bool mono = info.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME;
    if (info.Type != DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR &&
        info.Type != DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR && !mono)
        throw std::runtime_error("Unknown pointer shape type");
    if (!info.Width || !info.Height || info.Width > 4096 || info.Height > 8192 || (mono && info.Height % 2) ||
        info.Pitch < (mono ? (info.Width + 7) / 8 : info.Width * 4) ||
        bytes.size() < size_t(info.Pitch) * info.Height)
        throw std::runtime_error("Invalid pointer shape buffer");
    unsigned height = mono ? info.Height / 2 : info.Height;
    std::vector<unsigned char> result(size_t(info.Width) * height * 4);
    for (unsigned y = 0; y < height; ++y)
        for (unsigned x = 0; x < info.Width; ++x) {
            auto k = (size_t(y) * info.Width + x) * 4;
            if (mono) {
                unsigned bit = 0x80 >> (x % 8);
                result[k] = (bytes[size_t(y) * info.Pitch + x / 8] & bit) ? 255 : 0;
                result[k + 1] = (bytes[size_t(y + height) * info.Pitch + x / 8] & bit) ? 255 : 0;
                result[k + 2] = 0;
                result[k + 3] = 255;
            } else {
                auto at = size_t(y) * info.Pitch + x * 4;
                result[k] = bytes[at + 2];
                result[k + 1] = bytes[at + 1];
                result[k + 2] = bytes[at];
                result[k + 3] = bytes[at + 3];
            }
        }
    return result;
}
namespace {
const char code[] = R"(
cbuffer Settings:register(b0) { uint4 dimensions; int4 pointerInfo; uint4 shape; };
Texture2D desktop:register(t0); Texture2D pointerImage:register(t1);
float4 vs(uint id:SV_VertexID):SV_POSITION { float2 uv=float2((id<<1)&2,id&2); return float4(uv.x*2-1,1-uv.y*2,0,1); }
float4 ps(float4 position:SV_POSITION):SV_TARGET {
 int2 p=int2(position.xy), q=p;
 if(dimensions.z==2) q=int2(p.y,dimensions.y-1-p.x);
 if(dimensions.z==3) q=int2(dimensions.x-1-p.x,dimensions.y-1-p.y);
 if(dimensions.z==4) q=int2(dimensions.x-1-p.y,p.x);
 float4 base=desktop.Load(int3(q,0));
 int2 cursor=p-pointerInfo.xy;
 if(pointerInfo.z && all(cursor>=0) && cursor.x<(int)shape.x && cursor.y<(int)shape.y) {
   float4 c=pointerImage.Load(int3(cursor,0)); uint3 b=(uint3)round(saturate(base.rgb)*255);
   if(shape.z==1) base.rgb=((b & (c.r>.5?255:0)) ^ (c.g>.5?255:0))/255.;
   else if(shape.z==4) base.rgb=c.a>.5?(b ^ (uint3)round(c.rgb*255))/255.:c.rgb;
   else base.rgb=lerp(base.rgb,c.rgb,c.a);
 }
 return float4(base.rgb,1);
})";
struct Settings {
    unsigned dimensions[4];
    int pointer[4];
    unsigned shape[4];
};
} // namespace
struct CaptureTransform::Impl {
    Com<ID3D11Device> device;
    Com<ID3D11DeviceContext1> context;
    Com<ID3DDeviceContextState> state;
    Com<ID3D11VertexShader> vs;
    Com<ID3D11PixelShader> ps;
    Com<ID3D11Buffer> constants;
    Com<ID3D11Texture2D> texture;
    Com<ID3D11RenderTargetView> rt;
    Com<ID3D11ShaderResourceView> view, cursor;
    unsigned width{}, height{};
    DXGI_OUTDUPL_POINTER_SHAPE_INFO pointer{};
    explicit Impl(ID3D11Device* d) : device(d) {
        Com<ID3D11Device1> device1;
        check(d->QueryInterface(__uuidof(ID3D11Device1), reinterpret_cast<void**>(device1.put())),
              "Capture state isolation");
        Com<ID3D11DeviceContext> c;
        d->GetImmediateContext(c.put());
        check(c->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(context.put())),
              "Capture context1");
        auto level = d->GetFeatureLevel();
        check(device1->CreateDeviceContextState(0, &level, 1, D3D11_SDK_VERSION, __uuidof(ID3D11Device),
                                                nullptr, state.put()),
              "Capture isolated state");
        auto compile = [&](const char* entry, const char* profile) {
            Com<ID3DBlob> blob, errors;
            auto hr = D3DCompile(code, sizeof(code) - 1, nullptr, nullptr, nullptr, entry, profile,
                                 D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, blob.put(), errors.put());
            if (FAILED(hr))
                throw std::runtime_error(errors ? static_cast<char*>(errors->GetBufferPointer())
                                                : "Capture shader compile failed");
            return blob;
        };
        auto v = compile("vs", "vs_5_0"), p = compile("ps", "ps_5_0");
        check(d->CreateVertexShader(v->GetBufferPointer(), v->GetBufferSize(), nullptr, vs.put()),
              "Capture vertex shader");
        check(d->CreatePixelShader(p->GetBufferPointer(), p->GetBufferSize(), nullptr, ps.put()),
              "Capture pixel shader");
        D3D11_BUFFER_DESC b{};
        b.ByteWidth = sizeof(Settings);
        b.Usage = D3D11_USAGE_DEFAULT;
        b.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        check(d->CreateBuffer(&b, nullptr, constants.put()), "Capture constants");
    }
};
CaptureTransform::CaptureTransform(ID3D11Device* d) : impl_(std::make_unique<Impl>(d)) {}
CaptureTransform::~CaptureTransform() = default;
void CaptureTransform::pointer(const DXGI_OUTDUPL_POINTER_SHAPE_INFO& info,
                               std::span<const unsigned char> bytes) {
    auto pixels = pointer_pixels(info, bytes);
    auto& a = *impl_;
    D3D11_TEXTURE2D_DESC d{};
    d.Width = info.Width;
    d.Height = info.Type == 1 ? info.Height / 2 : info.Height;
    d.MipLevels = d.ArraySize = d.SampleDesc.Count = 1;
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    d.Usage = D3D11_USAGE_IMMUTABLE;
    D3D11_SUBRESOURCE_DATA initial{pixels.data(), info.Width * 4, 0};
    Com<ID3D11Texture2D> image;
    check(a.device->CreateTexture2D(&d, &initial, image.put()), "Capture pointer texture");
    check(a.device->CreateShaderResourceView(image.get(), nullptr, a.cursor.put()), "Capture pointer view");
    a.pointer = info;
}
ID3D11ShaderResourceView* CaptureTransform::render(ID3D11ShaderResourceView* source, unsigned w, unsigned h,
                                                   DXGI_MODE_ROTATION rotation, POINT position,
                                                   bool visible) {
    auto& a = *impl_;
    unsigned width = w, height = h;
    if (rotation == DXGI_MODE_ROTATION_ROTATE90 || rotation == DXGI_MODE_ROTATION_ROTATE270)
        std::swap(width, height);
    Com<ID3DDeviceContextState> previous;
    a.context->SwapDeviceContextState(a.state.get(), previous.put());
    struct Restore {
        ID3D11DeviceContext1* context;
        ID3DDeviceContextState* previous;
        ~Restore() {
            context->SwapDeviceContextState(previous, nullptr);
        }
    } restore{a.context.get(), previous.get()};
    if (!a.texture || width != a.width || height != a.height) {
        a.context->OMSetRenderTargets(0, nullptr, nullptr);
        a.rt.reset();
        a.view.reset();
        D3D11_TEXTURE2D_DESC d{};
        d.Width = width;
        d.Height = height;
        d.MipLevels = d.ArraySize = d.SampleDesc.Count = 1;
        d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        check(a.device->CreateTexture2D(&d, nullptr, a.texture.put()), "Normalized capture texture");
        check(a.device->CreateRenderTargetView(a.texture.get(), nullptr, a.rt.put()),
              "Normalized capture target");
        check(a.device->CreateShaderResourceView(a.texture.get(), nullptr, a.view.put()),
              "Normalized capture view");
        a.width = width;
        a.height = height;
    }
    auto* rt = a.rt.get();
    a.context->OMSetRenderTargets(1, &rt, nullptr);
    D3D11_VIEWPORT viewport{0, 0, float(width), float(height), 0, 1};
    a.context->RSSetViewports(1, &viewport);
    a.context->IASetInputLayout(nullptr);
    a.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    a.context->VSSetShader(a.vs.get(), nullptr, 0);
    a.context->PSSetShader(a.ps.get(), nullptr, 0);
    Settings settings{
        {w, h, unsigned(rotation), 0},
        {position.x, position.y, visible && a.cursor ? 1 : 0, 0},
        {a.pointer.Width, a.pointer.Type == 1 ? a.pointer.Height / 2 : a.pointer.Height, a.pointer.Type, 0}};
    a.context->UpdateSubresource(a.constants.get(), 0, nullptr, &settings, 0, 0);
    auto* constants = a.constants.get();
    a.context->PSSetConstantBuffers(0, 1, &constants);
    ID3D11ShaderResourceView* views[] = {source, a.cursor.get()};
    a.context->PSSetShaderResources(0, 2, views);
    a.context->Draw(3, 0);
    ID3D11ShaderResourceView* none[2]{};
    a.context->PSSetShaderResources(0, 2, none);
    a.context->OMSetRenderTargets(0, nullptr, nullptr);
    return a.view.get();
}
ID3D11Texture2D* CaptureTransform::texture() const {
    return impl_->texture.get();
}
} // namespace ps::win
