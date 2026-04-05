#pragma once

#include <filesystem>
#include <string>

namespace wcs::common::log {

enum class Level {
    Debug,
    Info,
    Warning,
    Error,
    Fatal,
};

bool InitializeDefault();
bool Initialize(const std::filesystem::path& log_file_path);
void Shutdown();

void Write(Level level, const std::string& message);
void Debug(const std::string& message);
void Info(const std::string& message);
void Warning(const std::string& message);
void Error(const std::string& message);
void Fatal(const std::string& message);

std::filesystem::path CurrentLogPath();

}  // namespace wcs::common::log
