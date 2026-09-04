#include "gbb/log.hpp"

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <new>
#include <string>
#include <system_error>
#include <vector>

namespace gbb {
namespace {

struct LoggerState {
    std::mutex mutex;
    LogLevel minimum_level{LogLevel::warning};
    std::FILE* file{};
    std::size_t memory_capacity{};
    std::deque<std::string> memory;
};

thread_local LogContext active_context{};

int level_rank(const LogLevel level) noexcept {
    return static_cast<int>(level);
}

LogLevel parse_level(const char* value) noexcept {
    if (value == nullptr) return LogLevel::warning;
    if (std::strcmp(value, "error") == 0) return LogLevel::error;
    if (std::strcmp(value, "warning") == 0 || std::strcmp(value, "warn") == 0) {
        return LogLevel::warning;
    }
    if (std::strcmp(value, "info") == 0) return LogLevel::info;
    if (std::strcmp(value, "debug") == 0) return LogLevel::debug;
    if (std::strcmp(value, "trace") == 0) return LogLevel::trace;
    return LogLevel::warning;
}

} // namespace

LogContextScope::LogContextScope(const LogContext context) noexcept
    : LogContextScope(context, true) {}

LogContextScope LogContextScope::exact(const LogContext context) noexcept {
    return LogContextScope(context, false);
}

LogContextScope::LogContextScope(const LogContext context,
                                 const bool inherit) noexcept
    : previous_(active_context) {
    active_context = inherit
                          ? LogContext{
                                context.session != 0 ? context.session
                                                      : previous_.session,
                                context.frame != 0 ? context.frame
                                                   : previous_.frame,
                                context.cycles != 0 ? context.cycles
                                                    : previous_.cycles,
                                context.rom != 0 ? context.rom : previous_.rom,
                            }
                          : context;
}

LogContextScope::~LogContextScope() noexcept { active_context = previous_; }

LogContext current_log_context() noexcept { return active_context; }

struct Logger::State : LoggerState {};

std::string_view log_level_name(const LogLevel level) noexcept {
    switch (level) {
    case LogLevel::error: return "error";
    case LogLevel::warning: return "warning";
    case LogLevel::info: return "info";
    case LogLevel::debug: return "debug";
    case LogLevel::trace: return "trace";
    }
    return "unknown";
}

std::string_view log_category_name(const LogCategory category) noexcept {
    switch (category) {
    case LogCategory::core: return "core";
    case LogCategory::ppu: return "ppu";
    case LogCategory::apu: return "apu";
    case LogCategory::link: return "link";
    case LogCategory::frontend: return "frontend";
    case LogCategory::persistence: return "persistence";
    case LogCategory::test: return "test";
    }
    return "unknown";
}

Logger::Logger() noexcept : state_(new (std::nothrow) State{}) {
    if (state_ == nullptr) return;
    const auto* configured_level = std::getenv("GBB_LOG_LEVEL");
    state_->minimum_level = parse_level(configured_level);
    const auto* trace_path = std::getenv("GBB_TRACE_WX");
    if (trace_path != nullptr && *trace_path != '\0' &&
        std::strcmp(trace_path, "0") != 0) {
        state_->minimum_level = LogLevel::trace;
        if (std::strcmp(trace_path, "1") != 0 &&
            std::strcmp(trace_path, "stderr") != 0) {
            static_cast<void>(set_file(std::filesystem::u8path(trace_path)));
        }
    } else if (const auto* log_path = std::getenv("GBB_LOG_FILE");
               log_path != nullptr && *log_path != '\0') {
        static_cast<void>(set_file(std::filesystem::u8path(log_path)));
    }
}

Logger::~Logger() {
    if (state_ == nullptr) return;
    close_file();
    delete state_;
    state_ = nullptr;
}

Logger& Logger::instance() noexcept {
    static Logger logger;
    return logger;
}

void Logger::set_level(const LogLevel level) noexcept {
    if (state_ == nullptr) return;
    std::lock_guard<std::mutex> lock(state_->mutex);
    state_->minimum_level = level;
}

LogLevel Logger::level() const noexcept {
    if (state_ == nullptr) return LogLevel::error;
    std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->minimum_level;
}

bool Logger::enabled(const LogLevel level) const noexcept {
    return state_ != nullptr && level_rank(level) <= level_rank(this->level());
}

void Logger::set_memory_capacity(const std::size_t capacity) noexcept {
    if (state_ == nullptr) return;
    std::lock_guard<std::mutex> lock(state_->mutex);
    state_->memory_capacity = capacity;
    while (state_->memory.size() > capacity) state_->memory.pop_front();
    if (capacity == 0) state_->memory.clear();
}

std::vector<std::string> Logger::recent_records() const {
    if (state_ == nullptr) return {};
    std::lock_guard<std::mutex> lock(state_->mutex);
    return {state_->memory.begin(), state_->memory.end()};
}

bool Logger::set_file(const std::filesystem::path& path) noexcept {
    if (state_ == nullptr) return false;
    std::lock_guard<std::mutex> lock(state_->mutex);
    if (state_->file != nullptr) {
        std::fclose(state_->file);
        state_->file = nullptr;
    }
    if (path.empty()) return true;
    if (const auto parent = path.parent_path(); !parent.empty()) {
        std::error_code error;
        std::filesystem::create_directories(parent, error);
        if (error) return false;
    }
    state_->file = std::fopen(path.string().c_str(), "a");
    return state_->file != nullptr;
}

void Logger::close_file() noexcept {
    if (state_ == nullptr) return;
    std::lock_guard<std::mutex> lock(state_->mutex);
    if (state_->file == nullptr) return;
    std::fflush(state_->file);
    std::fclose(state_->file);
    state_->file = nullptr;
}

void Logger::flush() noexcept {
    if (state_ == nullptr) return;
    std::lock_guard<std::mutex> lock(state_->mutex);
    std::fflush(stderr);
    if (state_->file != nullptr) std::fflush(state_->file);
}

void Logger::write(const LogLevel level, const LogCategory category,
                   const std::string_view message,
                   const LogContext context) noexcept {
    if (state_ == nullptr) return;
    std::lock_guard<std::mutex> lock(state_->mutex);
    if (level_rank(level) > level_rank(state_->minimum_level)) return;

    try {
        const auto inherited = current_log_context();
        LogContext effective{
            context.session != 0 ? context.session : inherited.session,
            context.frame != 0 ? context.frame : inherited.frame,
            context.cycles != 0 ? context.cycles : inherited.cycles,
            context.rom != 0 ? context.rom : inherited.rom,
        };
        std::string record;
        record.reserve(64 + message.size());
        record += '[';
        record += log_level_name(level);
        record += "][";
        record += log_category_name(category);
        record += ']';
        if (effective.session != 0) {
            record += " session=";
            record += std::to_string(effective.session);
        }
        if (effective.frame != 0) {
            record += " frame=";
            record += std::to_string(effective.frame);
        }
        if (effective.cycles != 0) {
            record += " cycles=";
            record += std::to_string(effective.cycles);
        }
        if (effective.rom != 0) {
            char hexadecimal[16]{};
            const auto converted = std::to_chars(
                hexadecimal, hexadecimal + sizeof(hexadecimal), effective.rom,
                16);
            if (converted.ec == std::errc{}) {
                record += " rom=0x";
                record.append(hexadecimal, converted.ptr);
            }
        }
        record += ' ';
        record.append(message.data(), message.size());

        std::fprintf(stderr, "%s\n", record.c_str());
        if (state_->file != nullptr && state_->file != stderr) {
            std::fprintf(state_->file, "%s\n", record.c_str());
        }
        if (state_->memory_capacity != 0) {
            state_->memory.push_back(record);
            while (state_->memory.size() > state_->memory_capacity) {
                state_->memory.pop_front();
            }
        }
    } catch (...) {
        // Logging must never take down the emulation loop if an allocation
        // fails while constructing an optional diagnostic record.
    }
}

} // namespace gbb
