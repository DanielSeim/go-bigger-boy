#include "pokemon_link_diagnostics.hpp"

#include "gameboy/cartridge.hpp"
#include "gameboy/emulator.hpp"
#include "gameboy/memory_bus.hpp"

#include <array>

namespace gbb::sdl {
namespace {

template <std::size_t Size>
bool vram_sequence(const gameboy::MemoryBus& bus,
                  const std::uint16_t map_offset,
                  const std::array<std::uint8_t, Size>& sequence) {
    for (unsigned row = 0; row < 18; ++row) {
        for (unsigned column = 0; column + sequence.size() <= 32; ++column) {
            const auto start = static_cast<std::uint16_t>(
                map_offset + row * 32U + column);
            auto matches = true;
            for (std::size_t index = 0; index < sequence.size(); ++index) {
                if (bus.debug_read_vram(
                        0, static_cast<std::uint16_t>(start + index)) !=
                    sequence[index]) {
                    matches = false;
                    break;
                }
            }
            if (matches) return true;
        }
    }
    return false;
}

} // namespace

bool is_pokemon_gen1(const gameboy::Emulator& emulator) {
    const auto title = emulator.bus().cartridge().title();
    return title == "POKEMON RED" || title == "POKEMON BLUE" ||
           title == "POKEMON YELLOW";
}

PokemonUiState pokemon_ui_state(const gameboy::MemoryBus& bus) {
    constexpr std::array<std::uint8_t, 13> waiting_german{
        0x81, 0x88, 0x93, 0x93, 0x84, 0x7f, 0x96,
        0x80, 0x91, 0x93, 0x84, 0x8d, 0xe7};
    constexpr std::array<std::uint8_t, 11> waiting_english{
        0x96, 0x80, 0x88, 0x93, 0x88, 0x8d, 0x86,
        0xae, 0xae, 0xae, 0xe7};
    constexpr std::array<std::uint8_t, 17> completed_german{
        0x93, 0x80, 0x94, 0x92, 0x82, 0x87, 0x7f, 0x95, 0x8e,
        0x8b, 0x8b, 0x99, 0x8e, 0x86, 0x84, 0x8d, 0xe7};
    constexpr std::array<std::uint8_t, 16> completed_english{
        0x93, 0x91, 0x80, 0x83, 0x84, 0x7f, 0x82, 0x8e,
        0x8c, 0x8f, 0x8b, 0x84, 0x93, 0x84, 0x83, 0xe7};
    constexpr std::array<std::uint16_t, 2> maps{0x1800, 0x1c00};
    for (const auto map : maps) {
        if (vram_sequence(bus, map, waiting_german) ||
            vram_sequence(bus, map, waiting_english)) {
            return PokemonUiState::waiting;
        }
        if (vram_sequence(bus, map, completed_german) ||
            vram_sequence(bus, map, completed_english)) {
            return PokemonUiState::trade_completed;
        }
    }
    return PokemonUiState::other;
}

const char* pokemon_ui_state_name(const PokemonUiState state) noexcept {
    switch (state) {
    case PokemonUiState::waiting: return "waiting";
    case PokemonUiState::trade_completed: return "trade_completed";
    case PokemonUiState::other: return "other";
    }
    return "other";
}

} // namespace gbb::sdl
