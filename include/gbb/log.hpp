#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace gbb {

enum class LogLevel : std::uint8_t {
    error,
    warning,
    info,
    debug,
    trace,
};

enum class LogCategory : std::uint8_t {
    core,
    ppu,
    apu,
    link,
    frontend,
    persistence,
    test,
};

struct LogContext {
    std::uint64_t session{};
    std::uint64_t frame{};
    std::uint64_t cycles{};
    std::uint64_t rom{};
};

// Installs diagnostic context for the current thread until destruction. Log
// records that omit individual fields inherit them automatically, which keeps
// session/frame/cycle metadata consistent across nested frontend helpers.
class LogContextScope final {
public:
    explicit LogContextScope(LogContext context) noexcept;
    ~LogContextScope() noexcept;

    LogContextScope(const LogContextScope&) = delete;
    LogContextScope& operator=(const LogContextScope&) = delete;

private:
    LogContext previous_{};
};

[[nodiscard]] LogContext current_log_context() noexcept;

class Logger final {
public:
    static Logger& instance() noexcept;

    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    void set_level(LogLevel level) noexcept;
    [[nodiscard]] LogLevel level() const noexcept;
    [[nodiscard]] bool enabled(LogLevel level) const noexcept;

    // Keep a bounded copy of recent records for failure reports and tests.
    // Capacity zero (the default) disables the in-memory sink.
    void set_memory_capacity(std::size_t capacity) noexcept;
    [[nodiscard]] std::vector<std::string> recent_records() const;

    // Replaces the current file sink. Passing an empty path disables file
    // output. The operation is safe while another thread is logging.
    [[nodiscard]] bool set_file(const std::filesystem::path& path) noexcept;
    void close_file() noexcept;
    void flush() noexcept;

    void write(LogLevel level, LogCategory category,
               std::string_view message,
               LogContext context = {}) noexcept;

private:
    Logger() noexcept;
    ~Logger();

    struct State;
    State* state_{};
};

[[nodiscard]] std::string_view log_level_name(LogLevel level) noexcept;
[[nodiscard]] std::string_view log_category_name(LogCategory category) noexcept;

} // namespace gbb
