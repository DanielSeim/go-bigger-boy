#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace gameboy::save_state_container {

// The container is deliberately independent from the emulator's hardware
// codec. This keeps framing, ROM identity, and integrity checks reusable by
// tools that inspect or transport save states without understanding hardware
// fields.
constexpr std::size_t header_size = 28;
constexpr std::size_t maximum_state_size = 2 * 1024 * 1024;
constexpr std::uint32_t current_version = 23;
constexpr std::uint32_t oldest_supported_version = 1;

struct DecodedState {
    std::uint32_t version{};
    std::vector<std::uint8_t> payload;
};

[[nodiscard]] std::vector<std::uint8_t> encode(
    std::uint64_t rom_fingerprint,
    const std::vector<std::uint8_t>& payload);

[[nodiscard]] DecodedState decode(const std::vector<std::uint8_t>& state,
                                  std::uint64_t rom_fingerprint);

} // namespace gameboy::save_state_container
