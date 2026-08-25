#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace gameboy {

// The presentation mode deliberately lives outside the emulator core. It is
// a frontend concern, but sharing the identifiers keeps settings.ini,
// desktop, Android, and web builds interoperable.
enum class VideoMode : std::uint8_t {
    nearest,
    bilinear,
    integer,
    lcd_shader,
    sharp_smoothing,
};

struct VideoModeInfo {
    VideoMode mode;
    std::string_view id;
    std::string_view name;
};

inline constexpr std::array<VideoModeInfo, 5> video_modes{{
    {VideoMode::nearest, "nearest", "Nearest neighbor"},
    {VideoMode::bilinear, "bilinear", "Bilinear"},
    {VideoMode::integer, "integer", "Integer scaling"},
    {VideoMode::lcd_shader, "lcd", "LCD shader"},
    {VideoMode::sharp_smoothing, "sharp", "Sharp smoothing"},
}};

inline constexpr VideoMode default_video_mode = VideoMode::nearest;

// Smooth only high-contrast edges in the source image. Flat regions and small
// details are left untouched, while an edge pixel gets a restrained blend with
// its four neighbors. The result is presented with nearest-neighbor scaling,
// so the transition is softened without making the whole image blurry.
inline constexpr std::uint32_t apply_sharp_smoothing(
    const std::uint32_t pixel, const std::uint32_t left,
    const std::uint32_t right, const std::uint32_t up,
    const std::uint32_t down) noexcept {
    const auto channel = [&](const unsigned shift) {
        const auto center = static_cast<int>((pixel >> shift) & 0xFFU);
        const auto left_value = static_cast<int>((left >> shift) & 0xFFU);
        const auto right_value = static_cast<int>((right >> shift) & 0xFFU);
        const auto up_value = static_cast<int>((up >> shift) & 0xFFU);
        const auto down_value = static_cast<int>((down >> shift) & 0xFFU);
        const auto low = std::min({left_value, right_value, up_value,
                                   down_value});
        const auto high = std::max({left_value, right_value, up_value,
                                    down_value});
        if (high - low < 48) return static_cast<std::uint32_t>(center);
        const auto average = (left_value + right_value + up_value +
                              down_value) /
                             4;
        const auto sharpened = (center * 3 + average) / 4;
        return static_cast<std::uint32_t>(
            sharpened < 0 ? 0 : sharpened > 255 ? 255 : sharpened);
    };
    return (pixel & UINT32_C(0xFF000000)) |
           (channel(16) << 16) | (channel(8) << 8) | channel(0);
}

inline constexpr const VideoModeInfo& video_mode_info(const VideoMode mode) {
    for (const auto& info : video_modes) {
        if (info.mode == mode) return info;
    }
    return video_modes.front();
}

inline constexpr VideoMode video_mode_from_id(const std::string_view id) {
    for (const auto& info : video_modes) {
        if (info.id == id) return info.mode;
    }
    return default_video_mode;
}

// A small, deterministic LCD mask that works on every SDL renderer. It is
// applied to the 160x144 source image before texture upload, so it also works
// on renderers that do not expose programmable fragment shaders (notably
// software and Android fallback renderers).
inline constexpr std::uint32_t apply_lcd_shader(const std::uint32_t pixel,
                                                const std::size_t x,
                                                const std::size_t y) noexcept {
    const auto channel = [](const std::uint32_t value,
                            const unsigned shift,
                            const unsigned multiplier) {
        const auto component = (value >> shift) & 0xFFU;
        return (component * multiplier + 127U) / 255U;
    };
    const auto phase = static_cast<unsigned>(x % 3);
    const auto red_multiplier = phase == 1 ? 255U : 232U;
    const auto green_multiplier = phase == 2 ? 255U : 238U;
    const auto blue_multiplier = phase == 0 ? 255U : 232U;
    const auto scanline = (y & 1U) != 0U ? 224U : 255U;
    const auto red = channel(pixel, 16, red_multiplier) * scanline / 255U;
    const auto green = channel(pixel, 8, green_multiplier) * scanline / 255U;
    const auto blue = channel(pixel, 0, blue_multiplier) * scanline / 255U;
    return (pixel & UINT32_C(0xFF000000)) |
           (red << 16) | (green << 8) | blue;
}

} // namespace gameboy
