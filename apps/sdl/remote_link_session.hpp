#pragma once

#include "gameboy/tcp_link_channel.hpp"
#include "gameboy/tcp_serial_endpoint.hpp"
#include "gameboy/lan_discovery.hpp"

#include <cstdint>
#include <chrono>
#include <string>

namespace gbb::sdl {

struct RemoteLinkOptions {
    std::string host{"127.0.0.1"};
    std::string bind_address{"127.0.0.1"};
    std::uint16_t port{8765};
    bool lan_discovery{};
};

// Frontend-owned state for a TCP link. Keeping this transport aggregate out
// of main.cpp makes it possible for another desktop frontend to reuse the
// same endpoint setup without depending on SDL's event loop implementation.
struct RemoteLinkSession {
    gameboy::TcpLinkChannel channel;
    gameboy::TcpSerialEndpoint endpoint;
    gameboy::LanDiscovery discovery;
    bool enabled{};
    bool hosting{};
    bool diagnostics{};
    bool scanning{};
    std::chrono::steady_clock::time_point scan_deadline{};

    [[nodiscard]] bool active() const noexcept { return enabled; }
};

} // namespace gbb::sdl
