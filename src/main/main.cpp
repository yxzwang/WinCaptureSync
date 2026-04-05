#include <Windows.h>
#include <objbase.h>

#include <exception>
#include <string>

#include "common/logger.h"
#include "common/runtime_diagnostics.h"
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
    wcs::common::log::InitializeDefault();
    wcs::common::runtime::InstallCrashHandlers();
    wcs::common::log::Info("Application start");

    EnableDpiAwareness();
    const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) {
        wcs::common::log::Error("CoInitializeEx failed");
        wcs::common::log::Shutdown();
        return 1;
    }

    int code = 0;
    try {
        wcs::gui::MainWindow window(instance);
        if (!window.CreateAndShow(show_cmd)) {
            wcs::common::log::Error("Main window create/show failed");
            CoUninitialize();
            wcs::common::log::Shutdown();
            return 1;
        }

        code = window.Run();
    } catch (const std::exception& ex) {
        wcs::common::log::Fatal(std::string("Unhandled exception in main loop: ") + ex.what());
        code = 1;
    } catch (...) {
        wcs::common::log::Fatal("Unhandled unknown exception in main loop");
        code = 1;
    }

    CoUninitialize();
    wcs::common::log::Info("Application exit code=" + std::to_string(code));
    wcs::common::log::Shutdown();
    return code;
}
