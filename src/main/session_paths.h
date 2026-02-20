#pragma once

#include <filesystem>

namespace wcs::mainapp {

struct SessionPaths {
    std::filesystem::path root{};
    std::filesystem::path session_dir{};
    std::filesystem::path video_path{};
    std::filesystem::path video_meta_path{};
    std::filesystem::path input_path{};
};

SessionPaths CreateSessionPaths(const std::filesystem::path& output_root);

}  // namespace wcs::mainapp
