#include "common/logger.h"

#include <Windows.h>

#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>

namespace wcs::common::log {

namespace {

std::mutex g_mutex;
std::ofstream g_file;
std::filesystem::path g_path;
bool g_initialized = false;

const char* LevelName(const Level level) {
    switch (level) {
        case Level::Debug:
            return "DEBUG";
        case Level::Info:
            return "INFO";
        case Level::Warning:
            return "WARN";
        case Level::Error:
            return "ERROR";
        case Level::Fatal:
            return "FATAL";
        default:
            return "UNKNOWN";
    }
}

std::wstring ExeDirPath() {
    wchar_t module_path[MAX_PATH] = {};
    const DWORD len = GetModuleFileNameW(nullptr, module_path, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        return L".";
    }
    std::filesystem::path p(module_path);
    return p.parent_path().wstring();
}

std::string NowIsoWithMillis() {
    const auto now = std::chrono::system_clock::now();
    const auto secs = std::chrono::time_point_cast<std::chrono::seconds>(now);
    const auto millis =
        std::chrono::duration_cast<std::chrono::milliseconds>(now - secs).count();

    const std::time_t now_t = std::chrono::system_clock::to_time_t(now);
    std::tm local_tm{};
    localtime_s(&local_tm, &now_t);

    std::ostringstream oss;
    oss << std::put_time(&local_tm, "%Y-%m-%d %H:%M:%S") << "." << std::setw(3) << std::setfill('0')
        << millis;
    return oss.str();
}

std::string BuildLine(const Level level, const std::string& message) {
    std::ostringstream oss;
    oss << "[" << NowIsoWithMillis() << "]"
        << "[" << LevelName(level) << "]"
        << "[tid:" << GetCurrentThreadId() << "] " << message;
    return oss.str();
}

std::filesystem::path BuildDefaultLogPath() {
    std::filesystem::path logs_dir = std::filesystem::path(ExeDirPath()) / "logs";
    std::error_code ec;
    std::filesystem::create_directories(logs_dir, ec);

    const auto now = std::chrono::system_clock::now();
    const std::time_t now_t = std::chrono::system_clock::to_time_t(now);
    std::tm local_tm{};
    localtime_s(&local_tm, &now_t);

    std::ostringstream name;
    name << "wincapturesync_" << std::put_time(&local_tm, "%Y%m%d_%H%M%S") << "_"
         << GetCurrentProcessId() << ".log";
    return logs_dir / name.str();
}

}  // namespace

bool InitializeDefault() {
    return Initialize(BuildDefaultLogPath());
}

bool Initialize(const std::filesystem::path& log_file_path) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_initialized) {
        return true;
    }

    std::error_code ec;
    if (!log_file_path.parent_path().empty()) {
        std::filesystem::create_directories(log_file_path.parent_path(), ec);
    }

    g_file.open(log_file_path, std::ios::binary | std::ios::out | std::ios::app);
    if (!g_file.is_open()) {
        return false;
    }
    g_path = log_file_path;
    g_initialized = true;

    const std::string line = BuildLine(Level::Info, "Logger initialized");
    g_file << line << "\n";
    g_file.flush();
    OutputDebugStringA((line + "\n").c_str());
    return true;
}

void Shutdown() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_initialized) {
        return;
    }
    const std::string line = BuildLine(Level::Info, "Logger shutdown");
    g_file << line << "\n";
    g_file.flush();
    g_file.close();
    g_initialized = false;
}

void Write(const Level level, const std::string& message) {
    std::lock_guard<std::mutex> lock(g_mutex);
    const std::string line = BuildLine(level, message);
    if (g_initialized && g_file.is_open()) {
        g_file << line << "\n";
        g_file.flush();
    }
    OutputDebugStringA((line + "\n").c_str());
}

void Debug(const std::string& message) {
    Write(Level::Debug, message);
}

void Info(const std::string& message) {
    Write(Level::Info, message);
}

void Warning(const std::string& message) {
    Write(Level::Warning, message);
}

void Error(const std::string& message) {
    Write(Level::Error, message);
}

void Fatal(const std::string& message) {
    Write(Level::Fatal, message);
}

std::filesystem::path CurrentLogPath() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_path;
}

}  // namespace wcs::common::log
