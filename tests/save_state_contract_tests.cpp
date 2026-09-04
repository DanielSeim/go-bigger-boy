#include "gameboy/emulator.hpp"
#include "gameboy/ppu.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <exception>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr std::uint16_t program_address = 0x0100;
int failures = 0;

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

void test_save_state_round_trip_and_validation() {
    auto rom = banked_rom(4, 0x03, 0x01, 0x03);
    const std::array<std::uint8_t, 7> program{
        0x3E, 0x42,       // LD A,42
        0xEA, 0x00, 0xC0, // LD (C000),A
        0x04,             // INC B
        0x00,             // NOP
    };
    std::copy(program.begin(), program.end(), rom.begin() + program_address);

    gameboy::Emulator emulator{gameboy::Cartridge{rom}};
    emulator.bus().write8(0x0000, 0x0A);
    emulator.bus().write8(0x6000, 1);
    emulator.bus().write8(0x4000, 2);
    emulator.bus().write8(0xA123, 0x5A);
    emulator.bus().write8(0xFF07, 0x05);
    emulator.bus().write8(0xFF06, 0x77);
    emulator.bus().write8(0xFF24, 0x77);
    emulator.bus().write8(0xFF25, 0x22);
    emulator.bus().write8(0xFF17, 0xF0);
    emulator.bus().write8(0xFF19, 0x80);
    emulator.set_button(gameboy::Button::start, true);
    static_cast<void>(emulator.step());
    static_cast<void>(emulator.step());
    emulator.bus().tick(1234);

    const auto saved = emulator.save_state();
    const auto saved_pc = emulator.cpu().registers().pc;
    const auto saved_cycles = emulator.cpu().total_cycles();
    check(saved.size() > 100000 && emulator.bus().read8(0xA123) == 0x5A,
          "save states include framebuffer, mapper RAM, and subsystem state");

    auto startup_snapshot_storage = std::make_unique<gameboy::Emulator>(
        gameboy::Cartridge{rom});
    auto& startup_snapshot = *startup_snapshot_storage;
    startup_snapshot.bus().tick(79);
    const auto startup_saved = startup_snapshot.save_state();
    startup_snapshot.bus().tick(1);
    check((startup_snapshot.bus().read8(0xFF41) & 0x03) == 3,
          "LCD startup advances after a saved boundary");
    startup_snapshot.load_state(startup_saved);
    check((startup_snapshot.bus().read8(0xFF41) & 0x03) == 0,
          "save states restore the LCD startup phase");
    startup_snapshot.bus().tick(1);
    check((startup_snapshot.bus().read8(0xFF41) & 0x03) == 3,
          "restored LCD startup resumes on the original dot");

    auto pipeline_snapshot_storage = std::make_unique<gameboy::Emulator>(
        gameboy::Cartridge{rom});
    auto& pipeline_snapshot = *pipeline_snapshot_storage;
    pipeline_snapshot.bus().tick(452 + 80);
    const auto pipeline_saved = pipeline_snapshot.save_state();
    check((pipeline_snapshot.bus().read8(0xFF41) & 0x03) == 2 &&
              pipeline_snapshot.bus().read8(0x8000) == 0xFF,
          "internal mode 3 can precede its CPU-visible STAT mode");
    pipeline_snapshot.bus().tick(1);
    pipeline_snapshot.load_state(pipeline_saved);
    check((pipeline_snapshot.bus().read8(0xFF41) & 0x03) == 2 &&
              pipeline_snapshot.bus().read8(0x8000) == 0xFF,
          "save states restore the internal STAT pipeline phase");
    pipeline_snapshot.bus().tick(1);
    check((pipeline_snapshot.bus().read8(0xFF41) & 0x03) == 3,
          "restored STAT mode becomes visible on the original dot");

    constexpr std::size_t state_header_size = 28;
    constexpr std::size_t version_two_dma_size = 7;
    constexpr std::size_t version_three_ppu_size = 6;
    constexpr std::size_t version_four_cgb_size =
        0x6000 + 0x2000 + 0x40 + 0x40 + 4 + 6 + 2;
    constexpr std::size_t version_five_ppu_size = 1;
    constexpr std::size_t version_six_state_size = 5;
    constexpr std::size_t version_seven_camera_size = 1;
    constexpr std::size_t version_eight_ppu_size = 1;
    constexpr std::size_t version_ten_window_latch_size = 4;
    constexpr std::size_t version_eleven_fetcher_size = 2;
    constexpr std::size_t version_twelve_sprite_size = 1;
    constexpr std::size_t version_thirteen_sprite_fetch_size = 8;
    constexpr std::size_t version_fourteen_sprite_deadline_size = 40;
    constexpr std::size_t version_fifteen_sprite_render_size = 48;
    constexpr std::size_t version_sixteen_audio_integrator_size = 8;
    constexpr std::size_t version_seventeen_background_history_size =
        gameboy::Ppu::screen_width * 3;
    constexpr std::size_t version_eighteen_object_deadline_size =
        gameboy::Ppu::screen_width;
    constexpr std::size_t version_nineteen_apu_size = 6;
    constexpr std::size_t version_twenty_timing_size = 3;
    constexpr std::size_t version_twenty_one_pulse_timing_size = 10;
    // Version 23 adds SGB border transfer latches and mask state after the
    // version 22 joypad/parser, palette, and attribute block. Strip both
    // when constructing the legacy v1-v21
    // fixtures below, just like the earlier version deltas.
    constexpr std::size_t version_twenty_two_sgb_size = 237 + 393;
    constexpr std::size_t version_twenty_three_sgb_border_size =
        1 + 0x2000 + 0x1000;
    constexpr std::size_t version_nine_fetcher_size =
        737 + version_ten_window_latch_size + version_eleven_fetcher_size +
        version_twelve_sprite_size + version_thirteen_sprite_fetch_size +
        version_fourteen_sprite_deadline_size + version_fifteen_sprite_render_size;
    auto legacy_saved = saved;
    legacy_saved.resize(legacy_saved.size() -
                        version_twenty_three_sgb_border_size -
                        version_twenty_two_sgb_size -
                        version_twenty_one_pulse_timing_size -
                        version_twenty_timing_size -
                        version_nineteen_apu_size -
                        version_eighteen_object_deadline_size -
                        version_seventeen_background_history_size -
                        version_sixteen_audio_integrator_size);
    auto version_one = legacy_saved;
    version_one.resize(version_one.size() - version_two_dma_size -
                       version_three_ppu_size - version_four_cgb_size -
                       version_five_ppu_size - version_six_state_size -
                       version_seven_camera_size - version_eight_ppu_size -
                       version_nine_fetcher_size);
    version_one[8] = 1;
    const auto old_payload_size = static_cast<std::uint32_t>(
        version_one.size() - state_header_size);
    write_little_u32(version_one, 20, old_payload_size);
    write_little_u32(
        version_one, 24,
        state_crc32(version_one.data() + state_header_size, old_payload_size));
    gameboy::Emulator old_state_loader{gameboy::Cartridge{rom}};
    old_state_loader.load_state(version_one);
    check(old_state_loader.cpu().registers().pc == saved_pc &&
              old_state_loader.cpu().total_cycles() == saved_cycles &&
              old_state_loader.bus().read8(0xA123) == 0x5A,
          "version 1 save states remain loadable after adding DMA state");

    auto version_two = legacy_saved;
    version_two.resize(version_two.size() - version_three_ppu_size -
                       version_four_cgb_size - version_five_ppu_size -
                       version_six_state_size - version_seven_camera_size -
                       version_eight_ppu_size - version_nine_fetcher_size);
    version_two[8] = 2;
    const auto version_two_payload_size = static_cast<std::uint32_t>(
        version_two.size() - state_header_size);
    write_little_u32(version_two, 20, version_two_payload_size);
    write_little_u32(
        version_two, 24,
        state_crc32(version_two.data() + state_header_size,
                    version_two_payload_size));
    gameboy::Emulator version_two_loader{gameboy::Cartridge{rom}};
    version_two_loader.load_state(version_two);
    check(version_two_loader.cpu().registers().pc == saved_pc &&
              version_two_loader.cpu().total_cycles() == saved_cycles &&
              version_two_loader.bus().read8(0xA123) == 0x5A,
          "version 2 save states remain loadable after adding PPU timing state");

    auto version_three = legacy_saved;
    version_three.resize(version_three.size() - version_four_cgb_size -
                         version_five_ppu_size - version_six_state_size -
                         version_seven_camera_size - version_eight_ppu_size -
                         version_nine_fetcher_size);
    version_three[8] = 3;
    const auto version_three_payload_size = static_cast<std::uint32_t>(
        version_three.size() - state_header_size);
    write_little_u32(version_three, 20, version_three_payload_size);
    write_little_u32(
        version_three, 24,
        state_crc32(version_three.data() + state_header_size,
                    version_three_payload_size));
    gameboy::Emulator version_three_loader{gameboy::Cartridge{rom}};
    version_three_loader.load_state(version_three);
    check(version_three_loader.cpu().registers().pc == saved_pc &&
              version_three_loader.cpu().total_cycles() == saved_cycles &&
              version_three_loader.bus().read8(0xA123) == 0x5A,
          "version 3 save states remain loadable after adding CGB state");

    auto version_four = legacy_saved;
    version_four.erase(version_four.end() - version_four_cgb_size -
                           version_five_ppu_size - version_six_state_size -
                           version_seven_camera_size - version_eight_ppu_size -
                           version_nine_fetcher_size,
                       version_four.end() - version_four_cgb_size -
                           version_seven_camera_size - version_eight_ppu_size -
                           version_nine_fetcher_size);
    version_four.resize(version_four.size() - version_seven_camera_size -
                        version_eight_ppu_size - version_nine_fetcher_size);
    version_four[8] = 4;
    const auto version_four_payload_size = static_cast<std::uint32_t>(
        version_four.size() - state_header_size);
    write_little_u32(version_four, 20, version_four_payload_size);
    write_little_u32(
        version_four, 24,
        state_crc32(version_four.data() + state_header_size,
                    version_four_payload_size));
    gameboy::Emulator version_four_loader{gameboy::Cartridge{rom}};
    version_four_loader.load_state(version_four);
    check(version_four_loader.cpu().registers().pc == saved_pc &&
              version_four_loader.cpu().total_cycles() == saved_cycles &&
              version_four_loader.bus().read8(0xA123) == 0x5A,
          "version 4 save states remain loadable after adding PPU coincidence state");

    auto version_five = legacy_saved;
    version_five.erase(version_five.end() - version_four_cgb_size -
                           version_six_state_size - version_seven_camera_size -
                           version_eight_ppu_size - version_nine_fetcher_size,
                       version_five.end() - version_four_cgb_size -
                           version_seven_camera_size - version_eight_ppu_size -
                           version_nine_fetcher_size);
    version_five.resize(version_five.size() - version_seven_camera_size -
                        version_eight_ppu_size - version_nine_fetcher_size);
    version_five[8] = 5;
    const auto version_five_payload_size = static_cast<std::uint32_t>(
        version_five.size() - state_header_size);
    write_little_u32(version_five, 20, version_five_payload_size);
    write_little_u32(
        version_five, 24,
        state_crc32(version_five.data() + state_header_size,
                    version_five_payload_size));
    auto version_five_loader_storage = std::make_unique<gameboy::Emulator>(
        gameboy::Cartridge{rom});
    auto& version_five_loader = *version_five_loader_storage;
    version_five_loader.load_state(version_five);
    check(version_five_loader.cpu().registers().pc == saved_pc &&
              version_five_loader.cpu().total_cycles() == saved_cycles &&
              version_five_loader.bus().read8(0xA123) == 0x5A,
          "version 5 save states remain loadable after adding PPU startup state");

    auto version_six = legacy_saved;
    version_six.resize(version_six.size() - version_seven_camera_size -
                       version_eight_ppu_size - version_nine_fetcher_size);
    version_six[8] = 6;
    const auto version_six_payload_size = static_cast<std::uint32_t>(
        version_six.size() - state_header_size);
    write_little_u32(version_six, 20, version_six_payload_size);
    write_little_u32(
        version_six, 24,
        state_crc32(version_six.data() + state_header_size,
                    version_six_payload_size));
    gameboy::Emulator version_six_loader{gameboy::Cartridge{rom}};
    version_six_loader.load_state(version_six);
    check(version_six_loader.cpu().registers().pc == saved_pc &&
              version_six_loader.cpu().total_cycles() == saved_cycles &&
              version_six_loader.bus().read8(0xA123) == 0x5A,
          "version 6 save states remain loadable after adding camera state");

    auto version_seven = legacy_saved;
    version_seven.resize(version_seven.size() - version_eight_ppu_size -
                         version_nine_fetcher_size);
    version_seven[8] = 7;
    const auto version_seven_payload_size = static_cast<std::uint32_t>(
        version_seven.size() - state_header_size);
    write_little_u32(version_seven, 20, version_seven_payload_size);
    write_little_u32(
        version_seven, 24,
        state_crc32(version_seven.data() + state_header_size,
                    version_seven_payload_size));
    gameboy::Emulator version_seven_loader{gameboy::Cartridge{rom}};
    version_seven_loader.load_state(version_seven);
    check(version_seven_loader.cpu().registers().pc == saved_pc &&
              version_seven_loader.cpu().total_cycles() == saved_cycles &&
              version_seven_loader.bus().read8(0xA123) == 0x5A,
          "version 7 save states remain loadable after adding PPU dot state");

    auto version_eight = legacy_saved;
    version_eight.resize(version_eight.size() - version_nine_fetcher_size);
    version_eight[8] = 8;
    const auto version_eight_payload_size = static_cast<std::uint32_t>(
        version_eight.size() - state_header_size);
    write_little_u32(version_eight, 20, version_eight_payload_size);
    write_little_u32(
        version_eight, 24,
        state_crc32(version_eight.data() + state_header_size,
                    version_eight_payload_size));
    gameboy::Emulator version_eight_loader{gameboy::Cartridge{rom}};
    version_eight_loader.load_state(version_eight);
    check(version_eight_loader.cpu().registers().pc == saved_pc &&
              version_eight_loader.cpu().total_cycles() == saved_cycles &&
              version_eight_loader.bus().read8(0xA123) == 0x5A,
          "version 8 save states remain loadable after adding PPU fetcher state");

    auto version_nine = legacy_saved;
    version_nine.resize(version_nine.size() - version_ten_window_latch_size -
                        version_eleven_fetcher_size -
                        version_twelve_sprite_size -
                        version_thirteen_sprite_fetch_size -
                        version_fourteen_sprite_deadline_size -
                        version_fifteen_sprite_render_size);
    version_nine[8] = 9;
    const auto version_nine_payload_size = static_cast<std::uint32_t>(
        version_nine.size() - state_header_size);
    write_little_u32(version_nine, 20, version_nine_payload_size);
    write_little_u32(
        version_nine, 24,
        state_crc32(version_nine.data() + state_header_size,
                    version_nine_payload_size));
    auto version_nine_loader_storage = std::make_unique<gameboy::Emulator>(
        gameboy::Cartridge{rom});
    auto& version_nine_loader = *version_nine_loader_storage;
    version_nine_loader.load_state(version_nine);
    check(version_nine_loader.cpu().registers().pc == saved_pc &&
              version_nine_loader.cpu().total_cycles() == saved_cycles &&
              version_nine_loader.bus().read8(0xA123) == 0x5A,
          "version 9 save states remain loadable after adding window latches");

    auto version_ten = legacy_saved;
    version_ten.resize(version_ten.size() - version_eleven_fetcher_size -
                       version_twelve_sprite_size -
                       version_thirteen_sprite_fetch_size -
                       version_fourteen_sprite_deadline_size -
                       version_fifteen_sprite_render_size);
    version_ten[8] = 10;
    const auto version_ten_payload_size = static_cast<std::uint32_t>(
        version_ten.size() - state_header_size);
    write_little_u32(version_ten, 20, version_ten_payload_size);
    write_little_u32(
        version_ten, 24,
        state_crc32(version_ten.data() + state_header_size,
                    version_ten_payload_size));
    gameboy::Emulator version_ten_loader{gameboy::Cartridge{rom}};
    version_ten_loader.load_state(version_ten);
    check(version_ten_loader.cpu().registers().pc == saved_pc &&
              version_ten_loader.cpu().total_cycles() == saved_cycles &&
              version_ten_loader.bus().read8(0xA123) == 0x5A,
          "version 10 save states remain loadable after refining fetch startup");

    auto version_eleven = legacy_saved;
    version_eleven.resize(version_eleven.size() - version_twelve_sprite_size -
                          version_thirteen_sprite_fetch_size -
                          version_fourteen_sprite_deadline_size -
                          version_fifteen_sprite_render_size);
    version_eleven[8] = 11;
    const auto version_eleven_payload_size = static_cast<std::uint32_t>(
        version_eleven.size() - state_header_size);
    write_little_u32(version_eleven, 20, version_eleven_payload_size);
    write_little_u32(
        version_eleven, 24,
        state_crc32(version_eleven.data() + state_header_size,
                    version_eleven_payload_size));
    gameboy::Emulator version_eleven_loader{gameboy::Cartridge{rom}};
    version_eleven_loader.load_state(version_eleven);
    check(version_eleven_loader.cpu().registers().pc == saved_pc &&
              version_eleven_loader.cpu().total_cycles() == saved_cycles &&
              version_eleven_loader.bus().read8(0xA123) == 0x5A,
          "version 11 save states remain loadable after adding sprite fetch state");

    auto version_twelve = legacy_saved;
    version_twelve.resize(version_twelve.size() - version_thirteen_sprite_fetch_size -
                          version_fourteen_sprite_deadline_size -
                          version_fifteen_sprite_render_size);
    version_twelve[8] = 12;
    const auto version_twelve_payload_size = static_cast<std::uint32_t>(
        version_twelve.size() - state_header_size);
    write_little_u32(version_twelve, 20, version_twelve_payload_size);
    write_little_u32(
        version_twelve, 24,
        state_crc32(version_twelve.data() + state_header_size,
                    version_twelve_payload_size));
    gameboy::Emulator version_twelve_loader{gameboy::Cartridge{rom}};
    version_twelve_loader.load_state(version_twelve);
    check(version_twelve_loader.cpu().registers().pc == saved_pc &&
              version_twelve_loader.cpu().total_cycles() == saved_cycles &&
              version_twelve_loader.bus().read8(0xA123) == 0x5A,
          "version 12 save states remain loadable after adding pending sprite state");

    auto version_thirteen = legacy_saved;
    version_thirteen.resize(version_thirteen.size() -
                            version_fourteen_sprite_deadline_size -
                            version_fifteen_sprite_render_size);
    version_thirteen[8] = 13;
    const auto version_thirteen_payload_size = static_cast<std::uint32_t>(
        version_thirteen.size() - state_header_size);
    write_little_u32(version_thirteen, 20, version_thirteen_payload_size);
    write_little_u32(
        version_thirteen, 24,
        state_crc32(version_thirteen.data() + state_header_size,
                    version_thirteen_payload_size));
    gameboy::Emulator version_thirteen_loader{gameboy::Cartridge{rom}};
    version_thirteen_loader.load_state(version_thirteen);
    check(version_thirteen_loader.cpu().registers().pc == saved_pc &&
              version_thirteen_loader.cpu().total_cycles() == saved_cycles &&
              version_thirteen_loader.bus().read8(0xA123) == 0x5A,
          "version 13 save states remain loadable after adding per-sprite deadlines");

    auto version_fourteen = legacy_saved;
    version_fourteen.resize(version_fourteen.size() -
                            version_fifteen_sprite_render_size);
    version_fourteen[8] = 14;
    const auto version_fourteen_payload_size = static_cast<std::uint32_t>(
        version_fourteen.size() - state_header_size);
    write_little_u32(version_fourteen, 20, version_fourteen_payload_size);
    write_little_u32(
        version_fourteen, 24,
        state_crc32(version_fourteen.data() + state_header_size,
                    version_fourteen_payload_size));
    gameboy::Emulator version_fourteen_loader{gameboy::Cartridge{rom}};
    version_fourteen_loader.load_state(version_fourteen);
    check(version_fourteen_loader.cpu().registers().pc == saved_pc &&
              version_fourteen_loader.cpu().total_cycles() == saved_cycles &&
              version_fourteen_loader.bus().read8(0xA123) == 0x5A,
          "version 14 save states remain loadable after adding rendered sprite state");

    emulator.bus().write8(0xA123, 0x99);
    emulator.bus().write8(0xC000, 0x11);
    emulator.set_button(gameboy::Button::start, false);
    static_cast<void>(emulator.step());
    emulator.bus().tick(4096);
    emulator.load_state(saved);
    check(emulator.cpu().registers().pc == saved_pc &&
              emulator.cpu().total_cycles() == saved_cycles &&
              emulator.bus().read8(0xC000) == 0x42 &&
              emulator.bus().read8(0xA123) == 0x5A,
          "loading a save state restores CPU, memory, and mapper state");
    check(emulator.save_state() == saved,
          "save-state serialization round trips byte for byte");

    gameboy::Emulator replay{gameboy::Cartridge{rom}};
    replay.load_state(saved);
    for (unsigned instruction = 0; instruction < 64; ++instruction) {
        static_cast<void>(emulator.step());
        static_cast<void>(replay.step());
    }
    check(emulator.save_state() == replay.save_state(),
          "restored emulators continue deterministically");

    const auto unchanged = emulator.save_state();
    auto corrupted = saved;
    corrupted.back() ^= 0x80;
    auto rejected_corruption = false;
    try {
        emulator.load_state(corrupted);
    } catch (const gameboy::SaveStateError&) {
        rejected_corruption = true;
    }
    check(rejected_corruption && emulator.save_state() == unchanged,
          "corrupt save states are rejected without changing emulator state");

    auto truncated = saved;
    truncated.resize(truncated.size() - 1);
    auto rejected_truncation = false;
    try {
        emulator.load_state(truncated);
    } catch (const gameboy::SaveStateError&) {
        rejected_truncation = true;
    }
    check(rejected_truncation && emulator.save_state() == unchanged,
          "truncated save states are rejected without changing emulator state");

    auto version_sixteen = saved;
    version_sixteen.resize(version_sixteen.size() -
                           version_twenty_three_sgb_border_size -
                           version_twenty_two_sgb_size -
                           version_twenty_one_pulse_timing_size -
                           version_twenty_timing_size -
                           version_nineteen_apu_size -
                           version_eighteen_object_deadline_size -
                           version_seventeen_background_history_size);
    version_sixteen[8] = 16;
    const auto version_sixteen_payload_size = static_cast<std::uint32_t>(
        version_sixteen.size() - state_header_size);
    write_little_u32(version_sixteen, 20, version_sixteen_payload_size);
    write_little_u32(
        version_sixteen, 24,
        state_crc32(version_sixteen.data() + state_header_size,
                    version_sixteen_payload_size));
    gameboy::Emulator version_sixteen_loader{gameboy::Cartridge{rom}};
    version_sixteen_loader.load_state(version_sixteen);
    check(version_sixteen_loader.cpu().registers().pc == saved_pc &&
              version_sixteen_loader.cpu().total_cycles() == saved_cycles &&
              version_sixteen_loader.bus().read8(0xA123) == 0x5A,
          "version 16 save states remain loadable after adding PPU background history");

    auto version_seventeen = saved;
    version_seventeen.resize(version_seventeen.size() -
                             version_twenty_three_sgb_border_size -
                             version_twenty_two_sgb_size -
                             version_twenty_one_pulse_timing_size -
                             version_twenty_timing_size -
                             version_nineteen_apu_size -
                             version_eighteen_object_deadline_size);
    version_seventeen[8] = 17;
    const auto version_seventeen_payload_size = static_cast<std::uint32_t>(
        version_seventeen.size() - state_header_size);
    write_little_u32(version_seventeen, 20, version_seventeen_payload_size);
    write_little_u32(
        version_seventeen, 24,
        state_crc32(version_seventeen.data() + state_header_size,
                    version_seventeen_payload_size));
    gameboy::Emulator version_seventeen_loader{gameboy::Cartridge{rom}};
    version_seventeen_loader.load_state(version_seventeen);
    check(version_seventeen_loader.cpu().registers().pc == saved_pc &&
              version_seventeen_loader.cpu().total_cycles() == saved_cycles &&
              version_seventeen_loader.bus().read8(0xA123) == 0x5A,
          "version 17 save states remain loadable after adding object deadlines");

    auto future_version = saved;
    future_version[8] = 24;
    auto rejected_version = false;
    try {
        emulator.load_state(future_version);
    } catch (const gameboy::SaveStateError&) {
        rejected_version = true;
    }
    check(rejected_version && emulator.save_state() == unchanged,
          "unknown save-state versions are rejected without changing emulator state");

    auto other_rom = rom;
    other_rom[0x0200] ^= 1;
    gameboy::Emulator other{gameboy::Cartridge{std::move(other_rom)}};
    const auto other_unchanged = other.save_state();
    auto rejected_rom = false;
    try {
        other.load_state(saved);
    } catch (const gameboy::SaveStateError&) {
        rejected_rom = true;
    }
    check(rejected_rom && other.save_state() == other_unchanged,
          "save states cannot be loaded into a different ROM");

    gameboy::Emulator rtc_emulator{
        gameboy::Cartridge{banked_rom(2, 0x10, 0x00, 0x03)}};
    rtc_emulator.bus().write8(0x0000, 0x0A);
    rtc_emulator.bus().write8(0x4000, 0x0C);
    rtc_emulator.bus().write8(0xA000, 0x40); // Halt the RTC for determinism.
    rtc_emulator.bus().write8(0x4000, 0x08);
    rtc_emulator.bus().write8(0xA000, 12);
    rtc_emulator.bus().write8(0x6000, 0);
    rtc_emulator.bus().write8(0x6000, 1);
    const auto rtc_state = rtc_emulator.save_state();
    rtc_emulator.bus().write8(0xA000, 34);
    rtc_emulator.load_state(rtc_state);
    check(rtc_emulator.bus().read8(0xA000) == 12 &&
              rtc_emulator.save_state() == rtc_state,
          "save states restore live and latched MBC3 RTC state");
}

} // namespace

int main() {
    try {
        test_save_state_round_trip_and_validation();
    } catch (const std::exception& error) {
        std::cerr << "Unexpected exception: " << error.what() << '\n';
        return 1;
    }

    if (failures == 0) {
        std::cout << "All save-state contract tests passed\n";
    }
    return failures == 0 ? 0 : 1;
}
