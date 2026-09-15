#pragma once

#include "gameboy/link_transport.hpp"

#include <cstdint>
#include <optional>

namespace gameboy {

// Packet-oriented byte-stream boundary shared by network transports.  The
// serial endpoint only consumes already framed packets; implementations must
// keep all potentially blocking I/O behind poll() and send().
class LinkPacketChannel {
public:
    enum class State { disconnected, listening, connecting, connected, failed };
    // Link frames are tiny and the endpoint normally consumes them on every
    // poll. Keep a stalled or hostile peer from turning the transport into an
    // unbounded allocation source.
    static constexpr std::size_t maximum_queued_packets = 256;
    static constexpr std::size_t maximum_buffered_bytes =
        maximum_queued_packets * LinkPacketCodec::wire_size;

    virtual ~LinkPacketChannel() = default;
    LinkPacketChannel(const LinkPacketChannel&) = delete;
    LinkPacketChannel& operator=(const LinkPacketChannel&) = delete;

    virtual void poll() noexcept = 0;
    virtual void close() noexcept = 0;
    [[nodiscard]] virtual bool send(const LinkPacket& packet) noexcept = 0;
    [[nodiscard]] virtual std::optional<LinkPacket> receive() noexcept = 0;
    [[nodiscard]] virtual State state() const noexcept = 0;
    [[nodiscard]] virtual std::uint16_t local_port() const noexcept { return 0; }
    [[nodiscard]] virtual std::size_t queued_packets() const noexcept {
        return 0;
    }
    [[nodiscard]] virtual std::size_t buffered_bytes() const noexcept {
        return 0;
    }
    [[nodiscard]] virtual std::uint64_t malformed_packets() const noexcept {
        return 0;
    }

protected:
    LinkPacketChannel() noexcept = default;
};

} // namespace gameboy
