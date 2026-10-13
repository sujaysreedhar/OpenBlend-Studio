// SPDX-License-Identifier: GPL-3.0-only
#include "ps/calibration.hpp"
#include "ps/scan.hpp"
#include <opencv2/calib3d.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/videoio.hpp>
#include <algorithm>
#include <cmath>
namespace ps {
namespace {
cv::Mat corrected(const std::uint8_t* pixels, int w, int h, int type, const Json& j) {
    if (j.at("width") != w || j.at("height") != h)
        throw std::runtime_error("Camera intrinsics resolution differs from capture");
    auto k = j.at("camera_matrix").get<std::vector<double>>();
    auto d = j.at("distortion").get<std::vector<double>>();
    if (k.size() != 9 ||
        (d.size() != 4 && d.size() != 5 && d.size() != 8 && d.size() != 12 && d.size() != 14) ||
        !std::all_of(k.begin(), k.end(), [](double x) { return std::isfinite(x); }) ||
        !std::all_of(d.begin(), d.end(), [](double x) { return std::isfinite(x); }) || k[0] <= 0 ||
        k[4] <= 0 || k[8] != 1)
        throw std::runtime_error("Invalid camera intrinsics");
    cv::Mat src(h, w, type, const_cast<std::uint8_t*>(pixels)), out;
    cv::undistort(src, out, cv::Mat(3, 3, CV_64F, k.data()), cv::Mat(d), cv::Mat(3, 3, CV_64F, k.data()));
    return out;
}
} // namespace
GrayFrame undistort_capture(const GrayFrame& f, const Json& j) {
    if (f.width < 1 || f.height < 1 || f.pixels.size() != size_t(f.width) * f.height)
        throw std::runtime_error("Invalid grayscale frame");
    auto out = corrected(f.pixels.data(), f.width, f.height, CV_8UC1, j);
    return {f.width, f.height, std::vector<std::uint8_t>(out.data, out.data + out.total())};
}
RgbFrame undistort_capture(const RgbFrame& f, const Json& j) {
    if (f.width < 1 || f.height < 1 || f.pixels.size() != size_t(f.width) * f.height * 3)
        throw std::runtime_error("Invalid RGB frame");
    auto out = corrected(f.pixels.data(), f.width, f.height, CV_8UC3, j);
    return {f.width, f.height, std::vector<std::uint8_t>(out.data, out.data + out.total() * 3)};
}
Json calibrate_camera(const std::vector<std::string>& files, int columns, int rows, double square) {
    if (columns < 3 || rows < 3 || square <= 0)
        throw std::invalid_argument("Invalid checkerboard dimensions");
    std::vector<std::vector<cv::Point2f>> image;
    std::vector<std::vector<cv::Point3f>> object;
    std::vector<cv::Point3f> grid;
    for (int y = 0; y < rows; ++y)
        for (int x = 0; x < columns; ++x)
            grid.emplace_back(float(x * square), float(y * square), 0);
    cv::Size size;
    for (auto& path : files) {
        auto frame = cv::imread(path, cv::IMREAD_GRAYSCALE);
        if (frame.empty())
            throw std::runtime_error("Cannot read calibration photo");
        if (size.area() && size != frame.size())
            throw std::runtime_error("Calibration image sizes differ");
        size = frame.size();
        std::vector<cv::Point2f> points;
        if (cv::findChessboardCornersSB(frame, {columns, rows}, points)) {
            image.push_back(points);
            object.push_back(grid);
        }
    }
    if (image.size() < 6)
        throw std::runtime_error("At least six detected checkerboard views are required");
    cv::Mat matrix, distortion;
    std::vector<cv::Mat> rotations, translations;
    double rms = cv::calibrateCamera(object, image, size, matrix, distortion, rotations, translations);
    std::vector<double> k(matrix.ptr<double>(), matrix.ptr<double>() + 9),
        d(distortion.ptr<double>(), distortion.ptr<double>() + distortion.total());
    return {{"width", size.width}, {"height", size.height}, {"camera_matrix", k},
            {"distortion", d},     {"rms_pixels", rms},     {"accepted_views", image.size()}};
}
} // namespace ps
