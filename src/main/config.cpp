#include "main/config.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <string>

namespace wcs::mainapp {

namespace {

std::string Trim(std::string text) {
    auto not_space = [](unsigned char ch) { return !std::isspace(ch); };
    text.erase(text.begin(), std::find_if(text.begin(), text.end(), not_space));
    text.erase(std::find_if(text.rbegin(), text.rend(), not_space).base(), text.end());
    return text;
}

std::string Upper(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char ch) {
        return static_cast<char>(std::toupper(ch));
    });
    return text;
}

UINT ParseModifiers(const std::string& text) {
    UINT mods = 0;
    std::stringstream ss(text);
    std::string token;
    while (std::getline(ss, token, '+')) {
        const std::string t = Upper(Trim(token));
        if (t == "CTRL" || t == "CONTROL") {
            mods |= MOD_CONTROL;
        } else if (t == "ALT") {
            mods |= MOD_ALT;
        } else if (t == "SHIFT") {
            mods |= MOD_SHIFT;
        } else if (t == "WIN" || t == "WINDOWS") {
            mods |= MOD_WIN;
        }
    }
    return mods;
}

UINT ParseVk(const std::string& text) {
    const std::string t = Upper(Trim(text));
    if (t.size() == 1) {
        const char ch = t[0];
        if ((ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9')) {
            return static_cast<UINT>(ch);
        }
    }

    if (t.size() >= 2 && t[0] == 'F') {
        try {
            const int f = std::stoi(t.substr(1));
            if (f >= 1 && f <= 24) {
                return static_cast<UINT>(VK_F1 + (f - 1));
            }
        } catch (...) {
            return VK_F9;
        }
    }

    if (t == "ESC") return VK_ESCAPE;
    if (t == "SPACE") return VK_SPACE;
    if (t == "TAB") return VK_TAB;
    if (t == "ENTER") return VK_RETURN;
    if (t == "INSERT") return VK_INSERT;
    if (t == "DELETE") return VK_DELETE;
    if (t == "HOME") return VK_HOME;
    if (t == "END") return VK_END;
    if (t == "PGUP") return VK_PRIOR;
    if (t == "PGDN") return VK_NEXT;
    if (t == "UP") return VK_UP;
    if (t == "DOWN") return VK_DOWN;
    if (t == "LEFT") return VK_LEFT;
    if (t == "RIGHT") return VK_RIGHT;

    return VK_F9;
}

std::string ModifiersToString(const UINT modifiers) {
    std::string out;
    if (modifiers & MOD_CONTROL) out += "CTRL+";
    if (modifiers & MOD_ALT) out += "ALT+";
    if (modifiers & MOD_SHIFT) out += "SHIFT+";
    if (modifiers & MOD_WIN) out += "WIN+";
    if (!out.empty() && out.back() == '+') {
        out.pop_back();
    }
    return out;
}

std::string VkToString(const UINT vk) {
    if (vk >= VK_F1 && vk <= VK_F24) {
        return "F" + std::to_string(vk - VK_F1 + 1);
    }
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) {
        return std::string(1, static_cast<char>(vk));
    }
    return std::to_string(vk);
}

CaptureCodec ParseCaptureCodec(const std::string& text) {
    const std::string t = Upper(Trim(text));
    if (t == "HEVC" || t == "H265" || t == "H.265") {
        return CaptureCodec::HEVC;
    }
    return CaptureCodec::H264;
}

bool ParseBool(const std::string& text, const bool default_value = false) {
    const std::string t = Upper(Trim(text));
    if (t == "1" || t == "TRUE" || t == "YES" || t == "ON") {
        return true;
    }
    if (t == "0" || t == "FALSE" || t == "NO" || t == "OFF") {
        return false;
    }
    return default_value;
}

}  // namespace

bool SaveDefaultConfig(const std::filesystem::path& path) {
    return SaveConfig(path, AppConfig{});
}

bool SaveConfig(const std::filesystem::path& path, const AppConfig& config) {
    std::ofstream out(path, std::ios::binary | std::ios::out | std::ios::trunc);
    if (!out.is_open()) {
        return false;
    }

    out << "# WinCaptureSync config\n";
    out << "hotkey_modifiers=" << ModifiersToString(config.hotkey_modifiers) << "\n";
    out << "hotkey_vk=" << VkToString(config.hotkey_vk) << "\n";
    out << "capture_fps=" << config.capture_fps << "\n";
    out << "capture_bitrate=" << config.capture_bitrate << "\n";
    out << "capture_width=" << config.capture_width << "\n";
    out << "capture_height=" << config.capture_height << "\n";
    out << "capture_primary_width=" << config.capture_primary_width << "\n";
    out << "capture_primary_height=" << config.capture_primary_height << "\n";
    out << "capture_secondary_width=" << config.capture_secondary_width << "\n";
    out << "capture_secondary_height=" << config.capture_secondary_height << "\n";
    out << "capture_codec=" << CaptureCodecToString(config.capture_codec) << "\n";
    out << "input_queue_capacity=" << config.input_queue_capacity << "\n";
    out << "input_batch_size=" << config.input_batch_size << "\n";
    out << "input_flush_interval_ms=" << config.input_flush_interval_ms << "\n";
    out << "input_diagnostic_mode=" << (config.input_diagnostic_mode ? "1" : "0") << "\n";
    out << "ui_source_mode=" << config.ui_source_mode << "\n";
    out << "ui_primary_source_id=" << config.ui_primary_source_id << "\n";
    out << "ui_secondary_source_id=" << config.ui_secondary_source_id << "\n";
    out << "output_root=" << config.output_root.string() << "\n";
    return true;
}

AppConfig LoadConfig(const std::filesystem::path& path) {
    AppConfig cfg;
    bool has_primary_width = false;
    bool has_primary_height = false;
    bool has_secondary_width = false;
    bool has_secondary_height = false;
    if (!std::filesystem::exists(path)) {
        SaveDefaultConfig(path);
        return cfg;
    }

    std::ifstream in(path, std::ios::binary | std::ios::in);
    if (!in.is_open()) {
        return cfg;
    }

    std::string line;
    while (std::getline(in, line)) {
        line = Trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';') {
            continue;
        }

        const auto pos = line.find('=');
        if (pos == std::string::npos) {
            continue;
        }

        const std::string key = Upper(Trim(line.substr(0, pos)));
        const std::string value = Trim(line.substr(pos + 1));
        try {
            if (key == "HOTKEY_MODIFIERS") {
                cfg.hotkey_modifiers = ParseModifiers(value);
            } else if (key == "HOTKEY_VK") {
                cfg.hotkey_vk = ParseVk(value);
            } else if (key == "CAPTURE_FPS") {
                cfg.capture_fps = static_cast<uint32_t>(std::stoul(value));
            } else if (key == "CAPTURE_BITRATE") {
                cfg.capture_bitrate = static_cast<uint32_t>(std::stoul(value));
            } else if (key == "CAPTURE_WIDTH") {
                cfg.capture_width = static_cast<uint32_t>(std::stoul(value));
            } else if (key == "CAPTURE_HEIGHT") {
                cfg.capture_height = static_cast<uint32_t>(std::stoul(value));
            } else if (key == "CAPTURE_PRIMARY_WIDTH") {
                cfg.capture_primary_width = static_cast<uint32_t>(std::stoul(value));
                has_primary_width = true;
            } else if (key == "CAPTURE_PRIMARY_HEIGHT") {
                cfg.capture_primary_height = static_cast<uint32_t>(std::stoul(value));
                has_primary_height = true;
            } else if (key == "CAPTURE_SECONDARY_WIDTH") {
                cfg.capture_secondary_width = static_cast<uint32_t>(std::stoul(value));
                has_secondary_width = true;
            } else if (key == "CAPTURE_SECONDARY_HEIGHT") {
                cfg.capture_secondary_height = static_cast<uint32_t>(std::stoul(value));
                has_secondary_height = true;
            } else if (key == "CAPTURE_CODEC") {
                cfg.capture_codec = ParseCaptureCodec(value);
            } else if (key == "INPUT_QUEUE_CAPACITY") {
                cfg.input_queue_capacity = static_cast<size_t>(std::stoull(value));
            } else if (key == "INPUT_BATCH_SIZE") {
                cfg.input_batch_size = static_cast<size_t>(std::stoull(value));
            } else if (key == "INPUT_FLUSH_INTERVAL_MS") {
                cfg.input_flush_interval_ms = std::stoi(value);
            } else if (key == "INPUT_DIAGNOSTIC_MODE") {
                cfg.input_diagnostic_mode = ParseBool(value, false);
            } else if (key == "UI_SOURCE_MODE") {
                cfg.ui_source_mode = std::stoi(value);
            } else if (key == "UI_PRIMARY_SOURCE_ID") {
                cfg.ui_primary_source_id = value;
            } else if (key == "UI_SECONDARY_SOURCE_ID") {
                cfg.ui_secondary_source_id = value;
            } else if (key == "OUTPUT_ROOT") {
                cfg.output_root = value;
            }
        } catch (...) {
            continue;
        }
    }

    if (cfg.hotkey_modifiers == 0) {
        cfg.hotkey_modifiers = MOD_CONTROL | MOD_ALT;
    }
    if (cfg.hotkey_vk == 0) {
        cfg.hotkey_vk = VK_F9;
    }
    if (cfg.ui_source_mode != 0 && cfg.ui_source_mode != 1) {
        cfg.ui_source_mode = 0;
    }
    if (cfg.ui_primary_source_id.empty()) {
        cfg.ui_primary_source_id = "monitor_primary";
    }
    if (cfg.ui_secondary_source_id.empty()) {
        cfg.ui_secondary_source_id = "none";
    }

    // Backward compatibility with older config.ini that used one global capture size.
    if (!has_primary_width) {
        cfg.capture_primary_width = cfg.capture_width;
    }
    if (!has_primary_height) {
        cfg.capture_primary_height = cfg.capture_height;
    }
    if (!has_secondary_width) {
        cfg.capture_secondary_width = cfg.capture_width;
    }
    if (!has_secondary_height) {
        cfg.capture_secondary_height = cfg.capture_height;
    }
    return cfg;
}

std::string HotkeyToString(const UINT modifiers, const UINT vk) {
    const std::string mod = ModifiersToString(modifiers);
    const std::string key = VkToString(vk);
    if (mod.empty()) {
        return key;
    }
    return mod + "+" + key;
}

const char* CaptureCodecToString(const CaptureCodec codec) {
    switch (codec) {
        case CaptureCodec::HEVC:
            return "hevc";
        case CaptureCodec::H264:
        default:
            return "h264";
    }
}

}  // namespace wcs::mainapp
