#include "save_state_bus.hpp"

#include "gameboy/memory_bus.hpp"
#include "save_state_apu.hpp"
#include "save_state_cartridge.hpp"
#include "save_state_joypad.hpp"
#include "save_state_ppu.hpp"
#include "save_state_timer.hpp"

#include <array>
#include <cstdint>

namespace gameboy {
namespace {

template <std::size_t Size>
void read_bytes(save_state_format::Reader& reader,
                std::array<std::uint8_t, Size>& values) {
    reader.bytes(values.data(), values.size());
}

} // namespace

// Keep compatibility migrations separate from the current payload layout so
// adding a versioned field does not obscure the canonical write order.
void SaveStateBusCodec::read(save_state_format::Reader& reader,
                             MemoryBus& bus,
                             const std::uint32_t version) {
    SaveStateCartridgeCodec::read(reader, bus.cartridge_);
    read_bytes(reader, bus.wram_);
    read_bytes(reader, bus.io_);
    read_bytes(reader, bus.hram_);
    bus.interrupt_enable_ = reader.u8();
    SaveStateJoypadCodec::read(reader, bus.joypad_);
    SaveStateApuCodec::read(reader, bus.apu_, version);
    SaveStatePpuCodec::read(reader, bus.ppu_);
    SaveStateTimerCodec::read(reader, bus.timer_);
    bus.serial_output_ = reader.string();
    bus.serial_cycles_remaining_ = reader.u32();
    if (version >= 2) {
        bus.oam_dma_active_ = reader.boolean();
        bus.oam_dma_source_ = static_cast<std::uint16_t>(reader.u8() << 8);
        bus.oam_dma_index_ = reader.u16();
        bus.oam_dma_cycle_ = reader.u8();
        bus.oam_dma_pending_source_ =
            static_cast<std::uint16_t>(reader.u8() << 8);
        bus.oam_dma_start_delay_ = reader.u8();
        if (bus.oam_dma_index_ > 0xA0 || bus.oam_dma_cycle_ >= 4 ||
            bus.oam_dma_start_delay_ > 8 ||
            (bus.oam_dma_active_ && bus.oam_dma_index_ == 0xA0)) {
            throw SaveStateError("Save state contains invalid OAM DMA state");
        }
    } else {
        bus.oam_dma_active_ = false;
        bus.oam_dma_source_ = 0;
        bus.oam_dma_index_ = 0;
        bus.oam_dma_cycle_ = 0;
        bus.oam_dma_pending_source_ = 0;
        bus.oam_dma_start_delay_ = 0;
    }
    if (version >= 3) {
        bus.ppu_.mode3_end_dot_ = reader.u32();
        bus.ppu_.window_line_ = reader.u8();
        bus.ppu_.window_y_triggered_ = reader.boolean();
        if (bus.ppu_.mode3_end_dot_ < 252 ||
            bus.ppu_.mode3_end_dot_ > 369 ||
            bus.ppu_.window_line_ > Ppu::screen_height) {
            throw SaveStateError("Save state contains invalid PPU timing state");
        }
    } else {
        bus.ppu_.mode3_end_dot_ = 252;
        bus.ppu_.window_y_triggered_ = bus.ppu_.ly_ >= bus.ppu_.window_y_;
        bus.ppu_.window_line_ = bus.ppu_.window_y_triggered_
                                    ? static_cast<std::uint8_t>(
                                          bus.ppu_.ly_ - bus.ppu_.window_y_)
                                    : 0;
    }
    if (version >= 5) {
        bus.ppu_.coincidence_ = reader.boolean();
    } else {
        bus.ppu_.coincidence_ = bus.ppu_.ly_ == bus.ppu_.lyc_;
    }
    if (version >= 6) {
        bus.ppu_.stat_mode_ = reader.u8();
        if (bus.ppu_.stat_mode_ > 3) {
            throw SaveStateError("invalid PPU STAT mode");
        }
        bus.ppu_.lcd_startup_ = reader.boolean();
        bus.timer_.reload_happened_ = reader.boolean();
        bus.serial_clock_ = reader.u16();
    } else {
        bus.ppu_.stat_mode_ = bus.ppu_.mode_;
        bus.ppu_.lcd_startup_ = false;
        bus.timer_.reload_happened_ = false;
        bus.serial_clock_ = 0;
    }
    if (version >= 4) {
        read_bytes(reader, *bus.cgb_wram_);
        read_bytes(reader, *bus.ppu_.cgb_vram_);
        read_bytes(reader, bus.ppu_.cgb_bg_palette_);
        read_bytes(reader, bus.ppu_.cgb_object_palette_);
        bus.wram_bank_ = reader.u8();
        bus.ppu_.vram_bank_ = reader.u8();
        bus.ppu_.bg_palette_index_ = reader.u8();
        bus.ppu_.object_palette_index_ = reader.u8();
        bus.hdma_source_ = reader.u16();
        bus.hdma_destination_ = reader.u16();
        bus.hdma_blocks_remaining_ = reader.u8();
        bus.hdma_active_ = reader.boolean();
        bus.double_speed_ = reader.boolean();
        bus.speed_switch_requested_ = reader.boolean();
        if (bus.wram_bank_ < 1 || bus.wram_bank_ > 7 ||
            bus.ppu_.vram_bank_ > 1 ||
            (bus.ppu_.bg_palette_index_ & 0x40) != 0 ||
            (bus.ppu_.object_palette_index_ & 0x40) != 0 ||
            bus.hdma_destination_ < 0x8000 ||
            bus.hdma_destination_ > 0x9FFF ||
            (bus.hdma_destination_ & 0x000F) != 0 ||
            (bus.hdma_source_ & 0x000F) != 0 ||
            (bus.hdma_active_ && bus.hdma_blocks_remaining_ == 0) ||
            (!bus.cgb_mode_ &&
             (bus.double_speed_ || bus.speed_switch_requested_))) {
            throw SaveStateError("Save state contains invalid CGB bank state");
        }
        bus.timer_.set_double_speed(bus.double_speed_);
    } else {
        bus.cgb_wram_->fill(0);
        bus.ppu_.cgb_vram_->fill(0);
        bus.ppu_.cgb_bg_palette_.fill(0xFF);
        bus.ppu_.cgb_object_palette_.fill(0xFF);
        bus.wram_bank_ = 1;
        bus.ppu_.vram_bank_ = 0;
        bus.ppu_.bg_palette_index_ = 0;
        bus.ppu_.object_palette_index_ = 0;
        bus.hdma_source_ = 0;
        bus.hdma_destination_ = 0x8000;
        bus.hdma_blocks_remaining_ = 0;
        bus.hdma_active_ = false;
        bus.double_speed_ = false;
        bus.speed_switch_requested_ = false;
        bus.timer_.set_double_speed(false);
    }
    bus.serial_.restore_state(
        bus.io_[0x01], bus.io_[0x02], 0, 0,
        (bus.io_[0x02] & 0x80) != 0,
        (bus.io_[0x02] & 0x01) != 0, (bus.io_[0x02] & 0x02) != 0);
    if (version >= 7) {
        const auto contains_camera = reader.boolean();
        if (contains_camera != bus.cartridge_.has_camera()) {
            throw SaveStateError(
                "Save-state camera hardware does not match cartridge");
        }
        if (contains_camera) {
            bus.cartridge_.camera_registers_mapped_ = reader.boolean();
            read_bytes(reader, bus.cartridge_.camera_registers_);
            reader.bytes(bus.cartridge_.camera_frame_.data(),
                         bus.cartridge_.camera_frame_.size());
            reader.bytes(bus.cartridge_.camera_image_.data(),
                         bus.cartridge_.camera_image_.size());
            bus.cartridge_.camera_image_dirty_ = true;
        }
    } else if (bus.cartridge_.has_camera()) {
        bus.cartridge_.camera_registers_mapped_ = false;
        bus.cartridge_.camera_registers_.fill(0);
        bus.cartridge_.capture_camera_image();
    }
    if (version >= 8) {
        bus.ppu_.window_rendered_this_line_ = reader.boolean();
    } else {
        bus.ppu_.window_rendered_this_line_ = false;
    }
    if (version >= 9) {
        bus.ppu_.background_fifo_size_ = reader.u8();
        for (auto& pixel : bus.ppu_.background_fifo_) {
            pixel.color = reader.u8();
            pixel.palette = reader.u8();
            pixel.priority = reader.boolean();
        }
        for (auto& pixel : bus.ppu_.object_pixels_) {
            pixel.color = reader.u8();
            pixel.attributes = reader.u8();
            pixel.oam_index = reader.u8();
            pixel.valid = reader.boolean();
        }
        read_bytes(reader, bus.ppu_.line_sprites_);
        bus.ppu_.fetcher_phase_ = reader.u8();
        bus.ppu_.fetcher_phase_ticks_ = reader.u8();
        bus.ppu_.fetcher_tile_index_ = reader.u8();
        bus.ppu_.fetched_tile_ = reader.u8();
        bus.ppu_.fetched_attributes_ = reader.u8();
        bus.ppu_.fetched_low_ = reader.u8();
        bus.ppu_.fetched_high_ = reader.u8();
        bus.ppu_.fetched_row_ = reader.u8();
        bus.ppu_.line_sprite_count_ = reader.u8();
        bus.ppu_.next_line_sprite_ = reader.u8();
        bus.ppu_.output_x_ = reader.u8();
        bus.ppu_.startup_delay_ = reader.u8();
        bus.ppu_.scroll_discard_ = reader.u8();
        bus.ppu_.window_delay_ = reader.u8();
        bus.ppu_.sprite_delay_ = reader.u8();
        bus.ppu_.fetched_source_x_ = static_cast<std::int16_t>(reader.u16());
        bus.ppu_.previous_sprite_tile_ =
            static_cast<std::int16_t>(reader.u16());
        bus.ppu_.fetched_window_ = reader.boolean();
        bus.ppu_.using_window_ = reader.boolean();
        bus.ppu_.previous_sprite_was_window_ = reader.boolean();
        bus.ppu_.have_previous_sprite_tile_ = reader.boolean();
        bus.ppu_.window_glitch_x_ = reader.u8();
        bus.ppu_.window_glitch_pending_ = reader.boolean();
        bus.ppu_.window_source_x_ = reader.u16();
        bus.ppu_.window_glitch_applied_x_ = reader.u8();
        bus.ppu_.window_glitch_restore_color_ = reader.u32();
        bus.ppu_.window_glitch_applied_ = reader.boolean();
        bus.ppu_.window_activation_count_ = reader.u8();
        bus.ppu_.window_fetch_line_ = reader.u8();
        bus.ppu_.window_retrigger_armed_ = reader.boolean();
        bus.ppu_.window_fetch_start_x_ = reader.u8();
        bus.ppu_.window_disable_pending_ = reader.boolean();
        if (version >= 10) {
            bus.ppu_.window_trigger_x_ = reader.u8();
            bus.ppu_.window_trigger_pending_ = reader.boolean();
            bus.ppu_.window_disable_source_x_ = reader.u16();
        } else {
            bus.ppu_.window_trigger_x_ = 0;
            bus.ppu_.window_trigger_pending_ = false;
            bus.ppu_.window_disable_source_x_ =
                bus.ppu_.window_disable_pending_
                    ? static_cast<std::uint16_t>(
                          (bus.ppu_.window_source_x_ + 7U) & ~7U)
                    : std::uint16_t{0};
        }
        bus.ppu_.discard_first_fetch_ =
            version >= 11 ? reader.boolean() : false;
        bus.ppu_.fetched_source_y_ =
            version >= 11 ? reader.u8() : bus.ppu_.fetched_row_;
        bus.ppu_.line_sprite_height_ =
            version >= 12 ? reader.u8()
                          : ((bus.ppu_.lcdc_ & 0x04) != 0 ? 16 : 8);
        bus.ppu_.pending_sprite_mask_ = version >= 13 ? reader.u64() : 0;
        bus.ppu_.rendered_sprite_mask_ = version >= 15 ? reader.u64() : 0;
        if (version >= 14) {
            read_bytes(reader, bus.ppu_.pending_sprite_deadlines_);
        } else {
            bus.ppu_.pending_sprite_deadlines_.fill(0);
            for (unsigned index = 0; index < 40; ++index) {
                if ((bus.ppu_.pending_sprite_mask_ &
                     (std::uint64_t{1} << index)) != 0) {
                    bus.ppu_.pending_sprite_deadlines_[index] =
                        bus.ppu_.sprite_delay_;
                }
            }
        }
        if (version >= 15) {
            read_bytes(reader, bus.ppu_.render_sprite_deadlines_);
        } else {
            bus.ppu_.render_sprite_deadlines_.fill(0);
            for (unsigned index = 0; index < 40; ++index) {
                if ((bus.ppu_.pending_sprite_mask_ &
                     (std::uint64_t{1} << index)) != 0) {
                    bus.ppu_.render_sprite_deadlines_[index] =
                        bus.ppu_.pending_sprite_deadlines_[index];
                }
            }
        }
        if (bus.ppu_.background_fifo_size_ >
                bus.ppu_.background_fifo_.size() ||
            bus.ppu_.fetcher_phase_ > 4 ||
            bus.ppu_.fetcher_phase_ticks_ > 2 ||
            bus.ppu_.line_sprite_count_ > bus.ppu_.line_sprites_.size() ||
            bus.ppu_.next_line_sprite_ > bus.ppu_.line_sprite_count_ ||
            bus.ppu_.output_x_ > Ppu::screen_width ||
            bus.ppu_.scroll_discard_ > 7 ||
            bus.ppu_.window_glitch_x_ >= Ppu::screen_width ||
            bus.ppu_.window_glitch_applied_x_ >= Ppu::screen_width ||
            bus.ppu_.window_trigger_x_ >= Ppu::screen_width ||
            bus.ppu_.window_disable_source_x_ > 256) {
            throw SaveStateError("Save state contains invalid PPU fetcher state");
        }
        if (bus.ppu_.line_sprite_height_ != 8 &&
            bus.ppu_.line_sprite_height_ != 16) {
            throw SaveStateError("Save state contains invalid sprite height");
        }
        if ((bus.ppu_.pending_sprite_mask_ >> 40) != 0) {
            throw SaveStateError("Save state contains invalid sprite fetch mask");
        }
        if ((bus.ppu_.rendered_sprite_mask_ >> 40) != 0) {
            throw SaveStateError("Save state contains invalid rendered sprite mask");
        }
        for (unsigned index = 0;
             index < bus.ppu_.pending_sprite_deadlines_.size(); ++index) {
            const auto deadline = bus.ppu_.pending_sprite_deadlines_[index];
            const auto pending =
                (bus.ppu_.pending_sprite_mask_ &
                 (std::uint64_t{1} << index)) != 0;
            const auto render_deadline =
                bus.ppu_.render_sprite_deadlines_[index];
            const auto rendered =
                (bus.ppu_.rendered_sprite_mask_ &
                 (std::uint64_t{1} << index)) != 0;
            if ((pending && deadline == 0) || (!pending && deadline != 0) ||
                (rendered && render_deadline != 0) ||
                (!rendered && render_deadline == 0 && pending)) {
                throw SaveStateError(
                    "Save state contains invalid sprite fetch deadline");
            }
        }
    } else {
        bus.ppu_.background_fifo_.fill(Ppu::BackgroundPixel{});
        bus.ppu_.object_pixels_.fill(Ppu::ObjectPixel{});
        bus.ppu_.line_sprites_.fill(0);
        bus.ppu_.background_fifo_size_ = 0;
        bus.ppu_.fetcher_phase_ = 0;
        bus.ppu_.fetcher_phase_ticks_ = 2;
        bus.ppu_.fetcher_tile_index_ = 0;
        bus.ppu_.fetched_tile_ = 0;
        bus.ppu_.fetched_attributes_ = 0;
        bus.ppu_.fetched_low_ = 0;
        bus.ppu_.fetched_high_ = 0;
        bus.ppu_.fetched_row_ = 0;
        bus.ppu_.fetched_source_y_ = 0;
        bus.ppu_.line_sprite_height_ = 8;
        bus.ppu_.pending_sprite_mask_ = 0;
        bus.ppu_.rendered_sprite_mask_ = 0;
        bus.ppu_.pending_sprite_deadlines_.fill(0);
        bus.ppu_.render_sprite_deadlines_.fill(0);
        bus.ppu_.line_sprite_count_ = 0;
        bus.ppu_.next_line_sprite_ = 0;
        bus.ppu_.output_x_ = 0;
        bus.ppu_.startup_delay_ = 0;
        bus.ppu_.scroll_discard_ = 0;
        bus.ppu_.window_delay_ = 0;
        bus.ppu_.sprite_delay_ = 0;
        bus.ppu_.fetched_source_x_ = 0;
        bus.ppu_.previous_sprite_tile_ = 0;
        bus.ppu_.fetched_window_ = false;
        bus.ppu_.using_window_ = false;
        bus.ppu_.previous_sprite_was_window_ = false;
        bus.ppu_.have_previous_sprite_tile_ = false;
        bus.ppu_.window_glitch_x_ = 0;
        bus.ppu_.window_glitch_pending_ = false;
        bus.ppu_.window_source_x_ = 0;
        bus.ppu_.window_glitch_applied_x_ = 0;
        bus.ppu_.window_glitch_restore_color_ = 0;
        bus.ppu_.window_glitch_applied_ = false;
        bus.ppu_.window_activation_count_ = 0;
        bus.ppu_.window_fetch_line_ = bus.ppu_.window_line_;
        bus.ppu_.window_retrigger_armed_ = false;
        bus.ppu_.window_fetch_start_x_ = 0;
        bus.ppu_.window_disable_pending_ = false;
        bus.ppu_.window_trigger_x_ = 0;
        bus.ppu_.window_trigger_pending_ = false;
        bus.ppu_.window_disable_source_x_ = 0;
        bus.ppu_.discard_first_fetch_ = false;
    }
    if (version >= 16) {
        bus.apu_.sample_integrator_left_ = reader.f32();
        bus.apu_.sample_integrator_right_ = reader.f32();
    }
    if (version >= 17) {
        for (auto& pixel : bus.ppu_.emitted_background_) {
            pixel.color = reader.u8();
            pixel.palette = reader.u8();
            pixel.priority = reader.boolean();
        }
    } else {
        bus.ppu_.emitted_background_.fill(Ppu::BackgroundPixel{});
    }
    if (version >= 18) {
        reader.bytes(bus.ppu_.object_pixel_deadlines_.data(),
                     bus.ppu_.object_pixel_deadlines_.size());
    } else {
        bus.ppu_.object_pixel_deadlines_.fill(0);
    }
    if (version >= 19) {
        bus.apu_.pulse1_.duty = reader.u8();
        bus.apu_.pulse1_.pending_duty = reader.u8();
        bus.apu_.pulse1_.duty_update_pending = reader.boolean();
        bus.apu_.pulse2_.duty = reader.u8();
        bus.apu_.pulse2_.pending_duty = reader.u8();
        bus.apu_.pulse2_.duty_update_pending = reader.boolean();
    }
    if (version >= 20) {
        bus.apu_.pulse1_.sample_suppressed = reader.boolean();
        bus.apu_.pulse2_.sample_suppressed = reader.boolean();
        bus.apu_cycle_phase_ = reader.boolean();
    } else {
        bus.apu_.pulse1_.sample_suppressed = false;
        bus.apu_.pulse2_.sample_suppressed = false;
        bus.apu_cycle_phase_ = false;
    }
    if (version >= 21) {
        bus.apu_.pulse1_.period = reader.u32();
        bus.apu_.pulse1_.just_reloaded = reader.boolean();
        bus.apu_.pulse2_.period = reader.u32();
        bus.apu_.pulse2_.just_reloaded = reader.boolean();
    } else {
        bus.apu_.pulse1_.period = 0;
        bus.apu_.pulse1_.just_reloaded = false;
        bus.apu_.pulse2_.period = 0;
        bus.apu_.pulse2_.just_reloaded = false;
    }
    if (version >= 22) {
        // The SGB block is append-only; version 23 adds the mask and border
        // data after the version 22 packet/palette fields.
        bus.joypad_.sgb_mode_ = reader.boolean();
        bus.joypad_.sgb_ready_for_pulse_ = reader.boolean();
        bus.joypad_.sgb_ready_for_write_ = reader.boolean();
        bus.joypad_.sgb_ready_for_stop_ = reader.boolean();
        bus.joypad_.sgb_bit_count_ = reader.u32();
        read_bytes(reader, bus.joypad_.sgb_command_);
        read_bytes(reader, bus.joypad_.sgb_packet_);
        bus.joypad_.sgb_packet_ready_ = reader.boolean();
        bus.joypad_.sgb_packet_bytes_ = reader.u32();
        if (bus.joypad_.sgb_bit_count_ >
                bus.joypad_.sgb_command_.size() * 8 ||
            bus.joypad_.sgb_packet_bytes_ > bus.joypad_.sgb_packet_.size()) {
            throw SaveStateError("Save state contains invalid SGB joypad state");
        }
        bus.ppu_.sgb_mode_ = reader.boolean();
        for (auto& color : bus.ppu_.sgb_palettes_) color = reader.u16();
        read_bytes(reader, bus.ppu_.sgb_attributes_);
        if (version >= 23) {
            bus.ppu_.sgb_mask_mode_ = reader.u8() & 3U;
            read_bytes(reader, *bus.ppu_.sgb_border_tiles_);
            read_bytes(reader, *bus.ppu_.sgb_border_pct_);
        } else {
            bus.ppu_.sgb_mask_mode_ = 0;
            bus.ppu_.sgb_border_tiles_->fill(0);
            bus.ppu_.sgb_border_pct_->fill(0);
        }
    } else {
        bus.joypad_.sgb_mode_ = false;
        bus.joypad_.reset_sgb_packet();
        bus.joypad_.sgb_packet_.fill(0);
        bus.joypad_.sgb_packet_ready_ = false;
        bus.joypad_.sgb_packet_bytes_ = 0;
        bus.ppu_.sgb_mode_ = false;
        bus.ppu_.sgb_palettes_.fill(0);
        bus.ppu_.sgb_attributes_.fill(0);
        bus.ppu_.sgb_mask_mode_ = 0;
        bus.ppu_.sgb_border_tiles_->fill(0);
        bus.ppu_.sgb_border_pct_->fill(0);
    }
    if (bus.printer_connected_) bus.printer_.reset();
}


} // namespace gameboy
