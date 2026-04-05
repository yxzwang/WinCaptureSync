#include "input/input_recorder.h"

#include <Xinput.h>
#include <Windowsx.h>

#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

#include "common/logger.h"

namespace wcs::input {

namespace {

constexpr wchar_t kRawInputWindowClassName[] = L"WCS_INPUT_RAW_WINDOW";

struct GamepadButtonMapEntry {
    WORD mask = 0;
    const char* name = "";
};

constexpr std::array<GamepadButtonMapEntry, 14> kGamepadButtons = {{
    {XINPUT_GAMEPAD_DPAD_UP, "dpad_up"},
    {XINPUT_GAMEPAD_DPAD_DOWN, "dpad_down"},
    {XINPUT_GAMEPAD_DPAD_LEFT, "dpad_left"},
    {XINPUT_GAMEPAD_DPAD_RIGHT, "dpad_right"},
    {XINPUT_GAMEPAD_START, "start"},
    {XINPUT_GAMEPAD_BACK, "back"},
    {XINPUT_GAMEPAD_LEFT_THUMB, "left_thumb"},
    {XINPUT_GAMEPAD_RIGHT_THUMB, "right_thumb"},
    {XINPUT_GAMEPAD_LEFT_SHOULDER, "left_shoulder"},
    {XINPUT_GAMEPAD_RIGHT_SHOULDER, "right_shoulder"},
    {XINPUT_GAMEPAD_A, "a"},
    {XINPUT_GAMEPAD_B, "b"},
    {XINPUT_GAMEPAD_X, "x"},
    {XINPUT_GAMEPAD_Y, "y"},
}};

int16_t ApplyStickDeadzone(const SHORT value, const SHORT deadzone) {
    return static_cast<int16_t>((std::abs(static_cast<int>(value)) <= deadzone) ? 0 : value);
}

uint8_t ApplyTriggerDeadzone(const BYTE value) {
    return static_cast<uint8_t>(value <= XINPUT_GAMEPAD_TRIGGER_THRESHOLD ? 0 : value);
}

std::string WideToUtf8(const std::wstring& wide) {
    if (wide.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, wide.data(),
                                         static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        return {};
    }

    std::string utf8(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), utf8.data(), size,
                        nullptr, nullptr);
    return utf8;
}

std::string VkToReadableName(const KBDLLHOOKSTRUCT& data) {
    UINT vk = data.vkCode;
    if (vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU) {
        const UINT mapped = MapVirtualKeyW(data.scanCode, MAPVK_VSC_TO_VK_EX);
        if (mapped != 0) {
            vk = mapped;
        }
    }

    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) {
        return std::string(1, static_cast<char>(vk));
    }
    if (vk >= VK_F1 && vk <= VK_F24) {
        return "F" + std::to_string(vk - VK_F1 + 1);
    }

    switch (vk) {
        case VK_ESCAPE:
            return "Esc";
        case VK_TAB:
            return "Tab";
        case VK_CAPITAL:
            return "CapsLock";
        case VK_SPACE:
            return "Space";
        case VK_RETURN:
            return "Enter";
        case VK_BACK:
            return "Backspace";
        case VK_DELETE:
            return "Delete";
        case VK_INSERT:
            return "Insert";
        case VK_HOME:
            return "Home";
        case VK_END:
            return "End";
        case VK_PRIOR:
            return "PageUp";
        case VK_NEXT:
            return "PageDown";
        case VK_UP:
            return "Up";
        case VK_DOWN:
            return "Down";
        case VK_LEFT:
            return "Left";
        case VK_RIGHT:
            return "Right";
        case VK_SNAPSHOT:
            return "PrintScreen";
        case VK_SCROLL:
            return "ScrollLock";
        case VK_PAUSE:
            return "Pause";
        case VK_NUMLOCK:
            return "NumLock";
        case VK_LSHIFT:
            return "Left Shift";
        case VK_RSHIFT:
            return "Right Shift";
        case VK_LCONTROL:
            return "Left Ctrl";
        case VK_RCONTROL:
            return "Right Ctrl";
        case VK_LMENU:
            return "Left Alt";
        case VK_RMENU:
            return "Right Alt";
        case VK_LWIN:
            return "Left Win";
        case VK_RWIN:
            return "Right Win";
        default:
            break;
    }

    LONG key_lparam = static_cast<LONG>((data.scanCode & 0xFF) << 16);
    if ((data.flags & LLKHF_EXTENDED) != 0) {
        key_lparam |= (1L << 24);
    }

    wchar_t key_name[128] = {};
    const int name_len = GetKeyNameTextW(key_lparam, key_name,
                                         static_cast<int>(sizeof(key_name) / sizeof(key_name[0])));
    if (name_len > 0) {
        return WideToUtf8(std::wstring(key_name, key_name + name_len));
    }
    return "VK_" + std::to_string(vk);
}

}  // namespace

InputRecorder* InputRecorder::instance_ = nullptr;

InputRecorder::~InputRecorder() {
    Stop();
}

bool InputRecorder::Start(const std::filesystem::path& output_path,
                          const wcs::time::UtcAnchor& utc_anchor,
                          const Options& options) {
    if (running_.load()) {
        return false;
    }
    wcs::common::log::Info("InputRecorder Start: " + output_path.string());

    anchor_ = utc_anchor;
    options_ = options;
    dropped_events_.store(0);
    start_qpc_.store(0);
    stop_requested_.store(false);
    ready_signaled_ = false;
    hooks_installed_ = false;
    raw_input_active_ = false;
    has_raw_signal_mouse_ = false;
    raw_signal_mouse_x_ = 0;
    raw_signal_mouse_y_ = 0;
    raw_input_hwnd_ = nullptr;
    diagnostic_mode_ = options_.diagnostic_mode;
    diag_raw_total_ = 0;
    diag_raw_keyboard_ = 0;
    diag_raw_mouse_ = 0;
    diag_raw_mouse_move_ = 0;
    diag_raw_mouse_button_or_wheel_ = 0;
    diag_raw_mouse_zero_delta_ = 0;
    diag_raw_mouse_relative_ = 0;
    diag_raw_mouse_absolute_ = 0;
    diag_get_raw_data_fail_ = 0;

    queue_ = std::make_unique<InputEventQueue>(options_.queue_capacity);
    if (!writer_.Open(output_path)) {
        wcs::common::log::Error("InputRecorder failed to open output file: " + output_path.string());
        queue_.reset();
        return false;
    }
    if (diagnostic_mode_) {
        const std::filesystem::path diag_path = output_path.parent_path() / "input_diag.jsonl";
        diag_file_.open(diag_path, std::ios::binary | std::ios::out | std::ios::trunc);
        if (!diag_file_.is_open()) {
            wcs::common::log::Warning("InputRecorder cannot open diagnostic file: " +
                                      diag_path.string());
            diagnostic_mode_ = false;
        }
    }

    running_.store(true);
    writer_thread_ = std::thread(&InputRecorder::WriterThreadMain, this);
    hook_thread_ = std::thread(&InputRecorder::HookThreadMain, this);

    std::unique_lock<std::mutex> lock(ready_mutex_);
    const bool signaled = ready_cv_.wait_for(lock, std::chrono::seconds(5),
                                             [this] { return ready_signaled_; });
    if (!signaled) {
        wcs::common::log::Error("InputRecorder start timeout waiting hook thread ready");
    }
    const bool ready = hooks_installed_;
    lock.unlock();

    if (!ready || !signaled) {
        wcs::common::log::Error("InputRecorder hook initialization failed");
        Stop();
        return false;
    }

    if (options_.gamepad_enabled) {
        gamepad_thread_ = std::thread(&InputRecorder::GamepadThreadMain, this);
    }
    wcs::common::log::Info("InputRecorder started");
    return true;
}

void InputRecorder::Stop() {
    if (!running_.load()) {
        return;
    }
    wcs::common::log::Info("InputRecorder stopping");

    stop_requested_.store(true);
    if (hook_thread_id_ != 0) {
        PostThreadMessageW(hook_thread_id_, WM_QUIT, 0, 0);
    }

    if (hook_thread_.joinable()) {
        hook_thread_.join();
    }
    if (gamepad_thread_.joinable()) {
        gamepad_thread_.join();
    }

    EmitDiagSummary();

    InputEvent stats{};
    stats.type = InputEventType::Stats;
    stats.t_qpc = wcs::time::QpcClock::NowTicks();
    stats.mods = CurrentMods();
    stats.dropped_events = dropped_events_.load();
    Enqueue(stats);

    if (queue_) {
        queue_->Stop();
    }

    if (writer_thread_.joinable()) {
        writer_thread_.join();
    }

    writer_.Close();
    if (diag_file_.is_open()) {
        diag_file_.flush();
        diag_file_.close();
    }
    queue_.reset();
    running_.store(false);
    hook_thread_id_ = 0;
    wcs::common::log::Info("InputRecorder stopped");
}

void InputRecorder::HookThreadMain() {
    try {
        hook_thread_id_ = GetCurrentThreadId();
        instance_ = this;

        MSG init_msg{};
        PeekMessageW(&init_msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);

        const bool raw_window_ok = CreateRawInputWindow();
        raw_input_active_ = raw_window_ok && RegisterRawInputSink();
        // Raw-input-only mode: do not install low-level hooks.
        hooks_installed_ = raw_input_active_;
        if (diagnostic_mode_) {
            std::ostringstream oss;
            oss << "{\"type\":\"diag_info\",\"t_qpc\":" << wcs::time::QpcClock::NowTicks()
                << ",\"raw_window_ok\":" << (raw_window_ok ? "true" : "false")
                << ",\"raw_input_active\":" << (raw_input_active_ ? "true" : "false")
                << ",\"raw_register_flags\":\"RIDEV_INPUTSINK\""
                << ",\"mode\":\"raw_input_only\"}";
            WriteDiagLine(oss.str());
        }
        wcs::common::log::Info(std::string("Input hook thread raw_input_active=") +
                               (raw_input_active_ ? "true" : "false"));

        if (hooks_installed_) {
            start_qpc_.store(wcs::time::QpcClock::NowTicks());
            EmitSessionHeader();
        }

        {
            std::lock_guard<std::mutex> lock(ready_mutex_);
            ready_signaled_ = true;
        }
        ready_cv_.notify_one();

        if (hooks_installed_) {
            MSG msg{};
            while (!stop_requested_.load()) {
                const BOOL result = GetMessageW(&msg, nullptr, 0, 0);
                if (result <= 0) {
                    break;
                }

                if (msg.message == WM_INPUT) {
                    HandleRawInput(reinterpret_cast<HRAWINPUT>(msg.lParam), msg.wParam);
                    DefWindowProcW(msg.hwnd, msg.message, msg.wParam, msg.lParam);
                    continue;
                }
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }
    } catch (const std::exception& ex) {
        wcs::common::log::Error(std::string("Input hook thread exception: ") + ex.what());
        stop_requested_.store(true);
    } catch (...) {
        wcs::common::log::Error("Input hook thread unknown exception");
        stop_requested_.store(true);
    }

    {
        std::lock_guard<std::mutex> lock(ready_mutex_);
        ready_signaled_ = true;
    }
    ready_cv_.notify_one();

    if (raw_input_hwnd_ != nullptr) {
        DestroyWindow(raw_input_hwnd_);
        raw_input_hwnd_ = nullptr;
    }

    if (keyboard_hook_ != nullptr) {
        UnhookWindowsHookEx(keyboard_hook_);
        keyboard_hook_ = nullptr;
    }
    if (mouse_hook_ != nullptr) {
        UnhookWindowsHookEx(mouse_hook_);
        mouse_hook_ = nullptr;
    }

    instance_ = nullptr;
}

void InputRecorder::GamepadThreadMain() {
    try {
        struct GamepadState {
            bool connected = false;
            XINPUT_STATE state{};
        };

        std::array<GamepadState, XUSER_MAX_COUNT> gamepads{};
        int poll_interval_ms = options_.gamepad_poll_interval_ms;
        if (poll_interval_ms < 1) {
            poll_interval_ms = 1;
        }
        wcs::common::log::Info("Gamepad thread started, poll_interval_ms=" +
                               std::to_string(poll_interval_ms));

        auto emit_connection = [this](const int32_t index,
                                      const InputEventType type,
                                      const uint32_t packet) {
            InputEvent event{};
            event.t_qpc = wcs::time::QpcClock::NowTicks();
            event.type = type;
            event.mods = CurrentMods();
            event.injected = false;
            event.gamepad_index = index;
            event.gamepad_packet = packet;
            Enqueue(event);
        };

        auto emit_button = [this](const int32_t index,
                                  const uint32_t packet,
                                  const char* button_name,
                                  const bool pressed) {
            InputEvent event{};
            event.t_qpc = wcs::time::QpcClock::NowTicks();
            event.type = pressed ? InputEventType::GamepadButtonDown : InputEventType::GamepadButtonUp;
            event.mods = CurrentMods();
            event.injected = false;
            event.gamepad_index = index;
            event.gamepad_packet = packet;
            event.gamepad_control = button_name;
            Enqueue(event);
        };

        auto emit_axis = [this](const int32_t index,
                                const uint32_t packet,
                                const char* axis_name,
                                const int32_t value,
                                const int32_t prev_value) {
            InputEvent event{};
            event.t_qpc = wcs::time::QpcClock::NowTicks();
            event.type = InputEventType::GamepadAxis;
            event.mods = CurrentMods();
            event.injected = false;
            event.gamepad_index = index;
            event.gamepad_packet = packet;
            event.gamepad_control = axis_name;
            event.gamepad_value = value;
            event.gamepad_prev_value = prev_value;
            Enqueue(event);
        };

        auto emit_changes = [&](const int32_t index,
                                const XINPUT_STATE& prev_state,
                                const XINPUT_STATE& curr_state) {
            const WORD prev_buttons = prev_state.Gamepad.wButtons;
            const WORD curr_buttons = curr_state.Gamepad.wButtons;
            const WORD changed = static_cast<WORD>(prev_buttons ^ curr_buttons);
            for (const auto& button : kGamepadButtons) {
                if ((changed & button.mask) == 0) {
                    continue;
                }
                const bool pressed = (curr_buttons & button.mask) != 0;
                emit_button(index, curr_state.dwPacketNumber, button.name, pressed);
            }

            const int32_t prev_lt = ApplyTriggerDeadzone(prev_state.Gamepad.bLeftTrigger);
            const int32_t curr_lt = ApplyTriggerDeadzone(curr_state.Gamepad.bLeftTrigger);
            if (curr_lt != prev_lt) {
                emit_axis(index, curr_state.dwPacketNumber, "left_trigger", curr_lt, prev_lt);
            }

            const int32_t prev_rt = ApplyTriggerDeadzone(prev_state.Gamepad.bRightTrigger);
            const int32_t curr_rt = ApplyTriggerDeadzone(curr_state.Gamepad.bRightTrigger);
            if (curr_rt != prev_rt) {
                emit_axis(index, curr_state.dwPacketNumber, "right_trigger", curr_rt, prev_rt);
            }

            const int32_t prev_lx =
                ApplyStickDeadzone(prev_state.Gamepad.sThumbLX, XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE);
            const int32_t curr_lx =
                ApplyStickDeadzone(curr_state.Gamepad.sThumbLX, XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE);
            if (curr_lx != prev_lx) {
                emit_axis(index, curr_state.dwPacketNumber, "left_stick_x", curr_lx, prev_lx);
            }

            const int32_t prev_ly =
                ApplyStickDeadzone(prev_state.Gamepad.sThumbLY, XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE);
            const int32_t curr_ly =
                ApplyStickDeadzone(curr_state.Gamepad.sThumbLY, XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE);
            if (curr_ly != prev_ly) {
                emit_axis(index, curr_state.dwPacketNumber, "left_stick_y", curr_ly, prev_ly);
            }

            const int32_t prev_rx =
                ApplyStickDeadzone(prev_state.Gamepad.sThumbRX, XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE);
            const int32_t curr_rx =
                ApplyStickDeadzone(curr_state.Gamepad.sThumbRX, XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE);
            if (curr_rx != prev_rx) {
                emit_axis(index, curr_state.dwPacketNumber, "right_stick_x", curr_rx, prev_rx);
            }

            const int32_t prev_ry =
                ApplyStickDeadzone(prev_state.Gamepad.sThumbRY, XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE);
            const int32_t curr_ry =
                ApplyStickDeadzone(curr_state.Gamepad.sThumbRY, XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE);
            if (curr_ry != prev_ry) {
                emit_axis(index, curr_state.dwPacketNumber, "right_stick_y", curr_ry, prev_ry);
            }
        };

        while (!stop_requested_.load()) {
            for (DWORD idx = 0; idx < XUSER_MAX_COUNT; ++idx) {
                XINPUT_STATE current_state{};
                const DWORD status = XInputGetState(idx, &current_state);
                auto& tracked = gamepads[idx];

                if (status != ERROR_SUCCESS) {
                    if (tracked.connected) {
                        emit_connection(static_cast<int32_t>(idx),
                                        InputEventType::GamepadDisconnected,
                                        tracked.state.dwPacketNumber);
                        tracked.connected = false;
                        tracked.state = XINPUT_STATE{};
                    }
                    continue;
                }

                if (!tracked.connected) {
                    tracked.connected = true;
                    emit_connection(static_cast<int32_t>(idx), InputEventType::GamepadConnected,
                                    current_state.dwPacketNumber);

                    // Emit current held state immediately after connection.
                    const XINPUT_STATE zero_state{};
                    emit_changes(static_cast<int32_t>(idx), zero_state, current_state);
                    tracked.state = current_state;
                    continue;
                }

                if (current_state.dwPacketNumber == tracked.state.dwPacketNumber) {
                    continue;
                }

                emit_changes(static_cast<int32_t>(idx), tracked.state, current_state);
                tracked.state = current_state;
            }

            Sleep(static_cast<DWORD>(poll_interval_ms));
        }
        wcs::common::log::Info("Gamepad thread exited");
    } catch (const std::exception& ex) {
        wcs::common::log::Error(std::string("Gamepad thread exception: ") + ex.what());
        stop_requested_.store(true);
    } catch (...) {
        wcs::common::log::Error("Gamepad thread unknown exception");
        stop_requested_.store(true);
    }
}

void InputRecorder::WriterThreadMain() {
    try {
        std::vector<InputEvent> batch;
        while (true) {
            batch.clear();
            if (queue_) {
                queue_->PopBatch(&batch, options_.batch_size,
                                 std::chrono::milliseconds(options_.flush_interval_ms));
            }

            if (!batch.empty()) {
                writer_.WriteBatch(batch);
            }

            if (!batch.empty() || stop_requested_.load()) {
                writer_.Flush();
            }

            if (stop_requested_.load() && (!queue_ || queue_->Empty())) {
                break;
            }
        }
    } catch (const std::exception& ex) {
        wcs::common::log::Error(std::string("Input writer thread exception: ") + ex.what());
        stop_requested_.store(true);
    } catch (...) {
        wcs::common::log::Error("Input writer thread unknown exception");
        stop_requested_.store(true);
    }
}

void InputRecorder::EmitSessionHeader() {
    InputEvent header{};
    header.type = InputEventType::SessionHeader;
    header.input_start_qpc = start_qpc_.load();
    header.qpc_freq = anchor_.qpc_freq;
    header.utc_anchor_qpc = anchor_.qpc_ticks;
    header.utc_anchor_ns = anchor_.utc_epoch_ns;

    header.screen_width = GetSystemMetrics(SM_CXSCREEN);
    header.screen_height = GetSystemMetrics(SM_CYSCREEN);
    header.virtual_left = GetSystemMetrics(SM_XVIRTUALSCREEN);
    header.virtual_top = GetSystemMetrics(SM_YVIRTUALSCREEN);
    header.virtual_width = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    header.virtual_height = GetSystemMetrics(SM_CYVIRTUALSCREEN);

    UINT dpi = 96;
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32 != nullptr) {
        using GetDpiForSystemFn = UINT(WINAPI*)();
        const auto get_dpi_for_system =
            reinterpret_cast<GetDpiForSystemFn>(GetProcAddress(user32, "GetDpiForSystem"));
        if (get_dpi_for_system != nullptr) {
            dpi = get_dpi_for_system();
        }
    }

    header.dpi = dpi;
    header.dpi_scale = static_cast<double>(dpi) / 96.0;
    Enqueue(header);
}

void InputRecorder::Enqueue(const InputEvent& event) {
    if (!queue_) {
        return;
    }
    if (!queue_->Push(event)) {
        dropped_events_.fetch_add(1);
    }
}

void InputRecorder::WriteDiagLine(const std::string& line) {
    if (!diagnostic_mode_ || !diag_file_.is_open()) {
        return;
    }
    diag_file_ << line << "\n";
}

void InputRecorder::EmitDiagSummary() {
    if (!diagnostic_mode_ || !diag_file_.is_open()) {
        return;
    }
    std::ostringstream oss;
    oss << "{\"type\":\"diag_summary\",\"t_qpc\":" << wcs::time::QpcClock::NowTicks()
        << ",\"raw_total\":" << diag_raw_total_ << ",\"raw_keyboard\":" << diag_raw_keyboard_
        << ",\"raw_mouse\":" << diag_raw_mouse_ << ",\"raw_mouse_move\":" << diag_raw_mouse_move_
        << ",\"raw_mouse_button_or_wheel\":" << diag_raw_mouse_button_or_wheel_
        << ",\"raw_mouse_zero_delta\":" << diag_raw_mouse_zero_delta_
        << ",\"raw_mouse_relative\":" << diag_raw_mouse_relative_
        << ",\"raw_mouse_absolute\":" << diag_raw_mouse_absolute_
        << ",\"raw_get_data_fail\":" << diag_get_raw_data_fail_ << "}";
    WriteDiagLine(oss.str());
}

ModifierState InputRecorder::CurrentMods() const {
    ModifierState mods{};
    mods.shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    mods.ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    mods.alt = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
    mods.win = ((GetAsyncKeyState(VK_LWIN) & 0x8000) != 0) ||
               ((GetAsyncKeyState(VK_RWIN) & 0x8000) != 0);
    return mods;
}

bool InputRecorder::CreateRawInputWindow() {
    HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSW wc{};
    wc.lpfnWndProc = &InputRecorder::RawInputWndProc;
    wc.hInstance = instance;
    wc.lpszClassName = kRawInputWindowClassName;

    if (!RegisterClassW(&wc)) {
        const DWORD err = GetLastError();
        if (err != ERROR_CLASS_ALREADY_EXISTS) {
            wcs::common::log::Error("RegisterClassW for raw input window failed, error=" +
                                    std::to_string(err));
            return false;
        }
    }

    raw_input_hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW, kRawInputWindowClassName, L"WCS_RAW_INPUT",
                                      WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, instance, nullptr);
    if (raw_input_hwnd_ != nullptr) {
        ShowWindow(raw_input_hwnd_, SW_HIDE);
    } else {
        wcs::common::log::Error("CreateWindowExW for raw input window failed, error=" +
                                std::to_string(GetLastError()));
    }
    return raw_input_hwnd_ != nullptr;
}

bool InputRecorder::RegisterRawInputSink() const {
    if (raw_input_hwnd_ == nullptr) {
        return false;
    }

    DWORD flags = RIDEV_INPUTSINK;
#ifdef RIDEV_DEVNOTIFY
    flags |= RIDEV_DEVNOTIFY;
#endif

    RAWINPUTDEVICE devices[2]{};
    devices[0].usUsagePage = 0x01;  // Generic desktop controls
    devices[0].usUsage = 0x02;      // Mouse
    devices[0].dwFlags = flags;
    devices[0].hwndTarget = raw_input_hwnd_;

    devices[1].usUsagePage = 0x01;  // Generic desktop controls
    devices[1].usUsage = 0x06;      // Keyboard
    devices[1].dwFlags = flags;
    devices[1].hwndTarget = raw_input_hwnd_;
    const bool ok = RegisterRawInputDevices(devices, 2, sizeof(RAWINPUTDEVICE)) == TRUE;
    if (!ok) {
        wcs::common::log::Error("RegisterRawInputDevices failed, error=" +
                                std::to_string(GetLastError()));
    }
    return ok;
}

void InputRecorder::HandleRawInput(const HRAWINPUT raw_input_handle, const WPARAM raw_input_w_param) {
    if (raw_input_handle == nullptr) {
        return;
    }
    ++diag_raw_total_;

    UINT size = 0;
    if (GetRawInputData(raw_input_handle, RID_INPUT, nullptr, &size, sizeof(RAWINPUTHEADER)) != 0 ||
        size == 0) {
        ++diag_get_raw_data_fail_;
        if (diagnostic_mode_) {
            std::ostringstream oss;
            oss << "{\"type\":\"diag_raw_error\",\"t_qpc\":" << wcs::time::QpcClock::NowTicks()
                << ",\"stage\":\"query_size\",\"raw_w_param\":" << static_cast<uint64_t>(raw_input_w_param)
                << ",\"size\":" << size << "}";
            WriteDiagLine(oss.str());
        }
        return;
    }

    std::vector<uint8_t> buffer(size);
    if (GetRawInputData(raw_input_handle, RID_INPUT, buffer.data(), &size, sizeof(RAWINPUTHEADER)) !=
        size) {
        ++diag_get_raw_data_fail_;
        if (diagnostic_mode_) {
            std::ostringstream oss;
            oss << "{\"type\":\"diag_raw_error\",\"t_qpc\":" << wcs::time::QpcClock::NowTicks()
                << ",\"stage\":\"read_payload\",\"raw_w_param\":"
                << static_cast<uint64_t>(raw_input_w_param) << ",\"size\":" << size << "}";
            WriteDiagLine(oss.str());
        }
        return;
    }

    const auto* raw_input = reinterpret_cast<const RAWINPUT*>(buffer.data());
    if (raw_input == nullptr) {
        return;
    }

    if (raw_input->header.dwType == RIM_TYPEKEYBOARD) {
        ++diag_raw_keyboard_;
        const auto& key = raw_input->data.keyboard;
        if (key.VKey == 0 || key.VKey == 255) {
            return;
        }

        const int64_t now_qpc = wcs::time::QpcClock::NowTicks();

        InputEvent event{};
        event.t_qpc = now_qpc;
        event.type = (key.Flags & RI_KEY_BREAK) != 0 ? InputEventType::KeyUp
                                                      : InputEventType::KeyDown;

        uint32_t vk = key.VKey;
        if (vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU) {
            const UINT mapped = MapVirtualKeyW(key.MakeCode, MAPVK_VSC_TO_VK_EX);
            if (mapped != 0) {
                vk = mapped;
            }
        }
        event.vk = vk;
        event.scan = key.MakeCode;
        event.flags = key.Flags;
        event.is_extended = (key.Flags & (RI_KEY_E0 | RI_KEY_E1)) != 0;

        KBDLLHOOKSTRUCT proxy{};
        proxy.vkCode = event.vk;
        proxy.scanCode = event.scan;
        proxy.flags = event.is_extended ? LLKHF_EXTENDED : 0;
        event.key_name = VkToReadableName(proxy);
        event.mods = CurrentMods();
        event.injected = false;
        Enqueue(event);
        if (diagnostic_mode_) {
            std::ostringstream oss;
            oss << "{\"type\":\"diag_raw_keyboard\",\"t_qpc\":" << now_qpc
                << ",\"raw_w_param\":" << static_cast<uint64_t>(raw_input_w_param)
                << ",\"vk\":" << event.vk << ",\"scan\":" << event.scan << ",\"flags\":" << event.flags
                << "}";
            WriteDiagLine(oss.str());
        }
        return;
    }

    if (raw_input->header.dwType == RIM_TYPEMOUSE) {
        ++diag_raw_mouse_;
        const auto& mouse = raw_input->data.mouse;
        const int64_t now_qpc = wcs::time::QpcClock::NowTicks();
        const bool is_absolute = (mouse.usFlags & MOUSE_MOVE_ABSOLUTE) != 0;
        if (is_absolute) {
            ++diag_raw_mouse_absolute_;
        } else {
            ++diag_raw_mouse_relative_;
        }
        const bool has_move = (mouse.lLastX != 0 || mouse.lLastY != 0);
        int32_t emitted_dx = 0;
        int32_t emitted_dy = 0;
        bool emitted_move = false;
        if (has_move) {
            int32_t dx = static_cast<int32_t>(mouse.lLastX);
            int32_t dy = static_cast<int32_t>(mouse.lLastY);

            if (is_absolute) {
                const bool use_virtual = (mouse.usFlags & MOUSE_VIRTUAL_DESKTOP) != 0;
                const int left = use_virtual ? GetSystemMetrics(SM_XVIRTUALSCREEN) : 0;
                const int top = use_virtual ? GetSystemMetrics(SM_YVIRTUALSCREEN) : 0;
                const int width =
                    use_virtual ? GetSystemMetrics(SM_CXVIRTUALSCREEN) : GetSystemMetrics(SM_CXSCREEN);
                const int height =
                    use_virtual ? GetSystemMetrics(SM_CYVIRTUALSCREEN) : GetSystemMetrics(SM_CYSCREEN);
                const int abs_x =
                    left + static_cast<int>((static_cast<int64_t>(mouse.lLastX) * (width - 1)) / 65535);
                const int abs_y =
                    top + static_cast<int>((static_cast<int64_t>(mouse.lLastY) * (height - 1)) / 65535);

                if (!has_raw_signal_mouse_) {
                    raw_signal_mouse_x_ = abs_x;
                    raw_signal_mouse_y_ = abs_y;
                    has_raw_signal_mouse_ = true;
                    dx = 0;
                    dy = 0;
                } else {
                    dx = abs_x - raw_signal_mouse_x_;
                    dy = abs_y - raw_signal_mouse_y_;
                    raw_signal_mouse_x_ = abs_x;
                    raw_signal_mouse_y_ = abs_y;
                }
            } else {
                if (!has_raw_signal_mouse_) {
                    POINT cursor{};
                    GetCursorPos(&cursor);
                    raw_signal_mouse_x_ = cursor.x;
                    raw_signal_mouse_y_ = cursor.y;
                    has_raw_signal_mouse_ = true;
                }
                raw_signal_mouse_x_ += dx;
                raw_signal_mouse_y_ += dy;
            }

            if (dx != 0 || dy != 0) {
                InputEvent move_event{};
                move_event.t_qpc = now_qpc;
                move_event.type = InputEventType::MouseMove;
                move_event.mods = CurrentMods();
                move_event.x = raw_signal_mouse_x_;
                move_event.y = raw_signal_mouse_y_;
                move_event.dx = dx;
                move_event.dy = dy;
                move_event.distance =
                    std::sqrt(static_cast<double>(dx) * dx + static_cast<double>(dy) * dy);
                move_event.injected = false;
                Enqueue(move_event);
                emitted_dx = dx;
                emitted_dy = dy;
                emitted_move = true;
                ++diag_raw_mouse_move_;
            } else {
                ++diag_raw_mouse_zero_delta_;
            }
        }

        if (diagnostic_mode_) {
            std::ostringstream oss;
            oss << "{\"type\":\"diag_raw_mouse\",\"t_qpc\":" << now_qpc
                << ",\"raw_w_param\":" << static_cast<uint64_t>(raw_input_w_param)
                << ",\"us_flags\":" << mouse.usFlags << ",\"button_flags\":" << mouse.usButtonFlags
                << ",\"button_data\":" << mouse.usButtonData << ",\"last_x\":" << mouse.lLastX
                << ",\"last_y\":" << mouse.lLastY << ",\"has_move\":"
                << (has_move ? "true" : "false") << ",\"emitted_move\":"
                << (emitted_move ? "true" : "false") << ",\"dx\":" << emitted_dx << ",\"dy\":"
                << emitted_dy << "}";
            WriteDiagLine(oss.str());
        }

        if (mouse.usButtonFlags == 0) {
            return;
        }
        ++diag_raw_mouse_button_or_wheel_;

        POINT cursor{};
        GetCursorPos(&cursor);

        auto emit_button = [&](const InputEventType type, const char* button_name) {
            InputEvent event{};
            event.t_qpc = now_qpc;
            event.type = type;
            event.mods = CurrentMods();
            event.x = cursor.x;
            event.y = cursor.y;
            event.button = button_name;
            event.injected = false;
            Enqueue(event);
        };

        if ((mouse.usButtonFlags & RI_MOUSE_LEFT_BUTTON_DOWN) != 0) {
            emit_button(InputEventType::MouseDown, "left");
        }
        if ((mouse.usButtonFlags & RI_MOUSE_LEFT_BUTTON_UP) != 0) {
            emit_button(InputEventType::MouseUp, "left");
        }
        if ((mouse.usButtonFlags & RI_MOUSE_RIGHT_BUTTON_DOWN) != 0) {
            emit_button(InputEventType::MouseDown, "right");
        }
        if ((mouse.usButtonFlags & RI_MOUSE_RIGHT_BUTTON_UP) != 0) {
            emit_button(InputEventType::MouseUp, "right");
        }
        if ((mouse.usButtonFlags & RI_MOUSE_MIDDLE_BUTTON_DOWN) != 0) {
            emit_button(InputEventType::MouseDown, "middle");
        }
        if ((mouse.usButtonFlags & RI_MOUSE_MIDDLE_BUTTON_UP) != 0) {
            emit_button(InputEventType::MouseUp, "middle");
        }
        if ((mouse.usButtonFlags & RI_MOUSE_BUTTON_4_DOWN) != 0) {
            emit_button(InputEventType::MouseDown, "x1");
        }
        if ((mouse.usButtonFlags & RI_MOUSE_BUTTON_4_UP) != 0) {
            emit_button(InputEventType::MouseUp, "x1");
        }
        if ((mouse.usButtonFlags & RI_MOUSE_BUTTON_5_DOWN) != 0) {
            emit_button(InputEventType::MouseDown, "x2");
        }
        if ((mouse.usButtonFlags & RI_MOUSE_BUTTON_5_UP) != 0) {
            emit_button(InputEventType::MouseUp, "x2");
        }
        if ((mouse.usButtonFlags & RI_MOUSE_WHEEL) != 0) {
            InputEvent event{};
            event.t_qpc = now_qpc;
            event.type = InputEventType::Wheel;
            event.mods = CurrentMods();
            event.x = cursor.x;
            event.y = cursor.y;
            event.button = "vertical";
            event.wheel_delta = static_cast<short>(mouse.usButtonData);
            event.injected = false;
            Enqueue(event);
        }
#ifdef RI_MOUSE_HWHEEL
        if ((mouse.usButtonFlags & RI_MOUSE_HWHEEL) != 0) {
            InputEvent event{};
            event.t_qpc = now_qpc;
            event.type = InputEventType::Wheel;
            event.mods = CurrentMods();
            event.x = cursor.x;
            event.y = cursor.y;
            event.button = "horizontal";
            event.wheel_delta = static_cast<short>(mouse.usButtonData);
            event.injected = false;
            Enqueue(event);
        }
#endif
    }
}

LRESULT InputRecorder::HandleKeyboard(const WPARAM w_param, const LPARAM l_param) {
    // Raw-input-only mode: LL hook path is intentionally disabled.
    (void)w_param;
    (void)l_param;
    return CallNextHookEx(nullptr, HC_ACTION, w_param, l_param);
}

LRESULT InputRecorder::HandleMouse(const WPARAM w_param, const LPARAM l_param) {
    // Raw-input-only mode: LL hook path is intentionally disabled.
    (void)w_param;
    (void)l_param;
    return CallNextHookEx(nullptr, HC_ACTION, w_param, l_param);
}

LRESULT CALLBACK InputRecorder::KeyboardProc(const int code,
                                             const WPARAM w_param,
                                             const LPARAM l_param) {
    if (code == HC_ACTION && instance_ != nullptr) {
        return instance_->HandleKeyboard(w_param, l_param);
    }
    return CallNextHookEx(nullptr, code, w_param, l_param);
}

LRESULT CALLBACK InputRecorder::MouseProc(const int code,
                                          const WPARAM w_param,
                                          const LPARAM l_param) {
    if (code == HC_ACTION && instance_ != nullptr) {
        return instance_->HandleMouse(w_param, l_param);
    }
    return CallNextHookEx(nullptr, code, w_param, l_param);
}

LRESULT CALLBACK InputRecorder::RawInputWndProc(HWND hwnd,
                                                const UINT msg,
                                                const WPARAM w_param,
                                                const LPARAM l_param) {
    return DefWindowProcW(hwnd, msg, w_param, l_param);
}

}  // namespace wcs::input
