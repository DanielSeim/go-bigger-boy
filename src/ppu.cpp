#include "gameboy/ppu.hpp"

#include <algorithm>

namespace gameboy {
namespace {
constexpr std::array<std::uint32_t, 4> dmg_colors{
    0xFFFFFFFF, 0xFFAAAAAA, 0xFF555555, 0xFF000000,
};
} // namespace

Ppu::Ppu() noexcept { framebuffer_.fill(dmg_colors[0]); }

std::uint8_t Ppu::read_vram(const std::uint16_t address) const noexcept {
    if (lcd_enabled() && mode_ == 3) {
        return 0xFF;
    }
    return vram_[address - 0x8000];
}

void Ppu::write_vram(const std::uint16_t address,
                     const std::uint8_t value) noexcept {
    if (!lcd_enabled() || mode_ != 3) {
        vram_[address - 0x8000] = value;
    }
}

std::uint8_t Ppu::read_oam(const std::uint16_t address) const noexcept {
    if (lcd_enabled() && (mode_ == 2 || mode_ == 3)) {
        return 0xFF;
    }
    return oam_[address - 0xFE00];
}

void Ppu::write_oam(const std::uint16_t address,
                    const std::uint8_t value) noexcept {
    if (!lcd_enabled() || (mode_ != 2 && mode_ != 3)) {
        oam_[address - 0xFE00] = value;
    }
}

void Ppu::dma_write_oam(const unsigned offset, const std::uint8_t value) noexcept {
    if (offset < oam_.size()) {
        oam_[offset] = value;
    }
}

bool Ppu::handles_register(const std::uint16_t address) noexcept {
    return (address >= 0xFF40 && address <= 0xFF45) ||
           (address >= 0xFF47 && address <= 0xFF4B);
}

std::uint8_t Ppu::read_register(const std::uint16_t address) const noexcept {
    switch (address) {
    case 0xFF40: return lcdc_;
    case 0xFF41:
        return static_cast<std::uint8_t>(
            0x80 | stat_select_ | (ly_ == lyc_ ? 0x04 : 0) |
            (lcd_enabled() ? mode_ : 0));
    case 0xFF42: return scy_;
    case 0xFF43: return scx_;
    case 0xFF44: return ly_;
    case 0xFF45: return lyc_;
    case 0xFF47: return bg_palette_;
    case 0xFF48: return object_palette_0_;
    case 0xFF49: return object_palette_1_;
    case 0xFF4A: return window_y_;
    case 0xFF4B: return window_x_;
    default: return 0xFF;
    }
}

bool Ppu::write_register(const std::uint16_t address,
                         const std::uint8_t value) noexcept {
    switch (address) {
    case 0xFF40: {
        const auto was_enabled = lcd_enabled();
        lcdc_ = value;
        if (was_enabled && !lcd_enabled()) {
            dot_ = 0;
            ly_ = 0;
            mode_ = 0;
            stat_line_ = false;
            frame_ready_ = false;
            framebuffer_.fill(dmg_colors[0]);
            return false;
        }
        if (!was_enabled && lcd_enabled()) {
            dot_ = 0;
            ly_ = 0;
            mode_ = 2;
        }
        break;
    }
    case 0xFF41: stat_select_ = static_cast<std::uint8_t>(value & 0x78); break;
    case 0xFF42: scy_ = value; break;
    case 0xFF43: scx_ = value; break;
    case 0xFF44: break; // LY is read-only.
    case 0xFF45: lyc_ = value; break;
    case 0xFF47: bg_palette_ = value; break;
    case 0xFF48: object_palette_0_ = value; break;
    case 0xFF49: object_palette_1_ = value; break;
    case 0xFF4A: window_y_ = value; break;
    case 0xFF4B: window_x_ = value; break;
    default: break;
    }
    return update_stat_line();
}

std::uint8_t Ppu::tick(const unsigned cycles) noexcept {
    if (!lcd_enabled()) {
        return 0;
    }

    std::uint8_t requests = 0;
    for (unsigned cycle = 0; cycle < cycles; ++cycle) {
        ++dot_;
        if (ly_ < screen_height) {
            if (dot_ == 80) {
                mode_ = 3;
                if (update_stat_line()) requests |= 0x02;
            } else if (dot_ == 252) {
                render_scanline();
                mode_ = 0;
                if (update_stat_line()) requests |= 0x02;
            }
        }

        if (dot_ == 456) {
            dot_ = 0;
            ++ly_;
            if (ly_ == screen_height) {
                mode_ = 1;
                frame_ready_ = true;
                requests |= 0x01;
            } else if (ly_ > 153) {
                ly_ = 0;
                mode_ = 2;
            } else if (ly_ < screen_height) {
                mode_ = 2;
            }
            if (update_stat_line()) requests |= 0x02;
        }
    }
    return requests;
}

const Ppu::Framebuffer& Ppu::framebuffer() const noexcept {
    return framebuffer_;
}

bool Ppu::frame_ready() const noexcept { return frame_ready_; }

void Ppu::consume_frame() noexcept { frame_ready_ = false; }

bool Ppu::lcd_enabled() const noexcept { return (lcdc_ & 0x80) != 0; }

bool Ppu::stat_condition() const noexcept {
    if (!lcd_enabled()) {
        return false;
    }
    return ((stat_select_ & 0x40) != 0 && ly_ == lyc_) ||
           ((stat_select_ & 0x20) != 0 && mode_ == 2) ||
           ((stat_select_ & 0x10) != 0 && mode_ == 1) ||
           ((stat_select_ & 0x08) != 0 && mode_ == 0);
}

bool Ppu::update_stat_line() noexcept {
    const auto new_line = stat_condition();
    const auto rising_edge = new_line && !stat_line_;
    stat_line_ = new_line;
    return rising_edge;
}

void Ppu::render_scanline() noexcept {
    std::array<std::uint8_t, screen_width> background_colors{};
    const auto background_enabled = (lcdc_ & 0x01) != 0;
    const auto window_enabled = background_enabled && (lcdc_ & 0x20) != 0 &&
                                ly_ >= window_y_;
    const auto window_start = static_cast<int>(window_x_) - 7;

    for (unsigned x = 0; x < screen_width; ++x) {
        std::uint8_t color = 0;
        if (background_enabled) {
            const auto use_window = window_enabled &&
                                    static_cast<int>(x) >= window_start;
            const auto pixel_x = use_window
                                     ? static_cast<unsigned>(
                                           static_cast<int>(x) - window_start)
                                     : static_cast<unsigned>(
                                           static_cast<std::uint8_t>(x + scx_));
            const auto pixel_y = use_window
                                     ? static_cast<unsigned>(ly_ - window_y_)
                                     : static_cast<unsigned>(
                                           static_cast<std::uint8_t>(ly_ + scy_));
            const auto map_base = use_window
                                      ? ((lcdc_ & 0x40) != 0 ? 0x1C00U : 0x1800U)
                                      : ((lcdc_ & 0x08) != 0 ? 0x1C00U : 0x1800U);
            const auto map_offset = map_base + (pixel_y / 8) * 32 + pixel_x / 8;
            const auto tile_number = vram_[map_offset & 0x1FFF];
            unsigned tile_offset = 0;
            if ((lcdc_ & 0x10) != 0) {
                tile_offset = static_cast<unsigned>(tile_number) * 16;
            } else {
                const auto signed_tile = tile_number < 0x80
                                             ? static_cast<int>(tile_number)
                                             : static_cast<int>(tile_number) - 0x100;
                tile_offset = static_cast<unsigned>(0x1000 + signed_tile * 16);
            }
            const auto row = (pixel_y & 7U) * 2;
            const auto low = vram_[tile_offset + row];
            const auto high = vram_[tile_offset + row + 1];
            const auto bit = 7U - (pixel_x & 7U);
            color = static_cast<std::uint8_t>(
                ((low >> bit) & 1U) | (((high >> bit) & 1U) << 1));
        }
        background_colors[x] = color;
        framebuffer_[static_cast<std::size_t>(ly_) * screen_width + x] =
            palette_color(bg_palette_, color);
    }

    if ((lcdc_ & 0x02) == 0) {
        return;
    }

    const auto sprite_height = (lcdc_ & 0x04) != 0 ? 16 : 8;
    std::array<unsigned, 10> sprites{};
    unsigned sprite_count = 0;
    for (unsigned index = 0; index < 40 && sprite_count < sprites.size(); ++index) {
        const auto sprite_y = static_cast<int>(oam_[index * 4]) - 16;
        if (static_cast<int>(ly_) >= sprite_y &&
            static_cast<int>(ly_) < sprite_y + sprite_height) {
            sprites[sprite_count++] = index;
        }
    }
    std::sort(sprites.begin(), sprites.begin() + sprite_count,
              [this](const unsigned left, const unsigned right) {
                  const auto left_x = oam_[left * 4 + 1];
                  const auto right_x = oam_[right * 4 + 1];
                  return left_x != right_x ? left_x > right_x : left > right;
              });

    for (unsigned selected = 0; selected < sprite_count; ++selected) {
        const auto index = sprites[selected];
        const auto sprite_y = static_cast<int>(oam_[index * 4]) - 16;
        const auto sprite_x = static_cast<int>(oam_[index * 4 + 1]) - 8;
        auto tile = oam_[index * 4 + 2];
        const auto attributes = oam_[index * 4 + 3];
        auto row = static_cast<unsigned>(static_cast<int>(ly_) - sprite_y);
        if ((attributes & 0x40) != 0) {
            row = static_cast<unsigned>(sprite_height - 1) - row;
        }
        if (sprite_height == 16) {
            tile &= 0xFE;
            if (row >= 8) {
                ++tile;
                row -= 8;
            }
        }
        const auto tile_offset = static_cast<unsigned>(tile) * 16 + row * 2;
        const auto low = vram_[tile_offset];
        const auto high = vram_[tile_offset + 1];
        for (unsigned pixel = 0; pixel < 8; ++pixel) {
            const auto screen_x = sprite_x + static_cast<int>(pixel);
            if (screen_x < 0 || screen_x >= static_cast<int>(screen_width)) {
                continue;
            }
            const auto bit = (attributes & 0x20) != 0 ? pixel : 7U - pixel;
            const auto color = static_cast<std::uint8_t>(
                ((low >> bit) & 1U) | (((high >> bit) & 1U) << 1));
            if (color == 0 ||
                ((attributes & 0x80) != 0 && background_colors[screen_x] != 0)) {
                continue;
            }
            const auto palette = (attributes & 0x10) != 0
                                     ? object_palette_1_
                                     : object_palette_0_;
            framebuffer_[static_cast<std::size_t>(ly_) * screen_width +
                         static_cast<unsigned>(screen_x)] =
                palette_color(palette, color);
        }
    }
}

std::uint32_t Ppu::palette_color(const std::uint8_t palette,
                                 const std::uint8_t color) const noexcept {
    return dmg_colors[(palette >> (color * 2)) & 0x03];
}

} // namespace gameboy
