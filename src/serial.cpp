#include "gameboy/serial.hpp"

namespace gameboy {

void SerialPort::set_endpoint(SerialEndpoint* endpoint) noexcept {
    if (endpoint_ != nullptr && active_ && internal_clock_) {
        endpoint_->release_internal_clock(*this);
    }
    endpoint_ = endpoint;
}

std::uint8_t SerialPort::read_control() const noexcept {
    return static_cast<std::uint8_t>((cgb_mode_ ? 0x7C : 0x7E) |
                                     (control_ & (cgb_mode_ ? 0x83 : 0x81)));
}

void SerialPort::write_control(const std::uint8_t value) noexcept {
    const auto masked = static_cast<std::uint8_t>(
        value & (cgb_mode_ ? 0x83 : 0x81));
    const auto was_active = active_;
    const auto was_internal = internal_clock_;
    const auto preserve_transfer = was_active && (masked & 0x80U) != 0 &&
                                   endpoint_ != nullptr &&
                                   endpoint_->preserve_active_transfer();
    const auto new_internal = (masked & 0x01U) != 0;

    // Re-arming SC while a linked byte is still in flight is common in the
    // Cable Club connection loop.  Changing from internal to external still
    // releases clock ownership, but writing the same mode must not restart
    // the pending TCP bit exchange.
    if (endpoint_ != nullptr && was_active && was_internal &&
        (!preserve_transfer || !new_internal || (masked & 0x80U) == 0)) {
        endpoint_->release_internal_clock(*this);
    }
    control_ = masked;
    if (endpoint_ != nullptr && !preserve_transfer) {
        // The cable owns startup edge pacing. Begin a linked probe from a
        // clean bit boundary; peer_ready() will hold that first edge until
        // the other port arms its receiver.
        phase_ = 0;
    }
    if ((control_ & 0x80) == 0) {
        active_ = false;
        bits_shifted_ = 0;
        phase_ = 0;
        return;
    }

    internal_clock_ = new_internal;
    if (!internal_clock_ && !preserve_transfer) external_data_ = data_;
    const auto retain_clock_owner = preserve_transfer && was_internal &&
                                    new_internal;
    if (internal_clock_ && endpoint_ != nullptr && !retain_clock_owner &&
        !endpoint_->request_internal_clock(*this)) {
        // If both consoles request the clock on the same emulated cycle, keep
        // one deterministic master. The losing side remains an external
        // receiver and restores the byte it placed on SB for the preceding
        // external probe, matching the hardware race that established the
        // connection in the first place.
        internal_clock_ = false;
        control_ = static_cast<std::uint8_t>(control_ & ~0x01U);
        // A simultaneous initial probe has not shifted anything yet, so the
        // losing internal request must restore the byte it placed on SB.
        // Once a linked edge has arrived, however, restoring would discard a
        // partially shifted byte and make the TCP handshake oscillate.
        if (!preserve_transfer || bits_shifted_ == 0) data_ = external_data_;
    }
    active_ = true;
    fast_clock_ = cgb_mode_ && (control_ & 0x02) != 0;
    if (!preserve_transfer) {
        transfer_byte_ = data_;
        bits_shifted_ = 0;
    }
}

void SerialPort::initialize_post_boot(const HardwareModel model) noexcept {
    // The boot ROM leaves SB cleared. Keep the serial divider phase from the
    // handoff as well; the DMG/MGB boot ROMs reach the cartridge 460 clocks
    // into a serial bit, which is observable when the first transfer starts.
    data_ = 0x00;
    external_data_ = data_;
    transfer_byte_ = data_;
    control_ = 0;
    phase_ = (model == HardwareModel::dmg || model == HardwareModel::mgb)
                 ? 460U
                 : 0U;
    bits_shifted_ = 0;
    active_ = false;
    internal_clock_ = false;
    fast_clock_ = false;
}

void SerialPort::reset_diagnostics() noexcept {
    transfers_completed_ = 0;
    last_transmitted_ = 0xFF;
    last_received_ = 0xFF;
}

void SerialPort::reset_link() noexcept {
    if (endpoint_ != nullptr) {
        if (active_ && internal_clock_) endpoint_->release_internal_clock(*this);
        endpoint_->cancel_internal_clock(*this);
    }
    control_ = 0;
    phase_ = 0;
    bits_shifted_ = 0;
    active_ = false;
    internal_clock_ = false;
    fast_clock_ = false;
    external_data_ = data_;
    transfer_byte_ = data_;
}

unsigned SerialPort::cycles_per_bit() const noexcept {
    // DMG serial transfers run at 8192 Hz (512 CPU clocks per bit). CGB fast
    // mode runs at 262144 Hz (16 clocks per bit at normal CPU speed).
    return fast_clock_ ? 16U : 512U;
}

void SerialPort::tick(const unsigned cycles) noexcept {
    if (!active_ || !internal_clock_) {
        // The serial clock is derived from the free-running system divider;
        // it continues advancing while the port is idle or waiting for an
        // external clock. This is observable when SC is written after boot.
        // A connected cable intentionally keeps its phase at the next edge
        // until a peer is armed, so startup probes cannot be skipped while
        // the two emulated CPUs reach the handshake at different times.
        if (endpoint_ == nullptr) phase_ = (phase_ + cycles) % 512U;
        return;
    }

    phase_ += cycles;
    const auto bit_cycles = cycles_per_bit();
    while (active_ && phase_ >= bit_cycles) {
        if (endpoint_ != nullptr) {
            endpoint_->prepare_bit((data_ & 0x80U) != 0);
        }
        if (endpoint_ != nullptr && !endpoint_->peer_ready()) {
            // Do not accumulate elapsed CPU time while the cable has no
            // receiver. Once the peer arms its port, the master should emit
            // one next edge at normal spacing, not replay a burst of missed
            // edges that can outrun the peer's serial interrupt handler.
            phase_ = bit_cycles - 1;
            break;
        }
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
    last_transmitted_ = transfer_byte_;
    last_received_ = data_;
    ++transfers_completed_;
    if (internal_clock_ && endpoint_ != nullptr) {
        endpoint_->release_internal_clock(*this);
    }
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

bool SerialCable::Endpoint::peer_ready() const noexcept {
    return peer_ != nullptr && peer_->transfer_active();
}

void SerialCable::Endpoint::prepare_bit(const bool /*outgoing*/) noexcept {}

bool SerialCable::Endpoint::request_internal_clock(SerialPort& port) noexcept {
    if (cable_ == nullptr) return true;
    if (cable_->internal_owner_ != nullptr &&
        cable_->internal_owner_ != &port) {
        return false;
    }
    cable_->internal_owner_ = &port;
    return true;
}

void SerialCable::Endpoint::release_internal_clock(SerialPort& port) noexcept {
    if (cable_ != nullptr && cable_->internal_owner_ == &port) {
        cable_->internal_owner_ = nullptr;
    }
}

void SerialCable::connect(SerialPort& first, SerialPort& second) noexcept {
    disconnect();
    first_ = &first;
    second_ = &second;
    internal_owner_ = nullptr;
    first_endpoint_.set_cable(this);
    second_endpoint_.set_cable(this);
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
    first_endpoint_.set_cable(nullptr);
    second_endpoint_.set_cable(nullptr);
    internal_owner_ = nullptr;
    first_ = nullptr;
    second_ = nullptr;
}

} // namespace gameboy
