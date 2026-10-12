// SPDX-License-Identifier: GPL-3.0-only
#include "ps/calibration.hpp"
#include "ps/color.hpp"
#include "ps/geometry.hpp"
#include "ps/model.hpp"
#include "ps/runtime.hpp"
#include "ps/simulator.hpp"
#include <cmath>
#include <fstream>
#include <functional>
#include <iostream>
int failures = 0, checks = 0;
void expect(bool ok, const char* message) {
    ++checks;
    if (!ok) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}
void near(double a, double b, double e, const char* message) {
    expect(std::abs(a - b) <= e, message);
}
void rejects(std::function<void()> f, const char* message) {
    bool threw = false;
    try {
        f();
    } catch (const std::exception&) {
        threw = true;
    }
    expect(threw, message);
}
int main() {
    using namespace ps;
    try {
        for (bool curved : {false, true}) {
            auto m = identity_mesh(7, 5);
            m.curved = curved;
            for (int i = 0; i <= 20; ++i) {
                double u = i / 20., v = (20 - i) / 20.;
                auto p = evaluate_mesh(m, u, v);
                near(p.x, u, 1e-12, "identity X");
                near(p.y, v, 1e-12, "identity Y");
            }
            for (auto& p : m.points) {
                p.x = .1 + .7 * p.x;
                p.y = .2 + .6 * p.y;
            }
            auto q = evaluate_mesh(m, .3, .6);
            near(q.x, .31, 1e-10, "affine grid");
            auto inverse = inverse_map(m, q);
            expect(bool(inverse), "inverse exists");
            if (inverse) {
                near(inverse->x, .3, 1e-6, "inverse U");
                near(inverse->y, .6, 1e-6, "inverse V");
            }
            expect(!inverse_map(m, {-1, -1}), "outside has no inverse");
            auto resized = resample_mesh(m, 13, 9);
            near(evaluate_mesh(resized, .3, .6).x, .31, 1e-7, "resample preserves mapping");
        }
        auto mesh = identity_mesh(5, 5);
        auto row = select_points(mesh, 12, SelectionMode::Row);
        expect(row.size() == 5, "row selection");
        move_points(mesh, row, {.1, .2}, AxisLock::X);
        near(mesh.points[12].x, .6, 1e-12, "axis locked move X");
        near(mesh.points[12].y, .5, 1e-12, "axis locked move Y");
        interpolate_interior(mesh);
        near(mesh.points[12].x, .6, 1e-12, "boundary interpolation");
        Mask mask;
        mask.enabled = true;
        mask.polygon = {{.2, .2}, {.8, .2}, {.8, .8}, {.2, .8}};
        near(mask_alpha(mask, {.5, .5}), 1, 0, "polygon inside");
        near(mask_alpha(mask, {0, 0}), 0, 0, "polygon outside");
        mask.feather = .1;
        near(mask_alpha(mask, {.2, .5}), .5, 1e-8, "feather edge");
        mask.inverted = true;
        near(mask_alpha(mask, {0, 0}), 1, 0, "mask inversion");
        Blend left, right;
        left.right = .2;
        right.left = .2;
        for (int i = 0; i <= 100; ++i) {
            double t = i / 100.;
            near(blend_weight(left, {.8 + .2 * t, .5}) + blend_weight(right, {.2 * t, .5}), 1, 1e-9,
                 "complementary linear blends");
        }
        for (int i = 0; i <= 255; ++i)
            near(linear_to_srgb(srgb_to_linear(i / 255.)), i / 255., 1e-9, "sRGB round trip");
        auto p = example_project(3);
        expect(validate(p).empty(), "example valid");
        auto json = project_json(p);
        expect(project_json(project_from_json(json)) == json, "JSON project roundtrip");
        json["schema_version"] = 999;
        rejects([&] { project_from_json(json); }, "reject unknown schema");
        json = project_json(p);
        json["outputs"][0]["surfaces"][0]["mesh"]["points"] = Json::array();
        rejects([&] { project_from_json(json); }, "reject malformed mesh");
        p.sources[0].type = "desktop";
        p.sources[0].display = "display-a";
        p.outputs[0].display = "display-a";
        expect(!validate(p).empty(), "reject capture feedback");
        p = example_project(16);
        for (auto& o : p.outputs)
            for (int i = 1; i < 16; ++i) {
                auto f = o.surfaces[0];
                f.id = make_id();
                o.surfaces.push_back(f);
            }
        expect(validate(p).empty(), "16 outputs and 256 surfaces");
        expect(project_from_json(project_json(p)).outputs[15].surfaces.size() == 16,
               "large project roundtrip");
        auto frames = gray_patterns(37, 23);
        auto points = decode_gray(frames, 37, 23);
        expect(points.size() == 37 * 23, "non-power-of-two gray decode");
        for (auto& point : points) {
            near(point.camera.x, point.projector.x, 0, "Gray X");
            near(point.camera.y, point.projector.y, 0, "Gray Y");
        }
        frames[0].pixels[0] = frames[1].pixels[0];
        expect(decode_gray(frames, 37, 23).size() == 37 * 23 - 1, "reject low contrast");
        std::vector<Correspondence> samples;
        for (int y = 0; y < 8; ++y)
            for (int x = 0; x < 10; ++x)
                samples.push_back({{x * 12. + 3, y * 7. + 9}, {x * 10., y * 10.}, 1});
        samples.push_back({{999, -333}, {25, 25}, 1});
        auto h = fit_homography(samples, .1);
        expect(h.inliers == 80, "RANSAC rejects outlier");
        near(h.rms, 0, 1e-7, "homography residual");
        auto t = transform(h, {20, 30});
        near(t.x, 27, 1e-6, "homography X");
        near(t.y, 30, 1e-6, "homography Y");
        auto photometry = photometric_fit(std::vector<double>{0, .25, .5, .75, 1},
                                          std::vector<double>{.1, .15, .3, .55, .9});
        near(photometry["gamma"].get<double>(), 2, 1e-8, "response gamma fit");
        auto tmp = std::filesystem::current_path() / ("projection-tests-" + make_id());
        std::filesystem::create_directories(tmp);
        p = example_project();
        auto v1 = project_json(p);
        v1["schema_version"] = 1;
        expect(project_from_json(v1).schema_version == 2, "v1 projects migrate in memory");
        auto migrated_path = tmp / "v1.json";
        atomic_write(migrated_path, v1.dump());
        save_project(project_from_json(v1), migrated_path);
        expect(std::filesystem::exists(migrated_path.string() + ".v1.bak"), "preserve v1 migration backup");
        ProjectReplica replica(p);
        auto changed = project_json(p);
        changed["outputs"][0]["color"]["gamma"] = 1.2;
        Json patch = {{"type", "patch"},
                      {"revision", 1},
                      {"base_revision", 0},
                      {"patch", Json::diff(project_json(p), changed)}};
        near(replica.apply(patch).outputs[0].color.gamma, 1.2, 0, "revisioned delta applies");
        rejects([&] { replica.apply(patch); }, "duplicate revision rejected");
        patch["revision"] = 2;
        rejects([&] { replica.apply(patch); }, "stale delta requires resync");
        patch["base_revision"] = 1;
        patch["patch"] =
            Json::array({{{"op", "replace"}, {"path", "/outputs/0/color/gamma"}, {"value", -4}}});
        rejects([&] { replica.apply(patch); }, "invalid delta rejected atomically");
        expect(replica.revision() == 1 && replica.document() == changed, "invalid delta preserves replica");
        replica.apply({{"type", "project"}, {"revision", 3}, {"project", project_json(p)}});
        expect(replica.revision() == 3, "full snapshot recovers revision gap");
        auto path = tmp / "project.json";
        save_project(p, path);
        p.name = "Changed";
        save_project(p, path);
        expect(load_project(path).name == "Changed", "atomic save");
        expect(load_project(path.string() + ".bak").name == "First light", "backup preserved");
        auto views = export_views(example_project(3));
        expect(views["views"].size() == 3, "three view export");
        near(views["views"][1]["yaw_degrees"].get<double>(), 0, 1e-9, "center view yaw");
        std::string original = "[CameraDefinition.0]\nTitle = Pilot\nInitialZoom = 0.5\n";
        auto replacement = msfs_camera_patch(original, 30, 0, .7);
        auto modern = msfs_camera_patch(original, 30, 0, .7, "2024");
        expect(modern.find("MSFS 2024") != std::string::npos &&
                   modern.find("InitialPbh = 0, 0, 30") != std::string::npos,
               "MSFS 2024 camera export");
        rejects([&] { msfs_camera_patch(original, 30, 0, .7, "invalid"); }, "Unknown simulator rejected");
        expect(replacement.starts_with(original), "camera patch preserves originals");
        expect(replacement.find("[CameraDefinition.1]") != std::string::npos, "next camera index");
        atomic_write(tmp / "cameras.cfg", original);
        apply_msfs_camera(tmp / "cameras.cfg", original, replacement);
        rejects([&] { apply_msfs_camera(tmp / "cameras.cfg", original, replacement); },
                "stale preview rejected");
        restore_backup(tmp / "cameras.cfg");
        // Only remove this test's newly created, unique temporary directory.
        std::filesystem::remove_all(tmp);
    } catch (const std::exception& e) {
        ++failures;
        std::cerr << "EXCEPTION: " << e.what() << '\n';
    }
    std::cout << checks << " checks, " << failures << " failures\n";
    return failures ? 1 : 0;
}
