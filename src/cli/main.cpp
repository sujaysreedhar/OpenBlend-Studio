// SPDX-License-Identifier: GPL-3.0-only
#include "ps/calibration.hpp"
#include "ps/model.hpp"
#include "ps/simulator.hpp"
#include "ps/scan.hpp"
#include <fstream>
#include <iomanip>
#include <iostream>
int main(int argc, char** argv) {
    try {
        if (argc < 2) {
            std::cout << "Projection Studio 0.4\nCommands:\n  example <project.json> [projectors] "
                         "[irregular]\n  validate <project.json>\n  views <project.json> <views.json>\n  "
                         "patterns <directory> <width> <height>\n  calibrate <capture-directory> "
                         "<projector-width> <projector-height> <result.json>\n  scan-plan <project.json> <empty-folder>\n"
                         "  scan-analyze <capture-folder>\n  scan-apply <project.json> <result.json> <new-project.json> [--blend] [--color]\n  capabilities\n";
            return 0;
        }
        std::string command = argv[1];
        auto require = [&](int n) {
            if (argc < n)
                throw std::invalid_argument("Missing command arguments; run without arguments for help");
        };
        if (command == "scan-plan") {
            require(4);auto p=ps::load_project(argv[2]);std::vector<std::string> ids;
            for(auto& o:p.outputs)if(o.enabled)ids.push_back(o.id);
            ps::create_scan(argv[3],ps::scan_manifest(p,ids));
        } else if (command == "scan-analyze") {
            require(3);auto j=ps::analyze_scan(argv[2]);
            std::cout<<j.at("targets").size()<<" projector(s) analyzed; result.json written\n";
        } else if (command == "scan-apply") {
            require(5);bool blend=false,color=false;
            for(int i=5;i<argc;++i){std::string flag=argv[i];if(flag=="--blend")blend=true;else if(flag=="--color")color=true;else throw std::invalid_argument("Unknown scan-apply option");}
            std::ifstream in(argv[3]);ps::Json j;in>>j;
            ps::save_project(ps::apply_scan(ps::load_project(argv[2]),j,blend,color),argv[4]);
        } else if (command == "example") {
            require(3);
            ps::save_project(ps::example_project(argc > 3 ? std::stoi(argv[3]) : 1,
                                                 argc > 4 && std::string(argv[4]) == "irregular"),
                             argv[2]);
        } else if (command == "validate") {
            require(3);
            auto p = ps::load_project(argv[2]);
            size_t surfaces = 0;
            for (auto& o : p.outputs)
                surfaces += o.surfaces.size();
            std::cout << p.name << ": " << p.outputs.size() << " outputs, " << surfaces
                      << " surfaces; valid\n";
        } else if (command == "views") {
            require(4);
            ps::atomic_write(argv[3], ps::export_views(ps::load_project(argv[2])).dump(2));
        } else if (command == "patterns") {
            require(5);
            auto frames = ps::gray_patterns(std::stoi(argv[3]), std::stoi(argv[4]));
            std::filesystem::create_directories(argv[2]);
            for (size_t i = 0; i < frames.size(); ++i) {
                std::ostringstream name;
                name << std::setw(3) << std::setfill('0') << i << ".pgm";
                ps::write_pgm(frames[i], std::filesystem::path(argv[2]) / name.str());
            }
            std::cout << frames.size() << " patterns written: white, black, then X/Y bit pairs\n";
        } else if (command == "calibrate") {
            require(6);
            std::vector<std::filesystem::path> files;
            for (auto& f : std::filesystem::directory_iterator(argv[2]))
                if (f.path().extension() == ".pgm")
                    files.push_back(f.path());
            std::sort(files.begin(), files.end());
            std::vector<ps::GrayFrame> frames;
            for (auto& f : files)
                frames.push_back(ps::read_pgm(f));
            auto points = ps::decode_gray(frames, std::stoi(argv[3]), std::stoi(argv[4]));
            // Bound RANSAC workload while retaining samples across the whole camera frame.
            std::vector<ps::Correspondence> sample;
            size_t step = std::max<size_t>(1, points.size() / 5000);
            for (size_t i = 0; i < points.size(); i += step)
                sample.push_back(points[i]);
            auto h = ps::fit_homography(sample);
            ps::Json j = {{"schema_version", 1},
                          {"model", "planar-homography"},
                          {"projector_width", std::stoi(argv[3])},
                          {"projector_height", std::stoi(argv[4])},
                          {"camera_width", frames[0].width},
                          {"camera_height", frames[0].height},
                          {"matrix", h.matrix},
                          {"rms_pixels", h.rms},
                          {"max_error_pixels", h.max_error},
                          {"inliers", h.inliers},
                          {"decoded_pixels", points.size()},
                          {"physically_validated", false}};
            ps::atomic_write(argv[5], j.dump(2));
            std::cout << "RMS " << h.rms << " camera pixels, " << h.inliers << " inliers\n";
        }
#ifdef PS_WITH_OPENCV
        else if (command == "camera") {
            require(7);
            std::vector<std::string> images;
            for (int i = 6; i < argc; ++i)
                images.push_back(argv[i]);
            ps::atomic_write(argv[2], ps::calibrate_camera(images, std::stoi(argv[3]), std::stoi(argv[4]),
                                                           std::stod(argv[5]))
                                          .dump(2));
        }
#endif
        else if (command == "restore") {
            require(3);
            ps::restore_backup(argv[2]);
        } else if (command == "capabilities")
            std::cout << "Project format v2; manual warp/mask/blend/color; desktop/window capture on "
                         "Windows; DX11/DX12 adapters; guided planar Gray-code scans and relative RGB "
                         "response estimates; generic frustum export. Live camera capture requires an "
                         "OpenCV build. See "
                         "docs/FEATURES.md for validation status.\n";
        else
            throw std::invalid_argument("Unknown command");
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << '\n';
        return 1;
    }
}
