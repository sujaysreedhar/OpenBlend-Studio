// SPDX-License-Identifier: GPL-3.0-only
#include "ps/qualification.hpp"
#include <algorithm>
#include <cmath>
namespace ps {
void add_check(Json& report, const std::string& id, const std::string& status, const std::string& detail,
               const std::string& action) {
    if (status != "pass" && status != "fail" && status != "blocked" && status != "not_run")
        throw std::invalid_argument("Invalid check status");
    if (!report.contains("checks"))
        report["checks"] = Json::array();
    report["checks"].push_back({{"id", id}, {"status", status}, {"detail", detail}, {"action", action}});
}
void finish_checks(Json& report) {
    bool failed = false, incomplete = false;
    for (const auto& check : report.value("checks", Json::array())) {
        failed |= check.at("status") == "fail";
        incomplete |= check.at("status") == "blocked" || check.at("status") == "not_run";
    }
    report["status"] = failed ? "fail" : incomplete ? "incomplete" : "pass";
    report["release_qualified"] = false; // Physical and simulator evidence is separate.
}
void RunEvidence::observe(const Json& j, const Project& expected) {
    if (!j.contains("elapsed_seconds") || !j["elapsed_seconds"].is_number())
        throw std::invalid_argument("Renderer telemetry has no elapsed time; update the renderer");
    double seconds = j.at("elapsed_seconds");
    if (!std::isfinite(seconds) || seconds < 0 || (samples_ && seconds <= last_time_))
        throw std::invalid_argument("Non-increasing renderer telemetry timestamp");
    if (!samples_) {
        first_time_ = seconds;
        first_memory_ = j.value("working_set_bytes", 0ULL);
    }
    ++samples_;
    last_time_ = seconds;
    last_memory_ = j.value("working_set_bytes", 0ULL);
    peak_memory_ = std::max(peak_memory_, last_memory_);
    source_error_ |= !j.value("source_errors", Json::object()).empty();
    output_error_ |= !j.value("output_errors", Json::object()).empty();
    device_recovery_ |= j.value("device_recoveries", 0ULL) > 0;
    const auto source_states = j.value("source_stats", Json::object());
    for (const auto& [id, source] : source_states.items()) {
        if (!id.empty() && source.value("used", true)) {
            auto frames = source.value("frames", 0ULL);
            auto [it, created] = capture_counts_.try_emplace(id, frames, frames);
            source_error_ |= source.value("recoveries", 0ULL) > 0 || !source.value("error", "").empty();
            if (!created) {
                source_error_ |= frames < it->second.second;
                it->second.second = frames;
            }
        }
    }
    auto states = j.value("output_stats", Json::object());
    for (const auto& output : expected.outputs) {
        auto& e = outputs_[output.id];
        if (!states.contains(output.id)) {
            e.interrupted = true;
            continue;
        }
        auto state = states.at(output.id);
        if (e.first.is_null()) {
            e.first = state;
            e.first_time = seconds;
        }
        if (!e.last.is_null() &&
            (state.value("resource_generation", 0ULL) != e.last.value("resource_generation", 0ULL) ||
             state.value("presents", 0ULL) < e.last.value("presents", 0ULL)))
            e.interrupted = true;
        e.error |= !state.value("error", "").empty() || state.value("failures", 0ULL) > 0;
        e.occluded |= state.value("occluded", 0ULL) > 0;
        e.wrong_size |= state.value("width", 0) != output.width || state.value("height", 0) != output.height;
        e.last = std::move(state);
        e.last_time = seconds;
    }
}
Json RunEvidence::report(const Project& expected, double requested_seconds, double hz, bool simulated,
                         bool cancelled, bool clean_stop, bool blackout) const {
    Json result = {
        {"kind", "output-run"},
        {"schema_version", 1},
        {"simulated", simulated},
        {"requested_seconds", requested_seconds},
        {"observed_seconds", last_time_ - first_time_},
        {"samples", samples_},
        {"target_hz", hz},
        {"cancelled", cancelled},
        {"memory",
         {{"first_bytes", first_memory_}, {"last_bytes", last_memory_}, {"peak_bytes", peak_memory_}}},
        {"outputs", Json::object()},
        {"physical_latency_ms", nullptr},
        {"dropped_frames", nullptr}};
    bool duration = samples_ >= 2 && last_time_ - first_time_ >= requested_seconds - .05;
    add_check(result, "duration",
              cancelled  ? "blocked"
              : duration ? "pass"
                         : "fail",
              cancelled ? "Run cancelled; partial evidence retained" : "Measured interval excludes warm-up");
    add_check(result, "source_health", source_error_ ? "fail" : "pass",
              "Capture and asset errors across all samples");
    add_check(result, "output_health", output_error_ || device_recovery_ ? "fail" : "pass",
              "Missing outputs and device recoveries during the measured interval");
    for (const auto& output : expected.outputs) {
        auto found = outputs_.find(output.id);
        if (found == outputs_.end() || found->second.first.is_null() || found->second.last.is_null()) {
            add_check(result, "output/" + output.id, "blocked", "No output samples",
                      "Check display assignment and start a new run");
            continue;
        }
        auto& e = found->second;
        double elapsed = e.last_time - e.first_time;
        double rate = elapsed > 0 && !e.interrupted
                          ? double(e.last.value("presents", 0ULL) - e.first.value("presents", 0ULL)) / elapsed
                          : 0;
        bool healthy = !e.interrupted && !e.error && !e.occluded && !e.wrong_size && rate >= hz * .95;
        result["outputs"][output.id] = {
            {"name", output.name},           {"successful_present_calls_per_second", rate},
            {"interrupted", e.interrupted},  {"ever_occluded", e.occluded},
            {"size_mismatch", e.wrong_size}, {"last_telemetry", e.last}};
        add_check(result, "output/" + output.id,
                  simulated ? "not_run"
                  : healthy ? "pass"
                            : "fail",
                  simulated ? "Simulation exercises software only"
                            : "Requires requested size, no interruption/occlusion/errors and at least 95% of "
                              "target Present-call rate",
                  healthy ? ""
                          : "Use visible displays at the requested mode; inspect the recorded telemetry");
    }
    auto routed = [&](const Source& source) {
        for (const auto& output : expected.outputs)
            if (output.enabled)
                for (const auto& surface : output.surfaces)
                    if (surface.source_id == source.id)
                        return true;
        return false;
    };
    bool uses_capture = std::any_of(expected.sources.begin(), expected.sources.end(), [&](const auto& s) {
        return (s.type == "window" || s.type == "desktop") && routed(s);
    });
    bool fresh_capture = uses_capture;
    for (const auto& source : expected.sources)
        if ((source.type == "window" || source.type == "desktop") && routed(source)) {
            bool found = false;
            for (const auto& [key, counts] : capture_counts_)
                if (key.ends_with("/" + source.id)) {
                    found = true;
                    fresh_capture &= counts.second > counts.first;
                }
            fresh_capture &= found;
        }
    add_check(result, "live_capture",
              !uses_capture                     ? "not_run"
              : fresh_capture && !source_error_ ? "pass"
                                                : "blocked",
              uses_capture ? "Capture frame arrival during the run" : "Project uses no live capture source",
              "Use a moving desktop/window source to exercise capture");
    add_check(result, "blackout_control", blackout ? "pass" : "fail",
              "Worker acknowledged and reported blackout before stopping");
    add_check(result, "graceful_stop", clean_stop ? "pass" : "fail", "Worker exited normally after Stop");
    add_check(result, "physical_delivery", "not_run",
              "Present-call rate and source age do not measure physical latency or dropped display frames",
              "Measure the complete capture-to-display path externally; retain simulator and "
              "physical-display test records");
    finish_checks(result);
    return result;
}
} // namespace ps
