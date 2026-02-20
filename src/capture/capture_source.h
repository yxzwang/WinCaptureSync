#pragma once

#include <Windows.h>

#include <cstdint>
#include <string>
#include <vector>

namespace wcs::capture {

enum class CaptureSourceType {
    PrimaryMonitor,
    Monitor,
    Window,
};

struct CaptureSource {
    CaptureSourceType type = CaptureSourceType::PrimaryMonitor;
    HMONITOR monitor = nullptr;
    RECT monitor_rect{};
    std::wstring monitor_name{};
    HWND window = nullptr;
    std::wstring window_title{};
};

bool ResolveSourceSize(const CaptureSource& source, uint32_t* width, uint32_t* height);
bool CaptureSourceToDc(const CaptureSource& source, HDC target_dc, int target_width, int target_height);
bool ResolveCompositeSize(const std::vector<CaptureSource>& sources,
                          uint32_t* width,
                          uint32_t* height);
bool CaptureSourcesToDc(const std::vector<CaptureSource>& sources,
                        HDC target_dc,
                        int target_width,
                        int target_height);
const wchar_t* CaptureSourceTypeName(CaptureSourceType type);

}  // namespace wcs::capture
