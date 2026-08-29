#include "gameboy/link_transport.hpp"

namespace gameboy {

void LocalLinkTransport::attach(SerialPort& first,
                                SerialPort& second) noexcept {
    detach();
    cable_.connect(first, second);
    cable_connected_ = true;
}

void LocalLinkTransport::detach() noexcept {
    cable_.disconnect();
    cable_connected_ = false;
}

std::array<std::uint8_t, LinkPacketCodec::wire_size> LinkPacketCodec::encode(
    const LinkPacket& packet) noexcept {
    std::array<std::uint8_t, wire_size> bytes{};
    bytes[0] = 'G';
    bytes[1] = 'B';
    bytes[2] = 1; // protocol version
    bytes[3] = static_cast<std::uint8_t>(packet.type);
    bytes[4] = static_cast<std::uint8_t>(packet.sequence);
    bytes[5] = static_cast<std::uint8_t>(packet.sequence >> 8);
    bytes[6] = static_cast<std::uint8_t>(packet.sequence >> 16);
    bytes[7] = static_cast<std::uint8_t>(packet.sequence >> 24);
    bytes[8] = packet.value;
    bytes[9] = packet.flags;
    std::uint8_t checksum = 0;
    for (std::size_t index = 0; index < wire_size - 1; ++index) {
        checksum = static_cast<std::uint8_t>(checksum ^ bytes[index]);
    }
    bytes[10] = checksum;
    return bytes;
}

std::optional<LinkPacket> LinkPacketCodec::decode(const std::uint8_t* bytes,
                                                  const std::size_t size) noexcept {
    if (bytes == nullptr || size != wire_size || bytes[0] != 'G' ||
        bytes[1] != 'B' || bytes[2] != 1 ||
        (bytes[3] < static_cast<std::uint8_t>(LinkPacketType::hello) ||
         bytes[3] > static_cast<std::uint8_t>(LinkPacketType::clock_release))) {
        return std::nullopt;
    }
    std::uint8_t checksum = 0;
    for (std::size_t index = 0; index < wire_size - 1; ++index) {
        checksum = static_cast<std::uint8_t>(checksum ^ bytes[index]);
    }
    if (checksum != bytes[wire_size - 1]) return std::nullopt;
    const auto sequence = static_cast<std::uint32_t>(bytes[4]) |
                          (static_cast<std::uint32_t>(bytes[5]) << 8) |
                          (static_cast<std::uint32_t>(bytes[6]) << 16) |
                          (static_cast<std::uint32_t>(bytes[7]) << 24);
    return LinkPacket{static_cast<LinkPacketType>(bytes[3]), sequence, bytes[8],
                      bytes[9]};
}

} // namespace gameboy
