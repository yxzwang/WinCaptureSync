#include "main/app_controller.h"

#include <filesystem>
#include <exception>
#include <memory>
#include <sstream>
#include <utility>
#include <vector>

#include "common/logger.h"

namespace wcs::mainapp {

namespace {

std::filesystem::path VideoPathForSourceIndex(const SessionPaths& session_paths, const size_t index) {
    if (index == 0) {
        return session_paths.video_path;
    }
    return session_paths.session_dir / ("video_" + std::to_string(index + 1) + ".mp4");
}

std::filesystem::path VideoMetaPathForSourceIndex(const SessionPaths& session_paths,
                                                  const size_t index) {
    if (index == 0) {
        return session_paths.video_meta_path;
    }
    return session_paths.session_dir / ("video_" + std::to_string(index + 1) + "_meta.json");
}

wcs::encode::MfH264Encoder::VideoCodec ToEncoderCodec(const CaptureCodec codec) {
    return codec == CaptureCodec::HEVC ? wcs::encode::MfH264Encoder::VideoCodec::HEVC
                                       : wcs::encode::MfH264Encoder::VideoCodec::H264;
}

}  // namespace

AppController::AppController(AppConfig config) : config_(std::move(config)) {}

AppController::~AppController() {
    Shutdown();
}

bool AppController::Initialize(HWND hotkey_target) {
    wcs::common::log::Info("AppController Initialize");
    std::error_code ec;
    std::filesystem::create_directories(config_.output_root, ec);
    if (ec) {
        wcs::common::log::Error("Failed to create output directory: " + config_.output_root.string());
        SetStatus(L"Failed to create output directory");
        state_ = RecorderState::Error;
        return false;
    }

    if (!RegisterHotkey(hotkey_target)) {
        wcs::common::log::Error("Register hotkey failed");
        state_ = RecorderState::Error;
        return false;
    }

    SetStatus(L"Ready");
    wcs::common::log::Info("AppController initialized");
    return true;
}

void AppController::Shutdown() {
    if (state_ == RecorderState::Recording || state_ == RecorderState::Stopping) {
        StopRecording();
    }
    UnregisterHotkey();
}

bool AppController::ToggleRecording() {
    if (state_ == RecorderState::Idle) {
        return StartRecording();
    }
    if (state_ == RecorderState::Recording) {
        StopRecording();
        return true;
    }
    return false;
}

bool AppController::StartRecording() {
    if (state_ != RecorderState::Idle) {
        return false;
    }

    try {
        state_ = RecorderState::Arming;
        anchor_ = wcs::time::QpcClock::SampleUtcAnchor();
        session_paths_ = CreateSessionPaths(config_.output_root);
        wcs::common::log::Info("StartRecording arming session: " + session_paths_.session_dir.string());

        std::error_code ec;
        std::filesystem::create_directories(session_paths_.session_dir, ec);
        if (ec) {
            wcs::common::log::Error("Failed to create session directory: " +
                                    session_paths_.session_dir.string());
            SetStatus(L"Failed to create session directory");
            state_ = RecorderState::Error;
            return false;
        }

        std::vector<wcs::capture::CaptureSource> sources = capture_sources_;
        if (sources.empty()) {
            sources.push_back(wcs::capture::CaptureSource{});
        }

        for (const auto& source : sources) {
            if (source.type == wcs::capture::CaptureSourceType::Window) {
                if (source.window == nullptr || !IsWindow(source.window)) {
                    wcs::common::log::Warning("Selected window source is invalid");
                    SetStatus(L"Selected window is not valid");
                    state_ = RecorderState::Idle;
                    return false;
                }
            }
        }

        wcs::capture::ScreenRecorder::Options capture_options{};
        capture_options.fps = config_.capture_fps;
        capture_options.bitrate = config_.capture_bitrate;
        capture_options.codec = ToEncoderCodec(config_.capture_codec);

        wcs::input::InputRecorder::Options input_options{};
        input_options.queue_capacity = config_.input_queue_capacity;
        input_options.batch_size = config_.input_batch_size;
        input_options.flush_interval_ms = config_.input_flush_interval_ms;
        input_options.diagnostic_mode = config_.input_diagnostic_mode;

        screen_recorders_.clear();

        for (size_t i = 0; i < sources.size(); ++i) {
            auto recorder = std::make_unique<wcs::capture::ScreenRecorder>();
            auto source_capture_options = capture_options;
            if (i == 0) {
                source_capture_options.width = config_.capture_primary_width;
                source_capture_options.height = config_.capture_primary_height;
            } else if (i == 1) {
                source_capture_options.width = config_.capture_secondary_width;
                source_capture_options.height = config_.capture_secondary_height;
            } else {
                source_capture_options.width = config_.capture_primary_width;
                source_capture_options.height = config_.capture_primary_height;
            }
            source_capture_options.source = sources[i];
            source_capture_options.sources.clear();
            source_capture_options.sources.push_back(sources[i]);

            const auto video_path = VideoPathForSourceIndex(session_paths_, i);
            const auto video_meta_path = VideoMetaPathForSourceIndex(session_paths_, i);
            const bool screen_ok =
                recorder->Start(video_path, video_meta_path, anchor_, source_capture_options);
            if (!screen_ok) {
                wcs::common::log::Error("Screen recorder start failed for source index=" +
                                        std::to_string(i));
                for (auto& started_recorder : screen_recorders_) {
                    started_recorder->Stop();
                }
                screen_recorders_.clear();
                SetStatus(L"Screen recorder start failed");
                state_ = RecorderState::Idle;
                return false;
            }

            screen_recorders_.push_back(std::move(recorder));
        }

        const bool input_ok = input_recorder_.Start(session_paths_.input_path, anchor_, input_options);
        if (!input_ok) {
            wcs::common::log::Error("Input recorder start failed");
            for (auto& recorder : screen_recorders_) {
                recorder->Stop();
            }
            screen_recorders_.clear();
            SetStatus(L"Input recorder start failed");
            state_ = RecorderState::Idle;
            return false;
        }

        const int64_t input_start_qpc = input_recorder_.StartQpc();
        for (auto& recorder : screen_recorders_) {
            recorder->SetInputStartQpc(input_start_qpc);
        }
        state_ = RecorderState::Recording;

        std::wstringstream ss;
        ss << L"Recording: " << session_paths_.session_dir.wstring();
        SetStatus(ss.str());
        wcs::common::log::Info("Recording started");
        return true;
    } catch (const std::exception& ex) {
        wcs::common::log::Error(std::string("StartRecording exception: ") + ex.what());
    } catch (...) {
        wcs::common::log::Error("StartRecording unknown exception");
    }

    SetStatus(L"Start recording failed (exception)");
    state_ = RecorderState::Idle;
    return false;
}

void AppController::StopRecording() {
    if (state_ != RecorderState::Recording && state_ != RecorderState::Stopping) {
        return;
    }

    try {
        wcs::common::log::Info("StopRecording begin");
        state_ = RecorderState::Stopping;
        input_recorder_.Stop();
        const int64_t input_start_qpc = input_recorder_.StartQpc();
        for (auto& recorder : screen_recorders_) {
            recorder->SetInputStartQpc(input_start_qpc);
        }
        for (auto& recorder : screen_recorders_) {
            recorder->Stop();
        }
        screen_recorders_.clear();

        std::wstringstream ss;
        ss << L"Saved: " << session_paths_.session_dir.wstring();
        SetStatus(ss.str());
        state_ = RecorderState::Idle;
        wcs::common::log::Info("StopRecording completed");
    } catch (const std::exception& ex) {
        wcs::common::log::Error(std::string("StopRecording exception: ") + ex.what());
        state_ = RecorderState::Error;
    } catch (...) {
        wcs::common::log::Error("StopRecording unknown exception");
        state_ = RecorderState::Error;
    }
}

void AppController::SetCaptureSources(const std::vector<wcs::capture::CaptureSource>& sources) {
    capture_sources_ = sources;
}

void AppController::SetCaptureCodec(const CaptureCodec codec) {
    config_.capture_codec = codec;
}

void AppController::SetCaptureResolutions(const uint32_t primary_width,
                                          const uint32_t primary_height,
                                          const uint32_t secondary_width,
                                          const uint32_t secondary_height) {
    config_.capture_primary_width = primary_width;
    config_.capture_primary_height = primary_height;
    config_.capture_secondary_width = secondary_width;
    config_.capture_secondary_height = secondary_height;

    // Keep legacy config fields in sync for backward compatibility.
    config_.capture_width = primary_width;
    config_.capture_height = primary_height;
}

void AppController::SetInputDiagnosticMode(const bool enabled) {
    config_.input_diagnostic_mode = enabled;
}

void AppController::SetCaptureSource(const wcs::capture::CaptureSource& source) {
    capture_sources_.clear();
    capture_sources_.push_back(source);
}

wcs::capture::CaptureSource AppController::GetCaptureSource() const {
    if (!capture_sources_.empty()) {
        return capture_sources_.front();
    }
    return wcs::capture::CaptureSource{};
}

bool AppController::IsOurHotkey(const WPARAM hotkey_id) const {
    return static_cast<int>(hotkey_id) == kHotkeyId;
}

std::wstring AppController::LastSessionDir() const {
    return session_paths_.session_dir.wstring();
}

bool AppController::RegisterHotkey(HWND hotkey_target) {
    hotkey_target_ = hotkey_target;
    const UINT modifiers = config_.hotkey_modifiers | MOD_NOREPEAT;
    if (!RegisterHotKey(hotkey_target_, kHotkeyId, modifiers, config_.hotkey_vk)) {
        SetStatus(L"RegisterHotKey failed");
        return false;
    }
    return true;
}

void AppController::UnregisterHotkey() {
    if (hotkey_target_ != nullptr) {
        UnregisterHotKey(hotkey_target_, kHotkeyId);
    }
    hotkey_target_ = nullptr;
}

void AppController::SetStatus(const std::wstring& status) {
    status_text_ = status;
}

}  // namespace wcs::mainapp
