// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "model.hpp"
#include <map>
namespace ps {
// A missing measurement is incomplete evidence, never a passing zero.
void add_check(Json& report, const std::string& id, const std::string& status, const std::string& detail,
               const std::string& action = "");
void finish_checks(Json& report);
class RunEvidence {
    struct OutputEvidence {
        Json first, last;
        double first_time{}, last_time{};
        bool interrupted{}, error{}, occluded{}, wrong_size{};
    };
    std::map<std::string, OutputEvidence> outputs_;
    std::uint64_t samples_{}, first_memory_{}, last_memory_{}, peak_memory_{};
    double first_time_{}, last_time_{};
    std::map<std::string, std::pair<std::uint64_t, std::uint64_t>> capture_counts_;
    bool source_error_{}, output_error_{}, device_recovery_{};

  public:
    void observe(const Json& sample, const Project& expected);
    Json report(const Project& expected, double requested_seconds, double target_hz, bool simulated,
                bool cancelled, bool clean_stop, bool blackout_confirmed) const;
};
} // namespace ps
