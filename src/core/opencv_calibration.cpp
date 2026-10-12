// SPDX-License-Identifier: GPL-3.0-only
#include "ps/calibration.hpp"
#include <opencv2/calib3d.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/videoio.hpp>
namespace ps {
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
