// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "windows.hpp"
namespace ps::win {
Json check_setup(const Project&, const std::filesystem::path& project_path, bool probe_capture,
                 double probe_seconds = 3, const std::function<bool()>& cancelled = {});
}
