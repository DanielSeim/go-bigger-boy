#include "gameboy/serial.hpp"

namespace gameboy {

std::uint8_t SerialPort::read_control() const noexcept {
    return static_cast<std::uint8_t>((cgb_mode_ ? 0x7C : 0x7E) |
                                     (control_ & (cgb_mode_ ? 0x83 : 0x81)));
}

void SerialPort::write_control(const std::uint8_t value) noexcept {
    control_ = static_cast<std::uint8_t>(value & (cgb_mode_ ? 0x83 : 0x81));
    if ((control_ & 0x80) == 0) {
        active_ = false;
        bits_shifted_ = 0;
        phase_ = 0;
        return;
    }

    active_ = true;
    transfer_byte_ = data_;
    internal_clock_ = (control_ & 0x01) != 0;
    fast_clock_ = cgb_mode_ && (control_ & 0x02) != 0;
    bits_shifted_ = 0;
    phase_ = 0;
}

void SerialPort::initialize_post_boot(const HardwareModel /*model*/) noexcept {
    data_ = 0xFF;
    control_ = 0;
    phase_ = 0;
    bits_shifted_ = 0;
    active_ = false;
    internal_clock_ = false;
    fast_clock_ = false;
}

unsigned SerialPort::cycles_per_bit() const noexcept {
    // DMG serial transfers run at 8192 Hz (512 CPU clocks per bit). CGB fast
    // mode runs at 262144 Hz (16 clocks per bit at normal CPU speed).
    return fast_clock_ ? 16U : 512U;
}

void SerialPort::tick(const unsigned cycles) noexcept {
    if (!active_ || !internal_clock_) return;

    phase_ += cycles;
    const auto bit_cycles = cycles_per_bit();
    while (active_ && phase_ >= bit_cycles) {
        phase_ -= bit_cycles;
        clock_internal_bit();
    }
}

void SerialPort::clock_internal_bit() noexcept {
    const auto outgoing = (data_ & 0x80U) != 0;
    const auto incoming = endpoint_ == nullptr
                              ? true
                              : endpoint_->exchange_bit(outgoing);
    shift_bit(incoming);
}

bool SerialPort::clock_external_bit(const bool incoming) noexcept {
    if (!active_ || internal_clock_) return true;
    const auto outgoing = (data_ & 0x80U) != 0;
    shift_bit(incoming);
    return outgoing;
}

void SerialPort::shift_bit(const bool incoming) noexcept {
    data_ = static_cast<std::uint8_t>((data_ << 1) | (incoming ? 1U : 0U));
    ++bits_shifted_;
    if (bits_shifted_ != 8) return;

    active_ = false;
    control_ = static_cast<std::uint8_t>(control_ & ~0x80U);
    if (completion_callback_ != nullptr) {
        completion_callback_(callback_context_, transfer_byte_, data_);
    }
}

void SerialPort::restore_state(const std::uint8_t data,
                               const std::uint8_t control,
                               const std::uint32_t phase,
                               const std::uint8_t bits_shifted,
                               const bool active,
                               const bool internal_clock,
                               const bool fast_clock) noexcept {
    data_ = data;
    control_ = static_cast<std::uint8_t>(control & (cgb_mode_ ? 0x83 : 0x81));
    bits_shifted_ = bits_shifted > 7 ? 0 : bits_shifted;
    internal_clock_ = internal_clock;
    fast_clock_ = cgb_mode_ && fast_clock;
    phase_ = phase % cycles_per_bit();
    active_ = active && (control_ & 0x80) != 0;
    transfer_byte_ = data_;
}

bool SerialCable::Endpoint::exchange_bit(const bool outgoing) noexcept {
    return peer_ == nullptr ? true : peer_->clock_external_bit(outgoing);
}

void SerialCable::connect(SerialPort& first, SerialPort& second) noexcept {
    disconnect();
    first_ = &first;
    second_ = &second;
    first_endpoint_.set_peer(second_);
    second_endpoint_.set_peer(first_);
    first_->set_endpoint(&first_endpoint_);
    second_->set_endpoint(&second_endpoint_);
}

void SerialCable::disconnect() noexcept {
    if (first_ != nullptr) first_->set_endpoint(nullptr);
    if (second_ != nullptr) second_->set_endpoint(nullptr);
    first_endpoint_.set_peer(nullptr);
    second_endpoint_.set_peer(nullptr);
    first_ = nullptr;
    second_ = nullptr;
}

} // namespace gameboy
