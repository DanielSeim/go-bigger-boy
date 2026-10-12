#pragma once

#include "gbb/log.hpp"

#include <string_view>

namespace gbb {

// Frontends retain ownership of user-facing presentation (dialogs, status
// bars, and stderr), while this adapter gives every diagnostic the same
// structured category and sink behavior.
inline void log_frontend(const LogLevel level, const std::string_view message,
                         const LogContext context = {}) noexcept {
    Logger::instance().write(level, LogCategory::frontend, message, context);
}

inline void log_frontend_error(const std::string_view message,
                               const LogContext context = {}) noexcept {
    log_frontend(LogLevel::error, message, context);
}

inline void log_frontend_warning(const std::string_view message,
                                 const LogContext context = {}) noexcept {
    log_frontend(LogLevel::warning, message, context);
}

inline void log_frontend_info(const std::string_view message,
                              const LogContext context = {}) noexcept {
    log_frontend(LogLevel::info, message, context);
}

inline bool initialize_frontend_file_logging(const std::filesystem::path& preferences) noexcept {
    try {
        if (!preferences.empty() &&
            Logger::instance().set_default_file(preferences / "logs" / "gbb.log")) return true;
    } catch (...) {
        // File diagnostics are best-effort; failure must not hide the UI error.
    }
    log_frontend_warning("Could not open the default frontend log file");
    return false;
}

inline void log_rom_launch_error(std::string_view rom, std::string_view model,
                                 std::string_view detail) noexcept {
    try {
        log_frontend_error(std::string("ROM launch failed: rom=") + std::string(rom) +
                           " model=" + std::string(model) + " error=" + std::string(detail));
    } catch (...) {
        log_frontend_error(detail);
    }
    Logger::instance().flush();
}

} // namespace gbb
