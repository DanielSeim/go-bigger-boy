#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

#include "gbb/trace_format.hpp"

namespace gbb::sdl {

// Owns only the lifecycle of an SDL link trace. Field formatting remains in
// the frontend for now, but file selection, session IDs, and timing are
// reusable and independently testable.
class LinkTraceFile final {
public:
    LinkTraceFile() = default;
    LinkTraceFile(const LinkTraceFile&) = delete;
    LinkTraceFile& operator=(const LinkTraceFile&) = delete;
    ~LinkTraceFile() { stop(); }

    void start(const std::filesystem::path& preference_path,
               const char* role_suffix = nullptr);
    void stop() noexcept;

    [[nodiscard]] bool is_open() const noexcept { return stream_.is_open(); }
    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }
    [[nodiscard]] std::uint64_t frame() const noexcept { return frame_; }
    [[nodiscard]] std::uint64_t session() const noexcept { return session_; }
    [[nodiscard]] std::uint64_t elapsed_ms() const noexcept;
    [[nodiscard]] std::string_view transport() const noexcept { return transport_; }
    [[nodiscard]] std::string_view role() const noexcept { return role_; }

    void advance_frame() noexcept {
        ++frame_;
        if ((frame_ % gbb::trace_flush_interval_frames) == 0) {
            stream_.flush();
        }
    }
    [[nodiscard]] std::ostream& stream() noexcept { return stream_; }

private:
    std::ofstream stream_;
    std::filesystem::path path_;
    std::uint64_t frame_{};
    std::uint64_t session_{};
    std::string transport_;
    std::string role_;
    std::chrono::steady_clock::time_point started_at_{};
};

} // namespace gbb::sdl
