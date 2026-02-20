#include "gui/window_selector.h"

#include <algorithm>
#include <utility>
#include <vector>

namespace wcs::gui {

namespace {

struct EnumContext {
    HWND exclude = nullptr;
    std::vector<WindowEntry> windows{};
};

struct MonitorEnumContext {
    std::vector<MonitorEntry> monitors{};
};

bool IsRecordableWindow(HWND hwnd, HWND exclude) {
    if (hwnd == nullptr || hwnd == exclude) {
        return false;
    }
    if (!IsWindowVisible(hwnd)) {
        return false;
    }

    const LONG_PTR ex_style = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if (ex_style & WS_EX_TOOLWINDOW) {
        return false;
    }

    int title_len = GetWindowTextLengthW(hwnd);
    if (title_len <= 0) {
        return false;
    }
    return true;
}

BOOL CALLBACK EnumWindowsProc(HWND hwnd, LPARAM l_param) {
    auto* ctx = reinterpret_cast<EnumContext*>(l_param);
    if (ctx == nullptr) {
        return FALSE;
    }

    if (!IsRecordableWindow(hwnd, ctx->exclude)) {
        return TRUE;
    }

    const int title_len = GetWindowTextLengthW(hwnd);
    std::wstring title(static_cast<size_t>(title_len + 1), L'\0');
    const int copied = GetWindowTextW(hwnd, title.data(), title_len + 1);
    title.resize((std::max)(0, copied));

    WindowEntry entry{};
    entry.hwnd = hwnd;
    entry.title = title;
    ctx->windows.push_back(std::move(entry));
    return TRUE;
}

BOOL CALLBACK EnumMonitorsProc(HMONITOR monitor, HDC, LPRECT, LPARAM l_param) {
    auto* ctx = reinterpret_cast<MonitorEnumContext*>(l_param);
    if (ctx == nullptr || monitor == nullptr) {
        return FALSE;
    }

    MONITORINFOEXW info{};
    info.cbSize = sizeof(MONITORINFOEXW);
    if (!GetMonitorInfoW(monitor, &info)) {
        return TRUE;
    }

    MonitorEntry entry{};
    entry.monitor = monitor;
    entry.rect = info.rcMonitor;
    entry.is_primary = (info.dwFlags & MONITORINFOF_PRIMARY) != 0;
    entry.device_name = info.szDevice;
    ctx->monitors.push_back(std::move(entry));
    return TRUE;
}

}  // namespace

std::vector<WindowEntry> EnumerateRecordableWindows(HWND exclude_window) {
    EnumContext context{};
    context.exclude = exclude_window;
    EnumWindows(&EnumWindowsProc, reinterpret_cast<LPARAM>(&context));

    std::sort(context.windows.begin(), context.windows.end(),
              [](const WindowEntry& a, const WindowEntry& b) { return a.title < b.title; });
    return context.windows;
}

std::vector<MonitorEntry> EnumerateMonitors() {
    MonitorEnumContext context{};
    EnumDisplayMonitors(nullptr, nullptr, &EnumMonitorsProc, reinterpret_cast<LPARAM>(&context));

    std::sort(context.monitors.begin(), context.monitors.end(),
              [](const MonitorEntry& a, const MonitorEntry& b) {
                  if (a.rect.left != b.rect.left) {
                      return a.rect.left < b.rect.left;
                  }
                  return a.rect.top < b.rect.top;
              });
    return context.monitors;
}

}  // namespace wcs::gui
