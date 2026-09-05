#pragma once

#include "gameboy/link_packet_channel.hpp"

#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <vector>

namespace gameboy {

// A non-blocking TCP channel for LinkPacket frames. poll() must be called by
// the frontend thread; send() only queues bytes and never waits for the peer.
// It is intentionally separate from LinkSession until a remote serial-edge
// adapter can provide ready bits without blocking the emulation thread.
class TcpLinkChannel final : public LinkPacketChannel {
public:
    using State = LinkPacketChannel::State;

    TcpLinkChannel() noexcept = default;
    TcpLinkChannel(const TcpLinkChannel&) = delete;
    TcpLinkChannel& operator=(const TcpLinkChannel&) = delete;
    ~TcpLinkChannel() override;

    // The one-argument form remains loopback-only for backwards
    // compatibility. Pass an explicit bind address (for example 0.0.0.0)
    // when the user has opted into LAN hosting.
    [[nodiscard]] bool listen(std::uint16_t port) noexcept;
    [[nodiscard]] bool listen(std::uint16_t port,
                              const std::string& bind_address) noexcept;
    [[nodiscard]] bool connect(const std::string& host,
                               std::uint16_t port) noexcept;
    void poll() noexcept override;
    void close() noexcept override;

    [[nodiscard]] bool send(const LinkPacket& packet) noexcept override;
    [[nodiscard]] std::optional<LinkPacket> receive() noexcept override;
    [[nodiscard]] State state() const noexcept override { return state_; }
    [[nodiscard]] std::uint16_t local_port() const noexcept override;
    // Number of complete frames rejected by LinkPacketCodec since the last
    // connection reset. Kept separate from socket failures for diagnostics.
    [[nodiscard]] std::uint64_t malformed_packets() const noexcept override {
        return malformed_packets_;
    }

private:
    void flush_send_queue() noexcept;
    void receive_available() noexcept;
    void fail() noexcept;

    std::intptr_t listener_{-1};
    std::intptr_t peer_{-1};
    State state_{State::disconnected};
    std::vector<std::uint8_t> send_buffer_;
    std::size_t send_offset_{};
    std::vector<std::uint8_t> receive_buffer_;
    std::deque<LinkPacket> packets_;
    std::uint64_t malformed_packets_{};
};

} // namespace gameboy
