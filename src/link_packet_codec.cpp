#include "gameboy/link_transport.hpp"

namespace gameboy {

namespace {

// CRC-16/CCITT-FALSE catches the short burst errors that XOR framing can
// miss. The wire format is deliberately fixed-size so transports remain
// simple byte-stream adapters.
std::uint16_t crc16(const std::uint8_t* bytes, const std::size_t size) noexcept {
    std::uint16_t crc = 0xFFFF;
    for (std::size_t index = 0; index < size; ++index) {
        crc ^= static_cast<std::uint16_t>(bytes[index]) << 8U;
        for (unsigned bit = 0; bit < 8; ++bit) {
            crc = static_cast<std::uint16_t>(
                (crc & 0x8000U) != 0
                    ? (crc << 1U) ^ 0x1021U
                    : crc << 1U);
        }
    }
    return crc;
}

} // namespace

std::array<std::uint8_t, LinkPacketCodec::wire_size> LinkPacketCodec::encode(
    const LinkPacket& packet) noexcept {
    std::array<std::uint8_t, wire_size> bytes{};
    bytes[0] = 'G';
    bytes[1] = 'B';
    bytes[2] = protocol_version;
    bytes[3] = static_cast<std::uint8_t>(packet.type);
    bytes[4] = static_cast<std::uint8_t>(packet.sequence);
    bytes[5] = static_cast<std::uint8_t>(packet.sequence >> 8);
    bytes[6] = static_cast<std::uint8_t>(packet.sequence >> 16);
    bytes[7] = static_cast<std::uint8_t>(packet.sequence >> 24);
    bytes[8] = packet.value;
    bytes[9] = packet.flags;
    for (unsigned byte = 0; byte < 8; ++byte) {
        bytes[10 + byte] = static_cast<std::uint8_t>(
            packet.session_id >> (byte * 8U));
    }
    const auto checksum = crc16(bytes.data(), wire_size - 2);
    bytes[18] = static_cast<std::uint8_t>(checksum);
    bytes[19] = static_cast<std::uint8_t>(checksum >> 8U);
    return bytes;
}

std::optional<LinkPacket> LinkPacketCodec::decode(const std::uint8_t* bytes,
                                                  const std::size_t size) noexcept {
    if (bytes == nullptr || size != wire_size || bytes[0] != 'G' ||
        bytes[1] != 'B' || bytes[2] != protocol_version ||
        (bytes[3] < static_cast<std::uint8_t>(LinkPacketType::hello) ||
         bytes[3] > static_cast<std::uint8_t>(LinkPacketType::heartbeat))) {
        return std::nullopt;
    }
    const auto checksum = crc16(bytes, wire_size - 2);
    const auto received_checksum = static_cast<std::uint16_t>(bytes[18]) |
                                   (static_cast<std::uint16_t>(bytes[19]) << 8U);
    if (checksum != received_checksum) return std::nullopt;
    const auto sequence = static_cast<std::uint32_t>(bytes[4]) |
                          (static_cast<std::uint32_t>(bytes[5]) << 8) |
                          (static_cast<std::uint32_t>(bytes[6]) << 16) |
                          (static_cast<std::uint32_t>(bytes[7]) << 24);
    std::uint64_t session_id = 0;
    for (unsigned byte = 0; byte < 8; ++byte) {
        session_id |= static_cast<std::uint64_t>(bytes[10 + byte])
                      << (byte * 8U);
    }
    return LinkPacket{static_cast<LinkPacketType>(bytes[3]), sequence, bytes[8],
                      bytes[9], session_id};
}

} // namespace gameboy
