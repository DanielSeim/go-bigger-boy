#include "gbb/video.hpp"

#include <algorithm>

namespace gbb {

void transform_video_frame(const std::uint32_t* source,
                           const std::size_t pixel_count,
                           const std::size_t width, const std::size_t height,
                           const gameboy::DisplayPalette& palette,
                           const bool native_colors,
                           const gameboy::VideoMode mode,
                           std::vector<std::uint32_t>& destination) {
    if (source == nullptr || width == 0 || height == 0 ||
        width > static_cast<std::size_t>(-1) / height ||
        pixel_count < width * height) {
        destination.clear();
        return;
    }

    const auto count = width * height;
    destination.resize(count);
    const auto color_at = [&](const std::size_t index) {
        return native_colors ? source[index]
                             : gameboy::apply_display_palette(source[index], palette);
    };
    for (std::size_t index = 0; index < count; ++index) {
        const auto x = index % width;
        const auto y = index / width;
        auto pixel = color_at(index);
        if (mode == gameboy::VideoMode::sharp_smoothing) {
            const auto left = x == 0 ? index : index - 1;
            const auto right = x + 1 == width ? index : index + 1;
            const auto up = y == 0 ? index : index - width;
            const auto down = y + 1 == height ? index : index + width;
            pixel = gameboy::apply_sharp_smoothing(
                pixel, color_at(left), color_at(right), color_at(up),
                color_at(down));
        } else if (mode == gameboy::VideoMode::lcd_shader) {
            pixel = gameboy::apply_lcd_shader(pixel, x, y);
        }
        destination[index] = pixel;
    }
}

} // namespace gbb
