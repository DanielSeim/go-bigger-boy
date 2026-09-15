#include "gameboy/link_transport.hpp"

#include <array>
#include <cstdint>
#include <deque>
#include <iostream>
#include <vector>

namespace {

int failures = 0;

std::uint32_t next_value(std::uint32_t& state) {
    // Small deterministic generator: reproducible across platforms and fast
    // enough to keep this contract test suitable for every build.
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

void check(const bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

void test_deterministic_round_trips() {
    std::uint32_t state = 0xC001D00DU;
    for (unsigned index = 0; index < 128; ++index) {
        const auto random = next_value(state);
        const auto type = static_cast<gameboy::LinkPacketType>(
            1U + (random % 7U));
        const gameboy::LinkPacket packet{type, next_value(state),
                                         static_cast<std::uint8_t>(next_value(state)),
                                         static_cast<std::uint8_t>(next_value(state)),
                                         (static_cast<std::uint64_t>(next_value(state)) << 32U) |
                                             next_value(state)};
        const auto wire = gameboy::LinkPacketCodec::encode(packet);
        const auto decoded = gameboy::LinkPacketCodec::decode(wire.data(),
                                                               wire.size());
        check(decoded.has_value(), "generated packet round-trips");
        if (decoded) {
            check(decoded->type == packet.type &&
                      decoded->sequence == packet.sequence &&
                      decoded->value == packet.value &&
                      decoded->flags == packet.flags &&
                      decoded->session_id == packet.session_id,
                  "generated packet fields survive round-trip");
        }
    }
}

void test_stream_resynchronizes_after_corruption() {
    const gameboy::LinkPacket damaged{gameboy::LinkPacketType::bit, 4, 0x11,
                                      0x01, UINT64_C(0x1111222233334444)};
    const gameboy::LinkPacket valid{gameboy::LinkPacketType::heartbeat, 5, 0,
                                    0, UINT64_C(0x1111222233334444)};
    const auto damaged_wire = gameboy::LinkPacketCodec::encode(damaged);
    const auto valid_wire = gameboy::LinkPacketCodec::encode(valid);
    auto corrupted_wire = damaged_wire;
    corrupted_wire[8] ^= 0x01;
    std::vector<std::uint8_t> stream{0x00, 0x7F};
    stream.insert(stream.end(), corrupted_wire.begin(), corrupted_wire.end());
    stream.push_back(0x42); // Noise between two complete frames.
    stream.insert(stream.end(), valid_wire.begin(), valid_wire.end());

    std::deque<gameboy::LinkPacket> packets;
    std::uint64_t malformed = 0;
    check(gameboy::LinkPacketCodec::decode_stream(stream, packets, malformed),
          "stream decoder accepts recoverable framing noise");
    check((malformed != 0 || stream.empty()) && packets.size() == 1 &&
              packets.front().type == valid.type &&
              packets.front().sequence == valid.sequence,
          "stream decoder resynchronizes on the next valid frame");
}

} // namespace

int main() {
    const gameboy::LinkPacket original{
        gameboy::LinkPacketType::clock_release, 0xDEADBEEFU, 0xA5, 0x18,
        UINT64_C(0x0123456789ABCDEF)};
    const auto wire = gameboy::LinkPacketCodec::encode(original);
    const auto decoded = gameboy::LinkPacketCodec::decode(wire.data(), wire.size());
    check(decoded.has_value(), "valid link packet decodes");
    if (decoded) {
        check(decoded->type == original.type &&
                  decoded->sequence == original.sequence &&
                  decoded->value == original.value &&
                  decoded->flags == original.flags &&
                  decoded->session_id == original.session_id,
              "packet fields survive encode/decode");
    }

    auto corrupted = wire;
    corrupted[8] ^= 0x01;
    check(!gameboy::LinkPacketCodec::decode(corrupted.data(), corrupted.size()),
          "checksum rejects corrupted payload");
    check(!gameboy::LinkPacketCodec::decode(wire.data(), wire.size() - 1),
          "truncated packet is rejected");
    check(!gameboy::LinkPacketCodec::decode(nullptr, wire.size()),
          "null packet buffer is rejected");

    for (std::size_t index = 0; index < wire.size(); ++index) {
        auto mutated = wire;
        mutated[index] ^= 0x01;
        check(!gameboy::LinkPacketCodec::decode(mutated.data(), mutated.size()),
              "single-bit packet mutations are rejected");
    }

    auto invalid_type = wire;
    invalid_type[3] = 0;
    invalid_type[18] = 0;
    invalid_type[19] = 0;
    check(!gameboy::LinkPacketCodec::decode(invalid_type.data(), invalid_type.size()),
          "unknown packet type is rejected even with a valid checksum");

    test_deterministic_round_trips();
    test_stream_resynchronizes_after_corruption();

    return failures == 0 ? 0 : 1;
}
