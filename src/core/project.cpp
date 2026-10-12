// SPDX-License-Identifier: GPL-3.0-only
#include "ps/model.hpp"
#include <fstream>
#include <sstream>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
namespace ps {
void atomic_write(const std::filesystem::path& path, const std::string& text) {
    if (!path.parent_path().empty())
        std::filesystem::create_directories(path.parent_path());
    auto temp = path;
    temp += ".tmp-" + make_id();
    try {
        {
            std::ofstream out(temp, std::ios::binary | std::ios::trunc);
            out.exceptions(std::ios::badbit | std::ios::failbit);
            out.write(text.data(), std::streamsize(text.size()));
            out.flush();
        }
        if (std::filesystem::exists(path)) {
            auto backup = path;
            backup += ".bak";
#ifdef _WIN32
            if (!CopyFileW(path.c_str(), backup.c_str(), FALSE))
                throw std::runtime_error("Cannot update backup: " + std::to_string(GetLastError()));
#else
            std::filesystem::copy_file(path, backup, std::filesystem::copy_options::overwrite_existing);
#endif
        }
#ifdef _WIN32
        if (!MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            throw std::runtime_error("Could not atomically replace file: " + std::to_string(GetLastError()));
#else
        std::filesystem::rename(temp, path);
#endif
    } catch (...) {
        std::error_code ec;
        std::filesystem::remove(temp, ec);
        throw;
    }
}
void save_project(const Project& p, const std::filesystem::path& path) {
    auto e = validate(p);
    if (!e.empty())
        throw std::runtime_error(e.front());
    if (std::filesystem::exists(path)) {
        std::ifstream previous(path, std::ios::binary);
        auto old = Json::parse(previous, nullptr, false);
        if (!old.is_discarded() && old.value("schema_version", 0) == 1) {
            auto migration_backup = path;
            migration_backup += ".v1.bak";
            // Keep the first v1 original permanently, independently of rotating .bak saves.
            if (!std::filesystem::exists(migration_backup))
                std::filesystem::copy_file(path, migration_backup);
        }
    }
    atomic_write(path, project_json(p).dump(2) + "\n");
}
Project load_project(const std::filesystem::path& path) {
    if (std::filesystem::file_size(path) > 32 * 1024 * 1024)
        throw std::runtime_error("Project exceeds 32 MiB limit");
    std::ifstream in(path, std::ios::binary);
    if (!in)
        throw std::runtime_error("Cannot open project");
    Json j;
    in >> j;
    return project_from_json(j);
}
std::filesystem::path resolve_asset(const std::filesystem::path& project, const std::string& asset) {
    std::filesystem::path p = std::filesystem::u8path(asset);
    return p.is_absolute() ? p : project.parent_path() / p;
}
} // namespace ps
