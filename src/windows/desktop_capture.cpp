// SPDX-License-Identifier: GPL-3.0-only
#include "ps/windows.hpp"
#include "ps/gpu_timing.hpp"
#include "ps/capture_transform.hpp"
namespace ps::win {
DesktopCapture::DesktopCapture(ID3D11Device* device, const std::string& name, bool cursor)
    : device_(device), display_(name), cursor_(cursor) {
    device->GetImmediateContext(context_.put());
    Com<IDXGIDevice> dxgi;
    check(device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(dxgi.put())),
          "Get capture adapter");
    Com<IDXGIAdapter> adapter;
    check(dxgi->GetAdapter(adapter.put()), "Capture adapter");
    for (unsigned i = 0;; ++i) {
        Com<IDXGIOutput> output;
        if (adapter->EnumOutputs(i, output.put()) == DXGI_ERROR_NOT_FOUND)
            break;
        DXGI_OUTPUT_DESC desc{};
        output->GetDesc(&desc);
        if (utf8(desc.DeviceName) != name)
            continue;
        rotation_ = desc.Rotation;
        transform_ = std::make_unique<CaptureTransform>(device);
        Com<IDXGIOutput1> o;
        check(output->QueryInterface(__uuidof(IDXGIOutput1), reinterpret_cast<void**>(o.put())),
              "Desktop duplication interface");
        check(o->DuplicateOutput(device, duplication_.put()), "Start desktop duplication");
        return;
    }
    throw std::runtime_error("Capture display is not on the render GPU");
}
DesktopCapture::~DesktopCapture() = default;
bool DesktopCapture::update() {
    DXGI_OUTDUPL_FRAME_INFO info{};
    Com<IDXGIResource> resource;
    HRESULT hr = duplication_->AcquireNextFrame(0, &info, resource.put());
    if (hr == DXGI_ERROR_WAIT_TIMEOUT)
        return false;
    check(hr, "Desktop capture interrupted");
    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    if (info.LastPresentTime.QuadPart)
        timestamp_ = std::int64_t(double(info.LastPresentTime.QuadPart) * 10000000. / frequency.QuadPart);
    struct Release {
        IDXGIOutputDuplication* p;
        ~Release() {
            p->ReleaseFrame();
        }
    } release{duplication_.get()};
    if (info.LastMouseUpdateTime.QuadPart)
        pointer_ = info.PointerPosition;
    if (info.PointerShapeBufferSize && cursor_) {
        std::vector<unsigned char> bytes(info.PointerShapeBufferSize);
        DXGI_OUTDUPL_POINTER_SHAPE_INFO shape{};
        UINT required{};
        check(duplication_->GetFramePointerShape(UINT(bytes.size()), bytes.data(), &required, &shape),
              "Desktop pointer shape");
        transform_->pointer(shape, bytes);
    }
    Com<ID3D11Texture2D> frame;
    check(resource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(frame.put())),
          "Capture texture");
    D3D11_TEXTURE2D_DESC d{};
    frame->GetDesc(&d);
    if (!texture_ || d.Width != width_ || d.Height != height_) {
        width_ = d.Width;
        height_ = d.Height;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        d.MiscFlags = 0;
        d.CPUAccessFlags = 0;
        d.Usage = D3D11_USAGE_DEFAULT;
        check(device_->CreateTexture2D(&d, nullptr, texture_.put()), "Capture copy texture");
        check(device_->CreateShaderResourceView(texture_.get(), nullptr, raw_view_.put()),
              "Capture texture view");
    }
    context_->CopyResource(texture_.get(), frame.get());
    view_ = Com<ID3D11ShaderResourceView>(transform_->render(raw_view_.get(), width_, height_, rotation_,
                                                             pointer_.Position, cursor_ && pointer_.Visible));
    return true;
}
} // namespace ps::win
