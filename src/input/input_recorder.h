#pragma once

#include <Windows.h>

#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <memory>
#include <thread>

#include "input/input_queue.h"
#include "input/input_writer.h"
#include "time/time_utils.h"

namespace wcs::input {

class InputRecorder {
public:
    struct Options {
        size_t queue_capacity = 32768;
        size_t batch_size = 512;
        int flush_interval_ms = 10;
        bool diagnostic_mode = false;
        bool gamepad_enabled = true;
        int gamepad_poll_interval_ms = 2;
    };

    InputRecorder() = default;
    ~InputRecorder();

    bool Start(const std::filesystem::path& output_path,
               const wcs::time::UtcAnchor& utc_anchor,
               const Options& options = Options{});
    void Stop();

    bool IsRunning() const { return running_.load(); }
    int64_t StartQpc() const { return start_qpc_.load(); }
    int64_t DroppedEvents() const { return dropped_events_.load(); }

private:
    void HookThreadMain();
    void GamepadThreadMain();
    void WriterThreadMain();
    void EmitSessionHeader();
    void Enqueue(const InputEvent& event);
    ModifierState CurrentMods() const;
    bool CreateRawInputWindow();
    bool RegisterRawInputSink() const;
    void HandleRawInput(HRAWINPUT raw_input_handle, WPARAM raw_input_w_param);
    void WriteDiagLine(const std::string& line);
    void EmitDiagSummary();

    LRESULT HandleKeyboard(WPARAM w_param, LPARAM l_param);
    LRESULT HandleMouse(WPARAM w_param, LPARAM l_param);

    static LRESULT CALLBACK KeyboardProc(int code, WPARAM w_param, LPARAM l_param);
    static LRESULT CALLBACK MouseProc(int code, WPARAM w_param, LPARAM l_param);
    static LRESULT CALLBACK RawInputWndProc(HWND hwnd, UINT msg, WPARAM w_param, LPARAM l_param);

    static InputRecorder* instance_;

    wcs::time::UtcAnchor anchor_{};
    Options options_{};
    InputWriter writer_{};
    std::unique_ptr<InputEventQueue> queue_{};

    std::thread hook_thread_{};
    std::thread gamepad_thread_{};
    std::thread writer_thread_{};
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_requested_{false};
    std::atomic<int64_t> start_qpc_{0};
    std::atomic<int64_t> dropped_events_{0};

    std::mutex ready_mutex_{};
    std::condition_variable ready_cv_{};
    bool ready_signaled_ = false;
    bool hooks_installed_ = false;
    bool raw_input_active_ = false;

    HHOOK keyboard_hook_ = nullptr;
    HHOOK mouse_hook_ = nullptr;
    HWND raw_input_hwnd_ = nullptr;
    DWORD hook_thread_id_ = 0;

    bool has_raw_signal_mouse_ = false;
    int32_t raw_signal_mouse_x_ = 0;
    int32_t raw_signal_mouse_y_ = 0;

    bool diagnostic_mode_ = false;
    std::ofstream diag_file_{};
    int64_t diag_raw_total_ = 0;
    int64_t diag_raw_keyboard_ = 0;
    int64_t diag_raw_mouse_ = 0;
    int64_t diag_raw_mouse_move_ = 0;
    int64_t diag_raw_mouse_button_or_wheel_ = 0;
    int64_t diag_raw_mouse_zero_delta_ = 0;
    int64_t diag_raw_mouse_relative_ = 0;
    int64_t diag_raw_mouse_absolute_ = 0;
    int64_t diag_get_raw_data_fail_ = 0;
};

}  // namespace wcs::input
