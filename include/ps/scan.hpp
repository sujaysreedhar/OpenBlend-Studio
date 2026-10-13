// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "calibration.hpp"
namespace ps {
struct RgbFrame {
    int width{}, height{};
    std::vector<std::uint8_t> pixels; // RGB, interleaved
};
struct ScanOptions {
    int camera_index{}, camera_width{1280}, camera_height{720}, settle_ms{500}, flush_frames{4};
    int min_contrast{20};
    double exposure{-6}, max_rms{2};
    Rect region{0, 0, 1, 1}; // common screen rectangle in normalized camera coordinates
    std::string intrinsics;
};
int gray_pattern_count(int width, int height);
GrayFrame gray_pattern(int width, int height, int index);
Json scan_manifest(const Project&, const std::vector<std::string>& output_ids, const ScanOptions& = {});
void create_scan(const std::filesystem::path&, const Json& manifest);
Json analyze_scan(const std::filesystem::path&); // completed captures only; writes masks and result.json
Project apply_scan(const Project&, const Json& result, bool blends, bool color);
void write_ppm(const RgbFrame&, const std::filesystem::path&);
RgbFrame read_ppm(const std::filesystem::path&);
#ifdef PS_WITH_OPENCV
GrayFrame undistort_capture(const GrayFrame&, const Json& intrinsics);
RgbFrame undistort_capture(const RgbFrame&, const Json& intrinsics);
#endif
} // namespace ps
