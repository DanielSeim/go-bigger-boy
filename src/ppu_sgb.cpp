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
    case 0x0A: { // PAL_SET
        if (size < 10) return;
        const auto palette_index = [&](const std::size_t offset) {
            return static_cast<std::size_t>(
                packet[offset] | ((packet[offset + 1] & 1U) << 8));
        };
        const auto indexes = std::array<std::size_t, 4>{
            palette_index(1), palette_index(3), palette_index(5),
            palette_index(7)};
        for (const auto index : indexes) {
            if (index >= 0x200 || index * 4 + 3 >= sgb_ram_palettes_->size()) {
                return;
            }
        }
        const auto color_zero = (*sgb_ram_palettes_)[indexes[0] * 4];
        for (std::size_t palette = 0; palette < indexes.size(); ++palette) {
            sgb_palettes_[palette * 4] = color_zero;
            for (std::size_t color = 1; color < 4; ++color) {
                sgb_palettes_[palette * 4 + color] =
                    (*sgb_ram_palettes_)[indexes[palette] * 4 + color];
            }
        }
        if ((packet[9] & 0x80U) != 0) {
            load_sgb_attribute_file(packet[9] & 0x3FU);
        }
        if ((packet[9] & 0x40U) != 0) sgb_mask_mode_ = 0;
        break;
    }
    case 0x0B: { // PAL_TRN
        // The 4 KiB VRAM transfer contains 2048 little-endian RGB555 entries.
        for (std::size_t index = 0; index < sgb_ram_palettes_->size(); ++index) {
            const auto offset = index * 2;
            (*sgb_ram_palettes_)[index] = static_cast<std::uint16_t>(
                vram_[offset] | (static_cast<std::uint16_t>(vram_[offset + 1])
                                 << 8));
        }
        break;
    }
    case 0x15: { // ATTR_TRN
        // Each of the 45 attribute files is a packed 20x18 map (90 bytes).
        std::copy_n(vram_.begin(), sgb_attribute_files_->size(),
                    sgb_attribute_files_->begin());
        break;
    }
    case 0x16: { // ATTR_SET
        if (size < 2) return;
        load_sgb_attribute_file(packet[1] & 0x3FU);
        if ((packet[1] & 0x40U) != 0) sgb_mask_mode_ = 0;
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
        sgb_border_transferred_ = true;
        break;
    case 0x17: // MASK_EN
        sgb_mask_mode_ = static_cast<std::uint8_t>(packet[1] & 3U);
        break;
    default: break;
    }
}

const Ppu::SgbFramebuffer& Ppu::sgb_framebuffer() const noexcept {
    constexpr auto black = UINT32_C(0xFF000000);
    sgb_framebuffer_->fill(black);

    // A border is only meaningful after PCT_TRN. Before that transfer, keep a
    // deterministic black surround and expose the native Game Boy viewport in
    // the same 48,40 letterbox used by the SGB hardware.
    constexpr std::size_t viewport_x = (sgb_border_width - screen_width) / 2;
    constexpr std::size_t viewport_y = (sgb_border_height - screen_height) / 2;
    if (!sgb_border_transferred_) {
        for (std::size_t y = 0; y < screen_height; ++y) {
            std::copy_n(framebuffer_->begin() + y * screen_width, screen_width,
                        sgb_framebuffer_->begin() +
                            (y + viewport_y) * sgb_border_width + viewport_x);
        }
        return *sgb_framebuffer_;
    }

    const auto expand = [](const unsigned component) {
        return (component << 3) | (component >> 2);
    };
    const auto border_color = [&](const unsigned palette,
                                  const unsigned color) {
        // PCT border tilemap entries select one of four 16-colour palettes.
        // The Game Boy viewport uses its separate four-colour SGB palettes.
        if (palette > 3) return black;
        const auto offset = 0x800U + palette * 32U +
                            std::min<unsigned>(color, 15U) * 2U;
        if (offset + 1 >= sgb_border_pct_->size()) return black;
        const auto rgb555 = static_cast<std::uint16_t>(
            (*sgb_border_pct_)[offset] |
            (static_cast<std::uint16_t>((*sgb_border_pct_)[offset + 1]) << 8));
        return UINT32_C(0xFF000000) | (expand(rgb555 & 0x1FU) << 16) |
               (expand((rgb555 >> 5) & 0x1FU) << 8) |
               expand((rgb555 >> 10) & 0x1FU);
    };

    for (std::size_t tile_y = 0; tile_y < 28; ++tile_y) {
        for (std::size_t tile_x = 0; tile_x < 32; ++tile_x) {
            const auto map_offset = (tile_y * 32 + tile_x) * 2;
            const auto map_entry = static_cast<std::uint16_t>(
                (*sgb_border_pct_)[map_offset] |
                (static_cast<std::uint16_t>((*sgb_border_pct_)[map_offset + 1])
                 << 8));
            const auto tile = static_cast<std::size_t>(map_entry & 0x03FFU);
            const auto palette = static_cast<unsigned>((map_entry >> 10) & 3U);
            const auto x_flip = (map_entry & 0x4000U) != 0;
            const auto y_flip = (map_entry & 0x8000U) != 0;
            if (tile >= 0x100 || tile * 32 + 31 >= sgb_border_tiles_->size()) {
                continue;
            }
            const auto tile_offset = tile * 32;
            for (std::size_t pixel_y = 0; pixel_y < 8; ++pixel_y) {
                const auto source_y = y_flip ? 7 - pixel_y : pixel_y;
                const auto low_offset = tile_offset + source_y * 2;
                const auto high_offset = tile_offset + 16 + source_y * 2;
                const auto low = (*sgb_border_tiles_)[low_offset];
                const auto low_high = (*sgb_border_tiles_)[low_offset + 1];
                const auto high = (*sgb_border_tiles_)[high_offset];
                const auto high_high = (*sgb_border_tiles_)[high_offset + 1];
                for (std::size_t pixel_x = 0; pixel_x < 8; ++pixel_x) {
                    const auto source_x = x_flip ? 7 - pixel_x : pixel_x;
                    const auto bit = 7U - static_cast<unsigned>(source_x);
                    const auto color = static_cast<unsigned>(
                        ((low >> bit) & 1U) | (((low_high >> bit) & 1U) << 1) |
                        (((high >> bit) & 1U) << 2) |
                        (((high_high >> bit) & 1U) << 3));
                    const auto x = tile_x * 8 + pixel_x;
                    const auto y = tile_y * 8 + pixel_y;
                    if (color == 0 && x >= viewport_x &&
                        x < viewport_x + screen_width && y >= viewport_y &&
                        y < viewport_y + screen_height) {
                        (*sgb_framebuffer_)[y * sgb_border_width + x] =
                            (*framebuffer_)[(y - viewport_y) * screen_width +
                                            (x - viewport_x)];
                    } else {
                        (*sgb_framebuffer_)[y * sgb_border_width + x] =
                            border_color(palette, color);
                    }
                }
            }
        }
    }
    return *sgb_framebuffer_;
}

void Ppu::load_sgb_attribute_file(const std::size_t index) noexcept {
    constexpr std::size_t file_count = 0x2D;
    constexpr std::size_t file_size = 90;
    if (index >= file_count) return;
    const auto* source = sgb_attribute_files_->data() + index * file_size;
    for (std::size_t entry = 0; entry < sgb_attributes_.size(); ++entry) {
        sgb_attributes_[entry] = static_cast<std::uint8_t>(
            (source[entry / 4] >> ((3U - (entry & 3U)) * 2U)) & 3U);
    }
}

} // namespace gameboy
