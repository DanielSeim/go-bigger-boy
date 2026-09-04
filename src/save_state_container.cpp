#include "save_state_container.hpp"

#include "save_state_format.hpp"

#include <array>
#include <string>

namespace gameboy::save_state_container {
namespace {

constexpr std::array<std::uint8_t, 8> state_magic{
    'G', 'B', 'B', 'S', 'T', 'A', 'T', 'E',
};

using save_state_format::Reader;
using save_state_format::Writer;
using save_state_format::crc32;

} // namespace

std::vector<std::uint8_t> encode(
    const std::uint64_t rom_fingerprint,
    const std::vector<std::uint8_t>& payload) {
    if (payload.size() > maximum_state_size - header_size) {
        throw SaveStateError("Save state payload is unexpectedly large");
    }

    Writer state;
    state.bytes(state_magic.data(), state_magic.size());
    state.u32(current_version);
    state.u64(rom_fingerprint);
    state.u32(static_cast<std::uint32_t>(payload.size()));
    state.u32(crc32(payload.data(), payload.size()));
    state.bytes(payload.data(), payload.size());
    return state.take();
}

DecodedState decode(const std::vector<std::uint8_t>& state,
                    const std::uint64_t rom_fingerprint) {
    if (state.size() > maximum_state_size) {
        throw SaveStateError("Save state is too large");
    }

    Reader header(state);
    std::array<std::uint8_t, state_magic.size()> magic{};
    header.bytes(magic.data(), magic.size());
    if (magic != state_magic) throw SaveStateError("Not a GBB save state");

    const auto version = header.u32();
    if (version < oldest_supported_version || version > current_version) {
        throw SaveStateError("Unsupported save-state version: " +
                             std::to_string(version));
    }
    if (header.u64() != rom_fingerprint) {
        throw SaveStateError("Save state belongs to a different ROM");
    }
    const auto payload_size = static_cast<std::size_t>(header.u32());
    const auto expected_crc = header.u32();
    if (payload_size != header.remaining()) {
        throw SaveStateError("Save-state payload size is invalid");
    }
    const auto payload_position = header.position();
    if (crc32(state.data() + payload_position, payload_size) != expected_crc) {
        throw SaveStateError("Save-state checksum does not match");
    }

    DecodedState decoded{version, {}};
    decoded.payload.assign(state.begin() + payload_position, state.end());
    return decoded;
}

} // namespace gameboy::save_state_container
