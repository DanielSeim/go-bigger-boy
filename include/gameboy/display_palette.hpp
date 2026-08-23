#pragma once

#include <array>
#include <cstdint>

namespace gameboy {

struct DisplayPalette {
    const char* id;
    const char* name;
    std::array<std::uint32_t, 4> colors;
};

inline constexpr std::array<DisplayPalette, 4> display_palettes{{
    {"grayscale", "Grayscale",
     {0xFFFFFFFF, 0xFFAAAAAA, 0xFF555555, 0xFF000000}},
    {"classic", "Classic green",
     {0xFF9BBC0F, 0xFF8BAC0F, 0xFF306230, 0xFF0F380F}},
    {"pocket", "Game Boy Pocket",
     {0xFFC4CFA1, 0xFF8B956D, 0xFF4D533C, 0xFF1F1F1F}},
    {"amber", "Amber",
     {0xFFFFF6D3, 0xFFE7B35A, 0xFF9D5918, 0xFF3B1F0B}},
}};

[[nodiscard]] constexpr std::uint32_t apply_display_palette(
    const std::uint32_t pixel, const DisplayPalette& palette) noexcept {
    switch (pixel) {
    case 0xFFFFFFFF: return palette.colors[0];
    case 0xFFAAAAAA: return palette.colors[1];
    case 0xFF555555: return palette.colors[2];
    default: return palette.colors[3];
    }
}

} // namespace gameboy
