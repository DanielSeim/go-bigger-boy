#include "link_trace_file.hpp"

#include "gbb/frontend_logging.hpp"
#include "gbb/trace_format.hpp"

#include <array>
#include <chrono>
#include <string>

namespace gbb::sdl {
namespace {

} // namespace

std::uint64_t LinkTraceFile::elapsed_ms() const noexcept {
    if (started_at_ == std::chrono::steady_clock::time_point{}) return 0;
    return static_cast<std::uint64_t>(std::chrono::duration_cast<
        std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                   started_at_).count());
}

void LinkTraceFile::start(const std::filesystem::path& preference_path,
                          const char* role_suffix) {
    stop();
    std::error_code temp_error;
    const auto temporary_directory =
        std::filesystem::temp_directory_path(temp_error);
    const auto suffix = role_suffix != nullptr && *role_suffix != '\0'
                            ? std::string{"-"} + role_suffix
                            : std::string{};
    const std::array<std::filesystem::path, 3> candidates{{
        temp_error ? std::filesystem::path{}
                   : temporary_directory /
                         (std::string{"gbb-link-trace"} + suffix + ".log"),
        preference_path.empty()
            ? std::filesystem::path{}
            : preference_path /
                  (std::string{"link-trace"} + suffix + ".log"),
        std::filesystem::current_path() /
            (std::string{"link-trace"} + suffix + ".log")}};

    frame_ = 0;
    path_.clear();
    transport_ = role_suffix == nullptr ? "local" : "tcp";
    role_ = role_suffix == nullptr ? "local" : role_suffix;
    started_at_ = std::chrono::steady_clock::now();
    session_ = gbb::next_trace_session_id();
    for (const auto& candidate : candidates) {
        if (candidate.empty()) continue;
        std::error_code error;
        std::filesystem::create_directories(candidate.parent_path(), error);
        stream_.clear();
        stream_.open(candidate, std::ios::trunc);
        if (!stream_.is_open()) continue;
        stream_ << "GBB link trace\n";
        gbb::write_trace_session_start(stream_, session_, transport_, role_);
        stream_.flush();
        if (!stream_.good()) {
            stream_.close();
            continue;
        }
        path_ = candidate;
        gbb::log_frontend_info("Link trace: " + candidate.string());
        break;
    }
}

void LinkTraceFile::stop() noexcept {
    if (stream_.is_open()) {
        gbb::write_trace_session_end(stream_, session_, frame_, elapsed_ms());
        stream_.flush();
        stream_.close();
    }
    frame_ = 0;
    path_.clear();
    transport_.clear();
    role_.clear();
    started_at_ = {};
}

} // namespace gbb::sdl
