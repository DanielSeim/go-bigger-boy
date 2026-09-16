#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string_view>

namespace gbb::sdl {

[[nodiscard]] bool frame_timing_trace_enabled() noexcept;

[[nodiscard]] inline std::int64_t microseconds_between(
    const std::chrono::steady_clock::time_point begin,
    const std::chrono::steady_clock::time_point end) noexcept {
    return std::chrono::duration_cast<std::chrono::microseconds>(end - begin)
        .count();
}

class FrameTimingTrace final {
public:
    explicit FrameTimingTrace(bool enabled);

    [[nodiscard]] bool enabled() const noexcept;
    void write(std::string_view record) noexcept;
    [[nodiscard]] const std::filesystem::path& path() const noexcept;

private:
    std::filesystem::path path_;
    std::ofstream output_;
};

} // namespace gbb::sdl
