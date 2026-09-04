#include "gameboy/rom_library.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
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
    constexpr std::string_view title = "CORE TEST";
    std::copy(title.begin(), title.end(), rom.begin() + 0x134);
    return rom;
}

void test_metadata_and_deduplication() {
    auto rom = test_rom();
    rom[0x143] = 0x80;
    std::fill(rom.begin() + 0x134, rom.begin() + 0x143, 0);
    constexpr std::string_view title = "POKEMON BLUE";
    std::copy(title.begin(), title.end(), rom.begin() + 0x134);
    rom[0x14A] = 1;
    const auto metadata = gameboy::inspect_rom(
        rom, "Pokemon Blue (USA) (En,Fr,De).gbc");
    check(metadata.title == "POKEMON BLUE" &&
              metadata.platform == gameboy::RomPlatform::game_boy_color &&
              metadata.language == "English, French, German" &&
              metadata.crc32 != 0 &&
              metadata.cover_name == "Pokemon Blue (USA) (En,Fr,De)",
          "ROM library extracts header and filename metadata");
    check(std::string{gameboy::platform_name(metadata.platform)} ==
                  "Game Boy Color" &&
              std::string{gameboy::cover_system_name(metadata.platform)} ==
                  "Nintendo - Game Boy Color",
          "ROM library maps platforms to display and cover-system names");

    const auto imported = gameboy::inspect_rom(
        rom, "0123456789abcdef-Pokemon Blue (USA) (En,Fr,De).gbc");
    check(imported.cover_name == metadata.cover_name &&
              imported.language == metadata.language,
          "ROM library ignores Android's fingerprint filename prefix");

    gameboy::RomLibrary library;
    library.remember("first/Pokemon Blue.gbc", metadata, 100);
    library.remember("renamed/Pokemon Blue.gbc", metadata, 200);
    check(library.entries().size() == 1 &&
              library.entries().front().path ==
                  std::filesystem::path{"renamed/Pokemon Blue.gbc"},
          "ROM library deduplicates renamed copies by fingerprint");

    auto other_rom = test_rom();
    other_rom[0x200] = 1;
    auto other = gameboy::inspect_rom(other_rom, "Other Game (Japan).gb");
    library.remember("Other Game.gb", other, 300);
    check(library.entries().size() == 2 &&
              library.entries().front().metadata.language == "Japanese",
          "ROM library keeps distinct games ordered by recent use");

    const auto german = gameboy::inspect_rom(rom, "Pokemon Blue (Germany).gbc");
    check(german.language == "German",
          "ROM library recognizes localized No-Intro region names");

    const auto directory = std::filesystem::temp_directory_path() /
                           "gbb-rom-library-test";
    std::filesystem::remove_all(directory);
    library.save(directory);
    const auto restored = gameboy::RomLibrary::load(directory);
    check(restored.entries().size() == 2 &&
              restored.entries()[0].metadata.fingerprint == other.fingerprint &&
              restored.entries()[0].metadata.crc32 == other.crc32 &&
              restored.entries()[1].metadata.fingerprint == metadata.fingerprint,
          "ROM library metadata persists in recency order");
    auto removable = restored;
    check(removable.remove(other.fingerprint) && removable.entries().size() == 1 &&
              !removable.remove(other.fingerprint),
          "ROM library entries can be removed by stable fingerprint");
    std::filesystem::remove_all(directory);
}

} // namespace

int main() {
    test_metadata_and_deduplication();
    return failures == 0 ? 0 : 1;
}
