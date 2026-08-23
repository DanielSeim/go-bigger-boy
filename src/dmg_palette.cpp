#include "gameboy/dmg_palette.hpp"

#include <algorithm>

namespace gameboy {
namespace {

// Official CGB compatibility colors and layer combinations, expressed as
// RGB555 words and palette-relative color offsets.
constexpr std::array<std::uint16_t, 30 * 4> colors{
    0x7FFF, 0x32BF, 0x00D0, 0x0000, 0x639F, 0x4279, 0x15B0, 0x04CB,
    0x7FFF, 0x6E31, 0x454A, 0x0000, 0x7FFF, 0x1BEF, 0x0200, 0x0000,
    0x7FFF, 0x421F, 0x1CF2, 0x0000, 0x7FFF, 0x5294, 0x294A, 0x0000,
    0x7FFF, 0x03FF, 0x012F, 0x0000, 0x7FFF, 0x03EF, 0x01D6, 0x0000,
    0x7FFF, 0x42B5, 0x3DC8, 0x0000, 0x7E74, 0x03FF, 0x0180, 0x0000,
    0x67FF, 0x77AC, 0x1A13, 0x2D6B, 0x7ED6, 0x4BFF, 0x2175, 0x0000,
    0x53FF, 0x4A5F, 0x7E52, 0x0000, 0x4FFF, 0x7ED2, 0x3A4C, 0x1CE0,
    0x03ED, 0x7FFF, 0x255F, 0x0000, 0x036A, 0x021F, 0x03FF, 0x7FFF,
    0x7FFF, 0x01DF, 0x0112, 0x0000, 0x231F, 0x035F, 0x00F2, 0x0009,
    0x7FFF, 0x03EA, 0x011F, 0x0000, 0x299F, 0x001A, 0x000C, 0x0000,
    0x7FFF, 0x027F, 0x001F, 0x0000, 0x7FFF, 0x03E0, 0x0206, 0x0120,
    0x7FFF, 0x7EEB, 0x001F, 0x7C00, 0x7FFF, 0x3FFF, 0x7E00, 0x001F,
    0x7FFF, 0x03FF, 0x001F, 0x0000, 0x03FF, 0x001F, 0x000C, 0x0000,
    0x7FFF, 0x033F, 0x0193, 0x0000, 0x0000, 0x4200, 0x037F, 0x7FFF,
    0x7FFF, 0x7E8C, 0x7C00, 0x0000, 0x7FFF, 0x1BEF, 0x6180, 0x0000,
};

struct Combination {
    std::uint8_t object_0;
    std::uint8_t object_1;
    std::uint8_t background;
};

constexpr Combination combination(const unsigned object_0,
                                  const unsigned object_1,
                                  const unsigned background) {
    return {static_cast<std::uint8_t>(object_0 * 4),
            static_cast<std::uint8_t>(object_1 * 4),
            static_cast<std::uint8_t>(background * 4)};
}

constexpr std::array<Combination, 51> combinations{
    combination(4, 4, 29),   combination(18, 18, 18),
    combination(20, 20, 20), combination(24, 24, 24),
    combination(9, 9, 9),    combination(0, 0, 0),
    combination(27, 27, 27), combination(5, 5, 5),
    combination(12, 12, 12), combination(26, 26, 26),
    combination(16, 8, 8),   combination(4, 28, 28),
    combination(4, 2, 2),    combination(3, 4, 4),
    combination(4, 29, 29),  combination(28, 4, 28),
    combination(2, 17, 2),   combination(16, 16, 8),
    combination(4, 4, 7),    combination(4, 4, 18),
    combination(4, 4, 20),   combination(19, 19, 9),
    Combination{15, 15, 44}, combination(17, 17, 2),
    combination(4, 4, 2),    combination(4, 4, 3),
    combination(28, 28, 0),  combination(3, 3, 0),
    combination(0, 0, 1),    combination(18, 22, 18),
    combination(20, 22, 20), combination(24, 22, 24),
    combination(16, 22, 8),  combination(17, 4, 13),
    Combination{111, 0, 56}, Combination{111, 16, 60},
    Combination{76, 91, 36}, combination(16, 28, 10),
    combination(4, 23, 28),  combination(17, 22, 2),
    combination(4, 0, 2),    combination(4, 28, 3),
    combination(28, 3, 0),   combination(3, 28, 4),
    combination(21, 28, 4),  combination(3, 28, 0),
    combination(25, 3, 28),  combination(0, 28, 8),
    combination(4, 3, 28),   combination(28, 3, 6),
    combination(4, 28, 29),
};

std::uint32_t expand_rgb555(const std::uint16_t color) noexcept {
    const auto expand = [](const unsigned component) {
        return (component << 3) | (component >> 2);
    };
    return UINT32_C(0xFF000000) | (expand(color & 0x1F) << 16) |
           (expand((color >> 5) & 0x1F) << 8) |
           expand((color >> 10) & 0x1F);
}

std::array<std::uint32_t, 4> palette_at(const unsigned offset) noexcept {
    std::array<std::uint32_t, 4> result{};
    for (unsigned index = 0; index < result.size(); ++index) {
        result[index] = expand_rgb555(colors[offset + index]);
    }
    return result;
}

} // namespace

DmgPalette cgb_compatibility_palette(const std::uint8_t palette_id) noexcept {
    const auto& selected = combinations[std::min<std::size_t>(
        palette_id, combinations.size() - 1)];
    return {palette_at(selected.background), palette_at(selected.object_0),
            palette_at(selected.object_1)};
}

} // namespace gameboy
