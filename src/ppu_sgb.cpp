#include "gameboy/ppu.hpp"

#include <algorithm>

namespace gameboy {

void Ppu::apply_sgb_command(
    const std::array<std::uint8_t, 16 * 7>& packet,
    const std::size_t size) noexcept {
    if (!sgb_mode_ || size < 2) return;
    const auto command = static_cast<unsigned>(packet[0] >> 3);
    const auto rgb555 = [&](const std::size_t offset) {
        return static_cast<std::uint16_t>(
            packet[offset] | (static_cast<std::uint16_t>(packet[offset + 1]) << 8));
    };
    const auto set_palette_pair = [&](const unsigned first,
                                      const unsigned second) {
        if (size < 15) return;
        const auto color_zero = rgb555(1);
        for (unsigned palette = 0; palette < 4; ++palette) {
            sgb_palettes_[palette * 4] = color_zero;
        }
        for (unsigned color = 1; color < 4; ++color) {
            sgb_palettes_[first * 4 + color] = rgb555(1 + color * 2);
            sgb_palettes_[second * 4 + color] = rgb555(7 + color * 2);
        }
    };
    switch (command) {
    case 0x00: set_palette_pair(0, 1); break; // PAL01
    case 0x01: set_palette_pair(2, 3); break; // PAL23
    case 0x02: set_palette_pair(0, 3); break; // PAL03
    case 0x03: set_palette_pair(1, 2); break; // PAL12
    case 0x04: { // ATTR_BLK
        if (size < 2) return;
        const auto count = std::min<std::size_t>(packet[1], 18);
        for (std::size_t index = 0; index < count; ++index) {
            const auto offset = 2 + index * 6;
            if (offset + 5 >= size) break;
            const auto control = packet[offset];
            const auto palettes = packet[offset + 1];
            const auto inside = (control & 1U) != 0;
            const auto middle = (control & 2U) != 0;
            const auto outside = (control & 4U) != 0;
            auto inside_palette = static_cast<std::uint8_t>(palettes & 3U);
            auto middle_palette = static_cast<std::uint8_t>((palettes >> 2) & 3U);
            auto outside_palette = static_cast<std::uint8_t>((palettes >> 4) & 3U);
            if (inside && !middle && !outside) middle_palette = inside_palette;
            else if (outside && !middle && !inside) middle_palette = outside_palette;
            const auto left = std::min<unsigned>(packet[offset + 2] & 0x1F, 19);
            const auto top = std::min<unsigned>(packet[offset + 3] & 0x1F, 17);
            const auto right = std::min<unsigned>(packet[offset + 4] & 0x1F, 19);
            const auto bottom = std::min<unsigned>(packet[offset + 5] & 0x1F, 17);
            for (unsigned y = 0; y < 18; ++y) {
                for (unsigned x = 0; x < 20; ++x) {
                    auto& attribute = sgb_attributes_[x + y * 20];
                    if (x < left || x > right || y < top || y > bottom) {
                        if (outside) attribute = outside_palette;
                    } else if (x > left && x < right && y > top && y < bottom) {
                        if (inside) attribute = inside_palette;
                    } else if (middle) attribute = middle_palette;
                }
            }
        }
        break;
    }
    case 0x05: { // ATTR_LIN
        const auto count = std::min<std::size_t>(packet[1], size - 2);
        for (std::size_t index = 0; index < count; ++index) {
            const auto value = packet[2 + index];
            const auto palette = static_cast<std::uint8_t>((value >> 5) & 3U);
            const auto line = static_cast<unsigned>(value & 0x1F);
            if ((value & 0x80) != 0) {
                if (line >= 18) continue;
                for (unsigned x = 0; x < 20; ++x) sgb_attributes_[x + line * 20] = palette;
            } else {
                if (line >= 20) continue;
                for (unsigned y = 0; y < 18; ++y) sgb_attributes_[line + y * 20] = palette;
            }
        }
        break;
    }
    case 0x06: { // ATTR_DIV
        if (size < 3) return;
        const auto value = packet[1];
        const auto high = static_cast<std::uint8_t>(value & 3U);
        const auto low = static_cast<std::uint8_t>((value >> 2) & 3U);
        const auto middle = static_cast<std::uint8_t>((value >> 4) & 3U);
        const auto horizontal = (value & 0x40U) != 0;
        const auto line = static_cast<unsigned>(packet[2] & 0x1F);
        for (unsigned y = 0; y < 18; ++y) {
            for (unsigned x = 0; x < 20; ++x) {
                const auto coordinate = horizontal ? y : x;
                sgb_attributes_[x + y * 20] = coordinate < line
                                                   ? low
                                                   : coordinate == line ? middle : high;
            }
        }
        break;
    }
    case 0x07: { // ATTR_CHR
        if (size < 6) return;
        const auto count = std::min<std::size_t>(
            static_cast<std::size_t>(packet[3] | (packet[4] << 8)),
            (size - 6) * 4);
        auto x = static_cast<unsigned>(packet[1]);
        auto y = static_cast<unsigned>(packet[2]);
        const auto vertical = packet[5] != 0;
        for (std::size_t index = 0; index < count && x < 20 && y < 18; ++index) {
            const auto palette = static_cast<std::uint8_t>(
                (packet[6 + index / 4] >> (((~index) & 3U) * 2U)) & 3U);
            sgb_attributes_[x + y * 20] = palette;
            if (vertical) {
                if (++y == 18) { y = 0; if (++x == 20) break; }
            } else {
                if (++x == 20) { x = 0; if (++y == 18) break; }
            }
        }
        break;
    }
    case 0x13: { // CHR_TRN
        const auto bank = static_cast<std::size_t>(packet[1] & 1U);
        std::copy_n(vram_.begin(), 0x1000,
                    sgb_border_tiles_->begin() + bank * 0x1000);
        break;
    }
    case 0x14: // PCT_TRN
        std::copy_n(vram_.begin(), 0x1000, sgb_border_pct_->begin());
        break;
    case 0x16: // ATTR_SET
        break;
    case 0x17: // MASK_EN
        sgb_mask_mode_ = static_cast<std::uint8_t>(packet[1] & 3U);
        break;
    default: break;
    }
}

} // namespace gameboy
