// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "model.hpp"
#include <optional>
#include <span>
namespace ps {
struct GrayFrame {
    int width{}, height{};
    std::vector<std::uint8_t> pixels;
};
struct Correspondence {
    Vec2 camera, projector;
    double confidence{};
};
struct Homography {
    std::array<double, 9> matrix{1, 0, 0, 0, 1, 0, 0, 0, 1};
    double rms{}, max_error{};
    std::size_t inliers{};
};
std::vector<GrayFrame> gray_patterns(int width, int height);
std::vector<Correspondence> decode_gray(const std::vector<GrayFrame>& captures, int projector_width,
                                        int projector_height, int min_contrast = 20);
Homography fit_homography(std::span<const Correspondence> points, double rejection_threshold = 2.0);
Vec2 transform(const Homography&, Vec2);
Mesh calibrated_mesh(const Homography& projector_to_camera, int projector_width, int projector_height,
                     Rect camera_bounds, int columns = 17, int rows = 17);
void write_pgm(const GrayFrame&, const std::filesystem::path&);
GrayFrame read_pgm(const std::filesystem::path&);
Json photometric_fit(std::span<const double> input, std::span<const double> measured);
#ifdef PS_WITH_OPENCV
Json calibrate_camera(const std::vector<std::string>& images, int columns, int rows, double square_size);
#endif
} // namespace ps
