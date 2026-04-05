#include "common/runtime_diagnostics.h"

#include <Windows.h>
#include <DbgHelp.h>

#include <atomic>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <sstream>
#include <string>

#include "common/logger.h"

namespace wcs::common::runtime {

namespace {

std::atomic<bool> g_installed{false};

std::filesystem::path BuildCrashDumpPath() {
    std::filesystem::path base = wcs::common::log::CurrentLogPath().parent_path();
    if (base.empty()) {
        wchar_t module_path[MAX_PATH] = {};
        const DWORD len = GetModuleFileNameW(nullptr, module_path, MAX_PATH);
        if (len > 0 && len < MAX_PATH) {
            base = std::filesystem::path(module_path).parent_path() / "logs";
        } else {
            base = "logs";
        }
    }

    std::error_code ec;
    std::filesystem::create_directories(base, ec);

    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t file_name[128] = {};
    swprintf_s(file_name, L"crash_%04u%02u%02u_%02u%02u%02u_%u.dmp", st.wYear, st.wMonth, st.wDay,
               st.wHour, st.wMinute, st.wSecond, GetCurrentProcessId());
    return base / file_name;
}

void WriteMiniDump(EXCEPTION_POINTERS* exception_pointers) {
    const auto dump_path = BuildCrashDumpPath();
    HANDLE file = CreateFileW(dump_path.wstring().c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        wcs::common::log::Error("Failed to create crash dump file");
        return;
    }

    MINIDUMP_EXCEPTION_INFORMATION mei{};
    mei.ThreadId = GetCurrentThreadId();
    mei.ExceptionPointers = exception_pointers;
    mei.ClientPointers = FALSE;

    const BOOL ok = MiniDumpWriteDump(
        GetCurrentProcess(), GetCurrentProcessId(), file,
        static_cast<MINIDUMP_TYPE>(MiniDumpWithIndirectlyReferencedMemory | MiniDumpScanMemory),
        exception_pointers != nullptr ? &mei : nullptr, nullptr, nullptr);
    CloseHandle(file);

    if (ok == TRUE) {
        wcs::common::log::Info("Crash dump written: " + dump_path.string());
    } else {
        std::ostringstream oss;
        oss << "MiniDumpWriteDump failed, error=" << GetLastError();
        wcs::common::log::Error(oss.str());
    }
}

LONG WINAPI UnhandledExceptionHandler(EXCEPTION_POINTERS* exception_pointers) {
    const DWORD code = exception_pointers != nullptr && exception_pointers->ExceptionRecord != nullptr
                           ? exception_pointers->ExceptionRecord->ExceptionCode
                           : 0;
    const void* address = exception_pointers != nullptr && exception_pointers->ExceptionRecord != nullptr
                              ? exception_pointers->ExceptionRecord->ExceptionAddress
                              : nullptr;
    std::ostringstream oss;
    oss << "Unhandled SEH exception code=0x" << std::hex << code << " address=" << address;
    wcs::common::log::Fatal(oss.str());
    WriteMiniDump(exception_pointers);
    return EXCEPTION_EXECUTE_HANDLER;
}

void TerminateHandler() {
    wcs::common::log::Fatal("std::terminate invoked");
    WriteMiniDump(nullptr);
    std::_Exit(3);
}

void SignalHandler(const int sig) {
    std::ostringstream oss;
    oss << "Process signal received: " << sig;
    wcs::common::log::Fatal(oss.str());
    WriteMiniDump(nullptr);
    std::_Exit(128 + sig);
}

}  // namespace

void InstallCrashHandlers() {
    bool expected = false;
    if (!g_installed.compare_exchange_strong(expected, true)) {
        return;
    }

    SetUnhandledExceptionFilter(&UnhandledExceptionHandler);
    std::set_terminate(&TerminateHandler);
    std::signal(SIGABRT, &SignalHandler);
    std::signal(SIGSEGV, &SignalHandler);
    std::signal(SIGILL, &SignalHandler);
    std::signal(SIGFPE, &SignalHandler);

    wcs::common::log::Info("Crash handlers installed");
}

}  // namespace wcs::common::runtime
