// SPDX-License-Identifier: GPL-3.0-only
#include "capture_abi.hpp"
#include "ps/windows.hpp"
#include <atomic>
#include <chrono>
#include <d3d11_4.h>
#include <roapi.h>
namespace ps::win {
namespace {
using namespace capture_abi;
template <class T> Com<T> factory(const wchar_t* name, REFIID iid) {
    HSTRING text{};
    check(WindowsCreateString(name, UINT32(wcslen(name)), &text), "Windows Runtime class name");
    Com<T> result;
    auto hr = RoGetActivationFactory(text, iid, reinterpret_cast<void**>(result.put()));
    WindowsDeleteString(text);
    check(hr, "Windows Graphics Capture requires Windows 10 1903 or newer");
    return result;
}
void close(IUnknown* object) {
    if (!object)
        return;
    Com<Closable> c;
    if (SUCCEEDED(object->QueryInterface(close_id, reinterpret_cast<void**>(c.put()))))
        c->Close();
}
// An agile delegate activates frame delivery; frames are consumed by the render thread.
struct Arrived : IUnknown {
    std::atomic<ULONG> references{1};
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** value) override {
        static constexpr GUID handler{
            0x51a947f7, 0x79cf, 0x5a3e, {0xa3, 0xa5, 0x12, 0x89, 0xcf, 0xa6, 0xdf, 0xe8}};
        static constexpr GUID agile{
            0x94ea2b94, 0xe9cc, 0x49e0, {0xc0, 0xff, 0xee, 0x64, 0xca, 0x8f, 0x5b, 0x90}};
        if (!value)
            return E_POINTER;
        *value = nullptr;
        if (iid == IID_IUnknown || iid == handler || iid == agile) {
            *value = this;
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {
        return ++references;
    }
    ULONG STDMETHODCALLTYPE Release() override {
        auto n = --references;
        if (!n)
            delete this;
        return n;
    }
    virtual HRESULT STDMETHODCALLTYPE Invoke(IInspectable*, IInspectable*) {
        return S_OK;
    }
};
} // namespace
struct WindowCapture::Impl {
    std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
    HWND window{};
    DWORD window_pid{};
    Com<ID3D11Device> device;
    Com<ID3D11DeviceContext> context;
    Com<IInspectable> runtime_device;
    Com<capture_abi::Item> item;
    Com<capture_abi::Pool> pool;
    Com<capture_abi::Session> session;
    Com<ID3D11Texture2D> texture;
    Com<ID3D11ShaderResourceView> view;
    capture_abi::Size size{};
    INT64 timestamp{}, arrived_token{};
    Com<Arrived> arrived;
    ~Impl() {
        close(session.get());
        if (pool && arrived)
            pool->RemoveArrived(arrived_token);
        close(pool.get());
    }
};
WindowCapture::WindowCapture(ID3D11Device* device, HWND window, bool cursor)
    : impl_(std::make_unique<Impl>()) {
    auto& p = *impl_;
    p.window = window;
    p.device = Com<ID3D11Device>(device);
    device->GetImmediateContext(p.context.put());
    DWORD pid{};
    GetWindowThreadProcessId(window, &pid);
    p.window_pid = pid;
    if (!IsWindow(window) || pid == GetCurrentProcessId())
        throw std::runtime_error("Select an existing window outside the rendering process");
    wchar_t class_name[256]{};
    GetClassNameW(window, class_name, 256);
    if (std::wstring(class_name) == L"ProjectionStudioOutput")
        throw std::runtime_error("Recursive capture of a projector output is not allowed");
    Com<ID3D11Multithread> protection;
    if (SUCCEEDED(p.context->QueryInterface(__uuidof(ID3D11Multithread),
                                            reinterpret_cast<void**>(protection.put()))))
        protection->SetMultithreadProtected(TRUE);
    auto item_factory = factory<capture_abi::ItemInterop>(L"Windows.Graphics.Capture.GraphicsCaptureItem",
                                                          capture_abi::interop_id);
    check(item_factory->CreateForWindow(window, capture_abi::item_id, reinterpret_cast<void**>(p.item.put())),
          "Create window capture item");
    check(p.item->GetSize(&p.size), "Capture size");
    Com<IDXGIDevice> dxgi;
    check(device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(dxgi.put())),
          "Capture DXGI device");
    using Create = HRESULT(WINAPI*)(IDXGIDevice*, IInspectable**);
    auto create = reinterpret_cast<Create>(
        GetProcAddress(GetModuleHandleW(L"d3d11.dll"), "CreateDirect3D11DeviceFromDXGIDevice"));
    if (!create)
        throw std::runtime_error("Windows Graphics Capture device interop unavailable");
    Com<IInspectable> inspectable;
    check(create(dxgi.get(), inspectable.put()), "Create capture device");
    check(
        inspectable->QueryInterface(capture_abi::device_id, reinterpret_cast<void**>(p.runtime_device.put())),
        "Capture device interface");
    auto statics = factory<capture_abi::PoolStatics>(L"Windows.Graphics.Capture.Direct3D11CaptureFramePool",
                                                     capture_abi::pool_statics_id);
    check(statics->CreateFreeThreaded(p.runtime_device.get(), 87, 2, p.size, p.pool.put()),
          "Create free-threaded capture pool");
    *p.arrived.put() = new Arrived;
    check(p.pool->AddArrived(p.arrived.get(), &p.arrived_token), "Subscribe to capture frames");
    check(p.pool->CreateSession(p.item.get(), p.session.put()), "Create window capture session");
    Com<capture_abi::Cursor> cursor_settings;
    if (SUCCEEDED(p.session->QueryInterface(capture_abi::cursor_id,
                                            reinterpret_cast<void**>(cursor_settings.put()))))
        cursor_settings->SetEnabled(cursor);
    check(p.session->StartCapture(), "Start window capture");
}
WindowCapture::~WindowCapture() = default;
bool WindowCapture::update() {
    auto& p = *impl_;
    DWORD pid{};
    GetWindowThreadProcessId(p.window, &pid);
    if (!IsWindow(p.window) || pid != p.window_pid)
        throw std::runtime_error("Captured window has closed; select its replacement");
    if (IsIconic(p.window) || !IsWindowVisible(p.window))
        throw std::runtime_error("Source window is minimized or hidden; waiting for restoration");
    Com<capture_abi::Frame> frame;
    check(p.pool->Next(frame.put()), "Read captured window frame");
    if (!frame && !p.view && std::chrono::steady_clock::now() - p.started > std::chrono::seconds(3))
        throw std::runtime_error(
            "No capture frames: make the source window visible on an unlocked interactive desktop");
    if (!frame)
        return false;
    struct ReleaseFrame {
        capture_abi::Frame* f;
        ~ReleaseFrame() {
            close(f);
        }
    } release{frame.get()};
    capture_abi::Size size{};
    check(frame->ContentSize(&size), "Capture frame size");
    frame->Time(&p.timestamp);
    if (size.width <= 0 || size.height <= 0)
        return false;
    Com<IInspectable> surface;
    check(frame->Surface(surface.put()), "Capture surface");
    Com<capture_abi::Access> access;
    check(surface->QueryInterface(capture_abi::access_id, reinterpret_cast<void**>(access.put())),
          "Capture texture access");
    Com<ID3D11Texture2D> source;
    check(access->GetInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(source.put())),
          "Capture texture");
    D3D11_TEXTURE2D_DESC d{};
    source->GetDesc(&d);
    // Discard the transitional frame and recreate the pool before reading a resized window.
    if (size.width != p.size.width || size.height != p.size.height) {
        close(frame.get());
        release.f = nullptr;
        frame.reset();
        p.size = size;
        check(p.pool->Recreate(p.runtime_device.get(), 87, 2, size), "Resize capture pool");
        p.view.reset();
        p.texture.reset();
        return false;
    }
    if (!p.texture) {
        d.Width = UINT(size.width);
        d.Height = UINT(size.height);
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        d.MiscFlags = 0;
        d.CPUAccessFlags = 0;
        d.Usage = D3D11_USAGE_DEFAULT;
        check(p.device->CreateTexture2D(&d, nullptr, p.texture.put()), "Capture frame copy");
        check(p.device->CreateShaderResourceView(p.texture.get(), nullptr, p.view.put()),
              "Capture source view");
    }
    D3D11_BOX box{0, 0, 0, std::min(UINT(size.width), d.Width), std::min(UINT(size.height), d.Height), 1};
    p.context->CopySubresourceRegion(p.texture.get(), 0, 0, 0, 0, source.get(), 0, &box);
    return true;
}
ID3D11ShaderResourceView* WindowCapture::view() const {
    return impl_->view.get();
}
std::int64_t WindowCapture::timestamp() const {
    return impl_->timestamp;
}
} // namespace ps::win
