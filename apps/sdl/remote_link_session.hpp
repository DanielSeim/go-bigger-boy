#pragma once

#include "gameboy/tcp_link_channel.hpp"
#include "gameboy/tcp_serial_endpoint.hpp"

namespace gbb::sdl {

// Frontend-owned state for a TCP link. Keeping this transport aggregate out
// of main.cpp makes it possible for another desktop frontend to reuse the
// same endpoint setup without depending on SDL's event loop implementation.
struct RemoteLinkSession {
    gameboy::TcpLinkChannel channel;
    gameboy::TcpSerialEndpoint endpoint;
    bool enabled{};
    bool hosting{};
    bool diagnostics{};

    [[nodiscard]] bool active() const noexcept { return enabled; }
};

} // namespace gbb::sdl
