#include "gameboy/joypad.hpp"

namespace gameboy {

std::uint8_t Joypad::read() const noexcept {
    if (sgb_mode_ && sgb_player_count_ > 1 && select_ == 0x30) {
        return static_cast<std::uint8_t>(0xC0 | select_ |
                                         (0x0F - sgb_current_player_));
    }
    return static_cast<std::uint8_t>(0xC0 | select_ | input_lines());
}

bool Joypad::write(const std::uint8_t value) noexcept {
    const auto old_lines = input_lines();
    const auto old_select = select_;
    if (sgb_mode_) {
        // The SGB increments its active controller when P15 transitions from
        // low to high. P14 is independent and may be used to read either the
        // d-pad or action buttons for the newly selected controller.
        if (sgb_player_count_ > 1 && (value & 0x20U) != 0 &&
            (old_select & 0x20U) == 0) {
            sgb_current_player_ = static_cast<std::uint8_t>(
                (sgb_current_player_ + 1) & (sgb_player_count_ - 1));
        }
        select_ = static_cast<std::uint8_t>(value & 0x30);
        process_sgb_write(value);
    } else select_ = static_cast<std::uint8_t>(value & 0x30);
    const auto new_lines = input_lines();
    return (old_lines & static_cast<std::uint8_t>(~new_lines) & 0x0F) != 0;
}

bool Joypad::set_button(const Button button, const bool pressed) noexcept {
    const auto old_lines = input_lines();
    auto* group = &directions_;
    unsigned bit = 0;
    switch (button) {
    case Button::right: bit = 0; break;
    case Button::left: bit = 1; break;
    case Button::up: bit = 2; break;
    case Button::down: bit = 3; break;
    case Button::a: group = &actions_; bit = 0; break;
    case Button::b: group = &actions_; bit = 1; break;
    case Button::select: group = &actions_; bit = 2; break;
    case Button::start: group = &actions_; bit = 3; break;
    }

    const auto mask = static_cast<std::uint8_t>(1U << bit);
    if (pressed) {
        *group = static_cast<std::uint8_t>(*group | mask);
    } else {
        *group = static_cast<std::uint8_t>(*group & ~mask);
    }
    const auto new_lines = input_lines();
    return (old_lines & static_cast<std::uint8_t>(~new_lines) & 0x0F) != 0;
}

std::uint8_t Joypad::input_lines() const noexcept {
    auto lines = std::uint8_t{0x0F};
    if ((select_ & 0x10) == 0) {
        lines = static_cast<std::uint8_t>(lines & ~directions_);
    }
    if ((select_ & 0x20) == 0) {
        lines = static_cast<std::uint8_t>(lines & ~actions_);
    }
    return static_cast<std::uint8_t>(lines & 0x0F);
}

void Joypad::set_sgb_mode(const bool enabled) noexcept {
    if (sgb_mode_ != enabled) reset_sgb_packet();
    sgb_mode_ = enabled;
    if (!enabled) {
        sgb_player_count_ = 1;
        sgb_current_player_ = 0;
    } else {
        // The ICD2 is idle-high when the SGB link is enabled.  The first
        // command therefore begins with the game's 00 start write; there is
        // no preceding 30 pulse to arm the parser as there is between later
        // packets.  SameBoy models this initial armed state as well.
        sgb_ready_for_pulse_ = true;
    }
}

void Joypad::apply_sgb_command(
    const std::array<std::uint8_t, sgb_packet_size * sgb_max_packets>& packet,
    const std::size_t size) noexcept {
    if (!sgb_mode_ || size < 2 || (packet[0] >> 3) != 0x11) return;
    switch (packet[1] & 3U) {
    case 0: sgb_player_count_ = 1; break;
    case 1: sgb_player_count_ = 2; break;
    case 3: sgb_player_count_ = 4; break;
    default: sgb_player_count_ = 1; break;
    }
    sgb_current_player_ = static_cast<std::uint8_t>(
        sgb_current_player_ & (sgb_player_count_ - 1));
}

bool Joypad::take_sgb_packet(
    std::array<std::uint8_t, sgb_packet_size * sgb_max_packets>& packet,
    std::size_t& size) noexcept {
    if (!sgb_packet_ready_) return false;
    packet = sgb_packet_;
    size = sgb_packet_bytes_;
    sgb_packet_ready_ = false;
    sgb_packet_bytes_ = 0;
    return true;
}

std::size_t Joypad::sgb_command_bits() const noexcept {
    if (sgb_bit_count_ < 8) return sgb_packet_size * 8;
    const auto command = sgb_command_[0];
    if ((command & 0xF1) == 0xF1) return sgb_packet_size * 8;
    const auto packets = static_cast<std::size_t>(command & 0x07);
    return (packets == 0 ? 1 : packets) * sgb_packet_size * 8;
}

void Joypad::reset_sgb_packet() noexcept {
    sgb_ready_for_pulse_ = false;
    sgb_ready_for_write_ = false;
    sgb_ready_for_stop_ = false;
    sgb_bit_count_ = 0;
    sgb_command_.fill(0);
}

void Joypad::process_sgb_write(const std::uint8_t value) noexcept {
    // SGB command transfer uses JOYP bits 4/5 as a two-wire protocol:
    // 11 arms a pulse, 10 clocks a zero, 01 clocks a one, and 00 starts or
    // resets a packet transfer. This follows the ICD2 state machine used by
    // SGB-compatible games while leaving ordinary joypad reads unchanged.
    switch ((value >> 4) & 0x03U) {
    case 3: // arm pulse
        sgb_ready_for_pulse_ = true;
        break;
    case 2: // zero
        if (!sgb_ready_for_pulse_ || !sgb_ready_for_write_) return;
        if (sgb_ready_for_stop_) {
            if (sgb_bit_count_ == sgb_command_bits()) {
                sgb_packet_ = sgb_command_;
                sgb_packet_ready_ = true;
                sgb_packet_bytes_ = sgb_bit_count_ / 8;
            }
            sgb_ready_for_pulse_ = false;
            sgb_ready_for_write_ = false;
            sgb_ready_for_stop_ = false;
            if (sgb_packet_ready_) reset_sgb_packet();
            return;
        }
        if (sgb_bit_count_ < sgb_command_.size() * 8) {
            ++sgb_bit_count_;
            sgb_ready_for_pulse_ = false;
            if ((sgb_bit_count_ % (sgb_packet_size * 8)) == 0) {
                sgb_ready_for_stop_ = true;
            }
        }
        break;
    case 1: // one
        if (!sgb_ready_for_pulse_ || !sgb_ready_for_write_) return;
        if (sgb_ready_for_stop_) {
            // A stop pulse must be followed by the zero delimiter. A one at
            // this point indicates a malformed transfer and drops the packet.
            reset_sgb_packet();
            return;
        }
        if (sgb_bit_count_ < sgb_command_.size() * 8) {
            sgb_command_[sgb_bit_count_ / 8] = static_cast<std::uint8_t>(
                sgb_command_[sgb_bit_count_ / 8] |
                (1U << (sgb_bit_count_ & 7U)));
            ++sgb_bit_count_;
            sgb_ready_for_pulse_ = false;
            if ((sgb_bit_count_ % (sgb_packet_size * 8)) == 0) {
                sgb_ready_for_stop_ = true;
            }
        }
        break;
    case 0: // start / delimiter
        if (!sgb_ready_for_pulse_) return;
        sgb_ready_for_write_ = true;
        sgb_ready_for_pulse_ = false;
        if ((sgb_bit_count_ % (sgb_packet_size * 8)) != 0 ||
            sgb_bit_count_ == 0 || sgb_ready_for_stop_) {
            sgb_bit_count_ = 0;
            sgb_command_.fill(0);
            sgb_ready_for_stop_ = false;
        }
        break;
    default: break;
    }
}

} // namespace gameboy
