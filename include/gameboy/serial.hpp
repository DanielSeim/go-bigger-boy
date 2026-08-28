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
    [[nodiscard]] virtual bool exchange_bit(bool outgoing) noexcept = 0;
};

class SerialPort {
public:
    using CompletionCallback = void (*)(void*, std::uint8_t transmitted,
                                        std::uint8_t received) noexcept;

    explicit SerialPort(bool cgb_mode = false) noexcept : cgb_mode_(cgb_mode) {}

    [[nodiscard]] std::uint8_t read_data() const noexcept { return data_; }
    [[nodiscard]] std::uint8_t read_control() const noexcept;
    void write_data(std::uint8_t value) noexcept { data_ = value; }
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

    void restore_state(std::uint8_t data, std::uint8_t control,
                       std::uint32_t phase, std::uint8_t bits_shifted,
                       bool active, bool internal_clock,
                       bool fast_clock) noexcept;

    void set_endpoint(SerialEndpoint* endpoint) noexcept { endpoint_ = endpoint; }
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
    std::uint8_t transfer_byte_{0xFF};
    std::uint8_t control_{};
    std::uint32_t phase_{};
    std::uint8_t bits_shifted_{};
    bool active_{};
    bool internal_clock_{};
    bool fast_clock_{};
    SerialEndpoint* endpoint_{};
    void* callback_context_{};
    CompletionCallback completion_callback_{};
};

// A deterministic two-console cable. It does not create threads or perform
// I/O: the console supplying the internal clock clocks the other port
// directly, making local multiplayer reproducible and testable.
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
        [[nodiscard]] bool exchange_bit(bool outgoing) noexcept override;

    private:
        SerialPort* peer_{};
    } first_endpoint_, second_endpoint_;
    SerialPort* first_{};
    SerialPort* second_{};
};

} // namespace gameboy
