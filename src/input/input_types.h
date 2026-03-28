#pragma once

#include <cstdint>
#include <string>

namespace wcs::input {

enum class InputEventType {
    SessionHeader,
    KeyDown,
    KeyUp,
    MouseMove,
    MouseDown,
    MouseUp,
    Wheel,
    GamepadConnected,
    GamepadDisconnected,
    GamepadButtonDown,
    GamepadButtonUp,
    GamepadAxis,
    Stats
};

struct ModifierState {
    bool shift = false;
    bool ctrl = false;
    bool alt = false;
    bool win = false;
};

struct InputEvent {
    InputEventType type = InputEventType::SessionHeader;
    int64_t t_qpc = 0;

    uint32_t vk = 0;
    uint32_t scan = 0;
    uint32_t flags = 0;
    bool is_extended = false;
    std::string key_name;
    ModifierState mods{};

    int32_t x = 0;
    int32_t y = 0;
    int32_t dx = 0;
    int32_t dy = 0;
    double distance = 0.0;
    std::string button;
    int32_t wheel_delta = 0;
    bool injected = false;
    int32_t gamepad_index = -1;
    uint32_t gamepad_packet = 0;
    std::string gamepad_control;
    int32_t gamepad_value = 0;
    int32_t gamepad_prev_value = 0;

    int64_t input_start_qpc = 0;
    int64_t qpc_freq = 0;
    int64_t utc_anchor_qpc = 0;
    int64_t utc_anchor_ns = 0;
    int32_t screen_width = 0;
    int32_t screen_height = 0;
    int32_t virtual_left = 0;
    int32_t virtual_top = 0;
    int32_t virtual_width = 0;
    int32_t virtual_height = 0;
    uint32_t dpi = 96;
    double dpi_scale = 1.0;
    int64_t dropped_events = 0;
};

inline const char* ToString(const InputEventType type) {
    switch (type) {
        case InputEventType::SessionHeader:
            return "session_header";
        case InputEventType::KeyDown:
            return "key_down";
        case InputEventType::KeyUp:
            return "key_up";
        case InputEventType::MouseMove:
            return "mouse_move";
        case InputEventType::MouseDown:
            return "mouse_down";
        case InputEventType::MouseUp:
            return "mouse_up";
        case InputEventType::Wheel:
            return "wheel";
        case InputEventType::GamepadConnected:
            return "gamepad_connected";
        case InputEventType::GamepadDisconnected:
            return "gamepad_disconnected";
        case InputEventType::GamepadButtonDown:
            return "gamepad_button_down";
        case InputEventType::GamepadButtonUp:
            return "gamepad_button_up";
        case InputEventType::GamepadAxis:
            return "gamepad_axis";
        case InputEventType::Stats:
            return "stats";
        default:
            return "unknown";
    }
}

}  // namespace wcs::input
