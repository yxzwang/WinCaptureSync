#include "capture/capture_source.h"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace wcs::capture {

namespace {

bool IsValidRect(const RECT& rect) {
    return rect.right > rect.left && rect.bottom > rect.top;
}

bool IsMonitorType(const CaptureSourceType type) {
    return type == CaptureSourceType::PrimaryMonitor || type == CaptureSourceType::Monitor;
}

bool ResolveMonitorRect(const CaptureSource& source, RECT* rect) {
    if (rect == nullptr) {
        return false;
    }

    if (source.type == CaptureSourceType::PrimaryMonitor) {
        HMONITOR monitor = source.monitor;
        if (monitor == nullptr) {
            POINT origin{};
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

    if (source.type == CaptureSourceType::Monitor) {
        if (IsValidRect(source.monitor_rect)) {
            *rect = source.monitor_rect;
            return true;
        }
        if (source.monitor == nullptr) {
            return false;
        }

        MONITORINFO info{};
        info.cbSize = sizeof(MONITORINFO);
        if (!GetMonitorInfoW(source.monitor, &info) || !IsValidRect(info.rcMonitor)) {
            return false;
        }
        *rect = info.rcMonitor;
        return true;
    }

    return false;
}

#ifndef PW_RENDERFULLCONTENT
#define PW_RENDERFULLCONTENT 0x00000002
#endif

bool IsMostlyBlack(const uint32_t* pixels, const int width, const int height) {
    if (pixels == nullptr || width <= 0 || height <= 0) {
        return true;
    }

    const int step_x = (std::max)(1, width / 16);
    const int step_y = (std::max)(1, height / 16);
    int samples = 0;
    int dark_samples = 0;

    for (int y = 0; y < height; y += step_y) {
        for (int x = 0; x < width; x += step_x) {
            const uint32_t px = pixels[static_cast<size_t>(y) * static_cast<size_t>(width) +
                                       static_cast<size_t>(x)];
            const uint8_t b = static_cast<uint8_t>(px & 0xFF);
            const uint8_t g = static_cast<uint8_t>((px >> 8) & 0xFF);
            const uint8_t r = static_cast<uint8_t>((px >> 16) & 0xFF);
            const int luma = (static_cast<int>(r) * 30 + static_cast<int>(g) * 59 +
                              static_cast<int>(b) * 11) /
                             100;
            ++samples;
            if (luma < 8) {
                ++dark_samples;
            }
        }
    }

    if (samples == 0) {
        return true;
    }
    return dark_samples * 100 >= samples * 95;
}

bool TryBitBltWindow(HDC window_dc, HDC temp_dc, int width, int height) {
    return BitBlt(temp_dc, 0, 0, width, height, window_dc, 0, 0, SRCCOPY | CAPTUREBLT) == TRUE;
}

bool TryPrintWindow(HWND hwnd, HDC temp_dc, const UINT flags) {
    return PrintWindow(hwnd, temp_dc, flags) == TRUE;
}

bool CaptureSourceToDcRect(const CaptureSource& source,
                           HDC target_dc,
                           const int dst_x,
                           const int dst_y,
                           const int dst_width,
                           const int dst_height) {
    if (target_dc == nullptr || dst_width <= 0 || dst_height <= 0) {
        return false;
    }

    if (IsMonitorType(source.type)) {
        RECT src_rect{};
        if (!ResolveMonitorRect(source, &src_rect)) {
            return false;
        }

        HDC source_dc = nullptr;
        bool owns_source_dc = false;
        int src_x = src_rect.left;
        int src_y = src_rect.top;
        if (!source.monitor_name.empty()) {
            source_dc = CreateDCW(L"DISPLAY", source.monitor_name.c_str(), nullptr, nullptr);
            if (source_dc != nullptr) {
                // Per-monitor display DC uses monitor-local coordinates.
                src_x = 0;
                src_y = 0;
                owns_source_dc = true;
            }
        }
        if (source_dc == nullptr) {
            source_dc = GetDC(nullptr);
            owns_source_dc = false;
        }
        if (source_dc == nullptr) {
            return false;
        }

        const int src_width = src_rect.right - src_rect.left;
        const int src_height = src_rect.bottom - src_rect.top;
        BOOL ok = FALSE;
        if (dst_width == src_width && dst_height == src_height) {
            ok = BitBlt(target_dc, dst_x, dst_y, dst_width, dst_height, source_dc, src_x, src_y,
                        SRCCOPY);
        } else {
            // Prefer COLORONCOLOR to reduce monitor preview shimmer in scaled mode.
            SetStretchBltMode(target_dc, COLORONCOLOR);
            // For monitor capture, avoid CAPTUREBLT to reduce recursive/layered capture flicker.
            ok = StretchBlt(target_dc, dst_x, dst_y, dst_width, dst_height, source_dc, src_x,
                            src_y, src_width, src_height, SRCCOPY);
        }

        if (owns_source_dc) {
            DeleteDC(source_dc);
        } else {
            ReleaseDC(nullptr, source_dc);
        }
        return ok == TRUE;
    }

    if (source.type == CaptureSourceType::Window) {
        if (source.window == nullptr || !IsWindow(source.window)) {
            return false;
        }

        RECT rect{};
        if (!GetWindowRect(source.window, &rect) || !IsValidRect(rect)) {
            return false;
        }

        HDC window_dc = GetWindowDC(source.window);
        if (window_dc == nullptr) {
            return false;
        }

        const int src_width = rect.right - rect.left;
        const int src_height = rect.bottom - rect.top;
        HDC temp_dc = CreateCompatibleDC(window_dc);
        if (temp_dc == nullptr) {
            ReleaseDC(source.window, window_dc);
            return false;
        }

        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = src_width;
        bmi.bmiHeader.biHeight = -src_height;
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        void* bits = nullptr;
        HBITMAP temp_bmp = CreateDIBSection(window_dc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (temp_bmp == nullptr || bits == nullptr) {
            DeleteDC(temp_dc);
            ReleaseDC(source.window, window_dc);
            return false;
        }

        HGDIOBJ old_bmp = SelectObject(temp_dc, temp_bmp);
        bool has_good_frame = false;

        if (TryBitBltWindow(window_dc, temp_dc, src_width, src_height) &&
            !IsMostlyBlack(static_cast<const uint32_t*>(bits), src_width, src_height)) {
            has_good_frame = true;
        }

        if (!has_good_frame &&
            TryPrintWindow(source.window, temp_dc, PW_RENDERFULLCONTENT) &&
            !IsMostlyBlack(static_cast<const uint32_t*>(bits), src_width, src_height)) {
            has_good_frame = true;
        }

        if (!has_good_frame && TryPrintWindow(source.window, temp_dc, 0) &&
            !IsMostlyBlack(static_cast<const uint32_t*>(bits), src_width, src_height)) {
            has_good_frame = true;
        }

        BOOL blit_ok = FALSE;
        if (has_good_frame) {
            SetStretchBltMode(target_dc, HALFTONE);
            blit_ok = StretchBlt(target_dc, dst_x, dst_y, dst_width, dst_height, temp_dc, 0, 0,
                                 src_width, src_height, SRCCOPY);
        }

        if (old_bmp != nullptr) {
            SelectObject(temp_dc, old_bmp);
        }
        DeleteObject(temp_bmp);
        DeleteDC(temp_dc);
        ReleaseDC(source.window, window_dc);
        return blit_ok == TRUE;
    }

    return false;
}

}  // namespace

bool ResolveSourceSize(const CaptureSource& source, uint32_t* width, uint32_t* height) {
    if (width == nullptr || height == nullptr) {
        return false;
    }

    if (IsMonitorType(source.type)) {
        RECT rect{};
        if (!ResolveMonitorRect(source, &rect)) {
            return false;
        }
        *width = static_cast<uint32_t>(rect.right - rect.left);
        *height = static_cast<uint32_t>(rect.bottom - rect.top);
        return (*width > 0 && *height > 0);
    }

    if (source.type == CaptureSourceType::Window) {
        if (source.window == nullptr || !IsWindow(source.window)) {
            return false;
        }
        RECT rect{};
        if (!GetWindowRect(source.window, &rect) || !IsValidRect(rect)) {
            return false;
        }
        *width = static_cast<uint32_t>(rect.right - rect.left);
        *height = static_cast<uint32_t>(rect.bottom - rect.top);
        return true;
    }

    return false;
}

bool CaptureSourceToDc(const CaptureSource& source,
                       HDC target_dc,
                       const int target_width,
                       const int target_height) {
    return CaptureSourceToDcRect(source, target_dc, 0, 0, target_width, target_height);
}

bool ResolveCompositeSize(const std::vector<CaptureSource>& sources,
                          uint32_t* width,
                          uint32_t* height) {
    if (width == nullptr || height == nullptr || sources.empty()) {
        return false;
    }

    if (sources.size() == 1) {
        return ResolveSourceSize(sources.front(), width, height);
    }

    uint64_t total_width = 0;
    uint32_t max_height = 0;
    for (const auto& source : sources) {
        uint32_t src_w = 0;
        uint32_t src_h = 0;
        if (!ResolveSourceSize(source, &src_w, &src_h)) {
            continue;
        }
        total_width += src_w;
        max_height = (std::max)(max_height, src_h);
    }

    if (total_width == 0 || max_height == 0 || total_width > UINT32_MAX) {
        return false;
    }

    *width = static_cast<uint32_t>(total_width);
    *height = max_height;
    return true;
}

bool CaptureSourcesToDc(const std::vector<CaptureSource>& sources,
                        HDC target_dc,
                        const int target_width,
                        const int target_height) {
    if (target_dc == nullptr || target_width <= 0 || target_height <= 0 || sources.empty()) {
        return false;
    }

    if (sources.size() == 1) {
        return CaptureSourceToDc(sources.front(), target_dc, target_width, target_height);
    }

    struct SourceInfo {
        CaptureSource source;
        uint32_t width = 0;
        uint32_t height = 0;
    };

    std::vector<SourceInfo> valid_sources;
    uint64_t total_source_width = 0;

    for (const auto& source : sources) {
        uint32_t src_w = 0;
        uint32_t src_h = 0;
        if (!ResolveSourceSize(source, &src_w, &src_h)) {
            continue;
        }
        valid_sources.push_back(SourceInfo{source, src_w, src_h});
        total_source_width += src_w;
    }

    if (valid_sources.empty() || total_source_width == 0) {
        return false;
    }

    PatBlt(target_dc, 0, 0, target_width, target_height, BLACKNESS);

    bool captured_any = false;
    uint64_t cumulative_width = 0;

    for (size_t i = 0; i < valid_sources.size(); ++i) {
        const auto& info = valid_sources[i];
        const int left = static_cast<int>((cumulative_width * static_cast<uint64_t>(target_width)) /
                                          total_source_width);
        cumulative_width += info.width;
        const int right = (i + 1 == valid_sources.size())
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

        if (CaptureSourceToDcRect(info.source, target_dc, draw_x, draw_y, draw_width, draw_height)) {
            captured_any = true;
        }
    }

    return captured_any;
}

const wchar_t* CaptureSourceTypeName(const CaptureSourceType type) {
    switch (type) {
        case CaptureSourceType::PrimaryMonitor:
            return L"primary_monitor";
        case CaptureSourceType::Monitor:
            return L"monitor";
        case CaptureSourceType::Window:
            return L"window";
        default:
            return L"unknown";
    }
}

}  // namespace wcs::capture
