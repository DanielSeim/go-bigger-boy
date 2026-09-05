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

    virtual ~LinkPacketChannel() = default;
    LinkPacketChannel(const LinkPacketChannel&) = delete;
    LinkPacketChannel& operator=(const LinkPacketChannel&) = delete;

    virtual void poll() noexcept = 0;
    virtual void close() noexcept = 0;
    [[nodiscard]] virtual bool send(const LinkPacket& packet) noexcept = 0;
    [[nodiscard]] virtual std::optional<LinkPacket> receive() noexcept = 0;
    [[nodiscard]] virtual State state() const noexcept = 0;
    [[nodiscard]] virtual std::uint16_t local_port() const noexcept { return 0; }
    [[nodiscard]] virtual std::uint64_t malformed_packets() const noexcept {
        return 0;
    }

protected:
    LinkPacketChannel() noexcept = default;
};

} // namespace gameboy
