// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "model.hpp"
namespace ps {
struct ViewFrustum {
    std::string output_id;
    double left{}, right{}, bottom{}, top{}, near_clip{.1}, far_clip{10000}, yaw{}, pitch{};
};
std::vector<ViewFrustum> screen_views(const Project&);
Json export_views(const Project&);
std::string msfs_camera_patch(const std::string& original, double yaw, double pitch, double zoom,
                              const std::string& version = "2020");
void apply_msfs_camera(const std::filesystem::path&, const std::string& expected_original,
                       const std::string& replacement);
void restore_backup(const std::filesystem::path&);
} // namespace ps
