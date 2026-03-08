#pragma once

#include <Windows.h>
#include <dwmapi.h>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "gui/window_selector.h"
#include "main/app_controller.h"
#include "main/config.h"

namespace wcs::gui {

class MainWindow {
public:
    explicit MainWindow(HINSTANCE instance);
    ~MainWindow();

    bool CreateAndShow(int show_cmd);
    int Run();

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM w_param, LPARAM l_param);
    LRESULT HandleMessage(UINT msg, WPARAM w_param, LPARAM l_param);

    void OnCreate();
    void OnDestroy();
    void OnSize(int width, int height);
    void OnPaint();
    void OnTimer();
    void OnCommand(WPARAM w_param, LPARAM l_param);

    void ApplyUiOptionsToController();
    void RebuildWindowList();
    void SaveUiState();
    void RestoreUiStateFromConfig();
    void SyncRecordingDuration(wcs::mainapp::RecorderState prev_state,
                               wcs::mainapp::RecorderState current_state);
    void UpdateRecordingDurationText(bool force);
    std::string SourceIdFromPrimaryIndex(int index) const;
    std::string SourceIdFromSecondaryIndex(int index) const;
    int FindPrimaryIndexBySourceId(const std::string& source_id) const;
    int FindSecondaryIndexBySourceId(const std::string& source_id) const;
    void UpdateControlState();
    void UpdateStatus(const std::wstring& text);
    void EnsurePreviewBuffer(int width, int height);
    void ReleasePreviewBuffer();
    void UpdatePreviewPipeline();
    void UpdateDwmThumbnail();
    void ReleaseDwmThumbnail();
    void RenderPreviewFrame();
    bool IsWindowModeSelected() const;
    wcs::mainapp::CaptureCodec CurrentCodecFromUi() const;
    std::pair<uint32_t, uint32_t> ResolutionFromCombo(HWND combo) const;
    int ResolutionPresetIndex(uint32_t width, uint32_t height) const;
    std::vector<wcs::capture::CaptureSource> CurrentSourcesFromUi() const;
    std::wstring FormatMonitorLabel(const MonitorEntry& entry, size_t index) const;
    std::wstring FormatWindowLabel(const WindowEntry& entry) const;

    HINSTANCE instance_ = nullptr;
    HWND hwnd_ = nullptr;

    HWND source_mode_combo_ = nullptr;
    HWND codec_combo_ = nullptr;
    HWND diagnostic_check_ = nullptr;
    HWND resolution_primary_label_ = nullptr;
    HWND resolution_primary_combo_ = nullptr;
    HWND resolution_secondary_label_ = nullptr;
    HWND resolution_secondary_combo_ = nullptr;
    HWND window_primary_combo_ = nullptr;
    HWND window_secondary_combo_ = nullptr;
    HWND refresh_button_ = nullptr;
    HWND start_stop_button_ = nullptr;
    HWND recording_duration_label_ = nullptr;
    HWND recording_duration_value_ = nullptr;
    HWND status_label_ = nullptr;
    HWND hotkey_label_ = nullptr;

    RECT preview_rect_{};
    HDC preview_dc_ = nullptr;
    HBITMAP preview_bitmap_ = nullptr;
    HGDIOBJ preview_old_bitmap_ = nullptr;
    int preview_width_ = 0;
    int preview_height_ = 0;
    HTHUMBNAIL preview_thumbnail_primary_ = nullptr;
    HTHUMBNAIL preview_thumbnail_secondary_ = nullptr;
    HWND preview_thumbnail_primary_source_ = nullptr;
    HWND preview_thumbnail_secondary_source_ = nullptr;
    bool using_dwm_thumbnail_ = false;

    UINT_PTR preview_timer_ = 0;
    std::vector<MonitorEntry> monitors_{};
    std::vector<WindowEntry> windows_{};

    wcs::mainapp::AppConfig config_{};
    wcs::mainapp::AppController controller_;
    ULONGLONG recording_duration_start_ms_ = 0;
    ULONGLONG recording_duration_last_seconds_ = static_cast<ULONGLONG>(-1);
};

}  // namespace wcs::gui
