#pragma once

#include "gameboy/serial.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <vector>

namespace gameboy {

// A transport attaches two local serial endpoints. Implementations must not
// block from attach/detach or from the serial edge path; network transports
// should poll I/O on the frontend thread and queue ready edges instead.
class LinkTransport {
public:
    virtual ~LinkTransport() = default;
    LinkTransport(const LinkTransport&) = delete;
    LinkTransport& operator=(const LinkTransport&) = delete;

    virtual void attach(SerialPort& first, SerialPort& second) noexcept = 0;
    virtual void detach() noexcept = 0;
    [[nodiscard]] virtual bool connected() const noexcept = 0;

protected:
    LinkTransport() noexcept = default;
};

// The deterministic in-process implementation used by local multiplayer and
// tests. It is deliberately kept behind LinkTransport so a socket backend can
// be introduced without changing LinkSession's scheduling API.
class LocalLinkTransport final : public LinkTransport {
public:
    LocalLinkTransport() noexcept = default;
    ~LocalLinkTransport() override { detach(); }

    void attach(SerialPort& first, SerialPort& second) noexcept override;
    void detach() noexcept override;
    [[nodiscard]] bool connected() const noexcept override {
        return cable_connected_;
    }

private:
    SerialCable cable_{};
    bool cable_connected_{};
};

enum class LinkPacketType : std::uint8_t {
    hello = 1,
    bit = 2,
    acknowledgement = 3,
    clock_release = 4,
    // A negotiated byte exchange carries all eight serial bits in one
    // request/response pair. Older peers do not advertise this capability,
    // so endpoints automatically fall back to bit packets.
    byte = 5,
    // Transport heartbeat; it is acknowledged without touching guest serial
    // state and gives remote endpoints a wall-clock liveness signal.
    heartbeat = 6,
    // A post-hello digest barrier confirms both endpoints joined the same
    // ROM/session context before a serial edge can be issued.
    state_digest = 7,
};

struct LinkPacket {
    LinkPacketType type{LinkPacketType::hello};
    std::uint32_t sequence{};
    std::uint8_t value{};
    std::uint8_t flags{};
    // A fresh value is generated for every endpoint attachment. It fences
    // packets from an earlier connection from the current serial session.
    std::uint64_t session_id{};
};

// Fixed-size framing shared by future TCP/UDP transports. It includes a
// magic/version pair and a checksum so malformed or stale packets are
// rejected before they can affect serial state.
class LinkPacketCodec final {
public:
    static constexpr std::size_t wire_size = 20;
    static constexpr std::uint8_t protocol_version = 2;
    static constexpr std::size_t maximum_stream_packets = 256;

    [[nodiscard]] static std::array<std::uint8_t, wire_size> encode(
        const LinkPacket& packet) noexcept;
    [[nodiscard]] static std::optional<LinkPacket> decode(
        const std::uint8_t* bytes, std::size_t size) noexcept;
    // Consume as many complete frames as possible. Invalid bytes are dropped
    // one at a time until the next GB header, allowing a stream to recover
    // from insertion/deletion/corruption without a reconnect.
    [[nodiscard]] static bool decode_stream(
        std::vector<std::uint8_t>& buffer, std::deque<LinkPacket>& packets,
        std::uint64_t& malformed_packets,
        std::size_t maximum_packets = maximum_stream_packets) noexcept;
};

} // namespace gameboy
