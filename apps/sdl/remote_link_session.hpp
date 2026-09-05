#pragma once

#include "gameboy/tcp_link_channel.hpp"
#include "gameboy/tcp_serial_endpoint.hpp"
#include "gameboy/lan_discovery.hpp"

#include <chrono>
#include <cstdint>
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
    static constexpr auto pending_poll_interval = std::chrono::milliseconds(25);

    gameboy::TcpLinkChannel channel;
    gameboy::TcpSerialEndpoint endpoint;
    gameboy::LanDiscovery discovery;
    bool enabled{};
    bool hosting{};
    bool diagnostics{};
    bool scanning{};
    std::chrono::steady_clock::time_point scan_deadline{};
    std::chrono::steady_clock::time_point next_pending_poll{};

    [[nodiscard]] bool active() const noexcept { return enabled; }

    // A session is active as soon as a listener or non-blocking connect has
    // been created, but the serial endpoint must not take over the emulation
    // loop until the TCP peer is actually connected. Keeping this distinction
    // prevents a host waiting for a peer from paying per-instruction network
    // polling overhead on every emulated frame.
    [[nodiscard]] bool transport_connected() const noexcept {
        return enabled &&
               channel.state() == gameboy::TcpLinkChannel::State::connected;
    }

    // Listening and non-blocking connection setup do not need the
    // instruction-level poll rate used by an established serial link. Keep
    // the pending state responsive while avoiding a socket syscall on every
    // video frame (particularly visible on Windows hosts with no peer yet).
    void poll() noexcept {
        if (!enabled) return;
        if (!transport_connected()) {
            const auto now = std::chrono::steady_clock::now();
            if (next_pending_poll != std::chrono::steady_clock::time_point{} &&
                now < next_pending_poll) {
                return;
            }
            next_pending_poll = now + pending_poll_interval;
        }
        endpoint.poll();
    }
};

} // namespace gbb::sdl
