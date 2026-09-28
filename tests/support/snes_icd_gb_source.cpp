#include "snes_icd_gb_source.hpp"

#include "gameboy/cartridge.hpp"
#include "sgb_input_script.h"

#include <fstream>
#include <string>
#include <stdexcept>

namespace sgb_test {

SnesIcdGbSource::SnesIcdGbSource(const std::filesystem::path& rom,
                                 const std::filesystem::path& boot_rom,
                                 const gameboy::HardwareModel model)
    : gb_(std::make_unique<gameboy::Emulator>(
          gameboy::Cartridge::from_file(rom), model,
          gameboy::BootRomMode::diagnostic)) {
    if (std::filesystem::file_size(boot_rom) != boot_image_.size())
        throw std::runtime_error("GB boot ROM must be exactly 256 bytes");
    std::ifstream input(boot_rom, std::ios::binary);
    if (!input.read(reinterpret_cast<char*>(boot_image_.data()),
                    static_cast<std::streamsize>(boot_image_.size())))
        throw std::runtime_error("could not read complete GB boot ROM");
    gb_->bus().install_boot_rom(boot_image_);
    gb_->bus().debug_enable_io_trace(true);
}

void SnesIcdGbSource::load_input_script(const std::filesystem::path& path) {
    gbb_sgb_input_script script{};
    char error[128]{};
    if (!gbb_sgb_input_load(path.string().c_str(), &script,
                            error, sizeof(error)))
        throw std::runtime_error(std::string{"input script: "} + error);
    input_events_.clear();
    input_events_.reserve(script.count);
    for (std::size_t index = 0; index < script.count; ++index)
        input_events_.push_back({script.events[index].frame,
                                 script.events[index].mask});
    next_input_event_ = 0;
    held_buttons_ = 0;
    apply_input(0);
}

void SnesIcdGbSource::apply_input(const std::uint64_t frame) noexcept {
    if (next_input_event_ >= input_events_.size() ||
        input_events_[next_input_event_].frame != frame)
        return;
    const auto next = input_events_[next_input_event_++].mask;
    constexpr gameboy::Button buttons[] = {
        gameboy::Button::right, gameboy::Button::left,
        gameboy::Button::up, gameboy::Button::down,
        gameboy::Button::a, gameboy::Button::b,
        gameboy::Button::select, gameboy::Button::start};
    for (unsigned bit = 0; bit < 8; ++bit) {
        const auto flag = static_cast<std::uint8_t>(1U << bit);
        if ((held_buttons_ & flag) != (next & flag))
            gb_->set_button(buttons[bit], (next & flag) != 0);
    }
    held_buttons_ = next;
    ++input_events_applied_;
}

void SnesIcdGbSource::complete_packet() noexcept {
    if (queued_.size() >= 32) {
        missing_address_ = 0x7000;
        return;
    }
    if (continuation_packets_ == 0) {
        const auto command = static_cast<std::uint8_t>(building_[0] >> 3);
        const auto encoded = static_cast<unsigned>(building_[0] & 7U);
        continuation_packets_ = (encoded == 0 ? 1U : encoded) - 1U;
        if (command == 0x08) {
            if (sound_commands_ == 0 && audible_sound_substitution_) {
                building_[1] = 0x03;
                building_[2] = 0x80;
                building_[3] = 0x00;
            }
            if (sound_commands_ == 0) first_sound_packet_ = building_;
            ++sound_commands_;
            const auto a = building_[1];
            const auto b = building_[2];
            if ((building_[3] & 0x0CU) != 0x0CU &&
                ((a != 0 && a < 0x80) || (b != 0 && b < 0x80) ||
                 building_[4] != 0)) {
                if (audible_sound_commands_ == 0) {
                    first_audible_sound_packet_ = building_;
                    first_audible_frame_ = completed_frames_;
                }
                ++audible_sound_commands_;
            }
        }
        if (command == 0x09) ++transfer_commands_;
    } else {
        --continuation_packets_;
    }
    queued_.push_back(building_);
    ++packets_completed_;
    building_.fill(0);
    bit_count_ = 0;
    receiving_ = false;
    packet_pending_ = false;
}

void SnesIcdGbSource::joyp_write(const std::uint8_t value) noexcept {
    switch ((value >> 4) & 3U) {
    case 3: pulse_armed_ = true; break;
    case 0:
        if (pulse_armed_) {
            if (packet_pending_) complete_packet();
            receiving_ = true;
            pulse_armed_ = false;
        }
        break;
    case 1: case 2:
        if (!pulse_armed_ || !receiving_) break;
        if (packet_pending_) {
            if (((value >> 4) & 3U) == 2U) complete_packet();
            break;
        }
        if (bit_count_ < 128) {
            if (((value >> 4) & 3U) == 1U)
                building_[bit_count_ / 8] |= static_cast<std::uint8_t>(
                    1U << (bit_count_ & 7U));
            ++bit_count_;
            packet_pending_ = bit_count_ == 128;
            pulse_armed_ = false;
        }
        break;
    }
}

void SnesIcdGbSource::complete_tile_row(const unsigned tile_row) noexcept {
    if (tile_row >= 18) return;
    auto& row = rows_[tile_row & 3U];
    for (unsigned tile = 0; tile < 20; ++tile) {
        for (unsigned line = 0; line < 8; ++line) {
            std::uint8_t low{};
            std::uint8_t high{};
            for (unsigned x = 0; x < 8; ++x) {
                const auto pixel = gb_->bus().debug_sgb_source_pixel(
                    tile * 8 + x, tile_row * 8 + line);
                low |= static_cast<std::uint8_t>((pixel & 1U) << (7U - x));
                high |= static_cast<std::uint8_t>(((pixel >> 1) & 1U) << (7U - x));
            }
            row[tile * 16 + line * 2] = low;
            row[tile * 16 + line * 2 + 1] = high;
        }
    }
    row_valid_[tile_row & 3U] = true;
}

void SnesIcdGbSource::synchronize(const std::uint64_t master_clocks) noexcept {
    if (!released_ || master_clocks < release_clock_) return;
    const auto target = (master_clocks - release_clock_) / divider_;
    while (gb_cycles_ < target && missing_address_ == 0) {
        gb_cycles_ += gb_->step();
        if (gb_->frame_ready()) {
            ++completed_frames_;
            gb_->consume_frame();
            apply_input(completed_frames_);
        }
        const auto ly = gb_->bus().read8(0xFF44);
        if (ly != last_ly_) {
            if (ly <= 144 && ly != 0 && (ly & 7U) == 0)
                complete_tile_row(ly / 8U - 1U);
            last_ly_ = ly;
        }
        for (const auto& event : gb_->bus().debug_take_io_trace()) {
            if (event.address == 0xFF00) joyp_write(event.value);
        }
    }
}

bool SnesIcdGbSource::read(const std::uint16_t address,
                           const std::uint64_t master_clocks,
                           std::uint8_t& value) noexcept {
    synchronize(master_clocks);
    if (missing_address_ != 0) return false;
    switch (address) {
    case 0x6000: {
        const auto ly = gb_->bus().read8(0xFF44);
        value = static_cast<std::uint8_t>(
            ((ly >= 144 ? 0x11U : ly / 8U) << 3) | ((ly / 8U) & 3U));
        return true;
    }
    case 0x6002:
        value = queued_.empty() ? 0 : 1;
        return true;
    default:
        if (address >= 0x7800 && address <= 0x780F) {
            if (!row_valid_[selected_row_]) break;
            value = row_stream_offset_ < 320
                ? rows_[selected_row_][row_stream_offset_] : 0xFF;
            row_stream_offset_ = (row_stream_offset_ + 1U) & 511U;
            return true;
        }
        if (address >= 0x7000 && address <= 0x700F) {
            if (address == 0x7000) {
                if (queued_.empty()) break;
                latched_ = queued_.front();
                queued_.pop_front();
                ++packets_delivered_;
                if ((latched_[0] >> 3) == 0x08)
                    ++sound_packets_delivered_;
            }
            value = latched_[address & 0xF];
            return true;
        }
        break;
    }
    missing_address_ = address;
    return false;
}

bool SnesIcdGbSource::write(const std::uint16_t address,
                            const std::uint64_t master_clocks,
                            const std::uint8_t value) noexcept {
    if (address == 0x6003) {
        ++control_writes_;
        last_control_ = value;
        const bool run = (value & 0x80U) != 0;
        if (!run) {
            released_ = false;
            gb_->reset();
            gb_->bus().install_boot_rom(boot_image_);
            gb_->bus().debug_enable_io_trace(true);
            gb_cycles_ = 0;
            completed_frames_ = 0;
            next_input_event_ = 0;
            held_buttons_ = 0;
            apply_input(0);
            last_ly_ = gb_->bus().read8(0xFF44);
            row_valid_.fill(false);
            row_stream_offset_ = 0;
            queued_.clear();
            bit_count_ = 0;
            building_.fill(0);
            packet_pending_ = false;
            continuation_packets_ = 0;
            receiving_ = false;
            pulse_armed_ = true;
        } else if (!released_) {
            released_ = true;
            release_clock_ = master_clocks;
        }
        constexpr unsigned dividers[] = {4, 5, 7, 9};
        divider_ = dividers[value & 3U];
        return true;
    }
    synchronize(master_clocks);
    if (address == 0x6001) {
        selected_row_ = value & 3U;
        row_stream_offset_ = 0;
        return true;
    }
    if (address >= 0x6004 && address <= 0x6007) {
        constexpr gameboy::Button buttons[] = {
            gameboy::Button::right, gameboy::Button::left,
            gameboy::Button::up, gameboy::Button::down,
            gameboy::Button::a, gameboy::Button::b,
            gameboy::Button::select, gameboy::Button::start};
        for (unsigned bit = 0; bit < 8; ++bit)
            gb_->set_player_button(static_cast<std::uint8_t>(address - 0x6004),
                                   buttons[bit], (value & (1U << bit)) == 0);
        return true;
    }
    missing_address_ = address;
    return false;
}

} // namespace sgb_test
