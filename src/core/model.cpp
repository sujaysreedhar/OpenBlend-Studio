// SPDX-License-Identifier: GPL-3.0-only
#include "ps/model.hpp"
#include <atomic>
#include <chrono>
#include <cmath>
#include <random>
#include <sstream>
#include <unordered_set>
namespace ps {
std::string make_id() {
    static std::atomic<unsigned long long> counter{};
    static const auto seed = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    std::ostringstream out;
    out << std::hex << seed << '-' << ++counter;
    return out.str();
}
Mesh identity_mesh(int columns, int rows) {
    if (columns < 2 || rows < 2 || columns > 64 || rows > 64)
        throw std::invalid_argument("Grid dimensions must be 2..64");
    Mesh m;
    m.columns = columns;
    m.rows = rows;
    for (int y = 0; y < rows; ++y)
        for (int x = 0; x < columns; ++x)
            m.points.push_back({double(x) / (columns - 1), double(y) / (rows - 1)});
    return m;
}
const Source* find_source(const Project& p, const std::string& id) {
    for (auto& s : p.sources)
        if (s.id == id)
            return &s;
    return nullptr;
}
Project example_project(int n, bool irregular) {
    if (n < 1 || n > 16)
        throw std::invalid_argument("Example projector count must be 1..16");
    Project p;
    p.id = make_id();
    p.name = irregular ? "Architectural facets" : n == 1 ? "First light" : "Panoramic flight deck";
    Source s;
    s.id = make_id();
    s.name = "Alignment grid";
    s.width = 1920 * n;
    p.sources.push_back(s);
    for (int i = 0; i < n; ++i) {
        Output o;
        o.id = make_id();
        o.name = "Projector " + std::to_string(i + 1);
        if (n > 1) {
            o.blend.left = i ? 0.1 : 0;
            o.blend.right = i < n - 1 ? 0.1 : 0;
        }
        Surface f;
        f.id = make_id();
        f.name = "Main surface";
        f.source_id = s.id;
        f.mesh = identity_mesh();
        const double step = 1. / (n - (n - 1) * 0.1);
        f.source_rect = {i * step * 0.9, 0, step, 1};
        if (irregular) {
            f.mesh = identity_mesh(3, 3);
            f.mesh.points[0] = {.1, .2};
            f.mesh.points[2] = {.85, .05};
            f.mesh.points[6] = {.18, .9};
            f.mesh.points[8] = {.9, .85};
            f.mask.enabled = true;
            f.mask.polygon = {{0, 0}, {1, 0}, {1, .8}, {.65, 1}, {0, 1}};
            f.mask.feather = .02;
        }
        o.surfaces.push_back(f);
        p.outputs.push_back(o);
    }
    if (n > 1)
        p.screen.shape = "cylinder";
    p.applications.push_back({make_id(), "Microsoft Flight Simulator 2020", "FlightSimulator.exe", "dx11",
                              p.outputs[0].id, false, false, "2020"});
    p.applications.push_back({make_id(), "Microsoft Flight Simulator 2024", "FlightSimulator2024.exe", "dx12",
                              p.outputs[0].id, false, false, "2024"});
    return p;
}
std::vector<std::string> validate(const Project& p) {
    std::vector<std::string> e;
    auto check = [&](bool ok, const std::string& message) {
        if (!ok)
            e.push_back(message);
    };
    auto range = [](double x, double a, double b) { return std::isfinite(x) && x >= a && x <= b; };
    auto rect = [&](const Rect& r) {
        return range(r.x, 0, 1) && range(r.y, 0, 1) && range(r.w, .000001, 1) && range(r.h, .000001, 1) &&
               r.x + r.w <= 1.000001 && r.y + r.h <= 1.000001;
    };
    check(p.schema_version == 2, "Unsupported project schema version");
    check(!p.id.empty(), "Project ID is empty");
    check(!p.sources.empty() && p.sources.size() <= 256, "Expected 1..256 sources");
    check(!p.outputs.empty() && p.outputs.size() <= 64, "Expected 1..64 outputs");
    std::unordered_set<std::string> ids;
    auto id = [&](const std::string& x) {
        check(!x.empty() && ids.insert(x).second, "Missing or duplicate ID: " + x);
    };
    for (auto& s : p.sources) {
        id(s.id);
        check(s.type == "pattern" || s.type == "desktop" || s.type == "window" || s.type == "image",
              "Unknown source type: " + s.type);
        check(s.width > 0 && s.width <= 32768 && s.height > 0 && s.height <= 32768,
              "Invalid source dimensions");
    }
    auto outputs = [&](const std::vector<Output>& os, bool register_ids) {
        std::unordered_set<std::string> local;
        for (auto& o : os) {
            if (register_ids)
                id(o.id);
            else
                check(!o.id.empty() && local.insert(o.id).second, "Duplicate preset output");
            check(o.width > 0 && o.width <= 16384 && o.height > 0 && o.height <= 16384,
                  "Invalid output dimensions");
            check(rect(o.viewport), "Invalid output viewport");
            check(o.surfaces.size() <= 256, "Too many surfaces on output");
            check(range(o.blend.left, 0, 1) && range(o.blend.right, 0, 1) && range(o.blend.top, 0, 1) &&
                      range(o.blend.bottom, 0, 1),
                  "Invalid blend width");
            check(range(o.blend.gamma, .1, 8) && range(o.blend.black_level, 0, 1), "Invalid blend response");
            check(o.blend.curve.size() >= 2 && o.blend.curve.size() <= 64, "Blend curve needs 2..64 points");
            double previous = -1;
            for (auto pt : o.blend.curve) {
                check(range(pt.x, 0, 1) && range(pt.y, 0, 1) && pt.x > previous,
                      "Blend curve must have increasing finite positions");
                previous = pt.x;
            }
            if (o.blend.curve.size() >= 2)
                check(o.blend.curve.front().x == 0 && o.blend.curve.back().x == 1,
                      "Blend curve must cover 0..1");
            auto& c = o.color;
            check(range(c.gamma, .1, 8) && range(c.brightness, -1, 1) && range(c.contrast, 0, 4) &&
                      range(c.saturation, 0, 4),
                  "Invalid color adjustment");
            for (int i = 0; i < 3; ++i)
                check(range(c.gain[i], 0, 4) && range(c.offset[i], -1, 1), "Invalid color channel");
            for (auto& f : o.surfaces) {
                if (register_ids)
                    id(f.id);
                else
                    check(!f.id.empty() && local.insert(f.id).second, "Duplicate preset surface ID");
                check(find_source(p, f.source_id) != nullptr, "Unknown surface source: " + f.source_id);
                check(rect(f.source_rect), "Source rectangle is outside the image");
                check(range(f.opacity, 0, 1), "Invalid opacity");
                auto& m = f.mesh;
                check(m.columns >= 2 && m.columns <= 64 && m.rows >= 2 && m.rows <= 64,
                      "Grid dimensions outside 2..64");
                check(m.points.size() == static_cast<size_t>(m.columns) * static_cast<size_t>(m.rows),
                      "Grid point count mismatch");
                for (auto pt : m.points)
                    check(range(pt.x, -4, 5) && range(pt.y, -4, 5), "Invalid mesh point");
                check(range(f.mask.feather, 0, 1), "Invalid mask feather");
                check(f.mask.polygon.empty() || (f.mask.polygon.size() >= 3 && f.mask.polygon.size() <= 4096),
                      "Mask polygon needs at least 3 points");
                for (auto pt : f.mask.polygon)
                    check(range(pt.x, -4, 5) && range(pt.y, -4, 5), "Invalid mask point");
                auto* s = find_source(p, f.source_id);
                if (f.enabled && o.enabled && s && s->type == "desktop" && !s->display.empty())
                    for (auto& target : os)
                        if (target.enabled && !target.display.empty())
                            check(s->display != target.display,
                                  "Capture feedback: a source display is also an active output");
            }
        }
    };
    outputs(p.outputs, true);
    check(p.presets.size() <= 64, "Too many presets");
    for (auto& preset : p.presets) {
        id(preset.id);
        check(!preset.outputs.empty() && preset.outputs.size() <= 64, "Invalid preset output count");
        outputs(preset.outputs, false);
    }
    check(range(p.screen.width, .001, 10000) && range(p.screen.height, .001, 10000) &&
              range(p.screen.radius, .001, 10000) && range(p.screen.arc_degrees, 1, 360),
          "Invalid screen geometry");
    check(std::isfinite(p.screen.eye.x) && std::isfinite(p.screen.eye.y) && std::isfinite(p.screen.eye.z),
          "Invalid eye position");
    for (auto& a : p.applications) {
        id(a.id);
        check(a.api == "dx11" || a.api == "dx12" || a.api == "capture", "Unsupported application API");
    }
    if (!p.startup_preset.empty())
        check(std::any_of(p.presets.begin(), p.presets.end(),
                          [&](const Preset& preset) { return preset.id == p.startup_preset; }),
              "Startup preset does not exist");
    return e;
}
Json project_json(const Project& p) {
    return {{"schema_version", p.schema_version},
            {"id", p.id},
            {"name", p.name},
            {"locked", p.locked},
            {"screen", p.screen},
            {"sources", p.sources},
            {"outputs", p.outputs},
            {"presets", p.presets},
            {"applications", p.applications},
            {"calibration", p.calibration},
            {"startup_preset", p.startup_preset}};
}
Project project_from_json(const Json& j) {
    Project p;
    p.schema_version = j.at("schema_version").get<int>();
    if (p.schema_version == 1)
        p.schema_version = 2;
    p.id = j.at("id");
    p.name = j.value("name", p.name);
    p.locked = j.value("locked", false);
    p.sources = j.at("sources").get<std::vector<Source>>();
    p.outputs = j.at("outputs").get<std::vector<Output>>();
    p.screen = j.value("screen", Screen{});
    p.presets = j.value("presets", std::vector<Preset>{});
    p.applications = j.value("applications", std::vector<ApplicationProfile>{});
    p.calibration = j.value("calibration", Json::object());
    p.startup_preset = j.value("startup_preset", std::string{});
    auto errors = validate(p);
    if (!errors.empty())
        throw std::runtime_error(errors.front());
    return p;
}
bool activate_preset(Project& p, const std::string& id) {
    for (auto& preset : p.presets)
        if (preset.id == id) {
            auto candidate = p;
            candidate.outputs = preset.outputs;
            if (!validate(candidate).empty())
                return false;
            p.outputs = preset.outputs;
            return true;
        }
    return false;
}
} // namespace ps
