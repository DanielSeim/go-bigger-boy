#include "gameboy/emulator.hpp"
#include "gameboy/gameshark.hpp"

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(const bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

std::vector<std::uint8_t> test_rom() {
    std::vector<std::uint8_t> rom(0x8000, 0);
    const std::string title = "GAMESHARK TEST";
    for (std::size_t index = 0; index < title.size(); ++index) {
        rom[0x134 + index] = static_cast<std::uint8_t>(title[index]);
    }
    return rom;
}

void test_gameshark_contract() {
    const auto writes = gameboy::parse_gameshark_code("0199-00C0 + 010101C0");
    check(writes.size() == 2 && writes[0].address == 0xC000 &&
              writes[0].value == 0x99 && writes[1].address == 0xC001 &&
              writes[1].value == 0x01,
          "GameShark parser decodes type-01 values and little-endian addresses");

    bool rejected = false;
    try {
        static_cast<void>(gameboy::parse_gameshark_code("009900C0"));
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    check(rejected, "GameShark parser rejects unsupported code types");

    const std::string archive =
        "cheats = 2\n"
        "cheat0_desc = \"Infinite lives\"\n"
        "cheat0_code = \"019900C0\"\n"
        "cheat0_enable = true\n"
        "cheat1_desc = \"Unsupported\"\n"
        "cheat1_code = \"009900C0\"\n";
    auto cheats = gameboy::parse_libretro_cheats(archive);
    check(cheats.size() == 1 && cheats[0].description == "Infinite lives" &&
              cheats[0].enabled && cheats[0].from_archive,
          "Libretro cheat parser retains supported GameShark entries");

    gameboy::Emulator emulator{gameboy::Cartridge{test_rom()}};
    gameboy::apply_gameshark_cheats(cheats, emulator.bus());
    check(emulator.bus().read8(0xC000) == 0x99,
          "enabled GameShark cheats write emulator memory");
    const auto round_trip = gameboy::parse_libretro_cheats(
        gameboy::serialize_libretro_cheats(cheats), false);
    check(round_trip.size() == 1 && round_trip[0].enabled &&
              round_trip[0].from_archive && round_trip[0].code == "019900C0",
          "GameShark cheat files preserve enabled and source metadata");
}

} // namespace

int main() {
    test_gameshark_contract();
    return failures == 0 ? 0 : 1;
}
