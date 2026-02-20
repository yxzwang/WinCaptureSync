#include "input/input_writer.h"

#include <iomanip>
#include <sstream>

#include "common/json_utils.h"

namespace wcs::input {

namespace {

std::string ModsToJson(const ModifierState& mods) {
    std::ostringstream oss;
    oss << "{"
        << "\"shift\":" << (mods.shift ? "true" : "false") << ","
        << "\"ctrl\":" << (mods.ctrl ? "true" : "false") << ","
        << "\"alt\":" << (mods.alt ? "true" : "false") << ","
        << "\"win\":" << (mods.win ? "true" : "false") << "}";
    return oss.str();
}

}  // namespace

bool InputWriter::Open(const std::filesystem::path& path) {
    file_.open(path, std::ios::binary | std::ios::out | std::ios::trunc);
    return file_.is_open();
}

void InputWriter::Write(const InputEvent& event) {
    if (!file_.is_open()) {
        return;
    }
    file_ << Serialize(event) << "\n";
}

void InputWriter::WriteBatch(const std::vector<InputEvent>& events) {
    if (!file_.is_open()) {
        return;
    }
    for (const auto& event : events) {
        file_ << Serialize(event) << "\n";
    }
}

void InputWriter::Flush() {
    if (file_.is_open()) {
        file_.flush();
    }
}

void InputWriter::Close() {
    if (file_.is_open()) {
        file_.flush();
        file_.close();
    }
}

std::string InputWriter::Serialize(const InputEvent& event) const {
    std::ostringstream oss;
    oss << "{";
    oss << "\"type\":" << json::Quote(ToString(event.type));

    if (event.type == InputEventType::SessionHeader) {
        oss << ",\"input_start_qpc\":" << event.input_start_qpc;
        oss << ",\"qpc_freq\":" << event.qpc_freq;
        oss << ",\"utc_anchor\":{"
            << "\"qpc_ticks\":" << event.utc_anchor_qpc << ","
            << "\"utc_epoch_ns\":" << event.utc_anchor_ns << "}";
        oss << ",\"screen\":{"
            << "\"width\":" << event.screen_width << ","
            << "\"height\":" << event.screen_height << ","
            << "\"virtual_left\":" << event.virtual_left << ","
            << "\"virtual_top\":" << event.virtual_top << ","
            << "\"virtual_width\":" << event.virtual_width << ","
            << "\"virtual_height\":" << event.virtual_height << ","
            << "\"dpi\":" << event.dpi << ","
            << "\"dpi_scale\":" << std::fixed << std::setprecision(4) << event.dpi_scale
            << "}";
        oss << "}";
        return oss.str();
    }

    oss << ",\"t_qpc\":" << event.t_qpc;
    oss << ",\"mods\":" << ModsToJson(event.mods);
    oss << ",\"injected\":" << (event.injected ? "true" : "false");

    switch (event.type) {
        case InputEventType::KeyDown:
        case InputEventType::KeyUp:
            oss << ",\"vk\":" << event.vk;
            oss << ",\"scan\":" << event.scan;
            oss << ",\"flags\":" << event.flags;
            oss << ",\"is_extended\":" << (event.is_extended ? "true" : "false");
            oss << ",\"key_name\":" << json::Quote(event.key_name);
            break;
        case InputEventType::MouseMove:
            oss << ",\"x\":" << event.x;
            oss << ",\"y\":" << event.y;
            oss << ",\"dx\":" << event.dx;
            oss << ",\"dy\":" << event.dy;
            oss << ",\"distance\":" << std::fixed << std::setprecision(4) << event.distance;
            break;
        case InputEventType::MouseDown:
        case InputEventType::MouseUp:
            oss << ",\"x\":" << event.x;
            oss << ",\"y\":" << event.y;
            oss << ",\"button\":" << json::Quote(event.button);
            break;
        case InputEventType::Wheel:
            oss << ",\"x\":" << event.x;
            oss << ",\"y\":" << event.y;
            oss << ",\"wheel_delta\":" << event.wheel_delta;
            break;
        case InputEventType::Stats:
            oss << ",\"dropped_events\":" << event.dropped_events;
            break;
        default:
            break;
    }

    oss << "}";
    return oss.str();
}

}  // namespace wcs::input
