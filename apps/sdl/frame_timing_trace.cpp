#include "frame_timing_trace.hpp"

#include <cstdlib>
#include <string>

namespace gbb::sdl {

bool frame_timing_trace_enabled() noexcept {
    const auto* value = std::getenv("GBB_FRAME_TIMING");
    return value != nullptr && *value != '\0' && std::string_view{value} != "0";
}

FrameTimingTrace::FrameTimingTrace(const bool enabled) {
    if (!enabled) return;
    const auto* configured_path = std::getenv("GBB_FRAME_TIMING_FILE");
    path_ = configured_path != nullptr && *configured_path != '\0'
                ? std::filesystem::u8path(configured_path)
                : std::filesystem::temp_directory_path() /
                      "go-bigger-boy-frame-timing.log";
    output_.open(path_, std::ios::out | std::ios::trunc);
    if (output_) {
        output_ << "GBB frame timing trace version=1\n";
        output_.flush();
    }
}

bool FrameTimingTrace::enabled() const noexcept {
    return output_.is_open() && static_cast<bool>(output_);
}

void FrameTimingTrace::write(const std::string_view record) noexcept {
    if (!enabled()) return;
    output_ << record << '\n';
    output_.flush();
}

const std::filesystem::path& FrameTimingTrace::path() const noexcept {
    return path_;
}

} // namespace gbb::sdl
