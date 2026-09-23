#include "gameboy/sgb_adapter.hpp"

#include <algorithm>

namespace gameboy {

void SgbAdapter::set_enabled(const bool enabled) noexcept {
    if (enabled_ != enabled) reset_packet();
    enabled_ = enabled;
    if (!enabled_) {
        player_count_ = 1;
        current_player_ = 0;
        diagnostics_ = {};
        diagnostics_.enabled = false;
        diagnostics_.player_count = 1;
        command_history_.fill(CommandRecord{});
        command_history_size_ = 0;
        command_history_next_ = 0;
        return;
    }
    // The ICD2 is idle-high when the SGB link is enabled. The first command
    // begins with the game's direct 00 start write.
    ready_for_pulse_ = true;
    diagnostics_.enabled = true;
    diagnostics_.player_count = player_count_;
    diagnostics_.current_player = current_player_;
}

std::uint8_t SgbAdapter::read_joypad(const Joypad& joypad) const noexcept {
    if (!enabled_) return joypad.read();
    return joypad.read_sgb(player_count_, current_player_);
}

bool SgbAdapter::write_joypad(const std::uint8_t value, Joypad& joypad,
                              Ppu& ppu) noexcept {
    if (!enabled_) return joypad.write(value);

    const auto old_select = joypad.select_lines();
    if (player_count_ > 1 && (value & 0x20U) != 0 &&
        (old_select & 0x20U) == 0) {
        current_player_ = static_cast<std::uint8_t>(
            (current_player_ + 1) & (player_count_ - 1));
    }
    const auto interrupt = joypad.write(value);
    process_write(value);
    if (packet_ready_) {
        apply_command(packet_, packet_bytes_, ppu);
        packet_ready_ = false;
        packet_bytes_ = 0;
    }
    diagnostics_.player_count = player_count_;
    diagnostics_.current_player = current_player_;
    return interrupt;
}

void SgbAdapter::reset_diagnostics() noexcept {
    diagnostics_ = {};
    diagnostics_.enabled = enabled_;
    diagnostics_.player_count = player_count_;
    diagnostics_.current_player = current_player_;
    command_history_.fill(CommandRecord{});
    command_history_size_ = 0;
    command_history_next_ = 0;
}

std::size_t SgbAdapter::command_bits() const noexcept {
    if (bit_count_ < 8) return Joypad::sgb_packet_size * 8;
    const auto command = command_[0];
    if ((command & 0xF1) == 0xF1) return Joypad::sgb_packet_size * 8;
    const auto packets = static_cast<std::size_t>(command & 0x07);
    return (packets == 0 ? 1 : packets) * Joypad::sgb_packet_size * 8;
}

void SgbAdapter::process_write(const std::uint8_t value) noexcept {
    // JOYP bits 4/5 form the clean-room model's two-wire SGB command link:
    // 11 arms a pulse, 10 clocks zero, 01 clocks one, and 00 starts or
    // terminates a packet transfer.
    switch ((value >> 4) & 0x03U) {
    case 3:
        ready_for_pulse_ = true;
        break;
    case 2:
        if (!ready_for_pulse_ || !ready_for_write_) return;
        if (ready_for_stop_) {
            if (bit_count_ == command_bits()) {
                packet_ = command_;
                packet_ready_ = true;
                packet_bytes_ = bit_count_ / 8;
                ++diagnostics_.packets_completed;
            } else {
                ++diagnostics_.malformed_packets;
            }
            ready_for_pulse_ = false;
            ready_for_write_ = false;
            ready_for_stop_ = false;
            if (packet_ready_) reset_packet();
            return;
        }
        if (bit_count_ < command_.size() * 8) {
            ++bit_count_;
            ready_for_pulse_ = false;
            if ((bit_count_ % (Joypad::sgb_packet_size * 8)) == 0) {
                ready_for_stop_ = true;
            }
        }
        break;
    case 1:
        if (!ready_for_pulse_ || !ready_for_write_) return;
        if (ready_for_stop_) {
            ++diagnostics_.malformed_packets;
            reset_packet();
            return;
        }
        if (bit_count_ < command_.size() * 8) {
            command_[bit_count_ / 8] = static_cast<std::uint8_t>(
                command_[bit_count_ / 8] |
                (1U << (bit_count_ & 7U)));
            ++bit_count_;
            ready_for_pulse_ = false;
            if ((bit_count_ % (Joypad::sgb_packet_size * 8)) == 0) {
                ready_for_stop_ = true;
            }
        }
        break;
    case 0:
        if (!ready_for_pulse_) return;
        ready_for_write_ = true;
        ready_for_pulse_ = false;
        if ((bit_count_ % (Joypad::sgb_packet_size * 8)) != 0 ||
            bit_count_ == 0 || ready_for_stop_) {
            bit_count_ = 0;
            command_.fill(0);
            ready_for_stop_ = false;
        }
        break;
    default: break;
    }
}

void SgbAdapter::apply_command(const Packet& packet, const std::size_t size,
                                Ppu& ppu) noexcept {
    if (!enabled_ || size < Joypad::sgb_packet_size ||
        size > packet.size() || (size % Joypad::sgb_packet_size) != 0) {
        if (enabled_) ++diagnostics_.malformed_packets;
        return;
    }
    const auto encoded_packets = static_cast<std::size_t>(packet[0] & 0x07U);
    const auto expected_packets = encoded_packets == 0 ? 1 : encoded_packets;
    if (expected_packets * Joypad::sgb_packet_size != size) {
        ++diagnostics_.malformed_packets;
        return;
    }
    const auto command = static_cast<std::uint8_t>(packet[0] >> 3);
    diagnostics_.last_command = command;
    diagnostics_.last_packet_bytes = static_cast<std::uint8_t>(size);
    ++diagnostics_.commands_applied;
    auto& record = command_history_[command_history_next_];
    record.sequence = diagnostics_.commands_applied;
    record.command = command;
    record.packet_bytes = static_cast<std::uint8_t>(size);
    record.packet = packet;
    command_history_next_ =
        (command_history_next_ + 1) % command_history_capacity;
    command_history_size_ =
        std::min(command_history_size_ + 1, command_history_capacity);

    if (command == 0x11) {
        switch (packet[1] & 3U) {
        case 0: player_count_ = 1; break;
        case 1: player_count_ = 2; break;
        case 3: player_count_ = 4; break;
        default: player_count_ = 1; break;
        }
        current_player_ = static_cast<std::uint8_t>(
            current_player_ & (player_count_ - 1));
    }
    ppu.apply_sgb_command(packet, size);
    diagnostics_.player_count = player_count_;
    diagnostics_.current_player = current_player_;
}

void SgbAdapter::reset_packet() noexcept {
    ready_for_pulse_ = false;
    ready_for_write_ = false;
    ready_for_stop_ = false;
    bit_count_ = 0;
    command_.fill(0);
}

} // namespace gameboy
