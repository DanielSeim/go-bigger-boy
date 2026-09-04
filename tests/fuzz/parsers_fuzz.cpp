#include "gameboy/link_transport.hpp"
#include "gameboy/ppu.hpp"
#include "gameboy/save_state_error.hpp"
#include "gbb/settings.hpp"
#include "gbb/trace_parser.hpp"

#include "save_state_container.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <string_view>
#include <vector>

namespace {

constexpr std::size_t sgb_packet_size = 16U * 7U;
constexpr std::size_t settings_fuzz_max_bytes = 2U * 1024U * 1024U;

void fuzz_sgb(const std::uint8_t* data, const std::size_t size) {
    std::array<std::uint8_t, sgb_packet_size> packet{};
    const auto copied = std::min(size, packet.size());
    std::copy_n(data, copied, packet.begin());

    gameboy::Ppu ppu;
    ppu.set_sgb_mode(true);
    // The implementation must treat every short packet as incomplete and
    // every command ID as untrusted input. This call is intentionally made
    // with the original byte count, bounded by the fixed packet storage.
    ppu.apply_sgb_command(packet, copied);
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                       const std::size_t size) {
    if (data == nullptr) return 0;

    const std::string_view text{
        reinterpret_cast<const char*>(data),
        std::min(size, settings_fuzz_max_bytes)};
    (void)gbb::parse_settings_text(text);
    (void)gbb::parse_trace(text);

    (void)gameboy::LinkPacketCodec::decode(data, size);

    // Save-state decoding is expected to reject arbitrary bytes. Catching the
    // format exception is part of the harness contract; sanitizer failures or
    // unexpected process termination remain visible to libFuzzer.
    try {
        const auto state_size = std::min(
            size, gameboy::save_state_container::maximum_state_size + 1U);
        const std::vector<std::uint8_t> state(data, data + state_size);
        (void)gameboy::save_state_container::decode(state, 0);
    } catch (const gameboy::SaveStateError&) {
    }

    fuzz_sgb(data, size);
    return 0;
}
