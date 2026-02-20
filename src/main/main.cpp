#include <Windows.h>
#include <objbase.h>

#include "gui/main_window.h"

namespace {

void EnableDpiAwareness() {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32 == nullptr) {
        return;
    }
    using SetDpiAwarenessContextFn = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
    const auto set_dpi_awareness_context = reinterpret_cast<SetDpiAwarenessContextFn>(
        GetProcAddress(user32, "SetProcessDpiAwarenessContext"));
    if (set_dpi_awareness_context != nullptr) {
        set_dpi_awareness_context(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    } else {
        SetProcessDPIAware();
    }
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show_cmd) {
    EnableDpiAwareness();
    const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) {
        return 1;
    }

    wcs::gui::MainWindow window(instance);
    if (!window.CreateAndShow(show_cmd)) {
        CoUninitialize();
        return 1;
    }

    const int code = window.Run();
    CoUninitialize();
    return code;
}
