// SPDX-License-Identifier: GPL-3.0-only
#include "ps/windows.hpp"
#include "ps/geometry.hpp"
#include <wincodec.h>
#include <algorithm>
namespace ps::win {
std::vector<unsigned char> load_image_pixels(const std::filesystem::path& path, unsigned& w, unsigned& h) {
    Com<IWICImagingFactory> factory;
    check(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                           __uuidof(IWICImagingFactory), reinterpret_cast<void**>(factory.put())),
          "Image decoder");
    Com<IWICBitmapDecoder> decoder;
    check(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                             WICDecodeMetadataCacheOnLoad, decoder.put()),
          "Read image");
    Com<IWICBitmapFrameDecode> frame;
    check(decoder->GetFrame(0, frame.put()), "Read image frame");
    check(frame->GetSize(&w, &h), "Image dimensions");
    if (w > 16384 || h > 16384 || !w || !h)
        throw std::runtime_error("Image dimensions unsupported");
    Com<IWICFormatConverter> convert;
    check(factory->CreateFormatConverter(convert.put()), "Image converter");
    check(convert->Initialize(frame.get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0,
                              WICBitmapPaletteTypeCustom),
          "Convert image");
    std::vector<unsigned char> data(size_t(w) * h * 4);
    check(convert->CopyPixels(nullptr, w * 4, UINT(data.size()), data.data()), "Decode image pixels");
    return data;
}
std::vector<unsigned char> mask_pixels(const Mask& mask, const std::filesystem::path& assets, unsigned size) {
    std::vector<unsigned char> data(size * size);
    unsigned bw = 0, bh = 0;
    std::vector<unsigned char> bitmap;
    if (!mask.bitmap.empty())
        bitmap = load_image_pixels(resolve_asset(assets / "project.json", mask.bitmap), bw, bh);
    Mask polygon = mask;
    polygon.inverted = false;
    for (unsigned y = 0; y < size; ++y)
        for (unsigned x = 0; x < size; ++x) {
            double a = mask_alpha(polygon, {(x + .5) / size, (y + .5) / size});
            if (!bitmap.empty()) {
                auto bx = std::min(bw - 1, x * bw / size), by = std::min(bh - 1, y * bh / size);
                auto k = (size_t(by) * bw + bx) * 4;
                a *= (.2126 * bitmap[k] + .7152 * bitmap[k + 1] + .0722 * bitmap[k + 2]) / 255. *
                     bitmap[k + 3] / 255.;
            }
            if (mask.inverted)
                a = 1 - a;
            data[y * size + x] = static_cast<unsigned char>(std::clamp(a, 0., 1.) * 255 + .5);
        }
    if (!bitmap.empty() && mask.feather > 0) {
        int radius = std::clamp(int(mask.feather * size), 1, int(size / 2));
        std::vector<unsigned char> temporary(data.size());
        for (int pass = 0; pass < 2; ++pass) {
            for (unsigned line = 0; line < size; ++line) {
                auto pixel = [&](int at) -> unsigned char {
                    auto i = unsigned(std::clamp(at, 0, int(size) - 1));
                    return data[pass ? size_t(i) * size + line : size_t(line) * size + i];
                };
                unsigned sum = 0, count = unsigned(2 * radius + 1);
                for (int k = -radius; k <= radius; ++k)
                    sum += pixel(k);
                for (unsigned i = 0; i < size; ++i) {
                    temporary[pass ? size_t(i) * size + line : size_t(line) * size + i] =
                        static_cast<unsigned char>(sum / count);
                    sum -= pixel(int(i) - radius);
                    sum += pixel(int(i) + radius + 1);
                }
            }
            data.swap(temporary);
        }
    }
    return data;
}
} // namespace ps::win
