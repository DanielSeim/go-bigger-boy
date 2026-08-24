#include "gameboy/ppu.hpp"

#include "gameboy/hardware_model.hpp"

#include <algorithm>
#include <array>

namespace gameboy {
namespace {
constexpr std::array<std::uint32_t, 4> dmg_colors{
    0xFFFFFFFF, 0xFFAAAAAA, 0xFF555555, 0xFF000000,
};
} // namespace

Ppu::Ppu()
    : cgb_vram_(std::make_unique<std::array<std::uint8_t, 0x2000>>()) {
    framebuffer_.fill(dmg_colors[0]);
    cgb_bg_palette_.fill(0xFF);
    cgb_object_palette_.fill(0xFF);
}

void Ppu::set_cgb_mode(const bool enabled) noexcept {
    cgb_mode_ = enabled;
    if (enabled) cgb_hardware_ = true;
}

void Ppu::set_cgb_hardware(const bool enabled) noexcept {
    cgb_hardware_ = enabled;
}

bool Ppu::cgb_mode() const noexcept { return cgb_mode_; }

void Ppu::set_dmg_palette(const DmgPalette& palette) noexcept {
    dmg_palette_ = palette;
}

void Ppu::initialize_post_boot_phase(const HardwareModel model) noexcept {
    if (model == HardwareModel::cgb0 || model == HardwareModel::cgb) {
        bg_palette_index_ = 0x88;
        object_palette_index_ = 0x90;
        return;
    }
    if (model != HardwareModel::dmg0) return;

    // The original DMG boot ROM hands control to the cartridge near the end
    // of a frame, unlike later DMG/MGB boot ROM revisions.
    ly_ = 145;
    dot_ = 200;
    mode_ = 1;
    stat_mode_ = 1;
    coincidence_ = ly_ == lyc_;
    lcd_startup_ = false;
    static_cast<void>(update_stat_line());
}

std::uint8_t Ppu::read_vram(const std::uint16_t address) const noexcept {
    if (lcd_enabled() && (stat_mode_ == 3 || mode_ == 3)) {
        return 0xFF;
    }
    const auto& bank = cgb_mode_ && vram_bank_ != 0 ? *cgb_vram_ : vram_;
    return bank[address - 0x8000];
}

void Ppu::write_vram(const std::uint16_t address,
                     const std::uint8_t value) noexcept {
    if (!lcd_enabled() || mode_ != 3) {
        auto& bank = cgb_mode_ && vram_bank_ != 0 ? *cgb_vram_ : vram_;
        bank[address - 0x8000] = value;
    }
}

void Ppu::dma_write_vram(const std::uint16_t address,
                         const std::uint8_t value) noexcept {
    auto& bank = cgb_mode_ && vram_bank_ != 0 ? *cgb_vram_ : vram_;
    bank[address - 0x8000] = value;
}

std::uint8_t Ppu::read_oam(const std::uint16_t address) const noexcept {
    if (lcd_enabled() &&
        (stat_mode_ == 2 || mode_ == 2 || mode_ == 3)) {
        return 0xFF;
    }
    return oam_[address - 0xFE00];
}

void Ppu::write_oam(const std::uint16_t address,
                    const std::uint8_t value) noexcept {
    // DMG hardware accepts a write on the internal mode 2-to-3 handoff dot,
    // even though reads remain blocked throughout the transition.
    const auto mode_transition_write = !lcd_startup_ && dot_ == 80;
    if (!lcd_enabled() || (mode_ != 2 && mode_ != 3) ||
        mode_transition_write) {
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
           (address >= 0xFF47 && address <= 0xFF4B) || address == 0xFF4F ||
           (address >= 0xFF68 && address <= 0xFF6B);
}

std::uint8_t Ppu::read_register(const std::uint16_t address) const noexcept {
    switch (address) {
    case 0xFF40: return lcdc_;
    case 0xFF41:
        return static_cast<std::uint8_t>(
            0x80 | stat_select_ | (coincidence_ ? 0x04 : 0) |
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
    case 0xFF4F:
        return cgb_hardware_ ? static_cast<std::uint8_t>(0xFE | vram_bank_)
                             : 0xFF;
    case 0xFF68:
        return cgb_hardware_
                   ? static_cast<std::uint8_t>(0x40 | bg_palette_index_)
                   : 0xFF;
    case 0xFF69:
        return cgb_mode_ && mode_ != 3
                   ? cgb_bg_palette_[bg_palette_index_ & 0x3F]
                   : 0xFF;
    case 0xFF6A:
        return cgb_hardware_
                   ? static_cast<std::uint8_t>(0x40 | object_palette_index_)
                   : 0xFF;
    case 0xFF6B:
        return cgb_mode_ && mode_ != 3
                   ? cgb_object_palette_[object_palette_index_ & 0x3F]
                   : 0xFF;
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
            stat_mode_ = 0;
            mode3_end_dot_ = 252;
            window_line_ = 0;
            window_y_triggered_ = false;
            window_rendered_this_line_ = false;
            lcd_startup_ = false;
            frame_ready_ = false;
            framebuffer_.fill(dmg_colors[0]);
            return false;
        }
        if (!was_enabled && lcd_enabled()) {
            dot_ = 0;
            ly_ = 0;
            mode_ = 0;
            stat_mode_ = 0;
            window_line_ = 0;
            window_rendered_this_line_ = false;
            coincidence_ = ly_ == lyc_;
            lcd_startup_ = true;
            begin_visible_line();
        }
        break;
    }
    case 0xFF41: stat_select_ = static_cast<std::uint8_t>(value & 0x78); break;
    case 0xFF42: scy_ = value; break;
    case 0xFF43: scx_ = value; break;
    case 0xFF44: break; // LY is read-only.
    case 0xFF45:
        lyc_ = value;
        if (lcd_enabled()) coincidence_ = ly_ == lyc_;
        break;
    case 0xFF47: bg_palette_ = value; break;
    case 0xFF48: object_palette_0_ = value; break;
    case 0xFF49: object_palette_1_ = value; break;
    case 0xFF4A: window_y_ = value; break;
    case 0xFF4B: window_x_ = value; break;
    case 0xFF4F:
        if (cgb_hardware_) vram_bank_ = static_cast<std::uint8_t>(value & 0x01);
        break;
    case 0xFF68:
        if (cgb_hardware_) {
            bg_palette_index_ = static_cast<std::uint8_t>(value & 0xBF);
        }
        break;
    case 0xFF69:
        if (cgb_mode_) {
            if (mode_ != 3) cgb_bg_palette_[bg_palette_index_ & 0x3F] = value;
            if ((bg_palette_index_ & 0x80) != 0) {
                bg_palette_index_ = static_cast<std::uint8_t>(
                    0x80 | ((bg_palette_index_ + 1) & 0x3F));
            }
        }
        break;
    case 0xFF6A:
        if (cgb_hardware_) {
            object_palette_index_ = static_cast<std::uint8_t>(value & 0xBF);
        }
        break;
    case 0xFF6B:
        if (cgb_mode_) {
            if (mode_ != 3) {
                cgb_object_palette_[object_palette_index_ & 0x3F] = value;
            }
            if ((object_palette_index_ & 0x80) != 0) {
                object_palette_index_ = static_cast<std::uint8_t>(
                    0x80 | ((object_palette_index_ + 1) & 0x3F));
            }
        }
        break;
    default: break;
    }
    if (!lcd_enabled()) return false;
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
            // LCD startup exposes its transitions immediately. On subsequent
            // lines, STAT sources and memory arbitration change internally
            // one dot before the mode bits visible to the CPU.
            if (lcd_startup_ && dot_ == 80) {
                stat_mode_ = 3;
                mode_ = 3;
                mode3_end_dot_ = 80 + mode3_duration();
                if (update_stat_line()) requests |= 0x02;
            } else if (!lcd_startup_ && dot_ == 1) {
                mode_ = 2;
                coincidence_ = ly_ == lyc_;
                if (update_stat_line()) requests |= 0x02;
            } else if (!lcd_startup_ && dot_ == 80) {
                stat_mode_ = 3;
                mode3_end_dot_ = 80 + mode3_duration();
                if (update_stat_line()) requests |= 0x02;
            } else if (!lcd_startup_ && dot_ == 81) {
                mode_ = 3;
            }

            if (stat_mode_ == 3) {
                const auto pixel_start_dot = mode3_end_dot_ - screen_width;
                if (dot_ >= pixel_start_dot && dot_ < mode3_end_dot_) {
                    render_pixel(dot_ - pixel_start_dot);
                }
            }

            if (dot_ == mode3_end_dot_) {
                stat_mode_ = 0;
                if (window_rendered_this_line_) {
                    ++window_line_;
                }
                if (lcd_startup_) {
                    mode_ = 0;
                    requests |= 0x04;
                }
                if (update_stat_line()) requests |= 0x02;
            } else if (!lcd_startup_ && dot_ == mode3_end_dot_ + 1) {
                mode_ = 0;
                requests |= 0x04;
            }

            // On CGB hardware the mode-2 STAT source associated with line 144
            // rises one M-cycle before the VBlank request and visible mode 1.
            if (cgb_hardware_ && ly_ == screen_height - 1 && dot_ == 452 &&
                (stat_select_ & 0x20) != 0 && !stat_line_) {
                stat_line_ = true;
                requests |= 0x02;
            }
        }

        // The shortened first line is a DMG LCD-enable startup quirk.
        const auto line_length = lcd_startup_ ? 452U : 456U;
        if (dot_ == line_length) {
            dot_ = 0;
            ++ly_;
            lcd_startup_ = false;
            if (ly_ == screen_height) {
                coincidence_ = ly_ == lyc_;
                mode_ = 1;
                stat_mode_ = 1;
                frame_ready_ = true;
                requests |= 0x01;
            } else if (ly_ > 153) {
                ly_ = 0;
                coincidence_ = ly_ == lyc_;
                mode_ = 0;
                stat_mode_ = 2;
                window_line_ = 0;
                window_y_triggered_ = false;
                begin_visible_line();
            } else if (ly_ < screen_height) {
                coincidence_ = false;
                mode_ = 0;
                stat_mode_ = 2;
                begin_visible_line();
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
    return ((stat_select_ & 0x40) != 0 && coincidence_) ||
           ((stat_select_ & 0x20) != 0 &&
            (stat_mode_ == 2 ||
             (stat_mode_ == 1 && ly_ == screen_height))) ||
           ((stat_select_ & 0x10) != 0 && stat_mode_ == 1) ||
           ((stat_select_ & 0x08) != 0 && stat_mode_ == 0);
}

bool Ppu::update_stat_line() noexcept {
    const auto new_line = stat_condition();
    const auto rising_edge = new_line && !stat_line_;
    stat_line_ = new_line;
    return rising_edge;
}

bool Ppu::window_active_on_line() const noexcept {
    const auto background_allows_window = cgb_mode_ || (lcdc_ & 0x01) != 0;
    return background_allows_window && (lcdc_ & 0x20) != 0 &&
           window_y_triggered_ && window_x_ <= 166;
}

void Ppu::begin_visible_line() noexcept {
    window_rendered_this_line_ = false;
    if (ly_ == window_y_) {
        window_y_triggered_ = true;
    }
}

unsigned Ppu::mode3_duration() const noexcept {
    auto duration = 172U + (scx_ & 7U);

    const auto window_active = window_active_on_line();
    const auto window_start = static_cast<int>(window_x_) - 7;
    if (window_active) {
        duration += 6;
        if (window_x_ == 0 && (scx_ & 7U) != 0) {
            --duration;
        }
    }

    if ((lcdc_ & 0x02) == 0) {
        return duration;
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
                  return left_x != right_x ? left_x < right_x : left < right;
              });

    auto previous_tile = 0;
    auto previous_was_window = false;
    auto have_previous_tile = false;
    for (unsigned selected = 0; selected < sprite_count; ++selected) {
        const auto raw_x = static_cast<unsigned>(oam_[sprites[selected] * 4 + 1]);
        if (raw_x >= 168) {
            continue;
        }

        const auto screen_x = static_cast<int>(raw_x) - 8;
        const auto in_window = window_active && screen_x >= window_start;
        const auto fetch_x = in_window
                                 ? screen_x - window_start
                                 : screen_x + static_cast<int>(scx_);
        const auto tile = fetch_x >= 0 ? fetch_x / 8 : (fetch_x - 7) / 8;
        const auto same_tile = have_previous_tile && tile == previous_tile &&
                               in_window == previous_was_window;

        if (!same_tile) {
            const auto phase = raw_x == 0
                                   ? 0U
                                   : static_cast<unsigned>((fetch_x % 8 + 8) % 8);
            duration += phase < 5 ? 5 - phase : 0;
        }
        duration += 6;
        previous_tile = tile;
        previous_was_window = in_window;
        have_previous_tile = true;
    }

    return std::min(duration, 289U);
}

void Ppu::render_pixel(const unsigned x) noexcept {
    std::uint8_t background_color = 0;
    auto background_priority = false;
    const auto background_enabled = cgb_mode_ || (lcdc_ & 0x01) != 0;
    const auto window_enabled =
        background_enabled && (lcdc_ & 0x20) != 0 &&
        window_y_triggered_ && window_x_ <= 166;
    const auto window_start = static_cast<int>(window_x_) - 7;

    std::uint8_t palette_number = 0;
    if (background_enabled) {
        const auto use_window = window_enabled &&
                                static_cast<int>(x) >= window_start;
        if (use_window) window_rendered_this_line_ = true;
        const auto pixel_x = use_window
                                 ? static_cast<unsigned>(
                                       static_cast<int>(x) - window_start)
                                 : static_cast<unsigned>(
                                       static_cast<std::uint8_t>(x + scx_));
        const auto pixel_y = use_window
                                 ? static_cast<unsigned>(window_line_)
                                 : static_cast<unsigned>(
                                       static_cast<std::uint8_t>(ly_ + scy_));
        const auto map_base = use_window
                                  ? ((lcdc_ & 0x40) != 0 ? 0x1C00U : 0x1800U)
                                  : ((lcdc_ & 0x08) != 0 ? 0x1C00U : 0x1800U);
        const auto map_offset = map_base + (pixel_y / 8) * 32 + pixel_x / 8;
        const auto tile_number = vram_[map_offset & 0x1FFF];
        const auto attributes = cgb_mode_ ? (*cgb_vram_)[map_offset & 0x1FFF]
                                          : std::uint8_t{0};
        unsigned tile_offset = 0;
        if ((lcdc_ & 0x10) != 0) {
            tile_offset = static_cast<unsigned>(tile_number) * 16;
        } else {
            const auto signed_tile = tile_number < 0x80
                                         ? static_cast<int>(tile_number)
                                         : static_cast<int>(tile_number) - 0x100;
            tile_offset = static_cast<unsigned>(0x1000 + signed_tile * 16);
        }
        auto tile_row = pixel_y & 7U;
        if ((attributes & 0x40) != 0) tile_row = 7 - tile_row;
        const auto row = tile_row * 2;
        const auto& tile_bank = (attributes & 0x08) != 0 ? *cgb_vram_ : vram_;
        const auto low = tile_bank[tile_offset + row];
        const auto high = tile_bank[tile_offset + row + 1];
        const auto column = pixel_x & 7U;
        const auto bit = (attributes & 0x20) != 0 ? column : 7U - column;
        background_color = static_cast<std::uint8_t>(
            ((low >> bit) & 1U) | (((high >> bit) & 1U) << 1));
        palette_number = static_cast<std::uint8_t>(attributes & 0x07);
        background_priority = (attributes & 0x80) != 0;
    }

    const auto framebuffer_offset =
        static_cast<std::size_t>(ly_) * screen_width + x;
    framebuffer_[framebuffer_offset] =
        cgb_mode_
            ? cgb_palette_color(cgb_bg_palette_, palette_number,
                                background_color)
            : palette_color(bg_palette_, background_color,
                            dmg_palette_.background);

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
                  if (cgb_mode_) return left > right;
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
        const auto& tile_bank = cgb_mode_ && (attributes & 0x08) != 0
                                    ? *cgb_vram_
                                    : vram_;
        const auto low = tile_bank[tile_offset];
        const auto high = tile_bank[tile_offset + 1];
        if (static_cast<int>(x) < sprite_x ||
            static_cast<int>(x) >= sprite_x + 8) {
            continue;
        }
        const auto pixel = static_cast<unsigned>(static_cast<int>(x) - sprite_x);
        const auto bit = (attributes & 0x20) != 0 ? pixel : 7U - pixel;
        const auto color = static_cast<std::uint8_t>(
            ((low >> bit) & 1U) | (((high >> bit) & 1U) << 1));
        const auto background_blocks_object =
            background_color != 0 &&
            (cgb_mode_
                 ? (lcdc_ & 0x01) != 0 &&
                       (background_priority || (attributes & 0x80) != 0)
                 : (attributes & 0x80) != 0);
        if (color == 0 || background_blocks_object) {
            continue;
        }
        framebuffer_[framebuffer_offset] =
            cgb_mode_
                ? cgb_palette_color(cgb_object_palette_,
                                    static_cast<std::uint8_t>(attributes & 0x07),
                                    color)
                : palette_color(
                      (attributes & 0x10) != 0 ? object_palette_1_
                                              : object_palette_0_,
                      color, (attributes & 0x10) != 0
                                 ? dmg_palette_.object_1
                                 : dmg_palette_.object_0);
    }
}

std::uint32_t Ppu::cgb_palette_color(
    const std::array<std::uint8_t, 0x40>& palette, const std::uint8_t number,
    const std::uint8_t color) const noexcept {
    const auto offset = static_cast<unsigned>(number) * 8 + color * 2;
    const auto rgb555 = static_cast<std::uint16_t>(
        palette[offset] | (static_cast<std::uint16_t>(palette[offset + 1]) << 8));
    const auto expand = [](const unsigned component) {
        return (component << 3) | (component >> 2);
    };
    const auto red = expand(rgb555 & 0x1F);
    const auto green = expand((rgb555 >> 5) & 0x1F);
    const auto blue = expand((rgb555 >> 10) & 0x1F);
    return UINT32_C(0xFF000000) | (red << 16) | (green << 8) | blue;
}

std::uint32_t Ppu::palette_color(
    const std::uint8_t palette, const std::uint8_t color,
    const std::array<std::uint32_t, 4>& colors) const noexcept {
    return colors[(palette >> (color * 2)) & 0x03];
}

} // namespace gameboy
