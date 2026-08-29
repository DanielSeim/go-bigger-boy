#pragma once

#include "gameboy/hardware_model.hpp"

#include <cstdint>

namespace gameboy {

class SerialPort;

// A serial endpoint supplies the bit received by a port whenever that port's
// internal clock produces a rising edge. Returning true models the pull-up
// state of a disconnected link cable.
class SerialEndpoint {
public:
    virtual ~SerialEndpoint() = default;
    // Called immediately before peer_ready()/exchange_bit() for an internal
    // clock edge. Remote endpoints use this to queue the outgoing bit without
    // blocking the serial callback.
    virtual void prepare_bit(bool /*outgoing*/) noexcept {}
    [[nodiscard]] virtual bool exchange_bit(bool outgoing) noexcept = 0;
    [[nodiscard]] virtual bool peer_ready() const noexcept { return true; }
    [[nodiscard]] virtual bool request_internal_clock(
        SerialPort& /*port*/) noexcept {
        return true;
    }
    virtual void release_internal_clock(SerialPort& /*port*/) noexcept {}
    // Remote transports may need to span several guest frames while a byte
    // is in flight. The deterministic local cable keeps the hardware-style
    // restart behavior unless an endpoint opts into this mode.
    [[nodiscard]] virtual bool preserve_active_transfer() const noexcept {
        return false;
    }
    // Explicit cancellation used by reset_link(). A normal SC rewrite is
    // deliberately not treated as cancellation: a TCP response can still be
    // in flight and must remain consumable by the re-armed transfer.
    virtual void cancel_internal_clock(SerialPort& /*port*/) noexcept {}
};

class SerialPort {
public:
    using CompletionCallback = void (*)(void*, std::uint8_t transmitted,
                                        std::uint8_t received) noexcept;

    explicit SerialPort(bool cgb_mode = false) noexcept : cgb_mode_(cgb_mode) {}

    [[nodiscard]] std::uint8_t read_data() const noexcept { return data_; }
    [[nodiscard]] std::uint8_t read_control() const noexcept;
    void write_data(std::uint8_t value) noexcept {
        // Pokémon rewrites SB while it is polling for a peer.  A linked
        // transfer may still have a byte in flight while that polling loop
        // runs, so do not replace the shift register until that transfer has
        // completed (or reset_link() explicitly cancels it).
        // Before the first edge the guest may still be replacing its probe
        // byte (the Cable Club writes SB=02, then SB=01). Once at least one
        // linked edge has shifted, keep the in-flight byte intact.
        if (endpoint_ != nullptr && active_ && bits_shifted_ != 0 &&
            endpoint_->preserve_active_transfer()) return;
        data_ = value;
    }
    void write_control(std::uint8_t value) noexcept;

    void initialize_post_boot(HardwareModel model) noexcept;
    void tick(unsigned cycles) noexcept;

    // Called by a cable when the other console supplies a clock edge. The
    // return value is this port's outgoing bit for that edge.
    [[nodiscard]] bool clock_external_bit(bool incoming) noexcept;

    [[nodiscard]] bool transfer_active() const noexcept { return active_; }
    [[nodiscard]] bool has_endpoint() const noexcept { return endpoint_ != nullptr; }
    [[nodiscard]] std::uint32_t phase() const noexcept { return phase_; }
    [[nodiscard]] std::uint8_t bits_shifted() const noexcept {
        return bits_shifted_;
    }
    [[nodiscard]] bool internal_clock() const noexcept { return internal_clock_; }
    [[nodiscard]] bool fast_clock() const noexcept { return fast_clock_; }
    [[nodiscard]] std::uint64_t transfers_completed() const noexcept {
        return transfers_completed_;
    }
    [[nodiscard]] std::uint8_t last_transmitted() const noexcept {
        return last_transmitted_;
    }
    [[nodiscard]] std::uint8_t last_received() const noexcept {
        return last_received_;
    }

    // Diagnostics are intentionally separate from the emulated serial state.
    // A frontend can clear them when beginning a new link session without
    // disturbing an in-progress transfer or any guest-visible registers.
    void reset_diagnostics() noexcept;

    // Abort any in-flight transfer and return the port to an idle protocol
    // boundary. This does not reset CPU, memory, or cartridge state and is
    // used when a host retries a link session after a timeout.
    void reset_link() noexcept;

    void restore_state(std::uint8_t data, std::uint8_t control,
                       std::uint32_t phase, std::uint8_t bits_shifted,
                       bool active, bool internal_clock,
                       bool fast_clock) noexcept;

    void set_endpoint(SerialEndpoint* endpoint) noexcept;
    void set_completion_callback(void* context,
                                 CompletionCallback callback) noexcept {
        callback_context_ = context;
        completion_callback_ = callback;
    }

private:
    void clock_internal_bit() noexcept;
    void shift_bit(bool incoming) noexcept;
    [[nodiscard]] unsigned cycles_per_bit() const noexcept;

    bool cgb_mode_{};
    std::uint8_t data_{0xFF};
    std::uint8_t external_data_{0xFF};
    std::uint8_t transfer_byte_{0xFF};
    std::uint8_t control_{};
    std::uint32_t phase_{};
    std::uint8_t bits_shifted_{};
    bool active_{};
    bool internal_clock_{};
    bool fast_clock_{};
    std::uint64_t transfers_completed_{};
    std::uint8_t last_transmitted_{0xFF};
    std::uint8_t last_received_{0xFF};
    SerialEndpoint* endpoint_{};
    void* callback_context_{};
    CompletionCallback completion_callback_{};
};

// A deterministic two-console cable. It does not create threads or perform
// I/O: each console's serial edge is delivered directly to the other port,
// making local multiplayer reproducible and testable.
class SerialCable {
public:
    SerialCable() noexcept = default;
    SerialCable(const SerialCable&) = delete;
    SerialCable& operator=(const SerialCable&) = delete;
    ~SerialCable() { disconnect(); }

    void connect(SerialPort& first, SerialPort& second) noexcept;
    void disconnect() noexcept;

private:
    class Endpoint final : public SerialEndpoint {
    public:
        void set_peer(SerialPort* peer) noexcept { peer_ = peer; }
        void set_cable(SerialCable* cable) noexcept { cable_ = cable; }
        [[nodiscard]] bool exchange_bit(bool outgoing) noexcept override;
        [[nodiscard]] bool peer_ready() const noexcept override;
        void prepare_bit(bool outgoing) noexcept override;
        [[nodiscard]] bool request_internal_clock(
            SerialPort& port) noexcept override;
        void release_internal_clock(SerialPort& port) noexcept override;

    private:
        SerialPort* peer_{};
        SerialCable* cable_{};
    } first_endpoint_, second_endpoint_;
    SerialPort* first_{};
    SerialPort* second_{};
    SerialPort* internal_owner_{};
};

} // namespace gameboy
