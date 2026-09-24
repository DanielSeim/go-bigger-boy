#include "gameboy/cartridge.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
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
    for (std::size_t index = 0; index < title.size(); ++index) {
        rom[0x134 + index] = static_cast<std::uint8_t>(title[index]);
    }
    return rom;
}

void test_file_loading() {
    const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto path = std::filesystem::temp_directory_path() /
                      ("gameboy-loader-test-" + std::to_string(unique) + ".gb");
    const auto rom = test_rom();
    {
        std::ofstream output(path, std::ios::binary);
        output.write(reinterpret_cast<const char*>(rom.data()),
                     static_cast<std::streamsize>(rom.size()));
    }
    const auto cartridge = gameboy::Cartridge::from_file(path);
    std::filesystem::remove(path);
    check(cartridge.rom_size() == rom.size() && cartridge.title() == "CORE TEST",
          "cartridge loader accepts a successfully read ROM file");
}

void test_rom_fingerprint_is_cached_and_stable_across_move() {
    const auto rom = test_rom();
    auto expected = UINT64_C(14695981039346656037);
    for (const auto byte : rom) {
        expected ^= byte;
        expected *= UINT64_C(1099511628211);
    }
    gameboy::Cartridge cartridge{rom};
    check(cartridge.rom_fingerprint() == expected,
          "ROM fingerprint matches the stable byte hash");
    gameboy::Cartridge moved{std::move(cartridge)};
    check(moved.rom_fingerprint() == expected,
          "cached ROM fingerprint survives cartridge moves");
}

} // namespace

int main() {
    test_file_loading();
    test_rom_fingerprint_is_cached_and_stable_across_move();
    return failures == 0 ? 0 : 1;
}
