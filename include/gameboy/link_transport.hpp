#pragma once

#include "gameboy/serial.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

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
};

struct LinkPacket {
    LinkPacketType type{LinkPacketType::hello};
    std::uint32_t sequence{};
    std::uint8_t value{};
    std::uint8_t flags{};
};

// Fixed-size framing shared by future TCP/UDP transports. It includes a
// magic/version pair and a checksum so malformed or stale packets are
// rejected before they can affect serial state.
class LinkPacketCodec final {
public:
    static constexpr std::size_t wire_size = 11;

    [[nodiscard]] static std::array<std::uint8_t, wire_size> encode(
        const LinkPacket& packet) noexcept;
    [[nodiscard]] static std::optional<LinkPacket> decode(
        const std::uint8_t* bytes, std::size_t size) noexcept;
};

} // namespace gameboy
