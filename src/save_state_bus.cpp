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
void write_bytes(save_state_format::Writer& writer,
                 const std::array<std::uint8_t, Size>& values) {
    writer.bytes(values.data(), values.size());
}

} // namespace

void SaveStateBusCodec::write(save_state_format::Writer& writer,
                              const MemoryBus& bus) {
    SaveStateCartridgeCodec::write(writer, bus.cartridge_);
    write_bytes(writer, bus.wram_);
    write_bytes(writer, bus.io_);
    write_bytes(writer, bus.hram_);
    writer.u8(bus.interrupt_enable_);
    SaveStateJoypadCodec::write(writer, bus.joypad_);
    SaveStateApuCodec::write(writer, bus.apu_);
    SaveStatePpuCodec::write(writer, bus.ppu_);
    SaveStateTimerCodec::write(writer, bus.timer_);
    writer.string(bus.serial_output_);
    writer.u32(bus.serial_cycles_remaining_);
    writer.boolean(bus.oam_dma_active_);
    writer.u8(static_cast<std::uint8_t>(bus.oam_dma_source_ >> 8));
    writer.u16(bus.oam_dma_index_);
    writer.u8(static_cast<std::uint8_t>(bus.oam_dma_cycle_));
    writer.u8(static_cast<std::uint8_t>(bus.oam_dma_pending_source_ >> 8));
    writer.u8(static_cast<std::uint8_t>(bus.oam_dma_start_delay_));
    writer.u32(bus.ppu_.mode3_end_dot_);
    writer.u8(bus.ppu_.window_line_);
    writer.boolean(bus.ppu_.window_y_triggered_);
    writer.boolean(bus.ppu_.coincidence_);
    writer.u8(bus.ppu_.stat_mode_);
    writer.boolean(bus.ppu_.lcd_startup_);
    writer.boolean(bus.timer_.reload_happened_);
    writer.u16(bus.serial_clock_);
    write_bytes(writer, *bus.cgb_wram_);
    write_bytes(writer, *bus.ppu_.cgb_vram_);
    write_bytes(writer, bus.ppu_.cgb_bg_palette_);
    write_bytes(writer, bus.ppu_.cgb_object_palette_);
    writer.u8(bus.wram_bank_);
    writer.u8(bus.ppu_.vram_bank_);
    writer.u8(bus.ppu_.bg_palette_index_);
    writer.u8(bus.ppu_.object_palette_index_);
    writer.u16(bus.hdma_source_);
    writer.u16(bus.hdma_destination_);
    writer.u8(bus.hdma_blocks_remaining_);
    writer.boolean(bus.hdma_active_);
    writer.boolean(bus.double_speed_);
    writer.boolean(bus.speed_switch_requested_);
    writer.boolean(bus.cartridge_.has_camera());
    if (bus.cartridge_.has_camera()) {
        writer.boolean(bus.cartridge_.camera_registers_mapped_);
        write_bytes(writer, bus.cartridge_.camera_registers_);
        writer.bytes(bus.cartridge_.camera_frame_.data(),
                     bus.cartridge_.camera_frame_.size());
        writer.bytes(bus.cartridge_.camera_image_.data(),
                     bus.cartridge_.camera_image_.size());
    }
    writer.boolean(bus.ppu_.window_rendered_this_line_);
    writer.u8(bus.ppu_.background_fifo_size_);
    for (const auto& pixel : bus.ppu_.background_fifo_) {
        writer.u8(pixel.color);
        writer.u8(pixel.palette);
        writer.boolean(pixel.priority);
    }
    for (const auto& pixel : bus.ppu_.object_pixels_) {
        writer.u8(pixel.color);
        writer.u8(pixel.attributes);
        writer.u8(pixel.oam_index);
        writer.boolean(pixel.valid);
    }
    write_bytes(writer, bus.ppu_.line_sprites_);
    writer.u8(bus.ppu_.fetcher_phase_);
    writer.u8(bus.ppu_.fetcher_phase_ticks_);
    writer.u8(bus.ppu_.fetcher_tile_index_);
    writer.u8(bus.ppu_.fetched_tile_);
    writer.u8(bus.ppu_.fetched_attributes_);
    writer.u8(bus.ppu_.fetched_low_);
    writer.u8(bus.ppu_.fetched_high_);
    writer.u8(bus.ppu_.fetched_row_);
    writer.u8(bus.ppu_.line_sprite_count_);
    writer.u8(bus.ppu_.next_line_sprite_);
    writer.u8(bus.ppu_.output_x_);
    writer.u8(bus.ppu_.startup_delay_);
    writer.u8(bus.ppu_.scroll_discard_);
    writer.u8(bus.ppu_.window_delay_);
    writer.u8(bus.ppu_.sprite_delay_);
    writer.u16(static_cast<std::uint16_t>(bus.ppu_.fetched_source_x_));
    writer.u16(static_cast<std::uint16_t>(bus.ppu_.previous_sprite_tile_));
    writer.boolean(bus.ppu_.fetched_window_);
    writer.boolean(bus.ppu_.using_window_);
    writer.boolean(bus.ppu_.previous_sprite_was_window_);
    writer.boolean(bus.ppu_.have_previous_sprite_tile_);
    writer.u8(bus.ppu_.window_glitch_x_);
    writer.boolean(bus.ppu_.window_glitch_pending_);
    writer.u16(bus.ppu_.window_source_x_);
    writer.u8(bus.ppu_.window_glitch_applied_x_);
    writer.u32(bus.ppu_.window_glitch_restore_color_);
    writer.boolean(bus.ppu_.window_glitch_applied_);
    writer.u8(bus.ppu_.window_activation_count_);
    writer.u8(bus.ppu_.window_fetch_line_);
    writer.boolean(bus.ppu_.window_retrigger_armed_);
    writer.u8(bus.ppu_.window_fetch_start_x_);
    writer.boolean(bus.ppu_.window_disable_pending_);
    writer.u8(bus.ppu_.window_trigger_x_);
    writer.boolean(bus.ppu_.window_trigger_pending_);
    writer.u16(bus.ppu_.window_disable_source_x_);
    writer.boolean(bus.ppu_.discard_first_fetch_);
    writer.u8(bus.ppu_.fetched_source_y_);
    writer.u8(bus.ppu_.line_sprite_height_);
    writer.u64(bus.ppu_.pending_sprite_mask_);
    writer.u64(bus.ppu_.rendered_sprite_mask_);
    write_bytes(writer, bus.ppu_.pending_sprite_deadlines_);
    write_bytes(writer, bus.ppu_.render_sprite_deadlines_);
    writer.f32(bus.apu_.sample_integrator_left_);
    writer.f32(bus.apu_.sample_integrator_right_);
    for (const auto& pixel : bus.ppu_.emitted_background_) {
        writer.u8(pixel.color);
        writer.u8(pixel.palette);
        writer.boolean(pixel.priority);
    }
    writer.bytes(bus.ppu_.object_pixel_deadlines_.data(),
                 bus.ppu_.object_pixel_deadlines_.size());
    writer.u8(bus.apu_.pulse1_.duty);
    writer.u8(bus.apu_.pulse1_.pending_duty);
    writer.boolean(bus.apu_.pulse1_.duty_update_pending);
    writer.u8(bus.apu_.pulse2_.duty);
    writer.u8(bus.apu_.pulse2_.pending_duty);
    writer.boolean(bus.apu_.pulse2_.duty_update_pending);
    writer.boolean(bus.apu_.pulse1_.sample_suppressed);
    writer.boolean(bus.apu_.pulse2_.sample_suppressed);
    writer.boolean(bus.apu_cycle_phase_);
    writer.u32(bus.apu_.pulse1_.period);
    writer.boolean(bus.apu_.pulse1_.just_reloaded);
    writer.u32(bus.apu_.pulse2_.period);
    writer.boolean(bus.apu_.pulse2_.just_reloaded);
    // SGB state is appended as trailing blocks so older payload offsets stay
    // stable as new transfer and border state is introduced.
    writer.boolean(bus.joypad_.sgb_mode_);
    writer.boolean(bus.joypad_.sgb_ready_for_pulse_);
    writer.boolean(bus.joypad_.sgb_ready_for_write_);
    writer.boolean(bus.joypad_.sgb_ready_for_stop_);
    writer.u32(static_cast<std::uint32_t>(bus.joypad_.sgb_bit_count_));
    write_bytes(writer, bus.joypad_.sgb_command_);
    write_bytes(writer, bus.joypad_.sgb_packet_);
    writer.boolean(bus.joypad_.sgb_packet_ready_);
    writer.u32(static_cast<std::uint32_t>(bus.joypad_.sgb_packet_bytes_));
    writer.boolean(bus.ppu_.sgb_mode_);
    for (const auto color : bus.ppu_.sgb_palettes_) writer.u16(color);
    write_bytes(writer, bus.ppu_.sgb_attributes_);
    writer.u8(bus.ppu_.sgb_mask_mode_);
    write_bytes(writer, *bus.ppu_.sgb_border_tiles_);
    write_bytes(writer, *bus.ppu_.sgb_border_pct_);
}

} // namespace gameboy
