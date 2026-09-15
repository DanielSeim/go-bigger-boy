#pragma once

#include "gameboy/serial.hpp"
#include "gameboy/link_packet_channel.hpp"
#include "gameboy/link_compatibility.hpp"

#include <chrono>
#include <cstdint>
#include <optional>

namespace gameboy {

// Bridges one local SerialPort to a non-blocking packet channel. The endpoint
// never waits for the network: a local internal edge is held by peer_ready()
// until poll() receives the peer's response bit. The historical header and
// alias retain the TCP name for source compatibility with existing harnesses.
class LinkSerialEndpoint final : public SerialEndpoint {
public:
    LinkSerialEndpoint() noexcept = default;
    LinkSerialEndpoint(const LinkSerialEndpoint&) = delete;
    LinkSerialEndpoint& operator=(const LinkSerialEndpoint&) = delete;

    void attach(SerialPort& port, LinkPacketChannel& channel,
                std::uint64_t link_compatibility_id = 0,
                LinkCompatibilityProfile compatibility_profile = {}) noexcept;
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
               (arbitration_priority_ || peer_request_seen_) &&
               !reset_waiting_for_ack_;
    }
    [[nodiscard]] bool waiting_for_peer() const noexcept {
        return pending_sequence_.has_value() && !response_.has_value() &&
               !byte_response_.has_value();
    }

    // The frontend can avoid polling a quiet socket on every CPU slice. A
    // poll is still required while the hello exchange or a serial transfer is
    // active, including when this side is receiving the peer's clock.
    [[nodiscard]] bool needs_poll() const noexcept {
        return channel_ != nullptr &&
               (!peer_hello_seen_ || waiting_for_peer() ||
                deferred_request_.has_value() ||
                reset_waiting_for_ack_ ||
                (compatibility_profile_.known() && !peer_profile_seen_ &&
                 profile_wait_polls_ < profile_wait_limit) ||
                (connected() &&
                 (std::chrono::steady_clock::now() - last_heartbeat_sent_ >=
                      heartbeat_interval ||
                  std::chrono::steady_clock::now() - last_peer_activity_ >=
                      heartbeat_timeout)) ||
                (port_ != nullptr && port_->transfer_active()));
    }

    // Read-only arbitration and compatibility state used by opt-in link
    // diagnostics. These
    // values explain a slow but otherwise healthy exchange without exposing
    // transport internals to the emulated serial port.
    [[nodiscard]] bool response_ready() const noexcept {
        return response_.has_value() || byte_response_.has_value();
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
    [[nodiscard]] std::uint64_t session_id() const noexcept {
        return session_id_;
    }
    [[nodiscard]] std::uint64_t peer_session_id() const noexcept {
        return peer_session_id_.value_or(0);
    }
    [[nodiscard]] LinkCompatibilityProfile compatibility_profile() const noexcept {
        return compatibility_profile_;
    }
    [[nodiscard]] LinkCompatibilityProfile peer_compatibility_profile() const noexcept {
        return peer_compatibility_profile_;
    }
    [[nodiscard]] bool peer_request_seen() const noexcept {
        return peer_request_seen_;
    }
    [[nodiscard]] bool peer_byte_released() const noexcept {
        return peer_byte_released_;
    }
    [[nodiscard]] bool peer_byte_transfer() const noexcept {
        return peer_byte_transfer_;
    }
    [[nodiscard]] bool peer_clock_busy() const noexcept {
        return peer_clock_busy_;
    }
    // Byte packets are the established transport representation for every
    // negotiated Pokémon link profile, including Gen-I/Gen-II Time Capsule
    // entry.  The guest protocol owns the byte-level conversion and pacing;
    // changing the representation before that handshake completes can leave
    // the two games in different Cable Club states.
    [[nodiscard]] bool byte_transfer_allowed() const noexcept {
        return true;
    }
    [[nodiscard]] unsigned request_backoff() const noexcept {
        return request_backoff_;
    }
    [[nodiscard]] unsigned deferred_request_polls() const noexcept {
        return deferred_request_polls_;
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
    [[nodiscard]] std::uint64_t byte_packets_sent() const noexcept {
        return byte_packets_sent_;
    }
    [[nodiscard]] std::uint64_t byte_packets_received() const noexcept {
        return byte_packets_received_;
    }
    [[nodiscard]] std::uint64_t stale_session_packets() const noexcept {
        return stale_session_packets_;
    }
    [[nodiscard]] std::uint64_t out_of_order_requests() const noexcept {
        return out_of_order_requests_;
    }
    [[nodiscard]] std::uint64_t duplicate_requests() const noexcept {
        return duplicate_requests_;
    }
    [[nodiscard]] std::uint64_t protocol_errors() const noexcept {
        return protocol_errors_;
    }
    [[nodiscard]] std::uint64_t heartbeats_sent() const noexcept {
        return heartbeats_sent_;
    }
    [[nodiscard]] std::uint64_t heartbeats_received() const noexcept {
        return heartbeats_received_;
    }
    [[nodiscard]] std::uint64_t heartbeat_timeouts() const noexcept {
        return heartbeat_timeouts_;
    }
    [[nodiscard]] std::uint64_t reset_requests_sent() const noexcept {
        return reset_requests_sent_;
    }
    [[nodiscard]] std::uint64_t reset_acknowledgements() const noexcept {
        return reset_acknowledgements_;
    }
    [[nodiscard]] std::uint64_t transfers_completed() const noexcept {
        return port_ == nullptr ? 0 : port_->transfers_completed();
    }

    void prepare_bit(bool outgoing) noexcept override;
    [[nodiscard]] bool exchange_bit(bool outgoing) noexcept override;
    [[nodiscard]] bool peer_ready() const noexcept override {
        return response_.has_value() || byte_response_.has_value();
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
    static constexpr std::uint8_t reset_ack_flag = 0x40;
    static constexpr std::uint8_t heartbeat_ack_flag = 0x20;
    // A request may legitimately arrive a few frames before the peer arms
    // its receiver. Do not retain it forever when the peer has left serial.
    static constexpr unsigned deferred_request_poll_limit = 240;
    static constexpr std::uint8_t byte_transfer_capability = 0x10;
    static constexpr unsigned reset_ack_wait_poll_limit = 240;
    static constexpr auto heartbeat_interval = std::chrono::seconds(1);
    static constexpr auto heartbeat_timeout = std::chrono::seconds(5);

    SerialPort* port_{};
    LinkPacketChannel* channel_{};
    std::uint32_t next_sequence_{};
    std::uint32_t next_control_sequence_{};
    std::optional<std::uint32_t> pending_sequence_;
    std::optional<LinkPacketType> pending_type_;
    std::optional<bool> response_;
    std::optional<std::uint8_t> byte_response_;
    std::uint8_t byte_bits_consumed_{};
    std::optional<LinkPacket> deferred_request_;
    std::optional<std::uint32_t> last_peer_request_sequence_;
    std::optional<LinkPacket> last_peer_request_response_;
    unsigned request_backoff_{};
    unsigned deferred_request_polls_{};
    bool arbitration_priority_{};
    bool hello_sent_{};
    bool peer_hello_seen_{};
    bool peer_compatible_{true};
    std::uint8_t hello_parts_sent_{};
    std::uint8_t hello_parts_received_{};
    std::uint64_t compatibility_id_{};
    std::uint64_t peer_compatibility_id_{};
    LinkCompatibilityProfile compatibility_profile_{};
    LinkCompatibilityProfile peer_compatibility_profile_{};
    bool peer_profile_seen_{};
    std::uint16_t profile_wait_polls_{};
    static constexpr std::uint16_t profile_wait_limit = 120;
    bool peer_request_seen_{};
    bool peer_byte_released_{};
    bool peer_byte_transfer_{};
    bool peer_clock_busy_{};
    std::uint64_t session_id_{};
    std::optional<std::uint64_t> peer_session_id_;
    std::optional<std::uint32_t> reset_sequence_;
    bool reset_waiting_for_ack_{};
    unsigned reset_ack_wait_polls_{};
    std::chrono::steady_clock::time_point last_peer_activity_{};
    std::chrono::steady_clock::time_point last_heartbeat_sent_{};
    std::uint64_t requests_sent_{};
    std::uint64_t requests_received_{};
    std::uint64_t responses_sent_{};
    std::uint64_t responses_received_{};
    std::uint64_t denials_sent_{};
    std::uint64_t denials_received_{};
    std::uint64_t responses_unmatched_{};
    std::uint64_t byte_packets_sent_{};
    std::uint64_t byte_packets_received_{};
    std::uint64_t stale_session_packets_{};
    std::uint64_t out_of_order_requests_{};
    std::uint64_t duplicate_requests_{};
    std::uint64_t protocol_errors_{};
    std::uint64_t heartbeats_sent_{};
    std::uint64_t heartbeats_received_{};
    std::uint64_t heartbeat_timeouts_{};
    std::uint64_t reset_requests_sent_{};
    std::uint64_t reset_acknowledgements_{};
    std::uint64_t diagnostic_session_{};

    [[nodiscard]] bool send_packet(LinkPacket packet) noexcept;
    void protocol_fault(const char* message) noexcept;
    [[nodiscard]] static bool is_next_sequence(std::uint32_t previous,
                                                std::uint32_t next) noexcept;
    void reset_transfer_state() noexcept;
};

// Source compatibility for callers that adopted the original TCP-only name.
using TcpSerialEndpoint = LinkSerialEndpoint;

} // namespace gameboy
