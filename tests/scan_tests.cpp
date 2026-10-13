// SPDX-License-Identifier: GPL-3.0-only
#include "ps/scan.hpp"
#include <cmath>
#include <fstream>
#include <functional>
#include <iostream>
namespace {
void require(bool ok, const char* message) {
    if (!ok)
        throw std::runtime_error(message);
}
void rejects(const std::function<void()>& f, const char* message) {
    bool threw = false;
    try {
        f();
    } catch (const std::exception&) {
        threw = true;
    }
    require(threw, message);
}
} // namespace
int main() {
    using namespace ps;
    auto dir = std::filesystem::current_path() / ("scan-test-" + make_id());
    try {
        auto p = example_project(2);
        for (auto& o : p.outputs) {
            o.width = 64;
            o.height = 48;
        }
        ScanOptions options;
        options.camera_width = 112;
        options.camera_height = 48;
        auto m = scan_manifest(p, {p.outputs[0].id, p.outputs[1].id}, options);
        create_scan(dir, m);
        rejects([&] { create_scan(dir, m); }, "Evidence must never be overwritten");
        rejects([&] { analyze_scan(dir); }, "Incomplete scans rejected");
        auto locked = p;
        locked.locked = true;
        rejects([&] { scan_manifest(locked, {p.outputs[0].id}); }, "Locked scan rejected");
        for (int t = 0; t < 2; ++t) {
            auto frames = gray_patterns(64, 48);
            for (size_t i = 0; i < frames.size(); ++i) {
                require(frames[i].pixels == gray_pattern(64, 48, int(i)).pixels,
                        "Lazy pattern ordering differs from decoder");
                GrayFrame capture{112, 48, std::vector<std::uint8_t>(112 * 48, 10)};
                for (int y = 0; y < 48; ++y)
                    for (int x = 0; x < 64; ++x)
                        capture.pixels[y * 112 + x + t * 48] = frames[i].pixels[y * 64 + x] ? 230 : 10;
                write_pgm(capture, dir / m["targets"][t]["folder"].get<std::string>() /
                                       m["targets"][t]["patterns"][i].get<std::string>());
            }
            for (int i = 0; i < 5; ++i) {
                RgbFrame f{112, 48, std::vector<std::uint8_t>(112 * 48 * 3, 10)};
                double level = std::array<double, 5>{0, 64. / 255, 128. / 255, 192. / 255, 1}[i];
                for (int y = 0; y < 48; ++y)
                    for (int x = 0; x < 64; ++x)
                        for (int c = 0; c < 3; ++c)
                            f.pixels[(y * 112 + x + t * 48) * 3 + c] =
                                std::uint8_t(10 + level * (t ? 180 : 220));
                auto file = dir / m["targets"][t]["folder"].get<std::string>() /
                            m["targets"][t]["photometric"][i].get<std::string>();
                write_ppm(f, file);
                require(read_ppm(file).pixels == f.pixels, "RGB capture roundtrip failed");
            }
            m["targets"][t]["captured"] = frames.size() + 5;
        }
        m["status"] = "captured";
        atomic_write(dir / "session.json", m.dump(2));
        auto result = analyze_scan(dir);
        require(result["targets"].size() == 2 && result["overlaps"].size() == 1,
                "Shared-camera overlap not found");
        require(result["targets"][0]["rms_pixels"].get<double>() < 1e-6, "Exact translation fit failed");
        auto confidence = read_pgm(dir / "projector-0/confidence.pgm");
        require(confidence.pixels[0] > 0 && confidence.pixels[100] == 0,
                "Confidence mask failed to reject dark background");
        auto calibrated = apply_scan(p, result, true, true);
        require(validate(calibrated).empty() && calibrated.presets.size() == 2,
                "Calibrated project or reversible presets invalid");
        require(std::abs(calibrated.outputs[0].surfaces[0].source_rect.w - 63. / 111) < 1e-6,
                "Shared source crop is incorrect");
        require(std::abs(calibrated.outputs[0].blend.right - 15. / 63) < 1e-6, "Overlap width is incorrect");
        require(calibrated.outputs[0].color.gain[0] < calibrated.outputs[1].color.gain[0],
                "Brighter projector should be attenuated");
        require(activate_preset(calibrated, calibrated.presets[0].id), "Before preset cannot be restored");
        require(calibrated.outputs[0].surfaces[0].mesh == p.outputs[0].surfaces[0].mesh,
                "Before preset changed original mesh");
        auto different = p;
        different.id = make_id();
        rejects([&] { apply_scan(different, result, false, false); }, "Wrong project accepted");
        different = p;
        different.outputs[0].width = 65;
        rejects([&] { apply_scan(different, result, false, false); }, "Stale output accepted");
        different = p;
        different.sources[0].image = "changed.png";
        rejects([&] { apply_scan(different, result, false, false); }, "Stale source accepted");
        different = p;
        different.outputs[0].display = "changed-display";
        rejects([&] { apply_scan(different, result, false, false); }, "Stale display accepted");
        rejects([&] { apply_scan(locked, result, false, false); }, "Apply while locked accepted");
        auto bad = result;
        bad["targets"][0]["rms_pixels"] = 20;
        rejects([&] { apply_scan(p, bad, false, false); }, "Bad fit accepted");
        bad = result;
        bad["targets"][0]["photometric_valid"] = false;
        rejects([&] { apply_scan(p, bad, false, true); }, "Rejected exposure accepted for color");
        require(validate(apply_scan(p, bad, false, false)).empty(),
                "Geometry-only application should allow rejected color");
        auto missing = m;
        missing["targets"][0]["captured"] = 0;
        atomic_write(dir / "session.json", missing.dump(2));
        rejects([&] { analyze_scan(dir); }, "Missing captures accepted");
        missing = m;
        missing["targets"][0]["patterns"][0] = "../bad.pgm";
        atomic_write(dir / "session.json", missing.dump(2));
        rejects([&] { analyze_scan(dir); }, "Reordered/path traversal sequence accepted");
        auto low = read_pgm(dir / "projector-0/000.pgm");
        std::fill(low.pixels.begin(), low.pixels.end(), 10);
        write_pgm(low, dir / "projector-0/000.pgm");
        atomic_write(dir / "session.json", m.dump(2));
        rejects([&] { analyze_scan(dir); }, "Low contrast scan accepted");
        std::filesystem::remove_all(dir);
        std::cout << "Ordered capture, fit validation, shared ROI, overlap, photometry, masks, preset "
                     "restore and failure tests passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
