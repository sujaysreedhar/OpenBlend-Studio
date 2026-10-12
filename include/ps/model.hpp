// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <array>
#include <cstdint>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>
namespace ps {
using Json = nlohmann::json;
struct Vec2 {
    double x{}, y{};
    bool operator==(const Vec2&) const = default;
};
struct Vec3 {
    double x{}, y{}, z{};
};
struct Rect {
    double x{}, y{}, w{1}, h{1};
};
struct Color {
    double r{}, g{}, b{}, a{1};
};
struct Mesh {
    int columns{5}, rows{5};
    bool curved{false};
    std::vector<Vec2> points;
    bool operator==(const Mesh&) const = default;
};
struct Mask {
    bool enabled{false}, inverted{false};
    double feather{0};
    std::string bitmap;
    std::vector<Vec2> polygon;
    bool operator==(const Mask&) const = default;
};
struct Blend {
    double left{}, right{}, top{}, bottom{};
    double gamma{1}, black_level{};
    std::vector<Vec2> curve{{0, 0}, {1, 1}};
};
struct ColorSettings {
    std::array<double, 3> gain{1, 1, 1}, offset{0, 0, 0};
    double gamma{1}, brightness{0}, contrast{1}, saturation{1};
};
struct Source {
    std::string id, name, type{"pattern"}, pattern{"grid"}, display;
    std::string window_title, image;
    std::uint64_t window_handle{};
    int width{1920}, height{1080};
    bool cursor{true};
    std::string executable, window_class;
};
struct Surface {
    std::string id, name, source_id;
    bool enabled{true};
    double opacity{1};
    Rect source_rect;
    Mesh mesh;
    Mask mask;
};
struct Output {
    std::string id, name, display;
    int width{1920}, height{1080};
    bool enabled{true};
    Rect viewport;
    Blend blend;
    ColorSettings color;
    std::vector<Surface> surfaces;
};
struct Screen {
    std::string shape{"plane"};
    double width{3}, height{1.6875}, radius{2.5}, arc_degrees{180};
    Vec3 eye{0, 0, 2.5};
};
struct ApplicationProfile {
    std::string id, name, executable, api{"dx11"};
    std::string output_id;
    bool enabled{false}, excluded{false};
    std::string simulator_version;
};
struct Preset {
    std::string id, name;
    std::vector<Output> outputs;
};
struct Project {
    int schema_version{2};
    std::string id, name{"Untitled installation"};
    bool locked{false};
    Screen screen;
    std::vector<Source> sources;
    std::vector<Output> outputs;
    std::vector<Preset> presets;
    std::vector<ApplicationProfile> applications;
    Json calibration = Json::object();
    std::string startup_preset;
};
std::string make_id();
Mesh identity_mesh(int columns = 5, int rows = 5);
Project example_project(int projectors = 1, bool irregular = false);
std::vector<std::string> validate(const Project&);
Json project_json(const Project&);
Project project_from_json(const Json&);
Project load_project(const std::filesystem::path&);
void save_project(const Project&, const std::filesystem::path&);
void atomic_write(const std::filesystem::path&, const std::string&);
std::filesystem::path resolve_asset(const std::filesystem::path& project, const std::string& asset);
const Source* find_source(const Project&, const std::string& id);
bool activate_preset(Project&, const std::string& id);
void to_json(Json&, const Vec2&);
void from_json(const Json&, Vec2&);
void to_json(Json&, const Vec3&);
void from_json(const Json&, Vec3&);
void to_json(Json&, const Rect&);
void from_json(const Json&, Rect&);
void to_json(Json&, const Mesh&);
void from_json(const Json&, Mesh&);
void to_json(Json&, const Mask&);
void from_json(const Json&, Mask&);
void to_json(Json&, const Blend&);
void from_json(const Json&, Blend&);
void to_json(Json&, const ColorSettings&);
void from_json(const Json&, ColorSettings&);
void to_json(Json&, const Surface&);
void from_json(const Json&, Surface&);
void to_json(Json&, const Source&);
void from_json(const Json&, Source&);
void to_json(Json&, const Output&);
void from_json(const Json&, Output&);
void to_json(Json&, const Screen&);
void from_json(const Json&, Screen&);
void to_json(Json&, const ApplicationProfile&);
void from_json(const Json&, ApplicationProfile&);
void to_json(Json&, const Preset&);
void from_json(const Json&, Preset&);
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(Vec2, x, y)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(Vec3, x, y, z)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(Rect, x, y, w, h)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(Mesh, columns, rows, curved, points)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(Mask, enabled, inverted, feather, bitmap, polygon)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(Blend, left, right, top, bottom, gamma, black_level, curve)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(ColorSettings, gain, offset, gamma, brightness, contrast,
                                                saturation)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(Source, id, name, type, pattern, display, window_title, image,
                                                window_handle, width, height, cursor, executable,
                                                window_class)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(Surface, id, name, source_id, enabled, opacity, source_rect,
                                                mesh, mask)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(Output, id, name, display, width, height, enabled, viewport,
                                                blend, color, surfaces)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(Screen, shape, width, height, radius, arc_degrees, eye)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(ApplicationProfile, id, name, executable, api, output_id,
                                                enabled, excluded, simulator_version)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(Preset, id, name, outputs)
} // namespace ps
