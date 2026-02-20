#pragma once

#include <Windows.h>

#include <string>
#include <vector>

namespace wcs::gui {

struct WindowEntry {
    HWND hwnd = nullptr;
    std::wstring title{};
};

struct MonitorEntry {
    HMONITOR monitor = nullptr;
    RECT rect{};
    bool is_primary = false;
    std::wstring device_name{};
};

std::vector<WindowEntry> EnumerateRecordableWindows(HWND exclude_window);
std::vector<MonitorEntry> EnumerateMonitors();

}  // namespace wcs::gui
