#include "gameboy/link_transport.hpp"

#include <array>
#include <cstdint>
#include <iostream>

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
            1U + (random % 4U));
        const gameboy::LinkPacket packet{type, next_value(state),
                                         static_cast<std::uint8_t>(next_value(state)),
                                         static_cast<std::uint8_t>(next_value(state))};
        const auto wire = gameboy::LinkPacketCodec::encode(packet);
        const auto decoded = gameboy::LinkPacketCodec::decode(wire.data(),
                                                               wire.size());
        check(decoded.has_value(), "generated packet round-trips");
        if (decoded) {
            check(decoded->type == packet.type &&
                      decoded->sequence == packet.sequence &&
                      decoded->value == packet.value &&
                      decoded->flags == packet.flags,
                  "generated packet fields survive round-trip");
        }
    }
}

} // namespace

int main() {
    const gameboy::LinkPacket original{
        gameboy::LinkPacketType::clock_release, 0xDEADBEEFU, 0xA5, 0x18};
    const auto wire = gameboy::LinkPacketCodec::encode(original);
    const auto decoded = gameboy::LinkPacketCodec::decode(wire.data(), wire.size());
    check(decoded.has_value(), "valid link packet decodes");
    if (decoded) {
        check(decoded->type == original.type &&
                  decoded->sequence == original.sequence &&
                  decoded->value == original.value &&
                  decoded->flags == original.flags,
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
    std::uint8_t checksum = 0;
    for (std::size_t index = 0; index < invalid_type.size() - 1; ++index) {
        checksum = static_cast<std::uint8_t>(checksum ^ invalid_type[index]);
    }
    invalid_type.back() = checksum;
    check(!gameboy::LinkPacketCodec::decode(invalid_type.data(), invalid_type.size()),
          "unknown packet type is rejected even with a valid checksum");

    test_deterministic_round_trips();

    return failures == 0 ? 0 : 1;
}
