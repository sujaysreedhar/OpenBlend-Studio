// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "model.hpp"
#include <algorithm>
#include <deque>
#include <cmath>
#include <numeric>
namespace ps {
// IPC revisions are session-local, not persisted project schema versions.
class ProjectReplica {
    Json document_;
    std::uint64_t revision_{};

  public:
    explicit ProjectReplica(const Project& project) : document_(project_json(project)) {}
    std::uint64_t revision() const {
        return revision_;
    }
    const Json& document() const {
        return document_;
    }
    Project apply(const Json& command) {
        auto revision = command.at("revision").get<std::uint64_t>();
        if (revision <= revision_)
            throw std::runtime_error("Revision must increase");
        Json candidate;
        if (command.at("type") == "patch") {
            if (command.at("base_revision").get<std::uint64_t>() != revision_)
                throw std::runtime_error("resync_required");
            candidate = document_.patch(command.at("patch"));
        } else
            candidate = command.at("project");
        auto project = project_from_json(candidate); // Validation precedes either state change.
        document_ = project_json(project);
        revision_ = revision;
        return project;
    }
};
class Measurements {
    std::deque<double> samples_;
    std::uint64_t count_{};
    double sum_{}, maximum_{};

  public:
    void add(double value) {
        if (!std::isfinite(value) || value < 0)
            return;
        samples_.push_back(value);
        ++count_;
        sum_ += value;
        maximum_ = std::max(maximum_, value);
        if (samples_.size() > 4096)
            samples_.pop_front();
    }
    Json json() const {
        if (samples_.empty())
            return nullptr;
        std::vector<double> values(samples_.begin(), samples_.end());
        std::sort(values.begin(), values.end());
        auto percentile = [&](double q) {
            return values[std::min(values.size() - 1, size_t(q * (values.size() - 1)))];
        };
        return {{"samples", count_},
                {"mean", sum_ / double(count_)},
                {"max", maximum_},
                {"p50", percentile(.5)},
                {"p95", percentile(.95)},
                {"p99", percentile(.99)},
                {"percentile_window", values.size()}};
    }
};
} // namespace ps
