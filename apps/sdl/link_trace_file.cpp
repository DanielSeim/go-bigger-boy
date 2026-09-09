#include "link_trace_file.hpp"

#include "gbb/frontend_logging.hpp"
#include "gbb/trace_format.hpp"

#include <array>
#include <chrono>
#include <iterator>
#include <string>
#include <vector>

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
                          const char* role_suffix, const char* transport) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    stop();
    std::error_code temp_error;
    const auto temporary_directory =
        std::filesystem::temp_directory_path(temp_error);
    const auto suffix = role_suffix != nullptr && *role_suffix != '\0'
                            ? std::string{"-"} + role_suffix
                            : std::string{};
    const auto temporary_path =
        temp_error ? std::filesystem::path{}
                   : temporary_directory /
                         (std::string{"gbb-link-trace"} + suffix + ".log");
    const auto preference_file =
        preference_path.empty()
            ? std::filesystem::path{}
            : preference_path /
                  (std::string{"link-trace"} + suffix + ".log");
    const auto current_file = std::filesystem::current_path() /
                              (std::string{"link-trace"} + suffix + ".log");
#ifdef __ANDROID__
    // Keep Android traces in the app's durable files directory so the export
    // action can still find them after the system purges its cache directory.
    const std::array<std::filesystem::path, 3> candidates{{
        preference_file, temporary_path, current_file}};
#else
    const std::array<std::filesystem::path, 3> candidates{{
        temporary_path, preference_file, current_file}};
#endif

    frame_ = 0;
    path_.clear();
    transport_ = transport != nullptr && *transport != '\0'
                     ? transport
                     : (role_suffix == nullptr ? "local" : "tcp");
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
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (stream_.is_open()) {
        gbb::write_trace_session_end(stream_, session_, frame_, elapsed_ms());
        stream_.flush();
        stream_.close();
    }
    frame_ = 0;
    // Keep the last completed path available for Android export after a link
    // is stopped. A subsequent start() clears it before selecting a new file.
    transport_.clear();
    role_.clear();
    started_at_ = {};
}

std::vector<std::uint8_t> LinkTraceFile::snapshot() noexcept {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (path_.empty()) return {};
    if (stream_.is_open()) stream_.flush();

    std::error_code error;
    const auto size = std::filesystem::file_size(path_, error);
    // A runaway diagnostic session should never make the Android UI allocate
    // an unbounded JNI byte array. Normal traces are a few megabytes at most.
    constexpr std::uintmax_t maximum_snapshot_bytes = 32U * 1024U * 1024U;
    if (error || size == 0 || size > maximum_snapshot_bytes) return {};

    std::ifstream input(path_, std::ios::binary);
    if (!input.is_open()) return {};
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    input.read(reinterpret_cast<char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
    if (!input && !input.eof()) return {};
    return bytes;
}

} // namespace gbb::sdl
