// SPDX-License-Identifier: GPL-3.0-only
#include "ps/scan.hpp"
#include "ps/color.hpp"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <set>
#include <sstream>
namespace ps {
namespace {
Json read_json(const std::filesystem::path& path) {
    std::ifstream in(path);
    if (!in)
        throw std::runtime_error("Cannot read " + path.string());
    Json j;
    in >> j;
    return j;
}
std::string numbered(int n, const char* suffix) {
    std::ostringstream s;
    s << std::setw(3) << std::setfill('0') << n << suffix;
    return s.str();
}
bool finite(double v) {
    return std::isfinite(v);
}
Rect intersection(Rect a, Rect b) {
    double x = std::max(a.x, b.x), y = std::max(a.y, b.y);
    return {x, y, std::max(0., std::min(a.x + a.w, b.x + b.w) - x),
            std::max(0., std::min(a.y + a.h, b.y + b.h) - y)};
}
void check_options(const ScanOptions& o) {
    auto r = o.region;
    if (o.camera_index < 0 || o.camera_index > 31 || o.camera_width < 2 || o.camera_width > 8192 ||
        o.camera_height < 2 || o.camera_height > 8192 || o.settle_ms < 100 || o.settle_ms > 10000 ||
        o.flush_frames < 1 || o.flush_frames > 30 || o.min_contrast < 1 || o.min_contrast > 254 ||
        !finite(o.exposure) || !finite(o.max_rms) || o.max_rms <= 0 || o.max_rms > 10 || !finite(r.x) ||
        !finite(r.y) || !finite(r.w) || !finite(r.h) || r.x < 0 || r.y < 0 || r.w <= 0 || r.h <= 0 ||
        r.x + r.w > 1.000001 || r.y + r.h > 1.000001)
        throw std::invalid_argument("Invalid scan settings or camera region");
}
} // namespace
int gray_pattern_count(int w, int h) {
    if (w < 2 || h < 2 || w > 8192 || h > 8192)
        throw std::invalid_argument("Pattern dimensions must be 2..8192");
    auto bits = [](int n) {
        int b = 0;
        while ((1 << b) < n)
            ++b;
        return b;
    };
    return 2 + 2 * (bits(w) + bits(h));
}
GrayFrame gray_pattern(int w, int h, int index) {
    int count = gray_pattern_count(w, h);
    if (index < 0 || index >= count)
        throw std::invalid_argument("Invalid Gray pattern index");
    int bx = 0;
    while ((1 << bx) < w)
        ++bx;
    int bh = 0;
    while ((1 << bh) < h)
        ++bh;
    int pair = (index - 2) / 2, axis = pair >= bx, bit = axis ? bh - 1 - (pair - bx) : bx - 1 - pair;
    GrayFrame f{w, h, std::vector<std::uint8_t>(size_t(w) * h)};
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            int q = axis ? y : x;
            f.pixels[size_t(y) * w + x] = index == 0 ? 255
                                          : index == 1
                                              ? 0
                                              : ((((q ^ (q >> 1)) >> bit) & 1) ^ ((index - 2) % 2)) * 255;
        }
    return f;
}
Json scan_manifest(const Project& p, const std::vector<std::string>& ids, const ScanOptions& o) {
    check_options(o);
    if (p.locked)
        throw std::runtime_error("Unlock the project before calibration");
    if (ids.empty())
        throw std::invalid_argument("Select at least one projector");
    Json targets = Json::array();
    std::set<std::string> unique;
    std::string source;
    for (const auto& id : ids) {
        if (!unique.insert(id).second)
            throw std::invalid_argument("Duplicate projector selection");
        auto it = std::find_if(p.outputs.begin(), p.outputs.end(), [&](auto& a) { return a.id == id; });
        if (it == p.outputs.end() || !it->enabled || it->surfaces.size() != 1 || !it->surfaces[0].enabled)
            throw std::runtime_error("Calibration requires one enabled surface per selected projector");
        auto& s = it->surfaces[0];
        if (it->viewport.x != 0 || it->viewport.y != 0 || it->viewport.w != 1 || it->viewport.h != 1)
            throw std::runtime_error("Calibration requires a full output viewport");
        if (!find_source(p, s.source_id))
            throw std::runtime_error("Selected surface has no source");
        if (source.empty())
            source = s.source_id;
        if (s.source_id != source)
            throw std::runtime_error("Selected projectors must share a source");
        int count = gray_pattern_count(it->width, it->height);
        Json files = Json::array(), photos = Json::array();
        for (int i = 0; i < count; ++i)
            files.push_back(numbered(i, ".pgm"));
        for (int i = 0; i < 5; ++i)
            photos.push_back(numbered(i, ".ppm"));
        targets.push_back({{"output_id", id},
                           {"surface_id", s.id},
                           {"source_id", s.source_id},
                           {"name", it->name},
                           {"display", it->display},
                           {"width", it->width},
                           {"height", it->height},
                           {"folder", "projector-" + std::to_string(targets.size())},
                           {"patterns", files},
                           {"photometric", photos},
                           {"captured", 0},
                           {"output_before", *it},
                           {"source_before", *find_source(p, s.source_id)}});
    }
    return {{"schema_version", 1},
            {"model", "guided-planar-scan"},
            {"project_id", p.id},
            {"status", "pending"},
            {"physically_validated", false},
            {"targets", targets},
            {"camera",
             {{"index", o.camera_index},
              {"width", o.camera_width},
              {"height", o.camera_height},
              {"exposure", o.exposure},
              {"intrinsics", o.intrinsics}}},
            {"region", o.region},
            {"settle_ms", o.settle_ms},
            {"flush_frames", o.flush_frames},
            {"min_contrast", o.min_contrast},
            {"max_rms", o.max_rms},
            {"capture_synchronization", "software settling and frame draining; no hardware trigger"}};
}
void create_scan(const std::filesystem::path& path, const Json& manifest) {
    if (std::filesystem::exists(path) && !std::filesystem::is_empty(path))
        throw std::runtime_error("Choose an empty scan folder; existing evidence is preserved");
    Json saved = manifest, intrinsics;
    auto input = saved.at("camera").value("intrinsics", std::string{});
    if (!input.empty())
        intrinsics = read_json(std::filesystem::u8path(input));
    size_t n = 0;
    for (const auto& t : saved.at("targets"))
        if (t.at("folder") != "projector-" + std::to_string(n++))
            throw std::runtime_error("Invalid target folder");
    std::filesystem::create_directories(path);
    for (const auto& t : saved.at("targets"))
        std::filesystem::create_directory(path / t.at("folder").get<std::string>());
    if (!intrinsics.is_null()) {
        atomic_write(path / "intrinsics.json", intrinsics.dump(2));
        saved["camera"]["intrinsics"] = "intrinsics.json";
    }
    atomic_write(path / "session.json", saved.dump(2));
}
void write_ppm(const RgbFrame& f, const std::filesystem::path& path) {
    if (f.width < 1 || f.height < 1 || f.pixels.size() != size_t(f.width) * f.height * 3)
        throw std::invalid_argument("Invalid RGB frame");
    std::ofstream out(path, std::ios::binary);
    out.exceptions(std::ios::badbit | std::ios::failbit);
    out << "P6\n" << f.width << ' ' << f.height << "\n255\n";
    out.write(reinterpret_cast<const char*>(f.pixels.data()), std::streamsize(f.pixels.size()));
}
RgbFrame read_ppm(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::string magic;
    int w = 0, h = 0, max = 0;
    in >> magic >> w >> h >> max;
    if (magic != "P6" || max != 255 || w < 1 || h < 1 || w > 8192 || h > 8192)
        throw std::runtime_error("Expected binary 8-bit PPM");
    in.get();
    RgbFrame f{w, h, std::vector<std::uint8_t>(size_t(w) * h * 3)};
    in.read(reinterpret_cast<char*>(f.pixels.data()), std::streamsize(f.pixels.size()));
    if (!in)
        throw std::runtime_error("Truncated PPM");
    return f;
}
Json analyze_scan(const std::filesystem::path& path) {
    auto manifest = read_json(path / "session.json");
    if (manifest.at("schema_version") != 1 || manifest.at("model") != "guided-planar-scan" ||
        (manifest.at("status") != "captured" && manifest.at("status") != "complete" &&
         manifest.at("status") != "analysis_failed"))
        throw std::runtime_error("A complete captured scan is required");
    ScanOptions options;
    options.region = manifest.at("region").get<Rect>();
    options.min_contrast = manifest.at("min_contrast");
    options.max_rms = manifest.at("max_rms");
    check_options(options);
    Json intrinsics;
    auto ip = manifest.at("camera").value("intrinsics", std::string{});
    if (!ip.empty()) {
        if (ip != "intrinsics.json")
            throw std::runtime_error("Expected session-local intrinsics snapshot");
        intrinsics = read_json(path / "intrinsics.json");
    }
#ifndef PS_WITH_OPENCV
    if (!intrinsics.is_null())
        throw std::runtime_error("Camera undistortion requires an OpenCV build");
#endif
    Json results = Json::array();
    int cw = 0, ch = 0;
    size_t index = 0;
    for (const auto& t : manifest.at("targets")) {
        auto folder = "projector-" + std::to_string(index++);
        int pw = t.at("width"), ph = t.at("height"), count = gray_pattern_count(pw, ph);
        if (t.at("folder") != folder || t.at("patterns").size() != size_t(count) ||
            t.at("photometric").size() != 5 || t.at("captured") != count + 5)
            throw std::runtime_error("Incomplete or invalid capture sequence");
        std::vector<GrayFrame> frames;
        for (int i = 0; i < count; ++i) {
            if (t.at("patterns").at(i) != numbered(i, ".pgm"))
                throw std::runtime_error("Pattern ordering changed");
            auto f = read_pgm(path / folder / numbered(i, ".pgm"));
#ifdef PS_WITH_OPENCV
            if (!intrinsics.is_null())
                f = undistort_capture(f, intrinsics);
#endif
            frames.push_back(std::move(f));
        }
        if (!cw) {
            cw = frames[0].width;
            ch = frames[0].height;
        }
        if (cw != frames[0].width || ch != frames[0].height)
            throw std::runtime_error("Camera resolution changed between projectors");
        auto points = decode_gray(frames, pw, ph, options.min_contrast);
        Rect region{options.region.x * (cw - 1), options.region.y * (ch - 1), options.region.w * (cw - 1),
                    options.region.h * (ch - 1)};
        points.erase(std::remove_if(points.begin(), points.end(),
                                    [&](auto& p) {
                                        return p.camera.x < region.x || p.camera.x > region.x + region.w ||
                                               p.camera.y < region.y || p.camera.y > region.y + region.h;
                                    }),
                     points.end());
        if (points.size() < 64)
            throw std::runtime_error(
                t.at("name").get<std::string>() +
                ": fewer than 64 usable camera pixels; check exposure, region and pattern timing");
        std::vector<Correspondence> sample;
        size_t step = std::max<size_t>(1, (points.size() + 4999) / 5000);
        for (size_t i = 0; i < points.size(); i += step)
            sample.push_back(points[i]);
        auto h = fit_homography(sample, options.max_rms);
        double fraction = double(h.inliers) / sample.size();
        if (fraction < .7 || !finite(h.rms) || h.rms > options.max_rms)
            throw std::runtime_error(t.at("name").get<std::string>() +
                                     ": planar fit rejected; use a planar surface and stationary camera");
        GrayFrame confidence{cw, ch, std::vector<std::uint8_t>(size_t(cw) * ch)}, errors = confidence;
        size_t accepted = 0, clipped = 0;
        double sum = 0, maxerror = 0;
        for (auto& p : points) {
            auto q = transform(h, p.projector);
            double e = std::hypot(q.x - p.camera.x, q.y - p.camera.y);
            size_t k = size_t(p.camera.y) * cw + size_t(p.camera.x);
            errors.pixels[k] = std::uint8_t(std::clamp(e / options.max_rms * 255., 0., 255.));
            if (e <= options.max_rms) {
                confidence.pixels[k] = std::uint8_t(p.confidence * 255);
                ++accepted;
                sum += e * e;
                maxerror = std::max(maxerror, e);
                clipped += frames[0].pixels[k] >= 250;
            }
        }
        if (accepted < 64 || double(accepted) / points.size() < .7)
            throw std::runtime_error("Full-frame fit validation failed");
        Rect footprint{1e20, 1e20, 0, 0};
        double mx = -1e20, my = -1e20;
        for (Vec2 corner : std::array<Vec2, 4>{
                 {{0, 0}, {double(pw - 1), 0}, {0, double(ph - 1)}, {double(pw - 1), double(ph - 1)}}}) {
            auto q = transform(h, corner);
            footprint.x = std::min(footprint.x, q.x);
            footprint.y = std::min(footprint.y, q.y);
            mx = std::max(mx, q.x);
            my = std::max(my, q.y);
        }
        footprint.w = mx - footprint.x;
        footprint.h = my - footprint.y;
        auto tile = intersection(footprint, region);
        if (tile.w < 2 || tile.h < 2)
            throw std::runtime_error("Projector does not cover the selected screen region");
        auto mesh = calibrated_mesh(h, pw, ph, tile);
        Rect crop{(tile.x - region.x) / region.w, (tile.y - region.y) / region.h, tile.w / region.w,
                  tile.h / region.h};
        Json response = Json::array();
        const std::array<double, 5> levels{0, 64. / 255, 128. / 255, 192. / 255, 1};
        std::array<std::vector<double>, 3> measured;
        for (int i = 0; i < 5; ++i) {
            if (t.at("photometric").at(i) != numbered(i, ".ppm"))
                throw std::runtime_error("Photometric ordering changed");
            auto f = read_ppm(path / folder / numbered(i, ".ppm"));
#ifdef PS_WITH_OPENCV
            if (!intrinsics.is_null())
                f = undistort_capture(f, intrinsics);
#endif
            if (f.width != cw || f.height != ch)
                throw std::runtime_error("Photometric dimensions differ");
            std::array<double, 3> means{};
            for (size_t k = 0; k < confidence.pixels.size(); ++k)
                if (confidence.pixels[k])
                    for (int c = 0; c < 3; ++c)
                        means[c] += f.pixels[k * 3 + c] / 255.;
            for (int c = 0; c < 3; ++c)
                measured[c].push_back(means[c] / accepted);
        }
        bool photo_valid = double(clipped) / accepted < .05;
        for (int c = 0; c < 3; ++c) {
            try {
                for (int i = 1; i < 5; ++i)
                    if (measured[c][i] <= measured[c][i - 1] + .005)
                        throw std::runtime_error("Non-monotonic response");
                auto fit = photometric_fit(levels, measured[c]);
                double gamma = fit.at("gamma");
                if (!finite(gamma) || gamma < .1 || gamma > 8 || measured[c][4] >= .98)
                    photo_valid = false;
                fit["measurements"] = measured[c];
                response.push_back(fit);
            } catch (const std::exception&) {
                response.push_back(nullptr);
                photo_valid = false;
            }
        }
        write_pgm(confidence, path / folder / "confidence.pgm");
        write_pgm(errors, path / folder / "error.pgm");
        results.push_back({{"output_id", t.at("output_id")},
                           {"surface_id", t.at("surface_id")},
                           {"source_id", t.at("source_id")},
                           {"name", t.at("name")},
                           {"width", pw},
                           {"height", ph},
                           {"matrix", h.matrix},
                           {"mesh", mesh},
                           {"source_rect", crop},
                           {"footprint", tile},
                           {"output_before", t.at("output_before")},
                           {"source_before", t.at("source_before")},
                           {"decoded_pixels", points.size()},
                           {"inliers", accepted},
                           {"inlier_fraction", double(accepted) / points.size()},
                           {"rms_pixels", std::sqrt(sum / accepted)},
                           {"max_error_pixels", maxerror},
                           {"clipped_white_fraction", double(clipped) / accepted},
                           {"response", response},
                           {"photometric_valid", photo_valid},
                           {"confidence_mask", folder + "/confidence.pgm"},
                           {"error_map", folder + "/error.pgm"}});
    }
    if (results.empty())
        throw std::runtime_error("No projector captures");
    Json overlaps = Json::array();
    for (size_t a = 0; a < results.size(); ++a)
        for (size_t b = a + 1; b < results.size(); ++b) {
            auto r =
                intersection(results[a].at("footprint").get<Rect>(), results[b].at("footprint").get<Rect>());
            if (r.w > 0 && r.h > 0)
                overlaps.push_back({{"a", results[a].at("output_id")},
                                    {"b", results[b].at("output_id")},
                                    {"camera_bounds", r}});
        }
    Json result = {{"schema_version", 1},
                   {"model", "guided-planar-calibration"},
                   {"project_id", manifest.at("project_id")},
                   {"camera_width", cw},
                   {"camera_height", ch},
                   {"region", options.region},
                   {"targets", results},
                   {"overlaps", overlaps},
                   {"max_rms", options.max_rms},
                   {"physically_validated", false},
                   {"undistorted", !intrinsics.is_null()},
                   {"photometric_model", "relative camera RGB response; assumes fixed exposure and an sRGB "
                                         "camera; no colorimeter or spatial correction"},
                   {"blend_model",
                    "axis-aligned footprint estimates; manually verify perspective and three-way overlaps"}};
    atomic_write(path / "result.json", result.dump(2));
    return result;
}
Project apply_scan(const Project& original, const Json& result, bool blends, bool color) {
    if (original.locked)
        throw std::runtime_error("Unlock configuration before applying calibration");
    if (result.at("schema_version") != 1 || result.at("model") != "guided-planar-calibration" ||
        result.at("project_id") != original.id)
        throw std::runtime_error("Calibration belongs to a different project");
    Project p = original;
    std::set<std::string> ids;
    std::string source;
    std::array<double, 3> common_white{1, 1, 1};
    for (const auto& t : result.at("targets"))
        if (color) {
            if (!t.at("photometric_valid").get<bool>())
                throw std::runtime_error(
                    "Photometric measurements failed; apply geometry only or rescan exposure");
            for (int c = 0; c < 3; ++c) {
                double v = t.at("response").at(c).at("white").get<double>() -
                           t.at("response").at(c).at("black").get<double>();
                if (!finite(v) || v <= 0)
                    throw std::runtime_error("Invalid photometric response");
                common_white[c] = std::min(common_white[c], v);
            }
        }
    for (const auto& t : result.at("targets")) {
        std::string id = t.at("output_id");
        if (!ids.insert(id).second)
            throw std::runtime_error("Duplicate calibration output");
        auto o = std::find_if(p.outputs.begin(), p.outputs.end(), [&](auto& v) { return v.id == id; });
        if (o == p.outputs.end() || o->width != t.at("width") || o->height != t.at("height") ||
            o->surfaces.size() != 1 || o->surfaces[0].id != t.at("surface_id").get<std::string>() ||
            o->surfaces[0].source_id != t.at("source_id").get<std::string>())
            throw std::runtime_error("Projector configuration changed since scanning; scan again");
        if (Json(*o) != t.at("output_before") || !find_source(p, o->surfaces[0].source_id) ||
            Json(*find_source(p, o->surfaces[0].source_id)) != t.at("source_before"))
            throw std::runtime_error("Projector or source settings changed since scanning; scan again");
        if (source.empty())
            source = o->surfaces[0].source_id;
        if (source != o->surfaces[0].source_id)
            throw std::runtime_error("Calibration source routing changed");
        double rms = t.at("rms_pixels"), fraction = t.at("inlier_fraction");
        if (!finite(rms) || rms < 0 || rms > 10 || rms > result.at("max_rms").get<double>() ||
            !finite(fraction) || fraction < .7 || fraction > 1 || t.at("inliers").get<size_t>() < 64)
            throw std::runtime_error("Calibration quality gate failed");
        o->surfaces[0].mesh = t.at("mesh").get<Mesh>();
        o->surfaces[0].source_rect = t.at("source_rect").get<Rect>();
        // Existing artist masks remain editable and are not replaced by camera-space confidence images.
        if (blends) {
            o->blend.left = o->blend.right = o->blend.top = o->blend.bottom = 0;
            o->blend.gamma = 1;
            o->blend.curve = {{0, 0}, {1, 1}};
            auto r = t.at("footprint").get<Rect>();
            for (const auto& other : result.at("targets"))
                if (other.at("output_id") != id) {
                    auto q = other.at("footprint").get<Rect>(), v = intersection(r, q);
                    if (v.w <= 0 || v.h <= 0)
                        continue;
                    if (v.h / std::min(r.h, q.h) > .8 && v.w < std::min(r.w, q.w) * .5) {
                        if (q.x < r.x)
                            o->blend.left = std::max(o->blend.left, v.w / r.w);
                        else
                            o->blend.right = std::max(o->blend.right, v.w / r.w);
                    } else if (v.w / std::min(r.w, q.w) > .8 && v.h < std::min(r.h, q.h) * .5) {
                        if (q.y < r.y)
                            o->blend.top = std::max(o->blend.top, v.h / r.h);
                        else
                            o->blend.bottom = std::max(o->blend.bottom, v.h / r.h);
                    }
                }
        }
        if (color) {
            // The renderer applies gamma in linear light. Fit total sRGB-input/camera response and
            // compensate relative response against the median channel, including sRGB transfer.
            std::array<double, 3> gammas{};
            for (int c = 0; c < 3; ++c)
                gammas[c] = t.at("response").at(c).at("gamma");
            auto sorted = gammas;
            std::sort(sorted.begin(), sorted.end());
            o->color = ColorSettings{};
            o->color.gamma = std::clamp(sorted[1], .1, 8.);
            for (int c = 0; c < 3; ++c) {
                double black = t.at("response").at(c).at("black"), white = t.at("response").at(c).at("white");
                double encoded = std::pow(common_white[c] / (white - black), 1. / gammas[c]);
                double linear = srgb_to_linear(encoded);
                o->color.gain[c] = std::pow(linear, o->color.gamma);
            }
        }
    }
    if (ids.empty())
        throw std::runtime_error("Empty calibration result");
    p.presets.push_back({make_id(), "Before camera calibration", original.outputs});
    p.presets.push_back({make_id(), "Camera calibration", p.outputs});
    p.calibration = result;
    auto errors = validate(p);
    if (!errors.empty())
        throw std::runtime_error(errors.front());
    return p;
}
} // namespace ps
