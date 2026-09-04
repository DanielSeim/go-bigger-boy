#include "gameboy/emulator.hpp"
#include "gameboy/memory_bus.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

int failures = 0;

void check(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
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
    constexpr std::string_view title = "MBC1 TEST";
    std::copy(title.begin(), title.end(), rom.begin() + 0x134);
    rom[0x147] = type;
    rom[0x148] = rom_size_code;
    rom[0x149] = ram_size_code;
    return rom;
}

void test_mbc1_rom_banking() {
    gameboy::Cartridge cartridge{banked_rom(128, 0x01, 0x06, 0x00)};
    check(cartridge.read(0x4000) == 1,
          "MBC1 powers up with ROM bank 1 selected");
    cartridge.write(0x2000, 2);
    check(cartridge.read(0x4000) == 2,
          "MBC1 low register selects the switchable ROM bank");
    cartridge.write(0x2000, 0);
    check(cartridge.read(0x4000) == 1,
          "MBC1 remaps low bank value zero to one");
    cartridge.write(0x2000, 2);
    cartridge.write(0x4000, 1);
    check(cartridge.read(0x4000) == 34,
          "MBC1 combines upper and lower ROM bank registers");
    check(cartridge.read(0x0000) == 0,
          "MBC1 mode 0 keeps the lower ROM window at bank zero");
    cartridge.write(0x6000, 1);
    check(cartridge.read(0x0000) == 32 && cartridge.read(0x4000) == 34,
          "MBC1 mode 1 maps upper bits into both ROM windows");

    gameboy::Cartridge masked{banked_rom(16, 0x01, 0x03, 0x00)};
    masked.write(0x2000, 0x10);
    check(masked.read(0x4000) == 0,
          "MBC1 bank-zero translation occurs before physical ROM masking");

    auto multicart_rom = banked_rom(64, 0x01, 0x05, 0x00);
    constexpr std::array<std::uint8_t, 48> nintendo_logo{
        0xCE, 0xED, 0x66, 0x66, 0xCC, 0x0D, 0x00, 0x0B,
        0x03, 0x73, 0x00, 0x83, 0x00, 0x0C, 0x00, 0x0D,
        0x00, 0x08, 0x11, 0x1F, 0x88, 0x89, 0x00, 0x0E,
        0xDC, 0xCC, 0x6E, 0xE6, 0xDD, 0xDD, 0xD9, 0x99,
        0xBB, 0xBB, 0x67, 0x63, 0x6E, 0x0E, 0xEC, 0xCC,
        0xDD, 0xDC, 0x99, 0x9F, 0xBB, 0xB9, 0x33, 0x3E,
    };
    for (const auto offset : {std::size_t{0}, std::size_t{0x40000},
                              std::size_t{0x80000}, std::size_t{0xC0000}}) {
        std::copy(nintendo_logo.begin(), nintendo_logo.end(),
                  multicart_rom.begin() + offset + 0x0104);
    }
    gameboy::Cartridge multicart{std::move(multicart_rom)};
    multicart.write(0x4000, 1);
    multicart.write(0x2000, 2);
    check(multicart.read(0x4000) == 18,
          "MBC1 multicarts wire the upper register at ROM address bit 18");
    multicart.write(0x2000, 0x10);
    check(multicart.read(0x4000) == 16,
          "MBC1 multicarts ignore low-register bit four after zero remapping");
    multicart.write(0x6000, 1);
    check(multicart.read(0x0000) == 16,
          "MBC1 multicart mode one switches the lower ROM window by subgame");
}

void test_mbc2_banking_and_ram() {
    gameboy::Cartridge cartridge{banked_rom(16, 0x05, 0x03, 0x00)};
    check(cartridge.ram_size() == 0x200 && !cartridge.has_battery(),
          "MBC2 provides 512 built-in four-bit RAM cells");
    cartridge.write(0x2100, 2);
    check(cartridge.read(0x4000) == 2,
          "MBC2 uses address bit eight to select its ROM bank register");
    cartridge.write(0x2000, 3);
    check(cartridge.read(0x4000) == 2,
          "MBC2 ignores ROM bank writes with address bit eight clear");
    cartridge.write(0x0000, 0x0A);
    cartridge.write(0xA000, 0xAB);
    check(cartridge.read(0xA000) == 0xFB &&
              cartridge.read(0xA200) == 0xFB,
          "MBC2 RAM stores low nibbles, reads high bits set, and mirrors");
    cartridge.write(0x0000, 0);
    check(cartridge.read(0xA000) == 0xFF,
          "disabled MBC2 RAM reads as open bus");

    gameboy::Cartridge battery{banked_rom(16, 0x06, 0x03, 0x00)};
    check(battery.has_battery() && battery.export_battery_ram().size() == 0x200,
          "MBC2 battery cartridges expose their internal RAM for persistence");
}

void test_mbc1_ram_banking() {
    gameboy::Cartridge cartridge{banked_rom(32, 0x02, 0x04, 0x03)};
    check(cartridge.ram_size() == 0x8000 && !cartridge.has_battery(),
          "MBC1+RAM allocates header-declared non-battery RAM");
    cartridge.write(0xA000, 0x11);
    check(cartridge.read(0xA000) == 0xFF,
          "disabled MBC1 RAM reads as open bus and ignores writes");
    cartridge.write(0x0000, 0x1A);
    cartridge.write(0xA000, 0x10);
    cartridge.write(0x6000, 1);
    for (std::uint8_t bank = 1; bank < 4; ++bank) {
        cartridge.write(0x4000, bank);
        cartridge.write(0xA000, static_cast<std::uint8_t>(0x10 + bank));
    }
    cartridge.write(0x4000, 0);
    check(cartridge.read(0xA000) == 0x10,
          "MBC1 advanced mode retains RAM bank zero");
    for (std::uint8_t bank = 1; bank < 4; ++bank) {
        cartridge.write(0x4000, bank);
        check(cartridge.read(0xA000) == static_cast<std::uint8_t>(0x10 + bank),
              "MBC1 advanced mode selects independent RAM banks");
    }
    cartridge.write(0x6000, 0);
    check(cartridge.read(0xA000) == 0x10,
          "MBC1 simple mode locks external RAM to bank zero");
    cartridge.write(0x0000, 0);
    check(cartridge.read(0xA000) == 0xFF,
          "disabling MBC1 RAM restores open-bus reads");

    gameboy::Cartridge large{banked_rom(64, 0x02, 0x05, 0x02)};
    large.write(0x0000, 0x0A);
    large.write(0x6000, 1);
    large.write(0x4000, 3);
    large.write(0xA000, 0x77);
    large.write(0x4000, 0);
    check(large.read(0xA000) == 0x77,
          "large-ROM MBC1 wiring keeps its 8 KiB RAM unbanked");
}

void test_battery_ram_persistence() {
    const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto rom_path = std::filesystem::temp_directory_path() /
                          ("gameboy-battery-test-" + std::to_string(unique) +
                           ".gb");
    auto save_path = rom_path;
    save_path.replace_extension(".sav");
    const auto rom = banked_rom(4, 0x03, 0x01, 0x02);
    {
        std::ofstream output(rom_path, std::ios::binary);
        output.write(reinterpret_cast<const char*>(rom.data()),
                     static_cast<std::streamsize>(rom.size()));
    }
    {
        auto cartridge = gameboy::Cartridge::from_file(rom_path);
        check(cartridge.has_battery() && cartridge.ram_size() == 0x2000,
              "battery cartridge reports its persistent RAM capacity");
        cartridge.write(0x0000, 0x0A);
        cartridge.write(0xA000, 0x5A);
        cartridge.write(0xBFFF, 0xC3);
        cartridge.flush_battery();
    }
    check(std::filesystem::exists(save_path) &&
              std::filesystem::file_size(save_path) == 0x2000,
          "battery flush writes a complete sibling .sav file");
    {
        auto cartridge = gameboy::Cartridge::from_file(rom_path);
        cartridge.write(0x0000, 0x0A);
        check(cartridge.read(0xA000) == 0x5A &&
                  cartridge.read(0xBFFF) == 0xC3,
              "battery RAM reloads into a new cartridge instance");
    }
    std::filesystem::remove(save_path);
    std::filesystem::remove(rom_path);
}

void test_battery_data_import_export() {
    gameboy::Cartridge source{banked_rom(4, 0x03, 0x01, 0x02)};
    source.write(0x0000, 0x0A);
    source.write(0xA000, 0x5A);
    source.write(0xBFFF, 0xC3);
    const auto save = source.export_battery_ram();
    check(save.size() == 0x2000 && save.front() == 0x5A && save.back() == 0xC3,
          "battery RAM exports in desktop .sav byte order");

    gameboy::Cartridge restored{banked_rom(4, 0x03, 0x01, 0x02)};
    restored.import_battery_ram(save);
    restored.write(0x0000, 0x0A);
    check(restored.read(0xA000) == 0x5A && restored.read(0xBFFF) == 0xC3,
          "battery RAM imports into a fresh cartridge");

    auto wrong_size = save;
    wrong_size.pop_back();
    auto rejected = false;
    try {
        restored.import_battery_ram(wrong_size);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    check(rejected, "battery RAM import rejects a save for another cartridge");

    gameboy::Cartridge clock{banked_rom(2, 0x0F, 0x00, 0x00)};
    clock.write(0x0000, 0x0A);
    clock.write(0x4000, 0x0C);
    clock.write(0xA000, 0x40); // Halt for deterministic persistence.
    clock.write(0x4000, 0x08);
    clock.write(0xA000, 47);
    const auto rtc = clock.export_rtc_data();
    check(clock.has_rtc() && rtc.size() == 21 && rtc[8] == 47,
          "MBC3 RTC exports in the desktop .rtc format");

    gameboy::Cartridge restored_clock{banked_rom(2, 0x0F, 0x00, 0x00)};
    restored_clock.import_rtc_data(rtc);
    restored_clock.write(0x0000, 0x0A);
    restored_clock.write(0x6000, 0);
    restored_clock.write(0x6000, 1);
    restored_clock.write(0x4000, 0x08);
    check(restored_clock.read(0xA000) == 47,
          "MBC3 RTC imports into a fresh cartridge");
}

void test_mbc3_banking_and_rtc() {
    gameboy::Cartridge cartridge{banked_rom(128, 0x10, 0x06, 0x03)};
    check(cartridge.read(0x4000) == 1,
          "MBC3 powers up with ROM bank 1 selected");
    cartridge.write(0x2000, 0);
    check(cartridge.read(0x4000) == 1,
          "MBC3 remaps switchable ROM bank zero to bank one");
    cartridge.write(0x2000, 0x20);
    check(cartridge.read(0x4000) == 0x20 && cartridge.read(0x0000) == 0,
          "MBC3 selects a seven-bit ROM bank while keeping bank zero fixed");

    cartridge.write(0x0000, 0x0A);
    for (std::uint8_t bank = 0; bank < 4; ++bank) {
        cartridge.write(0x4000, bank);
        cartridge.write(0xA000, static_cast<std::uint8_t>(0x30 + bank));
    }
    for (std::uint8_t bank = 0; bank < 4; ++bank) {
        cartridge.write(0x4000, bank);
        check(cartridge.read(0xA000) == static_cast<std::uint8_t>(0x30 + bank),
              "MBC3 selects independent external RAM banks");
    }

    cartridge.write(0x4000, 0x0C);
    cartridge.write(0xA000, 0xC1); // Halt, carry, and day bit 8.
    cartridge.write(0x4000, 0x08);
    cartridge.write(0xA000, 42);
    cartridge.write(0x4000, 0x09);
    cartridge.write(0xA000, 37);
    cartridge.write(0x4000, 0x0A);
    cartridge.write(0xA000, 19);
    cartridge.write(0x4000, 0x0B);
    cartridge.write(0xA000, 0xA5);
    cartridge.write(0x6000, 0);
    cartridge.write(0x6000, 1);
    cartridge.write(0x4000, 0x08);
    check(cartridge.read(0xA000) == 42,
          "MBC3 latches and reads the RTC seconds register");
    cartridge.write(0x4000, 0x0A);
    check(cartridge.read(0xA000) == 19,
          "MBC3 latches and reads the RTC hours register");
    cartridge.write(0x4000, 0x0B);
    check(cartridge.read(0xA000) == 0xA5,
          "MBC3 latches and reads the RTC day counter");
    cartridge.write(0x4000, 0x0C);
    check(cartridge.read(0xA000) == 0xC1,
          "MBC3 preserves RTC day-high, halt, and carry flags");

    cartridge.write(0x4000, 0x08);
    cartridge.write(0xA000, 7);
    check(cartridge.read(0xA000) == 42,
          "MBC3 RTC reads remain stable until the next latch transition");
    cartridge.write(0x6000, 0);
    cartridge.write(0x6000, 1);
    check(cartridge.read(0xA000) == 7,
          "MBC3 refreshes its RTC snapshot on a zero-to-one latch transition");
    cartridge.write(0x0000, 0);
    check(cartridge.read(0xA000) == 0xFF,
          "disabled MBC3 RAM and RTC registers read as open bus");
}

void test_mbc3_rtc_persistence() {
    const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto rom_path = std::filesystem::temp_directory_path() /
                          ("gameboy-rtc-test-" + std::to_string(unique) + ".gb");
    auto rtc_path = rom_path;
    rtc_path.replace_extension(".rtc");
    auto save_path = rom_path;
    save_path.replace_extension(".sav");
    const auto rom = banked_rom(2, 0x0F, 0x00, 0x00);
    {
        std::ofstream output(rom_path, std::ios::binary);
        output.write(reinterpret_cast<const char*>(rom.data()),
                     static_cast<std::streamsize>(rom.size()));
    }
    {
        auto cartridge = gameboy::Cartridge::from_file(rom_path);
        cartridge.write(0x0000, 0x0A);
        cartridge.write(0x4000, 0x0C);
        cartridge.write(0xA000, 0x40); // Halt for deterministic persistence.
        cartridge.write(0x4000, 0x08);
        cartridge.write(0xA000, 51);
        cartridge.flush_battery();
    }
    check(std::filesystem::exists(rtc_path) &&
              std::filesystem::file_size(rtc_path) == 21 &&
              !std::filesystem::exists(save_path),
          "timer-only MBC3 cartridges persist a sibling RTC file without RAM");
    {
        auto cartridge = gameboy::Cartridge::from_file(rom_path);
        cartridge.write(0x0000, 0x0A);
        cartridge.write(0x6000, 0);
        cartridge.write(0x6000, 1);
        cartridge.write(0x4000, 0x08);
        check(cartridge.read(0xA000) == 51,
              "MBC3 reloads persisted RTC state into a new cartridge instance");
    }
    std::filesystem::remove(rtc_path);
    std::filesystem::remove(rom_path);
}

void test_mbc5_banking_and_rumble() {
    gameboy::Cartridge cartridge{banked_rom(512, 0x1B, 0x08, 0x04)};
    check(cartridge.has_battery() && cartridge.read(0x4000) == 1,
          "MBC5 battery cartridges load with ROM bank one selected");
    cartridge.write(0x2000, 0);
    check(cartridge.read(0x4000) == 0,
          "MBC5 permits ROM bank zero in the switchable window");
    cartridge.write(0x2000, 1);
    cartridge.write(0x3000, 1);
    check(cartridge.read(0x4000) == 1 && cartridge.read(0x4001) == 1,
          "MBC5 combines its low eight and high one ROM-bank bits");
    check(cartridge.read(0x0001) == 0,
          "MBC5 keeps the lower ROM window fixed at bank zero");

    cartridge.write(0x0000, 0x0A);
    cartridge.write(0x4000, 0);
    cartridge.write(0xA000, 0x55);
    cartridge.write(0x4000, 15);
    cartridge.write(0xA000, 0xAA);
    cartridge.write(0x4000, 0);
    check(cartridge.read(0xA000) == 0x55,
          "MBC5 selects external RAM bank zero");
    cartridge.write(0x4000, 15);
    check(cartridge.read(0xA000) == 0xAA,
          "MBC5 uses all four RAM-bank bits on non-rumble cartridges");

    gameboy::Cartridge rumble{banked_rom(2, 0x1E, 0x00, 0x03)};
    check(rumble.has_rumble() && !rumble.rumble_active(),
          "MBC5 rumble cartridges expose an initially inactive motor");
    rumble.write(0x0000, 0x0A);
    rumble.write(0x4000, 0x0B); // Rumble on, RAM bank 3.
    rumble.write(0xA000, 0x77);
    check(rumble.rumble_active(), "MBC5 bit three activates rumble");
    rumble.write(0x4000, 3);
    check(!rumble.rumble_active() && rumble.read(0xA000) == 0x77,
          "MBC5 rumble uses only the low three bits for its RAM bank");

    gameboy::Emulator rumble_emulator{
        gameboy::Cartridge{banked_rom(2, 0x1E, 0x00, 0x03)}};
    rumble_emulator.bus().write8(0x4000, 0x08);
    check(rumble_emulator.has_rumble() && rumble_emulator.rumble_active(),
          "emulator exposes MBC5 rumble state to platform frontends");
}


} // namespace

int main() {
    test_mbc1_rom_banking();
    test_mbc2_banking_and_ram();
    test_mbc1_ram_banking();
    test_battery_ram_persistence();
    test_battery_data_import_export();
    test_mbc3_banking_and_rtc();
    test_mbc3_rtc_persistence();
    test_mbc5_banking_and_rumble();
    return failures == 0 ? 0 : 1;
}
