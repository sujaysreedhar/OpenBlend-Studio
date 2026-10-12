// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "model.hpp"
#include <d3d11.h>
#include <dxgi1_2.h>
#include <functional>
#include <memory>
#include <unordered_map>
#include <windows.h>
namespace ps::win {
// MinGW filesystem timestamps can have whole-second resolution. Configuration
// edits within the same second must still reach an application adapter.
inline std::uint64_t file_write_stamp(const std::filesystem::path& path) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data))
        throw std::runtime_error("Cannot inspect configuration: " + std::to_string(GetLastError()));
    return (std::uint64_t(data.ftLastWriteTime.dwHighDateTime) << 32) | data.ftLastWriteTime.dwLowDateTime;
}
template <class T> class Com {
    T* p_{};

  public:
    Com() = default;
    explicit Com(T* p) : p_(p) {
        if (p_)
            p_->AddRef();
    }
    ~Com() {
        reset();
    }
    Com(const Com& x) : Com(x.p_) {}
    Com(Com&& x) noexcept : p_(x.p_) {
        x.p_ = nullptr;
    }
    Com& operator=(const Com& x) {
        if (this != &x) {
            reset();
            p_ = x.p_;
            if (p_)
                p_->AddRef();
        }
        return *this;
    }
    Com& operator=(Com&& x) noexcept {
        if (this != &x) {
            reset();
            p_ = x.p_;
            x.p_ = nullptr;
        }
        return *this;
    }
    void reset() {
        if (p_)
            p_->Release();
        p_ = nullptr;
    }
    T* get() const {
        return p_;
    }
    T** put() {
        reset();
        return &p_;
    }
    T* operator->() const {
        return p_;
    }
    explicit operator bool() const {
        return p_ != nullptr;
    }
};
void check(HRESULT, const char*);
std::string utf8(const std::wstring&);
std::wstring wide(const std::string&);
struct Display {
    std::string id, name;
    RECT bounds{};
    int rotation{};
    LUID adapter{};
    unsigned index{};
};
struct WindowInfo {
    std::string title, executable;
    HWND hwnd{};
    DWORD pid{};
    std::string window_class;
};
std::vector<Display> displays();
std::vector<WindowInfo> windows();
HWND resolve_window(const Source&);
Com<ID3D11Device> create_device(bool software = false, const LUID* adapter = nullptr);
class CaptureTransform;
class DesktopCapture {
    Com<ID3D11Device> device_;
    Com<ID3D11DeviceContext> context_;
    Com<IDXGIOutputDuplication> duplication_;
    Com<ID3D11Texture2D> texture_;
    Com<ID3D11ShaderResourceView> view_;
    Com<ID3D11ShaderResourceView> raw_view_;
    std::unique_ptr<CaptureTransform> transform_;
    DXGI_MODE_ROTATION rotation_{DXGI_MODE_ROTATION_IDENTITY};
    DXGI_OUTDUPL_POINTER_POSITION pointer_{};
    bool cursor_{true};
    std::string display_;
    unsigned width_{}, height_{};
    std::int64_t timestamp_{};

  public:
    DesktopCapture(ID3D11Device*, const std::string& display, bool cursor = true);
    ~DesktopCapture();
    bool update();
    std::int64_t timestamp() const {
        return timestamp_;
    }
    ID3D11ShaderResourceView* view() const {
        return view_.get();
    }
    unsigned width() const {
        return width_;
    }
    unsigned height() const {
        return height_;
    }
};
struct SourceTexture {
    ID3D11ShaderResourceView* view{};
    int pattern{};
};
class WindowCapture {
    struct Impl;
    std::unique_ptr<Impl> impl_;

  public:
    WindowCapture(ID3D11Device*, HWND, bool cursor = true);
    ~WindowCapture();
    bool update();
    ID3D11ShaderResourceView* view() const;
    std::int64_t timestamp() const;
};
using SourceProvider = std::function<SourceTexture(const std::string&)>;
struct RenderCounters {
    std::uint64_t mesh_uploads{}, mask_uploads{}, curve_uploads{}, target_allocations{};
};
class Renderer {
    struct Impl;
    std::unique_ptr<Impl> impl_;

  public:
    explicit Renderer(ID3D11Device*);
    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;
    void render(const Output&, const SourceProvider&, ID3D11RenderTargetView*, unsigned width,
                unsigned height, double seconds = 0, unsigned identifier = 0);
    void set_asset_directory(std::filesystem::path);
    RenderCounters counters() const;
    Com<ID3D11ShaderResourceView> load_image(const std::filesystem::path&);
    Com<ID3D11ShaderResourceView> texture_view(ID3D11Texture2D*);
};
int pattern_index(const std::string&);
std::vector<unsigned char> load_image_pixels(const std::filesystem::path&, unsigned& width, unsigned& height);
std::vector<unsigned char> mask_pixels(const Mask&, const std::filesystem::path& assets, unsigned size = 512);
Com<ID3D11Texture2D> render_offscreen(ID3D11Device*, Renderer&, const Output&, const SourceProvider&,
                                      unsigned w, unsigned h);
std::vector<std::uint8_t> read_pixels(ID3D11Device*, ID3D11Texture2D*);
} // namespace ps::win
#endif
