// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "windows.hpp"
#include "gpu_timing.hpp"
#include <chrono>
namespace ps::win {
// Owned and used only by the render thread; one capture copy per source/frame.
class SourceRegistry {
    struct Entry {
        Source source;
        std::unique_ptr<DesktopCapture> desktop;
        std::unique_ptr<WindowCapture> window;
        Com<ID3D11ShaderResourceView> image;
        HWND hwnd{};
        std::chrono::steady_clock::time_point retry{};
        std::string error;
        std::uint64_t frames{}, failures{};
        bool used{};
        std::int64_t timestamp{};
        Measurements arrival_age;
    };
    Com<ID3D11Device> device_;
    Renderer loader_;
    std::filesystem::path assets_;
    std::unordered_map<std::string, Entry> entries_;
    std::unordered_map<std::string, SourceTexture> frame_;

  public:
    SourceRegistry(ID3D11Device* d, std::filesystem::path assets)
        : device_(d), loader_(d), assets_(std::move(assets)) {}
    void configure(const Project& project) {
        std::erase_if(entries_, [&](const auto& entry) { return !find_source(project, entry.first); });
        for (auto& source : project.sources) {
            auto& e = entries_[source.id];
            if (Json(e.source) != Json(source)) {
                e = {};
                e.source = source;
            }
            e.used = false;
        }
        frame_.clear();
    }
    void begin_frame() {
        frame_.clear();
    }
    SourceTexture get(const std::string& id) {
        if (auto it = frame_.find(id); it != frame_.end())
            return it->second;
        auto it = entries_.find(id);
        if (it == entries_.end())
            return {nullptr, 2};
        auto& e = it->second;
        e.used = true;
        auto& s = e.source;
        SourceTexture result{nullptr, 2};
        auto now = std::chrono::steady_clock::now();
        if (now < e.retry)
            return result;
        try {
            bool fresh = false;
            if (s.type == "pattern")
                result.pattern = pattern_index(s.pattern);
            else if (s.type == "image") {
                if (!e.image)
                    e.image = loader_.load_image(resolve_asset(assets_ / "project.json", s.image));
                result = {e.image.get(), -1};
            } else if (s.type == "window") {
                if (!e.window || !IsWindow(e.hwnd)) {
                    e.window.reset();
                    e.hwnd = resolve_window(s);
                    e.window = std::make_unique<WindowCapture>(device_.get(), e.hwnd, s.cursor);
                }
                fresh = e.window->update();
                e.timestamp = e.window->timestamp();
                result = {e.window->view(), -1};
            } else if (s.type == "desktop") {
                if (!e.desktop)
                    e.desktop = std::make_unique<DesktopCapture>(device_.get(), s.display, s.cursor);
                fresh = e.desktop->update();
                e.timestamp = e.desktop->timestamp();
                result = {e.desktop->view(), -1};
            }
            if (fresh) {
                ++e.frames;
                if (e.timestamp > 0)
                    e.arrival_age.add(double(qpc_100ns() - e.timestamp) / 10000.);
            }
            if (s.type == "pattern" || result.view)
                e.error.clear();
            else
                e.error = "Waiting for visible source frames";
        } catch (const std::exception& error) {
            e.error = error.what();
            ++e.failures;
            e.window.reset();
            e.desktop.reset();
            e.retry = now + std::chrono::seconds(1);
        }
        frame_[id] = result;
        return result;
    }
    Json errors() const {
        Json result = Json::object();
        for (auto& [id, e] : entries_)
            if (!e.error.empty())
                result[id] = e.error;
        return result;
    }
    Json telemetry() const {
        Json result = Json::object();
        for (auto& [id, e] : entries_)
            result[id] = {{"frames", e.frames},
                          {"source_id", id},
                          {"type", e.source.type},
                          {"used", e.used},
                          {"last_frame_age_ms", e.timestamp > 0
                                                    ? Json(double(qpc_100ns() - e.timestamp) / 10000.)
                                                    : Json(nullptr)},
                          {"recoveries", e.failures},
                          {"capture_arrival_age_ms", e.arrival_age.json()},
                          {"last_timestamp_100ns", e.timestamp},
                          {"error", e.error}};
        return result;
    }
    double latest_age_ms() const {
        double age = 0;
        for (auto& [id, frame] : frame_)
            if (frame.view && entries_.at(id).timestamp > 0)
                age = std::max(age, double(qpc_100ns() - entries_.at(id).timestamp) / 10000.);
        return age;
    }
};
} // namespace ps::win
