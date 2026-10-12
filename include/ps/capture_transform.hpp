// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "windows.hpp"
#include <span>
namespace ps::win {
std::vector<unsigned char> pointer_pixels(const DXGI_OUTDUPL_POINTER_SHAPE_INFO&,
                                          std::span<const unsigned char>);
class CaptureTransform {
    struct Impl;
    std::unique_ptr<Impl> impl_;

  public:
    explicit CaptureTransform(ID3D11Device*);
    ~CaptureTransform();
    void pointer(const DXGI_OUTDUPL_POINTER_SHAPE_INFO&, std::span<const unsigned char>);
    ID3D11ShaderResourceView* render(ID3D11ShaderResourceView*, unsigned width, unsigned height,
                                     DXGI_MODE_ROTATION, POINT position, bool visible);
    ID3D11Texture2D* texture() const;
};
} // namespace ps::win
