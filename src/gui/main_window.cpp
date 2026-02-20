#include "gui/main_window.h"

#include <algorithm>
#include <cstdint>
#include <sstream>
#include <string>

namespace wcs::gui {

namespace {

constexpr wchar_t kWindowClassName[] = L"WCS_MAIN_WINDOW";
constexpr UINT_PTR kPreviewTimerId = 1;
constexpr UINT kPreviewIntervalMs = 33;

constexpr int kIdSourceMode = 1001;
constexpr int kIdWindowPrimaryCombo = 1002;
constexpr int kIdWindowSecondaryCombo = 1007;
constexpr int kIdCodecCombo = 1008;
constexpr int kIdDiagnosticCheck = 1009;
constexpr int kIdRefreshBtn = 1003;
constexpr int kIdStartStopBtn = 1004;
constexpr int kIdStatusLabel = 1005;
constexpr int kIdHotkeyLabel = 1006;

bool IsValidRect(const RECT& rect) {
    return rect.right > rect.left && rect.bottom > rect.top;
}

bool IsMonitorSourceType(const wcs::capture::CaptureSourceType type) {
    return type == wcs::capture::CaptureSourceType::PrimaryMonitor ||
           type == wcs::capture::CaptureSourceType::Monitor;
}

bool ResolveMonitorRectFromSource(const wcs::capture::CaptureSource& source, RECT* rect) {
    if (rect == nullptr || !IsMonitorSourceType(source.type)) {
        return false;
    }

    if (source.type == wcs::capture::CaptureSourceType::Monitor && IsValidRect(source.monitor_rect)) {
        *rect = source.monitor_rect;
        return true;
    }

    HMONITOR monitor = source.monitor;
    if (monitor == nullptr && source.type == wcs::capture::CaptureSourceType::PrimaryMonitor) {
        const POINT origin{};
        monitor = MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);
    }
    if (monitor == nullptr) {
        return false;
    }

    MONITORINFO info{};
    info.cbSize = sizeof(MONITORINFO);
    if (!GetMonitorInfoW(monitor, &info) || !IsValidRect(info.rcMonitor)) {
        return false;
    }

    *rect = info.rcMonitor;
    return true;
}

RECT ProjectRectToPreview(const RECT& source_rect, const RECT& source_clip, const RECT& preview_draw_rect) {
    RECT out{};
    const int src_w = source_rect.right - source_rect.left;
    const int src_h = source_rect.bottom - source_rect.top;
    const int dst_w = preview_draw_rect.right - preview_draw_rect.left;
    const int dst_h = preview_draw_rect.bottom - preview_draw_rect.top;
    if (src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0) {
        return out;
    }

    out.left = preview_draw_rect.left +
               static_cast<LONG>((static_cast<int64_t>(source_clip.left - source_rect.left) * dst_w) /
                                 src_w);
    out.top = preview_draw_rect.top +
              static_cast<LONG>((static_cast<int64_t>(source_clip.top - source_rect.top) * dst_h) /
                                src_h);
    out.right = preview_draw_rect.left +
                static_cast<LONG>((static_cast<int64_t>(source_clip.right - source_rect.left) * dst_w) /
                                  src_w);
    out.bottom = preview_draw_rect.top +
                 static_cast<LONG>((static_cast<int64_t>(source_clip.bottom - source_rect.top) * dst_h) /
                                   src_h);
    return out;
}

void MaskMainWindowFromMonitorPreview(const std::vector<wcs::capture::CaptureSource>& sources,
                                      HWND main_hwnd,
                                      HDC target_dc,
                                      const int target_width,
                                      const int target_height) {
    if (sources.empty() || main_hwnd == nullptr || target_dc == nullptr || target_width <= 0 ||
        target_height <= 0 || !IsWindow(main_hwnd)) {
        return;
    }

    RECT main_rect{};
    if (!GetWindowRect(main_hwnd, &main_rect) || !IsValidRect(main_rect)) {
        return;
    }

    struct SourceInfo {
        wcs::capture::CaptureSource source{};
        uint32_t width = 0;
        uint32_t height = 0;
        RECT monitor_rect{};
        bool has_monitor_rect = false;
    };

    std::vector<SourceInfo> infos;
    infos.reserve(sources.size());
    uint64_t total_source_width = 0;

    for (const auto& source : sources) {
        uint32_t src_w = 0;
        uint32_t src_h = 0;
        if (!wcs::capture::ResolveSourceSize(source, &src_w, &src_h)) {
            continue;
        }

        SourceInfo info{};
        info.source = source;
        info.width = src_w;
        info.height = src_h;
        info.has_monitor_rect = ResolveMonitorRectFromSource(source, &info.monitor_rect);
        infos.push_back(info);
        total_source_width += src_w;
    }

    if (infos.empty()) {
        return;
    }

    HBRUSH mask_brush = CreateSolidBrush(RGB(20, 20, 20));
    if (mask_brush == nullptr) {
        return;
    }

    auto mask_info = [&](const SourceInfo& info, const RECT& draw_rect) {
        if (!info.has_monitor_rect || !IsValidRect(info.monitor_rect) || !IsValidRect(draw_rect)) {
            return;
        }

        RECT overlap{};
        if (!IntersectRect(&overlap, &main_rect, &info.monitor_rect) || !IsValidRect(overlap)) {
            return;
        }

        RECT projected = ProjectRectToPreview(info.monitor_rect, overlap, draw_rect);
        if (!IsValidRect(projected)) {
            return;
        }

        FillRect(target_dc, &projected, mask_brush);
    };

    if (sources.size() == 1) {
        const RECT full = {0, 0, target_width, target_height};
        mask_info(infos.front(), full);
        DeleteObject(mask_brush);
        return;
    }

    if (total_source_width == 0) {
        DeleteObject(mask_brush);
        return;
    }

    uint64_t cumulative_width = 0;
    for (size_t i = 0; i < infos.size(); ++i) {
        const auto& info = infos[i];
        const int left = static_cast<int>((cumulative_width * static_cast<uint64_t>(target_width)) /
                                          total_source_width);
        cumulative_width += info.width;
        const int right = (i + 1 == infos.size())
                              ? target_width
                              : static_cast<int>((cumulative_width * static_cast<uint64_t>(target_width)) /
                                                 total_source_width);
        const int slot_width = (std::max)(1, right - left);
        const int slot_height = target_height;

        const double sx = static_cast<double>(slot_width) / static_cast<double>(info.width);
        const double sy = static_cast<double>(slot_height) / static_cast<double>(info.height);
        const double scale = (std::min)(sx, sy);
        const int draw_width = (std::max)(1, static_cast<int>(info.width * scale));
        const int draw_height = (std::max)(1, static_cast<int>(info.height * scale));
        const int draw_x = left + (slot_width - draw_width) / 2;
        const int draw_y = (slot_height - draw_height) / 2;

        const RECT draw_rect = {draw_x, draw_y, draw_x + draw_width, draw_y + draw_height};
        mask_info(info, draw_rect);
    }

    DeleteObject(mask_brush);
}

bool IsWindowSource(const wcs::capture::CaptureSource& source) {
    return source.type == wcs::capture::CaptureSourceType::Window && source.window != nullptr;
}

bool AreAllWindowSources(const std::vector<wcs::capture::CaptureSource>& sources) {
    if (sources.empty()) {
        return false;
    }
    for (const auto& source : sources) {
        if (!IsWindowSource(source)) {
            return false;
        }
    }
    return true;
}

bool AreSameSource(const wcs::capture::CaptureSource& a, const wcs::capture::CaptureSource& b) {
    if (a.type == wcs::capture::CaptureSourceType::Window ||
        b.type == wcs::capture::CaptureSourceType::Window) {
        return a.type == wcs::capture::CaptureSourceType::Window &&
               b.type == wcs::capture::CaptureSourceType::Window && a.window == b.window &&
               a.window != nullptr;
    }

    if (a.monitor != nullptr && b.monitor != nullptr) {
        return a.monitor == b.monitor;
    }

    return a.monitor_rect.left == b.monitor_rect.left && a.monitor_rect.top == b.monitor_rect.top &&
           a.monitor_rect.right == b.monitor_rect.right &&
           a.monitor_rect.bottom == b.monitor_rect.bottom;
}

}  // namespace

MainWindow::MainWindow(HINSTANCE instance)
    : instance_(instance), config_(wcs::mainapp::LoadConfig("config.ini")), controller_(config_) {}

MainWindow::~MainWindow() {
    controller_.Shutdown();
    ReleasePreviewBuffer();
}

bool MainWindow::CreateAndShow(const int show_cmd) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.lpfnWndProc = &MainWindow::WndProc;
    wc.hInstance = instance_;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = kWindowClassName;
    if (!RegisterClassExW(&wc)) {
        return false;
    }

    hwnd_ = CreateWindowExW(0, kWindowClassName, L"WinCaptureSync",
                            WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
                            CW_USEDEFAULT, CW_USEDEFAULT, 980, 680, nullptr, nullptr, instance_,
                            this);
    if (hwnd_ == nullptr) {
        return false;
    }

    ShowWindow(hwnd_, show_cmd);
    UpdateWindow(hwnd_);
    return true;
}

int MainWindow::Run() {
    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return static_cast<int>(msg.wParam);
}

LRESULT CALLBACK MainWindow::WndProc(HWND hwnd, const UINT msg, const WPARAM w_param,
                                     const LPARAM l_param) {
    MainWindow* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(l_param);
        self = static_cast<MainWindow*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
    } else {
        self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    if (self != nullptr) {
        return self->HandleMessage(msg, w_param, l_param);
    }
    return DefWindowProcW(hwnd, msg, w_param, l_param);
}

LRESULT MainWindow::HandleMessage(const UINT msg, const WPARAM w_param, const LPARAM l_param) {
    switch (msg) {
        case WM_CREATE:
            OnCreate();
            return 0;
        case WM_DESTROY:
            OnDestroy();
            return 0;
        case WM_SIZE:
            OnSize(LOWORD(l_param), HIWORD(l_param));
            return 0;
        case WM_COMMAND:
            OnCommand(w_param, l_param);
            return 0;
        case WM_TIMER:
            if (w_param == kPreviewTimerId) {
                OnTimer();
                return 0;
            }
            break;
        case WM_PAINT:
            OnPaint();
            return 0;
        case WM_ERASEBKGND:
            // Paint is fully handled in WM_PAINT to reduce flicker.
            return 1;
        case WM_HOTKEY:
            if (controller_.IsOurHotkey(w_param)) {
                if (controller_.State() == wcs::mainapp::RecorderState::Idle) {
                    const auto sources = CurrentSourcesFromUi();
                    if (sources.empty()) {
                        UpdateStatus(L"Please select at least one source");
                        return 0;
                    }
                    controller_.SetCaptureSources(sources);
                    controller_.SetCaptureCodec(CurrentCodecFromUi());
                    controller_.SetInputDiagnosticMode(
                        SendMessageW(diagnostic_check_, BM_GETCHECK, 0, 0) == BST_CHECKED);
                }
                controller_.ToggleRecording();
                UpdateControlState();
                UpdateStatus(controller_.StatusText());
                UpdatePreviewPipeline();
            }
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd_, msg, w_param, l_param);
}

void MainWindow::OnCreate() {
    CreateWindowExW(0, L"STATIC", L"Source", WS_CHILD | WS_VISIBLE, 12, 12, 56, 24, hwnd_, nullptr,
                    instance_, nullptr);
    source_mode_combo_ = CreateWindowExW(0, L"COMBOBOX", nullptr,
                                         WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                                         72, 10, 150, 240, hwnd_,
                                         reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdSourceMode)),
                                         instance_, nullptr);
    SendMessageW(source_mode_combo_, CB_ADDSTRING, 0,
                 reinterpret_cast<LPARAM>(L"Primary Monitor"));
    SendMessageW(source_mode_combo_, CB_ADDSTRING, 0,
                 reinterpret_cast<LPARAM>(L"Window / Monitor"));
    SendMessageW(source_mode_combo_, CB_SETCURSEL, 0, 0);

    CreateWindowExW(0, L"STATIC", L"Codec", WS_CHILD | WS_VISIBLE, 12, 46, 56, 24, hwnd_, nullptr,
                    instance_, nullptr);
    codec_combo_ = CreateWindowExW(0, L"COMBOBOX", nullptr,
                                   WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL, 72, 44,
                                   150, 120, hwnd_,
                                   reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdCodecCombo)),
                                   instance_, nullptr);
    SendMessageW(codec_combo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"H.264"));
    SendMessageW(codec_combo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"HEVC (H.265)"));
    SendMessageW(codec_combo_, CB_SETCURSEL,
                 config_.capture_codec == wcs::mainapp::CaptureCodec::HEVC ? 1 : 0, 0);
    diagnostic_check_ =
        CreateWindowExW(0, L"BUTTON", L"Input Diagnostic", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                        236, 44, 150, 24, hwnd_,
                        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdDiagnosticCheck)), instance_,
                        nullptr);
    SendMessageW(diagnostic_check_, BM_SETCHECK,
                 config_.input_diagnostic_mode ? BST_CHECKED : BST_UNCHECKED, 0);

    CreateWindowExW(0, L"STATIC", L"Window 1", WS_CHILD | WS_VISIBLE, 236, 12, 62, 24, hwnd_,
                    nullptr, instance_, nullptr);
    window_primary_combo_ = CreateWindowExW(
        0, L"COMBOBOX", nullptr, WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL, 300, 10,
        300, 300, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdWindowPrimaryCombo)),
        instance_, nullptr);

    CreateWindowExW(0, L"STATIC", L"Window 2", WS_CHILD | WS_VISIBLE, 606, 12, 62, 24, hwnd_,
                    nullptr, instance_, nullptr);
    window_secondary_combo_ = CreateWindowExW(
        0, L"COMBOBOX", nullptr, WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL, 670, 10,
        300, 300, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdWindowSecondaryCombo)),
        instance_, nullptr);

    refresh_button_ = CreateWindowExW(0, L"BUTTON", L"Refresh", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                       760, 44, 96, 24, hwnd_,
                                       reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdRefreshBtn)),
                                       instance_, nullptr);

    start_stop_button_ =
        CreateWindowExW(0, L"BUTTON", L"Start", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, 864, 44,
                        104, 24, hwnd_,
                        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdStartStopBtn)), instance_,
                        nullptr);

    hotkey_label_ = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE, 12, 72, 460, 24, hwnd_,
                                    reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdHotkeyLabel)),
                                    instance_, nullptr);
    status_label_ = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE, 480, 72, 472, 24, hwnd_,
                                    reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdStatusLabel)),
                                    instance_, nullptr);

    std::wstring hotkey_text = L"Hotkey: ";
    const auto hotkey_ascii = wcs::mainapp::HotkeyToString(config_.hotkey_modifiers, config_.hotkey_vk);
    hotkey_text.append(hotkey_ascii.begin(), hotkey_ascii.end());
    SetWindowTextW(hotkey_label_, hotkey_text.c_str());

    RebuildWindowList();

    if (!controller_.Initialize(hwnd_)) {
        UpdateStatus(std::wstring(L"Init failed: ") + controller_.StatusText());
    } else {
        UpdateStatus(L"Ready");
    }

    preview_timer_ = SetTimer(hwnd_, kPreviewTimerId, kPreviewIntervalMs, nullptr);
    UpdateControlState();
    UpdatePreviewPipeline();
}

void MainWindow::OnDestroy() {
    if (preview_timer_ != 0) {
        KillTimer(hwnd_, preview_timer_);
        preview_timer_ = 0;
    }
    ReleaseDwmThumbnail();
    controller_.Shutdown();
    ReleasePreviewBuffer();
    PostQuitMessage(0);
}

void MainWindow::OnSize(const int width, const int height) {
    const int margin = 12;
    const int top_controls_h = 104;

    MoveWindow(source_mode_combo_, 72, 10, 150, 300, TRUE);
    MoveWindow(codec_combo_, 72, 44, 150, 240, TRUE);
    MoveWindow(diagnostic_check_, 236, 44, 150, 24, TRUE);
    const int windows_left = 300;
    const int windows_total = (std::max)(220, width - windows_left - margin);
    const int each_window_width = (std::max)(110, (windows_total - 8) / 2);
    MoveWindow(window_primary_combo_, windows_left, 10, each_window_width, 300, TRUE);
    MoveWindow(window_secondary_combo_, windows_left + each_window_width + 8, 10,
               each_window_width, 300, TRUE);
    const int right_x = width - margin;
    MoveWindow(refresh_button_, right_x - 104 - 96, 44, 96, 24, TRUE);
    MoveWindow(start_stop_button_, right_x - 104, 44, 104, 24, TRUE);
    MoveWindow(hotkey_label_, margin, 72, (std::max)(200, width / 2 - margin), 24, TRUE);
    MoveWindow(status_label_, width / 2, 72, (std::max)(180, width / 2 - margin), 24, TRUE);

    preview_rect_.left = margin;
    preview_rect_.top = top_controls_h + margin;
    preview_rect_.right = width - margin;
    preview_rect_.bottom = height - margin;
    if (preview_rect_.right < preview_rect_.left) {
        preview_rect_.right = preview_rect_.left;
    }
    if (preview_rect_.bottom < preview_rect_.top) {
        preview_rect_.bottom = preview_rect_.top;
    }

    const int preview_w = preview_rect_.right - preview_rect_.left;
    const int preview_h = preview_rect_.bottom - preview_rect_.top;
    EnsurePreviewBuffer(preview_w, preview_h);
    UpdateDwmThumbnail();
}

void MainWindow::OnPaint() {
    PAINTSTRUCT ps{};
    HDC hdc = BeginPaint(hwnd_, &ps);

    RECT client{};
    GetClientRect(hwnd_, &client);
    const int client_w = (std::max)(1, static_cast<int>(client.right - client.left));
    const int client_h = (std::max)(1, static_cast<int>(client.bottom - client.top));

    HDC back_dc = CreateCompatibleDC(hdc);
    HBITMAP back_bmp = CreateCompatibleBitmap(hdc, client_w, client_h);
    HGDIOBJ old_back_bmp = nullptr;
    if (back_dc != nullptr && back_bmp != nullptr) {
        old_back_bmp = SelectObject(back_dc, back_bmp);
    }

    HDC paint_dc = (back_dc != nullptr && back_bmp != nullptr) ? back_dc : hdc;

    HBRUSH bg = CreateSolidBrush(RGB(245, 247, 250));
    FillRect(paint_dc, &client, bg);
    DeleteObject(bg);

    RECT border = preview_rect_;
    HBRUSH frame_brush = CreateSolidBrush(RGB(40, 44, 52));
    FrameRect(paint_dc, &border, frame_brush);
    DeleteObject(frame_brush);

    if (!using_dwm_thumbnail_ && preview_dc_ != nullptr && preview_width_ > 0 && preview_height_ > 0) {
        StretchBlt(paint_dc, preview_rect_.left + 1, preview_rect_.top + 1,
                   (preview_rect_.right - preview_rect_.left) - 2,
                   (preview_rect_.bottom - preview_rect_.top) - 2, preview_dc_, 0, 0, preview_width_,
                   preview_height_, SRCCOPY);
    }

    if (paint_dc == back_dc) {
        BitBlt(hdc, 0, 0, client_w, client_h, back_dc, 0, 0, SRCCOPY);
    }

    if (back_dc != nullptr) {
        if (old_back_bmp != nullptr) {
            SelectObject(back_dc, old_back_bmp);
        }
        if (back_bmp != nullptr) {
            DeleteObject(back_bmp);
        }
        DeleteDC(back_dc);
    }

    EndPaint(hwnd_, &ps);
}

void MainWindow::OnTimer() {
    const auto state = controller_.State();
    if (state == wcs::mainapp::RecorderState::Recording ||
        state == wcs::mainapp::RecorderState::Stopping) {
        // Avoid competing monitor grabs between preview and recorder threads.
        return;
    }

    if (using_dwm_thumbnail_) {
        return;
    }
    RenderPreviewFrame();
    InvalidateRect(hwnd_, &preview_rect_, FALSE);
}

void MainWindow::OnCommand(const WPARAM w_param, const LPARAM) {
    const int id = LOWORD(w_param);
    const int code = HIWORD(w_param);

    if (id == kIdRefreshBtn && code == BN_CLICKED) {
        RebuildWindowList();
        UpdatePreviewPipeline();
        return;
    }
    if (id == kIdStartStopBtn && code == BN_CLICKED) {
        if (controller_.State() == wcs::mainapp::RecorderState::Idle) {
            const auto sources = CurrentSourcesFromUi();
            if (sources.empty()) {
                UpdateStatus(L"Please select at least one source");
                return;
            }
            controller_.SetCaptureSources(sources);
            controller_.SetCaptureCodec(CurrentCodecFromUi());
            controller_.SetInputDiagnosticMode(
                SendMessageW(diagnostic_check_, BM_GETCHECK, 0, 0) == BST_CHECKED);
        }
        controller_.ToggleRecording();
        UpdateControlState();
        UpdateStatus(controller_.StatusText());
        UpdatePreviewPipeline();
        return;
    }
    if (id == kIdSourceMode && code == CBN_SELCHANGE) {
        UpdateControlState();
        UpdatePreviewPipeline();
        return;
    }
    if (id == kIdCodecCombo && code == CBN_SELCHANGE) {
        if (controller_.State() == wcs::mainapp::RecorderState::Idle) {
            controller_.SetCaptureCodec(CurrentCodecFromUi());
        }
        return;
    }
    if (id == kIdDiagnosticCheck && code == BN_CLICKED) {
        if (controller_.State() == wcs::mainapp::RecorderState::Idle) {
            controller_.SetInputDiagnosticMode(
                SendMessageW(diagnostic_check_, BM_GETCHECK, 0, 0) == BST_CHECKED);
        }
        return;
    }
    if (id == kIdWindowPrimaryCombo && code == CBN_SELCHANGE) {
        UpdateControlState();
        UpdatePreviewPipeline();
        return;
    }
    if (id == kIdWindowSecondaryCombo && code == CBN_SELCHANGE) {
        UpdateControlState();
        UpdatePreviewPipeline();
        return;
    }
}

void MainWindow::RebuildWindowList() {
    monitors_ = EnumerateMonitors();
    windows_ = EnumerateRecordableWindows(hwnd_);
    SendMessageW(window_primary_combo_, CB_RESETCONTENT, 0, 0);
    SendMessageW(window_secondary_combo_, CB_RESETCONTENT, 0, 0);
    SendMessageW(window_secondary_combo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"<None>"));

    for (size_t i = 0; i < monitors_.size(); ++i) {
        const std::wstring label = FormatMonitorLabel(monitors_[i], i);
        SendMessageW(window_primary_combo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
        SendMessageW(window_secondary_combo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
    }

    for (const auto& entry : windows_) {
        const std::wstring label = FormatWindowLabel(entry);
        SendMessageW(window_primary_combo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
        SendMessageW(window_secondary_combo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
    }

    const size_t selectable_count = monitors_.size() + windows_.size();
    if (selectable_count > 0) {
        SendMessageW(window_primary_combo_, CB_SETCURSEL, 0, 0);
    }
    SendMessageW(window_secondary_combo_, CB_SETCURSEL, 0, 0);
    UpdateControlState();
}

void MainWindow::UpdateControlState() {
    const bool is_recording = controller_.State() == wcs::mainapp::RecorderState::Recording ||
                              controller_.State() == wcs::mainapp::RecorderState::Stopping;
    const bool window_mode = (SendMessageW(source_mode_combo_, CB_GETCURSEL, 0, 0) == 1);
    const LRESULT selected_primary = SendMessageW(window_primary_combo_, CB_GETCURSEL, 0, 0);
    const bool has_primary_source = selected_primary >= 0;

    EnableWindow(source_mode_combo_, !is_recording);
    EnableWindow(codec_combo_, !is_recording);
    EnableWindow(diagnostic_check_, !is_recording);
    EnableWindow(window_primary_combo_, !is_recording && window_mode);
    EnableWindow(window_secondary_combo_, !is_recording && window_mode);
    EnableWindow(refresh_button_, !is_recording && window_mode);
    EnableWindow(start_stop_button_, is_recording || !window_mode || has_primary_source);
    SetWindowTextW(start_stop_button_, is_recording ? L"Stop" : L"Start");
}

void MainWindow::UpdateStatus(const std::wstring& text) {
    if (status_label_ != nullptr) {
        SetWindowTextW(status_label_, text.c_str());
    }
}

void MainWindow::EnsurePreviewBuffer(const int width, const int height) {
    if (width <= 0 || height <= 0) {
        ReleasePreviewBuffer();
        return;
    }
    if (preview_dc_ != nullptr && width == preview_width_ && height == preview_height_) {
        return;
    }

    ReleasePreviewBuffer();

    HDC window_dc = GetDC(hwnd_);
    preview_dc_ = CreateCompatibleDC(window_dc);
    ReleaseDC(hwnd_, window_dc);
    if (preview_dc_ == nullptr) {
        return;
    }

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = -height;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    preview_bitmap_ = CreateDIBSection(preview_dc_, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (preview_bitmap_ == nullptr) {
        DeleteDC(preview_dc_);
        preview_dc_ = nullptr;
        return;
    }
    preview_old_bitmap_ = SelectObject(preview_dc_, preview_bitmap_);
    preview_width_ = width;
    preview_height_ = height;
    PatBlt(preview_dc_, 0, 0, preview_width_, preview_height_, BLACKNESS);
}

void MainWindow::ReleasePreviewBuffer() {
    if (preview_dc_ != nullptr) {
        if (preview_old_bitmap_ != nullptr) {
            SelectObject(preview_dc_, preview_old_bitmap_);
            preview_old_bitmap_ = nullptr;
        }
        if (preview_bitmap_ != nullptr) {
            DeleteObject(preview_bitmap_);
            preview_bitmap_ = nullptr;
        }
        DeleteDC(preview_dc_);
        preview_dc_ = nullptr;
    }
    preview_width_ = 0;
    preview_height_ = 0;
}

void MainWindow::UpdatePreviewPipeline() {
    const auto sources = CurrentSourcesFromUi();
    if (IsWindowModeSelected() && AreAllWindowSources(sources)) {
        UpdateDwmThumbnail();
        using_dwm_thumbnail_ =
            (preview_thumbnail_primary_ != nullptr || preview_thumbnail_secondary_ != nullptr);
    } else {
        ReleaseDwmThumbnail();
        using_dwm_thumbnail_ = false;
    }
    InvalidateRect(hwnd_, &preview_rect_, FALSE);
}

void MainWindow::UpdateDwmThumbnail() {
    if (!IsWindowModeSelected()) {
        ReleaseDwmThumbnail();
        return;
    }

    const auto sources = CurrentSourcesFromUi();
    if (sources.empty() || !AreAllWindowSources(sources)) {
        ReleaseDwmThumbnail();
        return;
    }

    auto ensure_thumbnail = [this](HTHUMBNAIL* thumbnail, HWND* thumbnail_source, HWND source_hwnd) {
        if (source_hwnd == nullptr || !IsWindow(source_hwnd)) {
            if (*thumbnail != nullptr) {
                DwmUnregisterThumbnail(*thumbnail);
                *thumbnail = nullptr;
            }
            *thumbnail_source = nullptr;
            return false;
        }

        if (*thumbnail == nullptr || *thumbnail_source != source_hwnd) {
            if (*thumbnail != nullptr) {
                DwmUnregisterThumbnail(*thumbnail);
                *thumbnail = nullptr;
            }
            if (FAILED(DwmRegisterThumbnail(hwnd_, source_hwnd, thumbnail))) {
                *thumbnail = nullptr;
                *thumbnail_source = nullptr;
                return false;
            }
            *thumbnail_source = source_hwnd;
        }
        return true;
    };

    HWND primary_hwnd = sources[0].window;
    HWND secondary_hwnd = (sources.size() > 1) ? sources[1].window : nullptr;

    const bool primary_ok = ensure_thumbnail(&preview_thumbnail_primary_,
                                             &preview_thumbnail_primary_source_, primary_hwnd);
    const bool secondary_ok = ensure_thumbnail(&preview_thumbnail_secondary_,
                                               &preview_thumbnail_secondary_source_, secondary_hwnd);

    if (!primary_ok && !secondary_ok) {
        ReleaseDwmThumbnail();
        return;
    }

    const RECT inner = {preview_rect_.left + 1, preview_rect_.top + 1,
                        (std::max)(preview_rect_.left + 2, preview_rect_.right - 1),
                        (std::max)(preview_rect_.top + 2, preview_rect_.bottom - 1)};

    auto update_props = [](HTHUMBNAIL thumbnail, const RECT& dst) {
        if (thumbnail == nullptr) {
            return;
        }
        DWM_THUMBNAIL_PROPERTIES props{};
        props.dwFlags = DWM_TNP_RECTDESTINATION | DWM_TNP_VISIBLE | DWM_TNP_OPACITY |
                        DWM_TNP_SOURCECLIENTAREAONLY;
        props.fVisible = TRUE;
        props.opacity = 255;
        props.fSourceClientAreaOnly = FALSE;
        props.rcDestination = dst;
        DwmUpdateThumbnailProperties(thumbnail, &props);
    };

    if (secondary_ok) {
        const int split = inner.left + ((inner.right - inner.left) / 2);
        RECT left = {inner.left, inner.top, split - 1, inner.bottom};
        RECT right = {split + 1, inner.top, inner.right, inner.bottom};
        update_props(preview_thumbnail_primary_, left);
        update_props(preview_thumbnail_secondary_, right);
    } else {
        update_props(preview_thumbnail_primary_, inner);
    }
}

void MainWindow::ReleaseDwmThumbnail() {
    if (preview_thumbnail_primary_ != nullptr) {
        DwmUnregisterThumbnail(preview_thumbnail_primary_);
        preview_thumbnail_primary_ = nullptr;
    }
    if (preview_thumbnail_secondary_ != nullptr) {
        DwmUnregisterThumbnail(preview_thumbnail_secondary_);
        preview_thumbnail_secondary_ = nullptr;
    }
    preview_thumbnail_primary_source_ = nullptr;
    preview_thumbnail_secondary_source_ = nullptr;
}

void MainWindow::RenderPreviewFrame() {
    if (preview_dc_ == nullptr || preview_width_ <= 0 || preview_height_ <= 0) {
        return;
    }

    const auto sources = CurrentSourcesFromUi();
    if (sources.empty()) {
        PatBlt(preview_dc_, 0, 0, preview_width_, preview_height_, BLACKNESS);
        return;
    }

    // Keep the last good frame if this capture attempt fails.
    if (wcs::capture::CaptureSourcesToDc(sources, preview_dc_, preview_width_, preview_height_)) {
        MaskMainWindowFromMonitorPreview(sources, hwnd_, preview_dc_, preview_width_, preview_height_);
    }
}

wcs::capture::CaptureSource BuildWindowSourceFromIndex(const std::vector<WindowEntry>& windows,
                                                       const std::vector<MonitorEntry>& monitors,
                                                       LRESULT index) {
    wcs::capture::CaptureSource source{};
    if (index < 0) {
        source.type = wcs::capture::CaptureSourceType::Window;
        source.window = nullptr;
        return source;
    }

    const size_t idx = static_cast<size_t>(index);
    if (idx < monitors.size()) {
        const auto& monitor = monitors[idx];
        source.type = monitor.is_primary ? wcs::capture::CaptureSourceType::PrimaryMonitor
                                         : wcs::capture::CaptureSourceType::Monitor;
        source.monitor = monitor.monitor;
        source.monitor_rect = monitor.rect;
        source.monitor_name = monitor.device_name;
        return source;
    }

    const size_t window_index = idx - monitors.size();
    source.type = wcs::capture::CaptureSourceType::Window;
    if (window_index < windows.size()) {
        source.window = windows[window_index].hwnd;
        source.window_title = windows[window_index].title;
    }
    return source;
}

wcs::mainapp::CaptureCodec MainWindow::CurrentCodecFromUi() const {
    const LRESULT idx = SendMessageW(codec_combo_, CB_GETCURSEL, 0, 0);
    return idx == 1 ? wcs::mainapp::CaptureCodec::HEVC : wcs::mainapp::CaptureCodec::H264;
}

std::vector<wcs::capture::CaptureSource> MainWindow::CurrentSourcesFromUi() const {
    std::vector<wcs::capture::CaptureSource> sources;
    const LRESULT mode = SendMessageW(source_mode_combo_, CB_GETCURSEL, 0, 0);
    if (mode == 1) {
        const LRESULT primary_idx = SendMessageW(window_primary_combo_, CB_GETCURSEL, 0, 0);
        const auto primary = BuildWindowSourceFromIndex(windows_, monitors_, primary_idx);
        if (primary.type == wcs::capture::CaptureSourceType::Window) {
            if (primary.window != nullptr) {
                sources.push_back(primary);
            }
        } else {
            sources.push_back(primary);
        }

        // Secondary combo has slot 0 for <None>, so real sources start at 1.
        const LRESULT secondary_combo_idx = SendMessageW(window_secondary_combo_, CB_GETCURSEL, 0, 0);
        if (secondary_combo_idx > 0) {
            const auto secondary =
                BuildWindowSourceFromIndex(windows_, monitors_, secondary_combo_idx - 1);
            if (secondary.type == wcs::capture::CaptureSourceType::Window &&
                secondary.window == nullptr) {
                return sources;
            }
            bool duplicate = false;
            for (const auto& existing : sources) {
                if (AreSameSource(existing, secondary)) {
                    duplicate = true;
                    break;
                }
            }
            if (!duplicate) {
                sources.push_back(secondary);
            }
        }
    } else {
        sources.push_back(wcs::capture::CaptureSource{});
    }
    return sources;
}

std::wstring MainWindow::FormatWindowLabel(const WindowEntry& entry) const {
    std::wstringstream ss;
    ss << entry.title << L" [0x" << std::hex << reinterpret_cast<uintptr_t>(entry.hwnd) << L"]";
    return ss.str();
}

std::wstring MainWindow::FormatMonitorLabel(const MonitorEntry& entry, const size_t index) const {
    const int width = entry.rect.right - entry.rect.left;
    const int height = entry.rect.bottom - entry.rect.top;

    std::wstringstream ss;
    ss << L"Monitor " << (index + 1);
    if (entry.is_primary) {
        ss << L" (Primary)";
    }
    ss << L" " << width << L"x" << height << L" [" << entry.rect.left << L"," << entry.rect.top
       << L"]";
    if (!entry.device_name.empty()) {
        ss << L" " << entry.device_name;
    }
    return ss.str();
}

bool MainWindow::IsWindowModeSelected() const {
    return SendMessageW(source_mode_combo_, CB_GETCURSEL, 0, 0) == 1;
}

}  // namespace wcs::gui
