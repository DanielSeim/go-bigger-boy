#pragma once

#include <array>
#include <cstdint>

namespace gameboy {

struct DmgPalette {
    std::array<std::uint32_t, 4> background;
    std::array<std::uint32_t, 4> object_0;
    std::array<std::uint32_t, 4> object_1;
};

inline constexpr DmgPalette grayscale_dmg_palette{
    {0xFFFFFFFF, 0xFFAAAAAA, 0xFF555555, 0xFF000000},
    {0xFFFFFFFF, 0xFFAAAAAA, 0xFF555555, 0xFF000000},
    {0xFFFFFFFF, 0xFFAAAAAA, 0xFF555555, 0xFF000000},
};

[[nodiscard]] DmgPalette cgb_compatibility_palette(
    std::uint8_t palette_id) noexcept;

} // namespace gameboy
