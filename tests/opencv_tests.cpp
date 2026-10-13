// SPDX-License-Identifier: GPL-3.0-only
#include "ps/calibration.hpp"
#include "ps/scan.hpp"
#include <iostream>
#include <opencv2/calib3d.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
int main() {
    try {
        auto directory = std::filesystem::current_path() / ("camera-test-" + ps::make_id());
        std::filesystem::create_directories(directory);
        cv::Mat camera = (cv::Mat_<double>(3, 3) << 900, 0, 640, 0, 910, 360, 0, 0, 1);
        std::vector<std::string> paths;
        for (int i = 0; i < 10; ++i) {
            cv::Mat image(720, 1280, CV_8UC1, cv::Scalar(160));
            cv::Vec3d rotation(.08 * (i - 4), .1 * ((i % 4) - 1), .025 * i),
                translation(-.17 + .015 * (i % 3), -.10 + .01 * (i % 2), .75 + .04 * i);
            for (int y = -1; y < 6; ++y)
                for (int x = -1; x < 9; ++x) {
                    std::vector<cv::Point3f> square{{float(x * .04), float(y * .04), 0},
                                                    {float((x + 1) * .04), float(y * .04), 0},
                                                    {float((x + 1) * .04), float((y + 1) * .04), 0},
                                                    {float(x * .04), float((y + 1) * .04), 0}};
                    std::vector<cv::Point2f> projected;
                    cv::projectPoints(square, rotation, translation, camera, cv::Mat(), projected);
                    std::vector<cv::Point> polygon;
                    for (auto p : projected)
                        polygon.emplace_back(cvRound(p.x), cvRound(p.y));
                    cv::fillConvexPoly(image, polygon, cv::Scalar((x + y) % 2 ? 255 : 0), cv::LINE_AA);
                }
            cv::GaussianBlur(image, image, {3, 3}, .6);
            auto path = (directory / (std::to_string(i) + ".pgm")).string();
            cv::imwrite(path, image);
            paths.push_back(path);
        }
        auto result = ps::calibrate_camera(paths, 9, 6, .04);
        ps::GrayFrame frame{32,24,std::vector<std::uint8_t>(32*24)};
        for(size_t i=0;i<frame.pixels.size();++i)frame.pixels[i]=std::uint8_t(i%255);
        ps::Json intrinsics={{"width",32},{"height",24},{"camera_matrix",{30,0,16,0,30,12,0,0,1}},
                             {"distortion",{0,0,0,0,0}}};
        if(ps::undistort_capture(frame,intrinsics).pixels!=frame.pixels)
            throw std::runtime_error("Zero-distortion correction changed pixels");
        intrinsics["distortion"][0]=.25;
        if(ps::undistort_capture(frame,intrinsics).pixels==frame.pixels)
            throw std::runtime_error("Lens distortion correction did not remap pixels");
        intrinsics["width"]=33;bool rejected=false;
        try{ps::undistort_capture(frame,intrinsics);}catch(const std::exception&){rejected=true;}
        if(!rejected)throw std::runtime_error("Mismatched camera intrinsics accepted");
        std::cout << result.dump(2) << '\n';
        double fx = result["camera_matrix"][0], fy = result["camera_matrix"][4];
        if (std::abs(fx - 900) > 60 || std::abs(fy - 910) > 60 || result["rms_pixels"].get<double>() > 1)
            throw std::runtime_error("Synthetic intrinsics recovery outside tolerance");
        ps::atomic_write(std::filesystem::current_path() / "camera-test-results.json", result.dump(2));
        std::filesystem::remove_all(directory);
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
