// SPDX-License-Identifier: GPL-3.0-only
#include "ps/qualification.hpp"
#include <iostream>
using namespace ps;
int main() {
    int failures = 0;
    auto require = [&](bool valid, const char* message) {
        if (!valid) {
            ++failures;
            std::cerr << message << '\n';
        }
    };
    auto project = example_project(1);
    auto id = project.outputs[0].id;
    auto sample = [&](double time, std::uint64_t frames) {
        return Json{{"elapsed_seconds", time},
                    {"working_set_bytes", 1024},
                    {"source_errors", Json::object()},
                    {"output_errors", Json::object()},
                    {"source_stats", Json::object()},
                    {"output_stats",
                     {{id,
                       {{"resource_generation", 1},
                        {"presents", frames},
                        {"occluded", 0},
                        {"failures", 0},
                        {"width", project.outputs[0].width},
                        {"height", project.outputs[0].height},
                        {"error", ""}}}}}};
    };
    auto status = [&](const Json& report, const std::string& key) {
        for (auto& c : report.at("checks"))
            if (c.at("id") == key)
                return c.at("status").get<std::string>();
        return std::string();
    };
    auto assess = [&](Json first, Json last, bool sim = false) {
        RunEvidence e;
        e.observe(first, project);
        e.observe(last, project);
        return e.report(project, 10, 60, sim, false, true, true);
    };
    auto first = sample(5, 300), last = sample(15, 900);
    auto normal = assess(first, last);
    require(status(normal, "output/" + id) == "pass", "Stable output should pass submission checks");
    require(normal.at("status") == "incomplete" && !normal.at("release_qualified").get<bool>(),
            "Software evidence falsely qualified physical delivery");
    require(normal.at("dropped_frames").is_null(), "Missing drops must remain unknown");
    auto occluded = last;
    occluded["output_stats"][id]["occluded"] = 1;
    require(status(assess(first, occluded), "output/" + id) == "fail", "Occluded output falsely passed");
    auto rebuilt = last;
    rebuilt["output_stats"][id]["resource_generation"] = 2;
    require(status(assess(first, rebuilt), "output/" + id) == "fail", "Counter reset falsely passed");
    auto size = last;
    size["output_stats"][id]["width"] = 384;
    require(status(assess(first, size), "output/" + id) == "fail",
            "Small simulation falsely qualified 1080p");
    auto slow = sample(15, 700);
    require(status(assess(first, slow), "output/" + id) == "fail", "Slow output falsely passed");
    auto missing = last;
    missing["output_stats"] = Json::object();
    require(status(assess(first, missing), "output/" + id) != "pass", "Missing output falsely passed");
    require(status(assess(first, last, true), "output/" + id) == "not_run",
            "Simulation falsely qualified hardware");
    RunEvidence partial;
    partial.observe(first, project);
    require(status(partial.report(project, 10, 60, false, false, true, true), "duration") == "fail",
            "Partial evidence falsely passed endurance");
    require(status(partial.report(project, 10, 60, false, true, true, true), "duration") == "blocked",
            "Cancelled run not identified");
    bool threw = false;
    try {
        partial.observe(first, project);
    } catch (...) {
        threw = true;
    }
    require(threw, "Duplicate telemetry timestamp accepted");
    project.sources[0].type = "window";
    auto source = project.sources[0].id;
    first["source_stats"]["gpu/" + source] = {{"frames", 40}, {"used", true}};
    last["source_stats"]["gpu/" + source] = {{"frames", 40}, {"used", true}};
    require(status(assess(first, last), "live_capture") == "blocked",
            "Warm-up frames falsely passed live capture");
    last["source_stats"]["gpu/" + source]["frames"] = 60;
    require(status(assess(first, last), "live_capture") == "pass",
            "Fresh capture frames were not recognized");
    first["source_stats"]["unused-gpu/" + source] = {{"frames", 0}, {"used", false}};
    last["source_stats"]["unused-gpu/" + source] = {{"frames", 0}, {"used", false}};
    require(status(assess(first, last), "live_capture") == "pass",
            "Unused GPU source falsely blocked capture");
    auto recovered = last;
    recovered["source_stats"]["gpu/" + source]["recoveries"] = 1;
    require(status(assess(first, recovered), "source_health") == "fail",
            "Transient capture recovery was lost between samples");
    auto reset = last;
    reset["source_stats"]["gpu/" + source]["frames"] = 1;
    require(status(assess(first, reset), "source_health") == "fail", "Capture counter reset was accepted");
    auto extra = project.sources[0];
    extra.id = "unrouted";
    project.sources.push_back(extra);
    require(status(assess(first, last), "live_capture") == "pass",
            "Unrouted capture source blocked measurement");
    Json report;
    add_check(report, "missing", "blocked", "Unavailable");
    finish_checks(report);
    require(report.at("status") == "incomplete", "Blocked setup was reported as passed");
    std::cout << "Qualification evaluator failures: " << failures << '\n';
    return failures ? 1 : 0;
}
