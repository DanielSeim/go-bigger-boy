#include "gameboy/ppu.hpp"

#include "gameboy/hardware_model.hpp"

#include <algorithm>
#include <array>

namespace gameboy {
namespace {
constexpr std::array<std::uint32_t, 4> dmg_colors{
    0xFFFFFFFF, 0xFFAAAAAA, 0xFF555555, 0xFF000000,
};
constexpr std::array<std::uint16_t, 16> default_sgb_palettes{
    0x7FFF, 0x5294, 0x294A, 0x0000,
    0x7FFF, 0x5294, 0x294A, 0x0000,
    0x7FFF, 0x5294, 0x294A, 0x0000,
    0x7FFF, 0x5294, 0x294A, 0x0000,
};
} // namespace

Ppu::Ppu()
    : cgb_vram_(std::make_unique<std::array<std::uint8_t, 0x2000>>()),
      framebuffer_(std::make_unique<Framebuffer>()),
      sgb_border_tiles_(std::make_unique<std::array<std::uint8_t, 0x2000>>()),
      sgb_border_pct_(std::make_unique<std::array<std::uint8_t, 0x1000>>()) {
    framebuffer_->fill(dmg_colors[0]);
    cgb_bg_palette_.fill(0xFF);
    cgb_object_palette_.fill(0xFF);
    trace_window_state("construct");
}

void Ppu::set_cgb_mode(const bool enabled) noexcept {
    cgb_mode_ = enabled;
    if (enabled) cgb_hardware_ = true;
}

void Ppu::set_cgb_hardware(const bool enabled) noexcept {
    cgb_hardware_ = enabled;
}

void Ppu::set_sgb_mode(const bool enabled) noexcept {
    sgb_mode_ = enabled;
    if (!enabled) return;
    // SGB starts with the same four neutral colors as a DMG until the game
    // sends its first PAL command.
    sgb_palettes_ = default_sgb_palettes;
    sgb_attributes_.fill(0);
    sgb_border_tiles_->fill(0);
    sgb_border_pct_->fill(0);
    sgb_mask_mode_ = 0;
}

bool Ppu::cgb_mode() const noexcept { return cgb_mode_; }

void Ppu::set_dmg_palette(const DmgPalette& palette) noexcept {
    dmg_palette_ = palette;
}

void Ppu::initialize_post_boot_phase(const HardwareModel model) noexcept {
    set_sgb_mode(model == HardwareModel::sgb || model == HardwareModel::sgb2);
    if (model == HardwareModel::dmg || model == HardwareModel::mgb ||
        model == HardwareModel::sgb || model == HardwareModel::sgb2) {
        // Later monochrome boot ROMs leave the registered-trademark tile at
        // $8190. The emulator skips the boot ROM, so reproduce that observable
        // post-boot VRAM state explicitly.
        constexpr std::array<std::uint8_t, 16> trademark_tile{
            0x3C, 0x00, 0x42, 0x00, 0xB9, 0x00, 0xA5, 0x00,
            0xB9, 0x00, 0xA5, 0x00, 0x42, 0x00, 0x3C, 0x00,
        };
        std::copy(trademark_tile.begin(), trademark_tile.end(),
                  vram_.begin() + 0x190);
    }
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

std::uint8_t Ppu::debug_read_sgb_border_tile(
    const std::uint16_t offset) const noexcept {
    return offset < sgb_border_tiles_->size() ? (*sgb_border_tiles_)[offset]
                                              : 0xFF;
}

std::uint8_t Ppu::debug_read_sgb_border_pct(
    const std::uint16_t offset) const noexcept {
    return offset < sgb_border_pct_->size() ? (*sgb_border_pct_)[offset] : 0xFF;
}

std::uint8_t Ppu::sgb_mask_mode() const noexcept { return sgb_mask_mode_; }

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

std::uint8_t Ppu::debug_read_vram(const std::uint8_t bank,
                                  const std::uint16_t offset) const noexcept {
    if (offset >= 0x2000) return 0xFF;
    return bank != 0 && cgb_mode_ ? (*cgb_vram_)[offset] : vram_[offset];
}

std::uint8_t Ppu::debug_read_oam(const std::uint8_t offset) const noexcept {
    return offset < oam_.size() ? oam_[offset] : 0xFF;
}

void Ppu::debug_write_oam(const std::uint8_t offset,
                          const std::uint8_t value) noexcept {
    if (offset < oam_.size()) oam_[offset] = value;
}

std::uint8_t Ppu::debug_read_cgb_bg_palette(
    const std::uint8_t index) const noexcept {
    return index < cgb_bg_palette_.size() ? cgb_bg_palette_[index] : 0xFF;
}

std::uint8_t Ppu::debug_read_cgb_object_palette(
    const std::uint8_t index) const noexcept {
    return index < cgb_object_palette_.size() ? cgb_object_palette_[index] : 0xFF;
}

void Ppu::debug_write_vram(const std::uint8_t bank,
                           const std::uint16_t offset,
                           const std::uint8_t value) noexcept {
    if (offset >= 0x2000) return;
    auto& memory = bank != 0 && cgb_mode_ ? *cgb_vram_ : vram_;
    memory[offset] = value;
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
        const auto window_was_enabled = (lcdc_ & 0x20) != 0;
        const auto object_was_enabled = (lcdc_ & 0x02) != 0;
        if (window_was_enabled && (value & 0x20) == 0) {
            if (using_window_) {
                if (window_source_x_ != 0 && window_source_x_ <= 2) {
                    // The first window tile can still be queued before its
                    // hidden left-side pixels have been emitted. Clamp the
                    // rewind point instead of allowing unsigned underflow.
                    const auto activation_x = output_x_ > window_source_x_
                                                  ? output_x_ - window_source_x_
                                                  : 0U;
                    for (auto x = activation_x; x < output_x_; ++x) {
                        const auto offset = static_cast<std::size_t>(ly_) *
                                                screen_width + x;
                        (*framebuffer_)[offset] =
                            compose_pixel(x, background_pixel_at_screen(x));
                    }
                    resume_background_fetch();
                    const auto source_offset =
                        static_cast<unsigned>(scx_ & 7U) + output_x_;
                    fetcher_tile_index_ = static_cast<std::uint8_t>(
                        source_offset / 8U);
                    scroll_discard_ = static_cast<std::uint8_t>(
                        source_offset & 7U);
                } else if (window_source_x_ == 0 &&
                           (output_x_ & 7U) == 0) {
                    resume_background_fetch();
                    window_glitch_x_ = output_x_;
                    window_glitch_pending_ = true;
                } else {
                    window_disable_pending_ = true;
                    window_disable_source_x_ = static_cast<std::uint16_t>(
                        (window_source_x_ + 7U) & ~7U);
                    if (output_x_ == 0 && window_source_x_ != 0) {
                        window_disable_source_x_ = static_cast<std::uint16_t>(
                            window_disable_source_x_ + 8U);
                    }
                }
            } else if (stat_mode_ == 3 && window_y_triggered_ &&
                       window_x_ <= 166 && window_activation_count_ == 0) {
                const auto window_start = std::max(0, static_cast<int>(window_x_) - 7);
                if (window_start <= static_cast<int>(output_x_) + 7) {
                    window_trigger_x_ = static_cast<std::uint8_t>(
                        std::max(window_start,
                                 static_cast<int>(output_x_) + 1));
                    window_trigger_pending_ = true;
                }
            }
        }
        lcdc_ = value;
        trace_window_state("lcdc_write");
        if (object_was_enabled && (value & 0x02) == 0) {
            // A fetch can have populated the object FIFO before the PPU has
            // reached its cancellation boundary. Remove only those sprites;
            // already-emitted pixels are restored to the background.
            const auto cancelled = pending_sprite_mask_;
            for (unsigned x = 0; x < screen_width; ++x) {
                const auto source = object_pixels_[x].oam_index;
                if (!object_pixels_[x].valid || source >= 40 ||
                    (cancelled & (std::uint64_t{1} << source)) == 0 ||
                    object_pixel_deadlines_[x] == 0) {
                    continue;
                }
                // Invalidate the queued object before recomposing the pixel.
                // compose_pixel() consults object_pixels_, so doing this in
                // the opposite order would redraw the very OBJ pixel that
                // the LCDC write is meant to cancel.
                object_pixels_[x].valid = false;
                if (x < output_x_) {
                    (*framebuffer_)[static_cast<std::size_t>(ly_) * screen_width +
                                     x] = compose_pixel(
                        x, emitted_background_[x]);
                }
            }
            pending_sprite_mask_ = 0;
            rendered_sprite_mask_ &= ~cancelled;
            pending_sprite_deadlines_.fill(0);
            render_sprite_deadlines_.fill(0);
            // Disabling OBJ leaves the fetcher in its bus handoff phase for
            // eight dots before background output can resume.
            sprite_delay_ = static_cast<std::uint8_t>(sprite_delay_ + 8);
        }
        if (was_enabled && !lcd_enabled()) {
            dot_ = 0;
            ly_ = 0;
            mode_ = 0;
            stat_mode_ = 0;
            mode3_end_dot_ = 252;
            window_line_ = 0;
            window_y_triggered_ = false;
            window_rendered_this_line_ = false;
            background_fifo_size_ = 0;
            line_sprite_count_ = 0;
            output_x_ = 0;
            using_window_ = false;
            window_disable_pending_ = false;
            window_disable_source_x_ = 0;
            window_trigger_pending_ = false;
            window_trigger_x_ = 0;
            lcd_startup_ = false;
            frame_ready_ = false;
            framebuffer_->fill(dmg_colors[0]);
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
    case 0xFF4B: {
        const auto new_window_start = static_cast<int>(value) - 7;
        if (window_glitch_applied_ &&
            output_x_ <= static_cast<unsigned>(window_glitch_applied_x_) + 3) {
            const auto applied_x =
                static_cast<unsigned>(window_glitch_applied_x_);
            const auto affected_pixels = output_x_ - applied_x;
            if (affected_pixels <= 2) {
                const auto offset = static_cast<std::size_t>(ly_) *
                                        screen_width + applied_x;
                (*framebuffer_)[offset] = window_glitch_restore_color_;
                if (background_fifo_size_ != 0) {
                    const auto last_x = static_cast<unsigned>(output_x_ - 1);
                    if (last_x != applied_x) {
                        (*framebuffer_)[static_cast<std::size_t>(ly_) *
                                            screen_width + last_x] =
                            compose_pixel(last_x, pop_background_pixel());
                    } else {
                        static_cast<void>(pop_background_pixel());
                    }
                    ++window_source_x_;
                }
            } else {
                for (auto x = applied_x;
                     x < output_x_ && background_fifo_size_ != 0; ++x) {
                    const auto background = pop_background_pixel();
                    const auto offset = static_cast<std::size_t>(ly_) *
                                            screen_width + x;
                    (*framebuffer_)[offset] = compose_pixel(x, background);
                    ++window_source_x_;
                }
            }
        }
        window_glitch_applied_ = false;
        window_glitch_pending_ = false;
        if (using_window_ && new_window_start >= output_x_ &&
            new_window_start < static_cast<int>(screen_width)) {
            const auto pixels_until_target = static_cast<unsigned>(
                new_window_start - static_cast<int>(output_x_));
            if (((window_source_x_ + pixels_until_target) & 7U) == 0) {
                window_glitch_x_ = static_cast<std::uint8_t>(new_window_start);
                window_glitch_pending_ = true;
            }
        } else if (using_window_ && new_window_start <
                   static_cast<int>(output_x_) &&
                   output_x_ < 8 && window_x_ == 6 && value < 6) {
            // A WX write that moves the comparator behind the first few
            // visible pixels cancels the in-flight window handoff. The
            // already-emitted prefix remains window data; subsequent pixels
            // resume the background fetch pipeline.
            window_disable_pending_ = true;
            window_disable_source_x_ = 8;
        }
        const auto comparator_start = std::max(0, new_window_start);
        if (window_activation_count_ != 0 &&
            comparator_start >= static_cast<int>(output_x_) &&
            comparator_start < static_cast<int>(screen_width)) {
            window_retrigger_armed_ = true;
        }
        window_x_ = value;
        trace_window_state("wx_write");
        break;
    }
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

} // namespace gameboy
