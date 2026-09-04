#pragma once

#include <cstdint>

namespace gameboy {
class Emulator;
class MemoryBus;
}

namespace gbb::sdl {

enum class PokemonUiState : std::uint8_t {
    other,
    waiting,
    trade_completed,
};

[[nodiscard]] bool is_pokemon_gen1(const gameboy::Emulator& emulator);
[[nodiscard]] PokemonUiState pokemon_ui_state(const gameboy::MemoryBus& bus);
[[nodiscard]] const char* pokemon_ui_state_name(PokemonUiState state) noexcept;

} // namespace gbb::sdl
