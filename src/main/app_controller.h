#pragma once

#include <Windows.h>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "capture/capture_source.h"
#include "capture/screen_recorder.h"
#include "input/input_recorder.h"
#include "main/config.h"
#include "main/session_paths.h"
#include "time/time_utils.h"

namespace wcs::mainapp {

enum class RecorderState {
    Idle,
    Arming,
    Recording,
    Stopping,
    Error
};

class AppController {
public:
    explicit AppController(AppConfig config);
    ~AppController();

    bool Initialize(HWND hotkey_target);
    void Shutdown();
    bool ToggleRecording();
    bool StartRecording();
    void StopRecording();

    void SetCaptureSources(const std::vector<wcs::capture::CaptureSource>& sources);
    void SetCaptureCodec(CaptureCodec codec);
    void SetCaptureResolutions(uint32_t primary_width,
                               uint32_t primary_height,
                               uint32_t secondary_width,
                               uint32_t secondary_height);
    void SetInputDiagnosticMode(bool enabled);
    CaptureCodec GetCaptureCodec() const { return config_.capture_codec; }
    bool GetInputDiagnosticMode() const { return config_.input_diagnostic_mode; }
    std::vector<wcs::capture::CaptureSource> GetCaptureSources() const { return capture_sources_; }
    void SetCaptureSource(const wcs::capture::CaptureSource& source);
    wcs::capture::CaptureSource GetCaptureSource() const;
    RecorderState State() const { return state_; }
    std::wstring StatusText() const { return status_text_; }
    bool IsOurHotkey(WPARAM hotkey_id) const;
    std::wstring LastSessionDir() const;

private:
    bool RegisterHotkey(HWND hotkey_target);
    void UnregisterHotkey();
    void SetStatus(const std::wstring& status);

    AppConfig config_{};
    RecorderState state_ = RecorderState::Idle;
    std::wstring status_text_ = L"Idle";
    HWND hotkey_target_ = nullptr;

    SessionPaths session_paths_{};
    wcs::time::UtcAnchor anchor_{};
    std::vector<std::unique_ptr<wcs::capture::ScreenRecorder>> screen_recorders_{};
    wcs::input::InputRecorder input_recorder_{};
    std::vector<wcs::capture::CaptureSource> capture_sources_{};

    static constexpr int kHotkeyId = 0xBEEF;
};

}  // namespace wcs::mainapp
