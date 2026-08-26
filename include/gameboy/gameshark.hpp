#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace gameboy {

class MemoryBus;

struct GameSharkWrite {
    std::uint16_t address{};
    std::uint8_t value{};
};

struct GameSharkCheat {
    std::string description;
    std::string code;
    bool enabled{};
    bool from_archive{};
    std::vector<GameSharkWrite> writes;
};

// Parses one or more conventional Game Boy GameShark type-01 codes. Codes
// may be joined with '+', commas, or whitespace. Throws invalid_argument for
// malformed or unsupported codes.
[[nodiscard]] std::vector<GameSharkWrite> parse_gameshark_code(
    std::string_view code);

[[nodiscard]] std::vector<GameSharkCheat> parse_libretro_cheats(
    std::string_view text, bool from_archive = true);
[[nodiscard]] std::string serialize_libretro_cheats(
    const std::vector<GameSharkCheat>& cheats);
void apply_gameshark_cheats(const std::vector<GameSharkCheat>& cheats,
                            MemoryBus& bus);

} // namespace gameboy
