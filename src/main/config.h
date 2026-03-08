#pragma once

#include <Windows.h>

#include <cstdint>
#include <filesystem>
#include <string>

namespace wcs::mainapp {

enum class CaptureCodec {
    H264,
    HEVC
};

struct AppConfig {
    UINT hotkey_modifiers = MOD_CONTROL | MOD_ALT;
    UINT hotkey_vk = VK_F9;

    uint32_t capture_fps = 60;
    uint32_t capture_bitrate = 12000000;
    // Legacy global resolution (kept for backward compatibility with older config.ini).
    uint32_t capture_width = 0;
    uint32_t capture_height = 0;
    uint32_t capture_primary_width = 0;
    uint32_t capture_primary_height = 0;
    uint32_t capture_secondary_width = 0;
    uint32_t capture_secondary_height = 0;
    CaptureCodec capture_codec = CaptureCodec::H264;

    size_t input_queue_capacity = 32768;
    size_t input_batch_size = 512;
    int input_flush_interval_ms = 10;
    bool input_diagnostic_mode = false;
    int ui_source_mode = 0;
    std::string ui_primary_source_id = "monitor_primary";
    std::string ui_secondary_source_id = "none";

    std::filesystem::path output_root = "captures";
};

AppConfig LoadConfig(const std::filesystem::path& path);
bool SaveDefaultConfig(const std::filesystem::path& path);
bool SaveConfig(const std::filesystem::path& path, const AppConfig& config);
std::string HotkeyToString(UINT modifiers, UINT vk);
const char* CaptureCodecToString(CaptureCodec codec);

}  // namespace wcs::mainapp
