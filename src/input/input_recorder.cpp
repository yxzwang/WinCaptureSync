#include "input/input_recorder.h"

#include <Windowsx.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

namespace wcs::input {

namespace {

constexpr wchar_t kRawInputWindowClassName[] = L"WCS_INPUT_RAW_WINDOW";

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
        queue_.reset();
        return false;
    }
    if (diagnostic_mode_) {
        const std::filesystem::path diag_path = output_path.parent_path() / "input_diag.jsonl";
        diag_file_.open(diag_path, std::ios::binary | std::ios::out | std::ios::trunc);
        if (!diag_file_.is_open()) {
            diagnostic_mode_ = false;
        }
    }

    running_.store(true);
    writer_thread_ = std::thread(&InputRecorder::WriterThreadMain, this);
    hook_thread_ = std::thread(&InputRecorder::HookThreadMain, this);

    std::unique_lock<std::mutex> lock(ready_mutex_);
    ready_cv_.wait(lock, [this] { return ready_signaled_; });
    const bool ready = hooks_installed_;
    lock.unlock();

    if (!ready) {
        Stop();
        return false;
    }
    return true;
}

void InputRecorder::Stop() {
    if (!running_.load()) {
        return;
    }

    stop_requested_.store(true);
    if (hook_thread_id_ != 0) {
        PostThreadMessageW(hook_thread_id_, WM_QUIT, 0, 0);
    }

    if (hook_thread_.joinable()) {
        hook_thread_.join();
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
}

void InputRecorder::HookThreadMain() {
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

void InputRecorder::WriterThreadMain() {
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
            return false;
        }
    }

    raw_input_hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW, kRawInputWindowClassName, L"WCS_RAW_INPUT",
                                      WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, instance, nullptr);
    if (raw_input_hwnd_ != nullptr) {
        ShowWindow(raw_input_hwnd_, SW_HIDE);
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
    return RegisterRawInputDevices(devices, 2, sizeof(RAWINPUTDEVICE)) == TRUE;
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
