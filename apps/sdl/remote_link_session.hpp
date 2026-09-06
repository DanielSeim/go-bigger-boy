#pragma once

#include "gameboy/tcp_link_channel.hpp"
#include "gameboy/tcp_serial_endpoint.hpp"
#include "gameboy/bluetooth_link_channel.hpp"
#include "gameboy/lan_discovery.hpp"

#include <chrono>
#include <cstdint>
#include <string>

namespace gbb::sdl {

struct RemoteLinkOptions {
    // "tcp" remains the default for backwards-compatible settings files.
    std::string transport{"tcp"};
    std::string host{"127.0.0.1"};
    std::string bind_address{"127.0.0.1"};
    std::uint16_t port{8765};
    bool lan_discovery{};
    std::string bluetooth_address;
    std::string bluetooth_service_uuid{
        "7b8f5d6e-7a47-4e17-9f9d-4b4d9d8e4f3a"};
};

// Frontend-owned state for a remote link. Keeping this transport aggregate out
// of main.cpp makes it possible for another desktop frontend to reuse the
// same endpoint setup without depending on SDL's event loop implementation.
struct RemoteLinkSession {
    static constexpr auto pending_poll_interval = std::chrono::milliseconds(25);

    gameboy::TcpLinkChannel channel;
    gameboy::BluetoothLinkChannel bluetooth_channel;
    gameboy::TcpSerialEndpoint endpoint;
    bool bluetooth{};
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
               active_channel().state() ==
                   gameboy::LinkPacketChannel::State::connected;
    }

    [[nodiscard]] gameboy::LinkPacketChannel& active_channel() noexcept {
        return bluetooth ? static_cast<gameboy::LinkPacketChannel&>(bluetooth_channel)
                         : static_cast<gameboy::LinkPacketChannel&>(channel);
    }
    [[nodiscard]] const gameboy::LinkPacketChannel& active_channel() const noexcept {
        return bluetooth ? static_cast<const gameboy::LinkPacketChannel&>(bluetooth_channel)
                         : static_cast<const gameboy::LinkPacketChannel&>(channel);
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
        if (!transport_connected() || endpoint.needs_poll()) endpoint.poll();
        // Hosts answer LAN discovery queries on the same frontend thread as
        // the serial endpoint. Without polling this socket after start_host,
        // scanners can broadcast successfully but never receive a response.
        if (discovery.active()) discovery.poll();
    }
};

} // namespace gbb::sdl
