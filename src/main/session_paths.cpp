#include "main/session_paths.h"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace wcs::mainapp {

namespace {

std::string MakeSessionName() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t now_t = std::chrono::system_clock::to_time_t(now);
    std::tm local_tm{};
    localtime_s(&local_tm, &now_t);

    std::ostringstream oss;
    oss << std::put_time(&local_tm, "%Y%m%d_%H%M%S");
    return oss.str();
}

}  // namespace

SessionPaths CreateSessionPaths(const std::filesystem::path& output_root) {
    SessionPaths paths;
    paths.root = output_root;
    paths.session_dir = output_root / MakeSessionName();
    paths.video_path = paths.session_dir / "video.mp4";
    paths.video_meta_path = paths.session_dir / "video_meta.json";
    paths.input_path = paths.session_dir / "input.jsonl";
    return paths;
}

}  // namespace wcs::mainapp
