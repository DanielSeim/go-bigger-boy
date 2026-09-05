#pragma once

#include "gameboy/serial.hpp"
#include "gameboy/tcp_link_channel.hpp"

#include <cstdint>
#include <optional>

namespace gameboy {

// Bridges one local SerialPort to a non-blocking TCP channel. The endpoint
// never waits for the network: a local internal edge is held by peer_ready()
// until poll() receives the peer's response bit.
class TcpSerialEndpoint final : public SerialEndpoint {
public:
    TcpSerialEndpoint() noexcept = default;
    TcpSerialEndpoint(const TcpSerialEndpoint&) = delete;
    TcpSerialEndpoint& operator=(const TcpSerialEndpoint&) = delete;

    void attach(SerialPort& port, TcpLinkChannel& channel,
                std::uint64_t link_compatibility_id = 0) noexcept;
    void detach() noexcept;
    void poll() noexcept;
    void set_arbitration_priority(bool priority) noexcept {
        arbitration_priority_ = priority;
    }
    [[nodiscard]] bool connected() const noexcept;
    [[nodiscard]] bool preserve_active_transfer() const noexcept override {
        return true;
    }
    [[nodiscard]] bool peer_ready_for_link() const noexcept {
        return connected() && peer_hello_seen_ && peer_compatible_ &&
               (arbitration_priority_ || peer_request_seen_);
    }
    [[nodiscard]] bool waiting_for_peer() const noexcept {
        return pending_sequence_.has_value() && !response_.has_value();
    }

    // Read-only arbitration and compatibility state used by opt-in link
    // diagnostics. These
    // values explain a slow but otherwise healthy exchange without exposing
    // transport internals to the emulated serial port.
    [[nodiscard]] bool response_ready() const noexcept {
        return response_.has_value();
    }
    [[nodiscard]] bool peer_hello_seen() const noexcept {
        return peer_hello_seen_;
    }
    [[nodiscard]] bool peer_compatible() const noexcept {
        return peer_compatible_;
    }
    [[nodiscard]] std::uint64_t peer_compatibility_id() const noexcept {
        return peer_compatibility_id_;
    }
    [[nodiscard]] bool peer_request_seen() const noexcept {
        return peer_request_seen_;
    }
    [[nodiscard]] bool peer_byte_released() const noexcept {
        return peer_byte_released_;
    }
    [[nodiscard]] bool peer_clock_busy() const noexcept {
        return peer_clock_busy_;
    }
    [[nodiscard]] unsigned request_backoff() const noexcept {
        return request_backoff_;
    }

    // Transport diagnostics are intentionally read-only and do not expose or
    // alter guest-visible serial state. They make it possible to distinguish
    // a game that never starts its serial clock from a TCP exchange that is
    // not being serviced.
    [[nodiscard]] std::uint64_t requests_sent() const noexcept {
        return requests_sent_;
    }
    [[nodiscard]] std::uint64_t requests_received() const noexcept {
        return requests_received_;
    }
    [[nodiscard]] std::uint64_t responses_sent() const noexcept {
        return responses_sent_;
    }
    [[nodiscard]] std::uint64_t responses_received() const noexcept {
        return responses_received_;
    }
    [[nodiscard]] std::uint64_t denials_sent() const noexcept {
        return denials_sent_;
    }
    [[nodiscard]] std::uint64_t denials_received() const noexcept {
        return denials_received_;
    }
    [[nodiscard]] std::uint64_t responses_unmatched() const noexcept {
        return responses_unmatched_;
    }
    [[nodiscard]] std::uint64_t transfers_completed() const noexcept {
        return port_ == nullptr ? 0 : port_->transfers_completed();
    }

    void prepare_bit(bool outgoing) noexcept override;
    [[nodiscard]] bool exchange_bit(bool outgoing) noexcept override;
    [[nodiscard]] bool peer_ready() const noexcept override {
        return response_.has_value();
    }
    [[nodiscard]] bool request_internal_clock(
        SerialPort& /*port*/) noexcept override;
    void release_internal_clock(SerialPort& /*port*/) noexcept override;
    void cancel_internal_clock(SerialPort& /*port*/) noexcept override;

private:
    static constexpr std::uint8_t request_flag = 0x01;
    static constexpr std::uint8_t response_flag = 0x02;
    static constexpr std::uint8_t denied_flag = 0x04;
    static constexpr std::uint8_t not_ready_flag = 0x08;
    static constexpr std::uint8_t reset_flag = 0x80;

    SerialPort* port_{};
    TcpLinkChannel* channel_{};
    std::uint32_t next_sequence_{};
    std::optional<std::uint32_t> pending_sequence_;
    std::optional<bool> response_;
    std::optional<LinkPacket> deferred_request_;
    unsigned request_backoff_{};
    bool arbitration_priority_{};
    bool hello_sent_{};
    bool peer_hello_seen_{};
    bool peer_compatible_{true};
    std::uint8_t hello_parts_sent_{};
    std::uint8_t hello_parts_received_{};
    std::uint64_t compatibility_id_{};
    std::uint64_t peer_compatibility_id_{};
    bool peer_request_seen_{};
    bool peer_byte_released_{};
    bool peer_clock_busy_{};
    std::uint64_t requests_sent_{};
    std::uint64_t requests_received_{};
    std::uint64_t responses_sent_{};
    std::uint64_t responses_received_{};
    std::uint64_t denials_sent_{};
    std::uint64_t denials_received_{};
    std::uint64_t responses_unmatched_{};
    std::uint64_t diagnostic_session_{};
};

} // namespace gameboy
