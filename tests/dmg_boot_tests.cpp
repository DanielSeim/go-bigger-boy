#include "gameboy/emulator.hpp"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
int failures = 0;
class Endpoint final : public gameboy::SerialEndpoint {
public:
    unsigned cancellations = 0;
    unsigned releases = 0;
    bool exchange_bit(bool) noexcept override { return true; }
    void cancel_internal_clock(gameboy::SerialPort&) noexcept override { ++cancellations; }
    void release_internal_clock(gameboy::SerialPort&) noexcept override { ++releases; }
};
void check(bool condition, const char* message) {
    if (!condition) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
}

// Synthetic homebrew header: deliberately contains no Nintendo logo/data.
std::vector<std::uint8_t> cartridge(bool zero_checksum = false, bool battery = false,
                                    bool cgb = false) {
    std::vector<std::uint8_t> rom(0x8000, 0);
    rom[0] = 0x42;
    rom[0x100] = 0x76; // HALT, never executed before handoff assertions.
    rom[0x143] = cgb ? 0x80 : 0;
    rom[0x147] = battery ? 0x03 : 0;
    rom[0x149] = battery ? 0x02 : 0;
    auto checksum = [&] {
        std::uint8_t sum = 0;
        for (unsigned i = 0x134; i <= 0x14C; ++i)
            sum = static_cast<std::uint8_t>(sum - rom[i] - 1);
        return sum;
    };
    if (zero_checksum) rom[0x134] = checksum();
    rom[0x14D] = checksum();
    return rom;
}

bool finish_boot(gameboy::Emulator& emulator) {
    for (unsigned step = 0; step < 1'000'000 && emulator.bus().boot_rom_enabled(); ++step)
        static_cast<void>(emulator.step());
    return !emulator.bus().boot_rom_enabled();
}

void check_handoff(gameboy::Emulator& emulator, bool zero_checksum) {
    check(finish_boot(emulator), "firmware completes within a bounded instruction budget");
    const auto& r = emulator.cpu().registers();
    check(r.pc == 0x100 && r.sp == 0xFFFE, "unmap falls through directly to cartridge entry");
    check(r.a == 1 && r.f == (zero_checksum ? 0x80 : 0xB0) &&
          r.b == 0 && r.c == 0x13 && r.d == 0 && r.e == 0xD8 &&
          r.h == 1 && r.l == 0x4D, "documented DMG registers including checksum-dependent flags");
    const auto& bus = emulator.bus();
    check(bus.read8(0) == 0x42 && bus.read8(0xFF50) == 0xFF,
          "cartridge mapping restored at handoff");
    for (unsigned offset = 0; offset < 0x2000; ++offset)
        if (bus.debug_read_vram(0, static_cast<std::uint16_t>(offset)) != 0) {
            check(false, "all VRAM cleared by firmware, including both tile maps"); break;
        }
    const struct { std::uint16_t address; std::uint8_t value; } io[] = {
        {0xFF00,0xCF}, {0xFF01,0}, {0xFF02,0x7E}, {0xFF05,0}, {0xFF06,0},
        {0xFF07,0xF8}, {0xFF0F,0xE1}, {0xFF10,0x80}, {0xFF11,0xBF},
        {0xFF12,0xF3}, {0xFF13,0xFF}, {0xFF14,0xBF}, {0xFF16,0x3F},
        {0xFF17,0}, {0xFF18,0xFF}, {0xFF19,0xBF}, {0xFF1A,0x7F},
        {0xFF1B,0xFF}, {0xFF1C,0x9F}, {0xFF1D,0xFF}, {0xFF1E,0xBF},
        {0xFF20,0xFF}, {0xFF21,0}, {0xFF22,0}, {0xFF23,0xBF},
        {0xFF24,0x77}, {0xFF25,0xF3}, {0xFF26,0xF1}, {0xFF40,0x91},
        {0xFF42,0}, {0xFF43,0}, {0xFF44,0}, {0xFF45,0}, {0xFF47,0xFC},
        {0xFF48,0xFF}, {0xFF49,0xFF}, {0xFF4A,0}, {0xFF4B,0}, {0xFFFF,0},
    };
    for (const auto& item : io) {
        if (bus.read8(item.address) != item.value) {
            std::cerr << "I/O mismatch at " << std::hex << item.address << ": "
                      << unsigned(bus.read8(item.address)) << " expected "
                      << unsigned(item.value) << std::dec << '\n';
            check(false, "CPU-executed hardware initialization matches the DMG register contract");
        }
    }
    check(bus.read8(0xFF41) == 0x85,
          "LY zero on the final VBlank line hands off in mode 1, not pixel transfer");
    check(bus.debug_divider_counter() == 0xABC8 && bus.debug_ppu_dot() == 396,
          "both checksum paths establish the timer and LCD fast-start phase");
    check(bus.debug_apu_clock_state()[6] == 0,
          "canonical readable pulse registers do not leave a startup tone running");
    const auto clocks = bus.debug_apu_clock_state();
    check(clocks[0] == 1 && clocks[1] == 0 && clocks[3] == 30 && clocks[4] == 2,
          "firmware aligns inherited sequencer and silent pulse phase without a private snapshot");
    check(emulator.cpu().total_cycles() < 4'400'000,
          "silent fast startup remains below 1.05 emulated seconds");
    check(bus.read8(0xFF80) == 0, "replacement does not write the diagnostic HRAM marker");
    emulator.bus().write8(0xFF50, 0);
    check(!bus.boot_rom_enabled(), "FF50 cannot remap firmware after handoff");
}

void test_power_on_and_reset() {
    Endpoint endpoint;
    gameboy::Emulator emulator(gameboy::Cartridge(cartridge(false, true, true)),
        gameboy::HardwareModel::dmg, gameboy::BootRomMode::replacement_dmg);
    check(emulator.cpu().registers().pc == 0 && emulator.cpu().registers().sp == 0,
          "CPU begins at reset, not cartridge entry");
    check(emulator.bus().read8(0xFF40) == 0 && emulator.bus().read8(0xFF04) == 0 &&
          emulator.bus().read8(0xFF26) == 0x70,
          "LCD, divider and APU are cold before any firmware instruction");
    check(!emulator.bus().cgb_mode() && emulator.bus().read8(0xFF02) == 0x7E,
          "DMG hardware remains monochrome even for a CGB-compatible cartridge");
    check(emulator.bus().read8(0xFF00) == 0xCF,
          "cold JOYP selects both groups even before the boot writes it");
    auto undefined = emulator.cpu().registers();
    undefined.a = 0xA5; undefined.f = 0xF0;
    undefined.b = 0x55; undefined.c = 0xAA; undefined.d = 0xCC;
    undefined.e = 0x33; undefined.h = 0x77; undefined.l = 0x88;
    undefined.sp = 0x1234;
    emulator.set_cpu_registers(undefined);
    std::vector<std::uint8_t> battery(0x2000, 0xA5);
    emulator.import_battery_ram(battery);
    for (unsigned offset = 0; offset < 0x2000; ++offset)
        emulator.bus().debug_write_vram(0, static_cast<std::uint16_t>(offset), 0xA5);
    emulator.bus().write8(0xC123, 0x5A);
    check_handoff(emulator, false);
    check(emulator.bus().read8(0xC123) == 0x5A, "firmware does not overwrite WRAM");
    check(emulator.export_battery_ram() == battery, "firmware does not change cartridge battery RAM");
    emulator.set_audio_enabled(false);
    emulator.bus().serial_port().set_endpoint(&endpoint);
    emulator.bus().write8(0xFF01, 0xA5);
    emulator.bus().write8(0xFF02, 0x81);
    emulator.bus().write8(0xFF46, 0xC0); // Pending DMA must not leak into reset.
    emulator.bus().write8(0xFF07, 5);
    emulator.reset();
    check(emulator.bus().boot_rom_enabled() && emulator.cpu().registers().pc == 0 &&
          emulator.bus().read8(0xFF04) == 0 && emulator.bus().read8(0xFF40) == 0,
          "reset restarts the firmware from a cold machine");
    check(!emulator.audio_enabled(), "reset preserves the host audio preference");
    check(emulator.bus().serial_port().has_endpoint() && endpoint.cancellations == 1 &&
          endpoint.releases == 1, "reset cancels active serial ownership but retains host attachment");
    check_handoff(emulator, false);
    check(emulator.export_battery_ram() == battery, "reset preserves battery RAM");
}

void test_checksum_and_model_guards() {
    gameboy::Emulator zero(gameboy::Cartridge(cartridge(true)),
        gameboy::HardwareModel::dmg, gameboy::BootRomMode::replacement_dmg);
    check_handoff(zero, true);
    auto corrupt = cartridge();
    ++corrupt[0x14D];
    gameboy::Emulator invalid(gameboy::Cartridge(std::move(corrupt)),
        gameboy::HardwareModel::dmg, gameboy::BootRomMode::replacement_dmg);
    check(!finish_boot(invalid) && invalid.cpu().registers().pc < 0x100,
          "invalid header checksum keeps the boot image mapped, never starts cartridge");
    check(invalid.bus().read8(0xFF40) == 0, "failed checksum leaves LCD disabled");
    for (auto model : {gameboy::HardwareModel::dmg0, gameboy::HardwareModel::mgb,
                      gameboy::HardwareModel::cgb, gameboy::HardwareModel::sgb,
                      gameboy::HardwareModel::sgb2}) {
        bool rejected = false;
        try {
            gameboy::Emulator wrong(gameboy::Cartridge(cartridge()), model,
                                   gameboy::BootRomMode::replacement_dmg);
        } catch (const std::invalid_argument&) { rejected = true; }
        check(rejected, "unsupported model cannot silently execute DMG firmware");
    }
}

void test_state_resume() {
    gameboy::Emulator original(gameboy::Cartridge(cartridge()),
        gameboy::HardwareModel::dmg, gameboy::BootRomMode::replacement_dmg);
    for (unsigned step = 0; step < 137; ++step) static_cast<void>(original.step());
    auto state = original.save_state();
    gameboy::Emulator restored(gameboy::Cartridge(cartridge()), gameboy::HardwareModel::dmg);
    restored.load_state(state);
    check(restored.bus().boot_rom_enabled(), "mid-boot state preserves ROM mapping");
    restored.reset();
    check(!restored.bus().boot_rom_enabled() && restored.cpu().registers().pc == 0x100,
          "post-boot receiver reset unmaps a restored boot and follows its configured mode");
    restored.load_state(state);
    check(restored.bus().read8(0) == gameboy::dmg_boot_rom()[0],
          "state carries the actual firmware even into a different boot-mode constructor");
    for (unsigned step = 0; step < 1000; ++step) {
        check(original.step() == restored.step(), "mid-boot resume preserves instruction timing");
    }
    check(original.save_state() == restored.save_state(), "mid-boot resume is byte-exact");
    check_handoff(original, false);
    check_handoff(restored, false);
    check(original.save_state() == restored.save_state(), "resumed boot has identical handoff state");
    restored.load_state(original.save_state());
    check(!restored.bus().boot_rom_enabled(), "completed state does not remap boot ROM");

    // The shared mapped-image codec must also work for the older diagnostic mode.
    gameboy::Emulator diagnostic(gameboy::Cartridge(cartridge()), gameboy::HardwareModel::dmg,
                                gameboy::BootRomMode::diagnostic);
    auto diagnostic_state = diagnostic.save_state();
    restored.load_state(diagnostic_state);
    check(restored.bus().read8(0) == gameboy::diagnostic_boot_rom(gameboy::HardwareModel::dmg)[0],
          "mapped-image states preserve diagnostic firmware too");
    check(finish_boot(restored) && restored.bus().read8(0xFF80) == 'G',
          "restored diagnostic image executes its own handoff behavior");
}

void test_silent_handoff_and_later_audio() {
    for (bool checksum_zero : {false, true}) {
        gameboy::Emulator emulator(gameboy::Cartridge(cartridge(checksum_zero)),
            gameboy::HardwareModel::dmg, gameboy::BootRomMode::replacement_dmg);
        check_handoff(emulator, checksum_zero);
        for (unsigned instruction = 0; instruction < 25; ++instruction) (void)emulator.step();
        check(emulator.bus().debug_apu_clock_state()[3] == 182 &&
              emulator.bus().debug_apu_clock_state()[4] == 3,
              "inherited silent pulse reloads after 252 clocks, not the old 8192-clock period");
        (void)emulator.take_audio_samples(); // exclude the boot's DAC power-on transient
        const auto state = emulator.save_state();
        const auto start = emulator.cpu().total_cycles();
        while (emulator.cpu().total_cycles() - start < 4'194'304)
            (void)emulator.step(); // homebrew HALTs without writing any sound register
        const auto quiet = emulator.take_audio_samples();
        check(quiet.size() >= 95'998 &&
              std::all_of(quiet.begin(), quiet.end(), [](auto x) { return x >= -8 && x <= 8; }),
              "a cartridge that never touches the APU stays below -72 dBFS for a full second");
        gameboy::Emulator restored(gameboy::Cartridge(cartridge(checksum_zero)),
                                   gameboy::HardwareModel::dmg);
        restored.load_state(state);
        while (restored.cpu().total_cycles() < emulator.cpu().total_cycles())
            (void)restored.step();
        check(restored.take_audio_samples() == quiet,
              "save/load preserves the settled audio handoff and sample timing");
        // A cartridge can retrigger the canonical channel normally: no host mute,
        // private state patch, or DAC disable was used to obtain silence.
        emulator.bus().write8(0xFF13, 0x80);
        emulator.bus().write8(0xFF14, 0x87);
        const auto tone_start = emulator.cpu().total_cycles();
        while (emulator.cpu().total_cycles() - tone_start < 70'224)
            (void)emulator.step();
        const auto tone = emulator.take_audio_samples();
        check(std::any_of(tone.begin(), tone.end(), [](auto x) { return x > 1000 || x < -1000; }),
              "cartridge-triggered sound remains audible after silent startup");
    }
}

void test_cartridge_divider_boundary() {
    for (bool checksum_zero : {false, true}) {
        auto rom = cartridge(checksum_zero);
        // An original fixture: eight NOPs, then two consecutive DIV reads.
        // ABC8 + 32 + 12 = ABF4; the second read hits AC00 exactly.
        std::fill(rom.begin() + 0x100, rom.begin() + 0x108, 0);
        rom[0x108] = 0xF0; rom[0x109] = 0x04;
        rom[0x10A] = 0xF0; rom[0x10B] = 0x04;
        gameboy::Emulator emulator(gameboy::Cartridge(std::move(rom)),
            gameboy::HardwareModel::dmg, gameboy::BootRomMode::replacement_dmg);
        check_handoff(emulator, checksum_zero);
        for (unsigned instruction = 0; instruction < 9; ++instruction)
            (void)emulator.step();
        check(emulator.cpu().registers().a == 0xAB,
              "cartridge reads DIV before its first increment");
        (void)emulator.step();
        check(emulator.cpu().registers().a == 0xAC &&
              emulator.bus().debug_divider_counter() == 0xAC00,
              "cartridge sees the DIV increment on the exact read boundary");
    }
}

void test_envelope_settle_resume() {
    gameboy::Emulator emulator(gameboy::Cartridge(cartridge()),
        gameboy::HardwareModel::dmg, gameboy::BootRomMode::replacement_dmg);
    while (emulator.cpu().total_cycles() < 3'000'000) (void)emulator.step();
    check(emulator.bus().boot_rom_enabled() && emulator.bus().read8(0xFF25) == 0,
          "mid-envelope state is still a muted boot, not a cartridge snapshot");
    (void)emulator.take_audio_samples();
    const auto state = emulator.save_state();
    gameboy::Emulator restored(gameboy::Cartridge(cartridge()), gameboy::HardwareModel::dmg);
    restored.load_state(state);
    check_handoff(emulator, false);
    check_handoff(restored, false);
    check(emulator.take_audio_samples() == restored.take_audio_samples(),
          "envelope/filter startup resume preserves every PCM sample");
    check(emulator.save_state() == restored.save_state(),
          "envelope/filter startup resume preserves the exact handoff state");
}
} // namespace

int main() {
    try {
        check(gameboy::dmg_boot_rom().size() == 256 && gameboy::dmg_boot_rom()[254] == 0xE0 &&
              gameboy::dmg_boot_rom()[255] == 0x50, "fixed-size image ends with hardware unmap instruction");
        test_power_on_and_reset();
        test_checksum_and_model_guards();
        test_state_resume();
        test_silent_handoff_and_later_audio();
        test_cartridge_divider_boundary();
        test_envelope_settle_resume();
    } catch (const std::exception& error) {
        std::cerr << "Unexpected exception: " << error.what() << '\n'; return 1;
    }
    if (!failures) std::cout << "DMG cold-start firmware contracts passed\n";
    return failures ? 1 : 0;
}
