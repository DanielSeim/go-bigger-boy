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

} // namespace gbb
