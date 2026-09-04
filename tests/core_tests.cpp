#include "gameboy/cpu.hpp"
#include "gameboy/dmg_palette.hpp"
#include "gameboy/emulator.hpp"
#include "gameboy/gameshark.hpp"
#include "gameboy/memory_bus.hpp"
#include "gameboy/rom_library.hpp"
#include "gbb/core_registry.hpp"
#include "gbb/gameboy_core.hpp"
#include "gbb/scene_json.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

static_assert(sizeof(gameboy::Ppu) < 16 * 1024,
              "Keep large PPU buffers heap-backed for Windows stack safety");

constexpr std::uint16_t program_address = 0x0100;
int failures = 0;
std::filesystem::path executable_directory;

void check(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

std::uint32_t state_crc32(const std::uint8_t* data,
                          const std::size_t size) noexcept {
    auto crc = UINT32_C(0xFFFFFFFF);
    for (std::size_t index = 0; index < size; ++index) {
        crc ^= data[index];
        for (unsigned bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^
                  ((crc & 1) != 0 ? UINT32_C(0xEDB88320) : 0);
        }
    }
    return ~crc;
}

void write_little_u32(std::vector<std::uint8_t>& bytes,
                      const std::size_t offset,
                      const std::uint32_t value) {
    for (unsigned byte = 0; byte < 4; ++byte) {
        bytes[offset + byte] =
            static_cast<std::uint8_t>(value >> (byte * 8));
    }
}

std::vector<std::uint8_t> test_rom(
    const std::vector<std::uint8_t>& program = {}) {
    std::vector<std::uint8_t> rom(0x8000, 0);
    const std::string title = "CORE TEST";
    for (std::size_t i = 0; i < title.size(); ++i) {
        rom[0x134 + i] = static_cast<std::uint8_t>(title[i]);
    }
    for (std::size_t i = 0; i < program.size(); ++i) {
        rom[program_address + i] = program[i];
    }
    return rom;
}

std::vector<std::uint8_t> cgb_test_rom(
    const std::vector<std::uint8_t>& program = {}, const bool cgb_only = false) {
    auto rom = test_rom(program);
    rom[0x143] = cgb_only ? 0xC0 : 0x80;
    return rom;
}

std::vector<std::uint8_t> banked_rom(const unsigned banks,
                                     const std::uint8_t type,
                                     const std::uint8_t rom_size_code,
                                     const std::uint8_t ram_size_code) {
    std::vector<std::uint8_t> rom(static_cast<std::size_t>(banks) * 0x4000);
    for (unsigned bank = 0; bank < banks; ++bank) {
        std::fill(rom.begin() + static_cast<std::size_t>(bank) * 0x4000,
                  rom.begin() + static_cast<std::size_t>(bank + 1) * 0x4000,
                  static_cast<std::uint8_t>(bank));
        rom[static_cast<std::size_t>(bank) * 0x4000 + 1] =
            static_cast<std::uint8_t>(bank >> 8);
    }
    const std::string title = "MBC1 TEST";
    for (std::size_t index = 0; index < title.size(); ++index) {
        rom[0x134 + index] = static_cast<std::uint8_t>(title[index]);
    }
    rom[0x147] = type;
    rom[0x148] = rom_size_code;
    rom[0x149] = ram_size_code;
    return rom;
}

void test_cartridge_header() {
    gameboy::Cartridge cartridge{test_rom()};
    check(cartridge.title() == "CORE TEST", "cartridge title is parsed");
    check(cartridge.rom_size() == 0x8000, "ROM size is reported");

    gameboy::Cartridge compatible{cgb_test_rom()};
    gameboy::Cartridge exclusive{cgb_test_rom({}, true)};
    check(compatible.supports_cgb() && !compatible.requires_cgb() &&
              exclusive.supports_cgb() && exclusive.requires_cgb(),
          "the CGB header flag distinguishes enhanced and CGB-only ROMs");

    auto pokemon_blue_rom = test_rom();
    std::fill(pokemon_blue_rom.begin() + 0x134,
              pokemon_blue_rom.begin() + 0x144, 0);
    constexpr std::string_view pokemon_blue = "POKEMON BLUE";
    std::copy(pokemon_blue.begin(), pokemon_blue.end(),
              pokemon_blue_rom.begin() + 0x134);
    pokemon_blue_rom[0x14B] = 0x01;
    gameboy::Cartridge pokemon_blue_cartridge{pokemon_blue_rom};
    check(pokemon_blue_cartridge.cgb_compatibility_palette_id() == 11,
          "Nintendo title checksum selects Pokemon Blue's CGB palette");

    pokemon_blue_rom[0x14B] = 0;
    gameboy::Cartridge unlicensed{std::move(pokemon_blue_rom)};
    check(unlicensed.cgb_compatibility_palette_id() == 0,
          "non-Nintendo cartridges use the default CGB compatibility palette");

    const auto blue_palette = gameboy::cgb_compatibility_palette(11);
    check(blue_palette.background[0] == 0xFFFFFFFF &&
              blue_palette.background[2] == 0xFF0000FF &&
              blue_palette.background[3] == 0xFF000000 &&
              blue_palette.object_0 != blue_palette.background,
          "CGB compatibility palettes expand RGB555 and preserve layer colors");

    auto sgb_rom = test_rom();
    sgb_rom[0x146] = 0x03;
    gameboy::Cartridge sgb_cartridge{sgb_rom};
    check(sgb_cartridge.supports_sgb(),
          "the cartridge header advertises Super Game Boy software");
}

void test_gameboy_camera() {
    // Camera SRAM is fixed hardware and must not depend on the often-invalid
    // generic RAM-size byte in patched/development camera ROM headers.
    gameboy::Cartridge camera{banked_rom(64, 0xFC, 0x05, 0xFF)};
    check(camera.has_camera() && camera.has_battery() &&
              camera.ram_size() == 0x20000,
          "Game Boy Camera cartridges expose camera hardware and save RAM");

    auto hacked_rom = banked_rom(64, 0x1B, 0x05, 0xFF);
    constexpr std::string_view camera_title = "GAMEBOYCAMERA";
    std::copy(camera_title.begin(), camera_title.end(),
              hacked_rom.begin() + 0x134);
    gameboy::Cartridge hacked_camera{std::move(hacked_rom)};
    check(hacked_camera.has_camera() && hacked_camera.ram_size() == 0x20000,
          "MBC-type Game Boy Camera header hacks retain camera hardware");

    camera.write(0x2000, 7);
    check(camera.read(0x4000) == 7,
          "Game Boy Camera mapper selects ROM banks");
    camera.write(0x3000, 12);
    check(camera.read(0x4000) == 12,
          "Game Boy Camera uses one ROM bank register across 2000-3FFF");
    camera.write(0x3000, 0);
    check(camera.read(0x4000) == 0,
          "Game Boy Camera permits ROM bank zero in the switchable window");

    camera.write(0x4000, 0x10);
    camera.write(0xA001, 4);
    check(camera.read(0xA000) == 0,
          "Game Boy Camera registers work without SRAM write enable");
    camera.write(0x4000, 0);
    check(camera.read(0xA000) == 0,
          "Game Boy Camera SRAM can be read while writes are disabled");

    camera.write(0x0000, 0x0A);
    camera.write(0x4000, 3);
    camera.write(0xA000, 0x5A);
    camera.write(0x4000, 2);
    camera.write(0xA000, 0xA5);
    camera.write(0x4000, 3);
    check(camera.read(0xA000) == 0x5A,
          "Game Boy Camera mapper selects independent RAM banks");

    std::array<std::uint8_t,
               gameboy::Cartridge::camera_width *
                   gameboy::Cartridge::camera_height> frame{};
    frame.fill(80);
    camera.set_camera_frame(frame.data(), frame.size());
    camera.write(0x4000, 0x10);
    for (std::uint16_t matrix = 0; matrix < 16; ++matrix) {
        const auto address = static_cast<std::uint16_t>(0xA006 + matrix * 3);
        camera.write(address, 50);
        camera.write(static_cast<std::uint16_t>(address + 1), 100);
        camera.write(static_cast<std::uint16_t>(address + 2), 150);
    }
    camera.write(0xA001, 4); // Unity gain.
    camera.write(0xA002, 0x10); // Unity exposure.
    camera.write(0xA000, 1);
    check((camera.read(0xA000) & 1) == 0,
          "Game Boy Camera capture completes and clears its busy flag");
    camera.write(0x4000, 0);
    check(camera.read(0xA100) == 0x00 && camera.read(0xA101) == 0xFF,
          "Game Boy Camera converts a webcam frame into 2bpp sensor tiles");
    const auto exported_camera_save = camera.export_battery_save();
    check(exported_camera_save.size() > camera.ram_size(),
          "Camera save export includes the captured image payload");
    gameboy::Cartridge imported_camera{banked_rom(64, 0xFC, 0x05, 0xFF)};
    imported_camera.import_battery_save(exported_camera_save);
    imported_camera.write(0x4000, 0);
    check(imported_camera.read(0xA100) == camera.read(0xA100) &&
              imported_camera.read(0xA101) == camera.read(0xA101),
          "Camera save import restores the captured image payload");

    const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto camera_save_base = std::filesystem::temp_directory_path() /
                                  ("gameboy-camera-persistence-" +
                                   std::to_string(unique) + ".gb");
    auto camera_save_path = camera_save_base;
    camera_save_path.replace_extension(".sav");
    const auto capture_persistent_image = [&](gameboy::Cartridge& cartridge) {
        cartridge.set_camera_frame(frame.data(), frame.size());
        cartridge.write(0x4000, 0x10);
        for (std::uint16_t matrix = 0; matrix < 16; ++matrix) {
            const auto address = static_cast<std::uint16_t>(0xA006 + matrix * 3);
            cartridge.write(address, 50);
            cartridge.write(static_cast<std::uint16_t>(address + 1), 100);
            cartridge.write(static_cast<std::uint16_t>(address + 2), 150);
        }
        cartridge.write(0xA001, 4);
        cartridge.write(0xA002, 0x10);
        cartridge.write(0xA000, 1);
        cartridge.write(0x4000, 0);
    };
    std::uint8_t saved_camera_tile[2]{};
    {
        gameboy::Cartridge persistent{banked_rom(64, 0xFC, 0x05, 0x04)};
        persistent.set_persistence_path(camera_save_base);
        capture_persistent_image(persistent);
        saved_camera_tile[0] = persistent.read(0xA100);
        saved_camera_tile[1] = persistent.read(0xA101);
        persistent.flush_battery();
    }
    check(std::filesystem::exists(camera_save_path) &&
              std::filesystem::file_size(camera_save_path) > 0x20000,
          "Game Boy Camera flush stores the captured image alongside SRAM");
    {
        gameboy::Cartridge restored{banked_rom(64, 0xFC, 0x05, 0x04)};
        restored.set_persistence_path(camera_save_base);
        restored.write(0x4000, 0);
        check(restored.read(0xA100) == saved_camera_tile[0] &&
                  restored.read(0xA101) == saved_camera_tile[1],
              "Game Boy Camera images reload from persistent storage");
    }
    std::filesystem::remove(camera_save_path);

    gameboy::Emulator emulator{
        gameboy::Cartridge{banked_rom(64, 0xFC, 0x05, 0x04)}};
    check(emulator.has_camera(),
          "emulator exposes camera cartridges to frontends");
    emulator.bus().write8(0x0000, 0x0A);
    emulator.bus().write8(0x4000, 0x10);
    emulator.bus().write8(0xA006, 42);
    const auto state = emulator.save_state();
    emulator.bus().write8(0xA006, 99);
    emulator.load_state(state);
    check(!state.empty() && emulator.has_camera(),
          "save states preserve Game Boy Camera cartridge state");
}

void test_memory_map() {
    gameboy::MemoryBus bus{gameboy::Cartridge{test_rom()}};
    bus.write8(0xC123, 0x42);
    check(bus.read8(0xC123) == 0x42, "work RAM can be read and written");
    check(bus.read8(0xE123) == 0x42, "echo RAM mirrors work RAM");

    bus.write8(0xE456, 0x99);
    check(bus.read8(0xC456) == 0x99, "echo RAM writes mirror work RAM");

    bus.write16(0xFF80, 0xBEEF);
    check(bus.read16(0xFF80) == 0xBEEF, "16-bit accesses are little endian");

    bus.write8(0xFEA0, 0x12);
    check(bus.read8(0xFEA0) == 0xFF, "unusable memory reads as FF");
}

} // namespace

int main(const int argc, char** argv) {
    try {
        if (argc > 0 && argv != nullptr && argv[0] != nullptr) {
            std::error_code error;
            executable_directory =
                std::filesystem::absolute(argv[0], error).parent_path();
            if (error) executable_directory.clear();
        }
        test_cartridge_header();
        test_gameboy_camera();
        test_memory_map();
    } catch (const std::exception& error) {
        std::cerr << "Unexpected exception: " << error.what() << '\n';
        return 1;
    }

    if (failures == 0) {
        std::cout << "All tests passed\n";
    }
    return failures == 0 ? 0 : 1;
}
