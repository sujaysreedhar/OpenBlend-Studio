// SPDX-License-Identifier: GPL-3.0-only
#include "ps/simulator.hpp"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <numbers>
#include <sstream>
namespace ps {
std::vector<ViewFrustum> screen_views(const Project& p) {
    if (p.outputs.empty())
        throw std::invalid_argument("No outputs");
    if (p.screen.shape != "plane" && p.screen.shape != "cylinder")
        throw std::invalid_argument("View export currently supports plane and cylinder");
    std::vector<ViewFrustum> result;
    double n = double(p.outputs.size());
    bool panoramic = true;
    std::string shared_source;
    for (auto& output : p.outputs) {
        if (output.surfaces.size() != 1) {
            panoramic = false;
            break;
        }
        auto& source_id = output.surfaces[0].source_id;
        if (shared_source.empty())
            shared_source = source_id;
        else if (shared_source != source_id)
            panoramic = false;
    }
    for (size_t i = 0; i < p.outputs.size(); ++i) {
        ViewFrustum f;
        double u0 = i / n, u1 = (i + 1) / n;
        if (panoramic) {
            auto& crop = p.outputs[i].surfaces[0].source_rect;
            u0 = crop.x;
            u1 = crop.x + crop.w;
        }
        f.output_id = p.outputs[i].id;
        double distance = p.screen.shape == "plane" ? p.screen.eye.z : p.screen.radius;
        if (distance <= 0)
            throw std::invalid_argument("Eyepoint must face the screen");
        if (p.screen.shape == "cylinder") {
            if (std::abs(p.screen.eye.x) > 1e-6 || std::abs(p.screen.eye.y) > 1e-6)
                throw std::invalid_argument("Cylinder export requires a centered eyepoint");
            double angle = p.screen.arc_degrees * (u1 - u0);
            if (angle >= 175)
                throw std::invalid_argument("Each perspective channel must cover less than 175 degrees");
            f.yaw = -p.screen.arc_degrees / 2 + (u0 + u1) * .5 * p.screen.arc_degrees;
            double half = std::tan(angle * .5 * std::numbers::pi / 180) * f.near_clip;
            f.left = -half;
            f.right = half;
        } else {
            f.left = (-p.screen.width / 2 + p.screen.width * u0 - p.screen.eye.x) * f.near_clip / distance;
            f.right = (-p.screen.width / 2 + p.screen.width * u1 - p.screen.eye.x) * f.near_clip / distance;
        }
        f.bottom = (-p.screen.height / 2 - p.screen.eye.y) * f.near_clip / distance;
        f.top = (p.screen.height / 2 - p.screen.eye.y) * f.near_clip / distance;
        result.push_back(f);
    }
    return result;
}
Json export_views(const Project& p) {
    Json views = Json::array();
    for (auto& f : screen_views(p)) {
        auto l = f.left, r = f.right, b = f.bottom, t = f.top, n = f.near_clip, z = f.far_clip;
        // Right-handed, OpenGL clip depth [-1,1], row-major storage.
        std::array<double, 16> projection{2 * n / (r - l),
                                          0,
                                          (r + l) / (r - l),
                                          0,
                                          0,
                                          2 * n / (t - b),
                                          (t + b) / (t - b),
                                          0,
                                          0,
                                          0,
                                          -(z + n) / (z - n),
                                          -2 * z * n / (z - n),
                                          0,
                                          0,
                                          -1,
                                          0};
        views.push_back(
            {{"output_id", f.output_id},
             {"yaw_degrees", f.yaw},
             {"pitch_degrees", f.pitch},
             {"frustum", {{"left", l}, {"right", r}, {"bottom", b}, {"top", t}, {"near", n}, {"far", z}}},
             {"projection_row_major", projection}});
    }
    return {{"schema_version", 1},
            {"units", "meters"},
            {"coordinate_system", "right-handed, +Y up, camera looks -Z"},
            {"clip_depth", "negative-one-to-one"},
            {"project_id", p.id},
            {"views", views}};
}
static std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}
std::string msfs_camera_patch(const std::string& text, double yaw, double pitch, double zoom,
                              const std::string& version) {
    if (version != "2020" && version != "2024")
        throw std::invalid_argument("Supported simulator versions: 2020 and 2024");
    if (!std::isfinite(yaw) || !std::isfinite(pitch) || !std::isfinite(zoom) || std::abs(yaw) > 180 ||
        std::abs(pitch) > 90 || zoom <= 0 || zoom > 128)
        throw std::invalid_argument("Invalid MSFS camera parameters");
    if (lower(text).find("[cameradefinition.") == std::string::npos)
        throw std::runtime_error("Not a cameras.cfg file");
    if (lower(text).find("fa32f4bb-2c79-4e7f-a229-314159265358") != std::string::npos)
        throw std::runtime_error(
            "Projection Studio camera already exists; restore the backup before exporting a replacement");
    // Create a new camera definition; preserve every existing aircraft camera verbatim.
    std::istringstream in(text);
    std::string line;
    int maximum = -1;
    while (std::getline(in, line)) {
        auto l = lower(line);
        auto at = l.find("[cameradefinition.");
        if (at != std::string::npos) {
            try {
                maximum = std::max(maximum, std::stoi(l.substr(at + 18)));
            } catch (const std::exception&) {
            }
        }
    }
    std::ostringstream out;
    out << text << "\n\n; Projection Studio export for MSFS " << version << "\n[CameraDefinition."
        << maximum + 1 << "]\nTitle = Projection Studio\nGuid = {";
    // Stable GUID for this exported camera, distinct from aircraft defaults.
    out << "fa32f4bb-2c79-4e7f-a229-314159265358}\nOrigin = Virtual Cockpit\nCategory = Custom\nSubCategory "
           "= Custom\nInitialPbh = "
        << pitch << ", 0, " << yaw << "\nInitialXyz = 0, 0, 0\nInitialZoom = " << zoom
        << "\nAllowZoom = TRUE\n";
    return out.str();
}
static std::string read_text(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in)
        throw std::runtime_error("Cannot read configuration");
    return {std::istreambuf_iterator<char>(in), {}};
}
void apply_msfs_camera(const std::filesystem::path& path, const std::string& original,
                       const std::string& replacement) {
    if (read_text(path) != original)
        throw std::runtime_error("Configuration changed since preview; reload it before applying");
    atomic_write(path, replacement);
}
void restore_backup(const std::filesystem::path& path) {
    auto backup = path;
    backup += ".bak";
    auto original = read_text(backup);
    atomic_write(path, original);
}
} // namespace ps
