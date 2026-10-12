// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <inspectable.h>
#include <winstring.h>
// Minimal public Windows Runtime ABI declarations, kept private to this backend.
// Signatures and IIDs: Windows SDK metadata / microsoft/windows-rs Graphics/Capture.
// This avoids requiring a particular C++/WinRT projection or SDK header revision.
namespace ps::win::capture_abi {
struct Size {
    INT32 width, height;
};
inline constexpr GUID item_id{0x79c3f95b, 0x31f7, 0x4ec2, {0xa4, 0x64, 0x63, 0x2e, 0xf5, 0xd3, 0x07, 0x60}};
inline constexpr GUID interop_id{
    0x3628e81b, 0x3cac, 0x4c60, {0xb7, 0xf4, 0x23, 0xce, 0x0e, 0x0c, 0x33, 0x56}};
inline constexpr GUID pool_statics_id{
    0x589b103f, 0x6bbc, 0x5df5, {0xa9, 0x91, 0x02, 0xe2, 0x8b, 0x3b, 0x66, 0xd5}};
inline constexpr GUID device_id{0xa37624ab, 0x8d5f, 0x4650, {0x9d, 0x3e, 0x9e, 0xae, 0x3d, 0x9b, 0xc6, 0x70}};
inline constexpr GUID access_id{0xa9b3d012, 0x3df2, 0x4ee3, {0xb8, 0xd1, 0x86, 0x95, 0xf4, 0x57, 0xd3, 0xc1}};
inline constexpr GUID close_id{0x30d5a829, 0x7fa4, 0x4026, {0x83, 0xbb, 0xd7, 0x5b, 0xae, 0x4e, 0xa9, 0x9e}};
inline constexpr GUID cursor_id{0x2c39ae40, 0x7d2e, 0x5044, {0x80, 0x4e, 0x8b, 0x67, 0x99, 0xd4, 0xcf, 0x9e}};
struct Closable : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE Close() = 0;
};
struct Item : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE DisplayName(HSTRING*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetSize(Size*) = 0;
    virtual HRESULT STDMETHODCALLTYPE AddClosed(IUnknown*, INT64*) = 0;
    virtual HRESULT STDMETHODCALLTYPE RemoveClosed(INT64) = 0;
};
struct ItemInterop : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE CreateForWindow(HWND, REFIID, void**) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateForMonitor(HMONITOR, REFIID, void**) = 0;
};
struct Access : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetInterface(REFIID, void**) = 0;
};
struct Session : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE StartCapture() = 0;
};
struct Cursor : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE GetEnabled(boolean*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetEnabled(boolean) = 0;
};
struct Frame : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE Surface(IInspectable**) = 0;
    virtual HRESULT STDMETHODCALLTYPE Time(INT64*) = 0;
    virtual HRESULT STDMETHODCALLTYPE ContentSize(Size*) = 0;
};
struct Pool : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE Recreate(IInspectable*, INT32, INT32, Size) = 0;
    virtual HRESULT STDMETHODCALLTYPE Next(Frame**) = 0;
    virtual HRESULT STDMETHODCALLTYPE AddArrived(IUnknown*, INT64*) = 0;
    virtual HRESULT STDMETHODCALLTYPE RemoveArrived(INT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateSession(Item*, Session**) = 0;
    virtual HRESULT STDMETHODCALLTYPE Dispatcher(IInspectable**) = 0;
};
struct PoolStatics : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE CreateFreeThreaded(IInspectable*, INT32, INT32, Size, Pool**) = 0;
};
} // namespace ps::win::capture_abi
