#include "gameboy/cpu.hpp"
#include "gameboy/dmg_palette.hpp"
#include "gameboy/emulator.hpp"
#include "gameboy/memory_bus.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
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
std::vector<std::uint8_t> test_rom(
    const std::vector<std::uint8_t>& program = {}) {
    std::vector<std::uint8_t> rom(0x8000, 0);
    constexpr std::string_view title = "PPU TIMING TEST";
    std::copy(title.begin(), title.end(), rom.begin() + 0x134);
    std::copy(program.begin(), program.end(), rom.begin() + program_address);
    return rom;
}

void test_cpu_machine_cycle_bus_timing() {
    gameboy::MemoryBus bus{
        gameboy::Cartridge{test_rom({0xFA, 0x05, 0xFF})}}; // LD A,(FF05)
    bus.write8(0xFF07, 0x05); // TIMA increments every 16 clocks.
    bus.tick(12);

    gameboy::Cpu cpu;
    check(cpu.step(bus) == 16 && cpu.registers().a == 1,
          "CPU memory reads observe hardware changes from earlier machine cycles");
    check(bus.read8(0xFF04) == 0 && bus.read8(0xFF05) == 1,
          "calling Cpu::step directly advances bus hardware exactly once");
}

void test_ppu_modes_and_memory_access() {
    gameboy::MemoryBus bus{gameboy::Cartridge{test_rom()}};
    bus.write8(0x8000, 0x12);
    bus.write8(0xFE00, 0x34);
    bus.write8(0xFF40, 0x80);
    check((bus.read8(0xFF41) & 0x03) == 0 && bus.read8(0xFF44) == 0,
          "enabling LCD starts line zero in startup mode 0");
    check(bus.read8(0x8000) == 0x12,
          "VRAM remains accessible during startup mode 0");
    check(bus.read8(0xFE00) == 0x34,
          "OAM remains accessible during startup mode 0");

    bus.tick(79);
    check((bus.read8(0xFF41) & 0x03) == 0,
          "line-zero startup mode lasts eighty dots");
    bus.tick(1);
    check((bus.read8(0xFF41) & 0x03) == 3,
          "line-zero mode 3 begins on dot eighty");
    check(bus.read8(0x8000) == 0xFF && bus.read8(0xFE00) == 0xFF,
          "VRAM and OAM are blocked during mode 3");
    bus.write8(0x8000, 0x99);

    bus.tick(172);
    check((bus.read8(0xFF41) & 0x03) == 0,
          "line-zero minimum-length mode 3 ends on dot 252");
    check(bus.read8(0x8000) == 0x12,
          "writes to VRAM during mode 3 are ignored");
    check(bus.read8(0xFE00) == 0x34,
          "OAM becomes accessible during HBlank");

    bus.tick(200);
    check(bus.read8(0xFF44) == 1 && (bus.read8(0xFF41) & 0x03) == 0 &&
              bus.read8(0xFE00) == 0xFF,
          "the 452-dot startup line begins OAM scan before mode 2 is visible");
    bus.tick(1);
    check((bus.read8(0xFF41) & 0x03) == 2,
          "CPU-visible mode 2 follows its internal boundary by one dot");
    bus.write8(0xFF44, 99);
    check(bus.read8(0xFF44) == 1, "LY ignores CPU writes");

    bus.write8(0xFF40, 0);
    check(bus.read8(0xFF44) == 0 && (bus.read8(0xFF41) & 0x03) == 0,
          "disabling LCD resets LY and reports mode 0");

    gameboy::MemoryBus scrolling{gameboy::Cartridge{test_rom()}};
    scrolling.write8(0xFF43, 5);
    scrolling.write8(0xFF40, 0x81);
    scrolling.tick(254);
    check((scrolling.read8(0xFF41) & 0x03) == 3,
          "fine horizontal scrolling lengthens mode 3");
    scrolling.tick(5);
    check((scrolling.read8(0xFF41) & 0x03) == 0,
          "mode 3 includes the SCX fine-scroll discard penalty");

    gameboy::MemoryBus window_timing{gameboy::Cartridge{test_rom()}};
    window_timing.write8(0xFF4A, 0);
    window_timing.write8(0xFF4B, 7);
    window_timing.write8(0xFF40, 0xA1);
    window_timing.tick(257);
    check((window_timing.read8(0xFF41) & 0x03) == 3,
          "starting the window stalls the background fetcher for six dots");
    window_timing.tick(1);
    check((window_timing.read8(0xFF41) & 0x03) == 0,
          "window fetch startup extends mode 3 by six dots");

    gameboy::MemoryBus wx_zero_timing{gameboy::Cartridge{test_rom()}};
    wx_zero_timing.write8(0xFF43, 1);
    wx_zero_timing.write8(0xFF4A, 0);
    wx_zero_timing.write8(0xFF4B, 0);
    wx_zero_timing.write8(0xFF40, 0xA1);
    wx_zero_timing.tick(259);
    check((wx_zero_timing.read8(0xFF41) & 0x03) == 3,
          "WX zero adds its DMG window stall with fractional SCX");
    wx_zero_timing.tick(1);
    check((wx_zero_timing.read8(0xFF41) & 0x03) == 0,
          "the WX-zero fractional-scroll stall extends mode 3 by one dot");

    gameboy::MemoryBus sprite_timing{gameboy::Cartridge{test_rom()}};
    sprite_timing.write8(0xFE00, 16);
    sprite_timing.write8(0xFE01, 8);
    sprite_timing.write8(0xFF40, 0x83);
    sprite_timing.tick(262);
    check((sprite_timing.read8(0xFF41) & 0x03) == 3,
          "a selected aligned sprite extends mode 3 by eleven dots");
    sprite_timing.tick(1);
    check((sprite_timing.read8(0xFF41) & 0x03) == 0,
          "sprite fetch timing controls the start of HBlank");

    gameboy::MemoryBus arbitration{gameboy::Cartridge{test_rom()}};
    arbitration.write8(0xFE00, 0x34);
    arbitration.write8(0xFF40, 0x80);
    arbitration.tick(452 + 79);
    arbitration.write8(0xFE00, 0x55);
    arbitration.tick(1);
    check((arbitration.read8(0xFF41) & 0x03) == 2 &&
              arbitration.read8(0x8000) == 0xFF,
          "VRAM locks on the internal mode-3 boundary before STAT changes");
    arbitration.write8(0xFE00, 0x66);
    arbitration.tick(173);
    check(arbitration.read8(0xFE00) == 0x66,
          "DMG OAM accepts a write on the mode 2-to-3 transition dot");
}

void test_ppu_stat_interrupts() {
    gameboy::MemoryBus coincidence{gameboy::Cartridge{test_rom()}};
    coincidence.write8(0xFF45, 1);
    coincidence.write8(0xFF41, 0x40);
    coincidence.write8(0xFF40, 0x80);
    coincidence.write8(0xFF0F, 0);
    coincidence.tick(452);
    check((coincidence.read8(0xFF41) & 0x04) == 0,
          "coincidence clears while a new visible line is starting");
    coincidence.tick(1);
    check((coincidence.read8(0xFF41) & 0x04) != 0 &&
              (coincidence.read8(0xFF0F) & 0x02) != 0,
          "LY=LYC raises the coincidence flag and STAT interrupt");

    gameboy::MemoryBus retained{gameboy::Cartridge{test_rom()}};
    retained.write8(0xFF41, 0x40);
    retained.write8(0xFF45, 0);
    retained.write8(0xFF40, 0x80);
    retained.write8(0xFF40, 0);
    retained.write8(0xFF0F, 0);
    retained.write8(0xFF45, 1);
    check((retained.read8(0xFF41) & 0x04) != 0,
          "LCD-off LYC writes retain the stopped coincidence result");
    retained.write8(0xFF40, 0x80);
    check((retained.read8(0xFF41) & 0x07) == 0 &&
              (retained.read8(0xFF0F) & 0x02) == 0,
          "LCD startup refreshes coincidence without a stale STAT edge");

    gameboy::MemoryBus modes{gameboy::Cartridge{test_rom()}};
    modes.write8(0xFF41, 0x28); // Mode 2 and mode 0 interrupt sources.
    modes.write8(0xFF40, 0x80);
    check((modes.read8(0xFF0F) & 0x02) != 0,
          "enabling LCD in startup mode 0 can raise STAT");
    modes.write8(0xFF0F, 0);
    modes.tick(251);
    check((modes.read8(0xFF0F) & 0x02) == 0,
          "mode-0 STAT remains low through the last mode-3 dot");
    modes.tick(1);
    check((modes.read8(0xFF0F) & 0x02) != 0,
          "entering enabled mode 0 raises STAT on a rising edge");
    modes.write8(0xFF0F, 0);
    modes.tick(200);
    check((modes.read8(0xFF0F) & 0x02) == 0,
          "adjacent enabled STAT sources block a second interrupt edge");
}

void test_ppu_vblank_and_frame_publication() {
    constexpr auto startup_visible_cycles = 452U + 456U * 143U;
    gameboy::MemoryBus bus{gameboy::Cartridge{test_rom()}};
    bus.write8(0xFF41, 0x10); // Mode 1 STAT source.
    bus.write8(0xFF40, 0x80);
    bus.write8(0xFF0F, 0);
    bus.tick(startup_visible_cycles);
    check(bus.read8(0xFF44) == 144 && (bus.read8(0xFF41) & 0x03) == 1,
          "line 144 begins VBlank mode");
    check((bus.read8(0xFF0F) & 0x03) == 0x03,
          "entering VBlank requests VBlank and enabled mode-1 STAT interrupts");
    check(bus.frame_ready(), "entering VBlank publishes the completed frame");

    gameboy::MemoryBus mode2_vblank{gameboy::Cartridge{test_rom()}};
    mode2_vblank.write8(0xFF41, 0x20);
    mode2_vblank.write8(0xFF40, 0x80);
    mode2_vblank.write8(0xFF0F, 0);
    mode2_vblank.tick(startup_visible_cycles);
    check((mode2_vblank.read8(0xFF0F) & 0x02) != 0,
          "DMG mode-2 STAT selection also interrupts at VBlank entry");

    gameboy::MemoryBus wrapped_coincidence{
        gameboy::Cartridge{test_rom()}};
    wrapped_coincidence.write8(0xFF45, 0);
    wrapped_coincidence.write8(0xFF40, 0x80);
    wrapped_coincidence.tick(452 + 456 * 153);
    check(wrapped_coincidence.read8(0xFF44) == 0 &&
              (wrapped_coincidence.read8(0xFF41) & 0x04) != 0,
          "LY wraparound refreshes the cached coincidence flag for line zero");
    bus.consume_frame();
    check(!bus.frame_ready(), "frontend can acknowledge a published frame");
    bus.tick(456 * 10);
    check(bus.read8(0xFF44) == 0 && (bus.read8(0xFF41) & 0x03) == 0,
          "ten VBlank lines wrap LY before mode 2 becomes visible");
    bus.tick(1);
    check((bus.read8(0xFF41) & 0x03) == 2,
          "mode 2 becomes visible one dot after VBlank wraps");
}

void test_ppu_background_window_and_sprites() {
    gameboy::Emulator dmg_post_boot{
        gameboy::Cartridge{test_rom()}, gameboy::HardwareModel::dmg};
    constexpr std::array<std::uint8_t, 16> trademark_tile{
        0x3C, 0x00, 0x42, 0x00, 0xB9, 0x00, 0xA5, 0x00,
        0xB9, 0x00, 0xA5, 0x00, 0x42, 0x00, 0x3C, 0x00,
    };
    for (std::size_t index = 0; index < trademark_tile.size(); ++index) {
        check(dmg_post_boot.bus().read8(
                  static_cast<std::uint16_t>(0x8190 + index)) ==
                  trademark_tile[index],
              "DMG post-boot state preserves the trademark VRAM tile");
    }

    gameboy::MemoryBus background{gameboy::Cartridge{test_rom()}};
    background.write8(0xFF47, 0xE4); // Identity DMG palette.
    background.write8(0x8000, 0x80);
    background.write8(0x8001, 0x80); // Tile 0, first pixel color 3.
    background.write8(0x9800, 0x00);
    background.write8(0xFF40, 0x91);
    background.tick(254);
    check(background.framebuffer()[0] == 0xFF000000 &&
              background.framebuffer()[1] == 0xFFFFFFFF,
          "background tile data renders through BGP into the framebuffer");

    gameboy::MemoryBus mid_scanline{gameboy::Cartridge{test_rom()}};
    mid_scanline.write8(0xFF47, 0xE4);
    mid_scanline.write8(0x8000, 0xFF); // Tile 0 is color 1 throughout.
    mid_scanline.write8(0x8001, 0x00);
    mid_scanline.write8(0x9800, 0x00);
    mid_scanline.write8(0xFF40, 0x91);
    mid_scanline.tick(100);            // Pixels 0 through 8 have been emitted.
    mid_scanline.write8(0xFF47, 0xE8); // Map color 1 from shade 1 to shade 2.
    mid_scanline.tick(152);
    check(mid_scanline.framebuffer()[8] == 0xFFAAAAAA &&
              mid_scanline.framebuffer()[9] == 0xFF555555,
          "mode-3 palette writes affect only pixels emitted afterward");

    gameboy::MemoryBus fetch_latency{gameboy::Cartridge{test_rom()}};
    fetch_latency.write8(0xFF47, 0xE4);
    fetch_latency.write8(0x8010, 0xFF);
    fetch_latency.write8(0x8011, 0xFF); // Tile 1 is color 3.
    for (unsigned tile = 0; tile < 32; ++tile) {
        fetch_latency.write8(static_cast<std::uint16_t>(0x9C00 + tile), 1);
    }
    fetch_latency.write8(0xFF40, 0x91);
    fetch_latency.tick(93);
    fetch_latency.write8(0xFF40, 0x99); // Select $9C00 during mode 3.
    fetch_latency.tick(30);
    check(fetch_latency.framebuffer()[7] == 0xFFFFFFFF &&
              fetch_latency.framebuffer()[8] == 0xFF000000,
          "tile-map changes take effect at the fetcher's tile-map read phase");

    gameboy::MemoryBus window{gameboy::Cartridge{test_rom()}};
    window.write8(0xFF47, 0xE4);
    window.write8(0x8010, 0xFF);
    window.write8(0x8011, 0x00); // Tile 1 row is color 1.
    window.write8(0x9C00, 0x01);
    window.write8(0xFF4A, 0);
    window.write8(0xFF4B, 7);
    window.write8(0xFF40, 0xF1);
    window.tick(260);
    check(window.framebuffer()[0] == 0xFFAAAAAA,
          "enabled window uses WX/WY and its selected tile map");

    // WX values below seven all trigger at the visible left edge, but they
    // select a different column from the first queued window tile. Keep the
    // edge comparator explicit for every value covered by the exploratory
    // Mealybug WX cases instead of only testing the WX=0 and WX=7 endpoints.
    constexpr std::array<std::uint32_t, 4> window_shades{
        0xFFFFFFFF, 0xFFAAAAAA, 0xFF555555, 0xFF000000};
    for (std::uint8_t wx = 1; wx <= 6; ++wx) {
        gameboy::MemoryBus edge{gameboy::Cartridge{test_rom()}};
        edge.write8(0xFF47, 0xE4);
        edge.write8(0x8010, 0x55); // Columns 1,3,5,7 are color bit 0.
        edge.write8(0x8011, 0x33); // Columns 2,3,6,7 are color bit 1.
        edge.write8(0x9C00, 0x01);
        edge.write8(0xFF4A, 0);
        edge.write8(0xFF4B, wx);
        edge.write8(0xFF40, 0xF1);
        edge.tick(280);
        const auto source_column = static_cast<unsigned>(7 - wx);
        const auto color = ((source_column & 1U) != 0 ? 1U : 0U) |
                           ((source_column & 2U) != 0 ? 2U : 0U);
        check(edge.framebuffer()[0] == window_shades[color],
              "WX=1..6 starts at the visible edge with the correct tile column");
    }

    gameboy::MemoryBus window_lines{gameboy::Cartridge{test_rom()}};
    window_lines.write8(0xFF47, 0xE4);
    window_lines.write8(0x8010, 0xFF); // Tile 1 row 0: color 1.
    window_lines.write8(0x8011, 0x00);
    window_lines.write8(0x8012, 0x00); // Tile 1 row 1: color 2.
    window_lines.write8(0x8013, 0xFF);
    window_lines.write8(0x8014, 0xFF); // Tile 1 row 2: color 3.
    window_lines.write8(0x8015, 0xFF);
    window_lines.write8(0x9C00, 0x01);
    window_lines.write8(0xFF4A, 0);
    window_lines.write8(0xFF4B, 7);
    window_lines.write8(0xFF40, 0xF1);
    window_lines.tick(260);
    window_lines.write8(0xFF40, 0xD1); // Hide the window for line 1.
    window_lines.tick(198);
    window_lines.tick(456);
    window_lines.write8(0xFF40, 0xF1);
    window_lines.tick(258);
    check(window_lines.framebuffer()[2 * gameboy::Ppu::screen_width] ==
              0xFF555555,
          "the internal window line advances only on lines that draw the window");

    gameboy::MemoryBus sprites{gameboy::Cartridge{test_rom()}};
    sprites.write8(0xFF47, 0xE4);
    sprites.write8(0xFF48, 0xE4);
    sprites.write8(0x8010, 0x00);
    sprites.write8(0x8011, 0x80); // Tile 1, first pixel color 2.
    sprites.write8(0xFE00, 16);   // Screen Y = 0.
    sprites.write8(0xFE01, 8);    // Screen X = 0.
    sprites.write8(0xFE02, 1);
    sprites.write8(0xFE03, 0);
    sprites.write8(0xFF40, 0x93);
    sprites.tick(265);
    check(sprites.framebuffer()[0] == 0xFF555555,
          "visible OBJ pixels render with their selected DMG palette");

    gameboy::MemoryBus colored{gameboy::Cartridge{test_rom()}};
    gameboy::DmgPalette layer_colors{
        {0xFFFF0000, 0xFFFF0000, 0xFFFF0000, 0xFFFF0000},
        {0xFF00FF00, 0xFF00FF00, 0xFF00FF00, 0xFF00FF00},
        {0xFF0000FF, 0xFF0000FF, 0xFF0000FF, 0xFF0000FF}};
    colored.set_dmg_palette(layer_colors);
    colored.write8(0xFF47, 0xE4);
    colored.write8(0xFF48, 0xE4);
    colored.write8(0xFF49, 0xE4);
    colored.write8(0x8010, 0x00);
    colored.write8(0x8011, 0x80);
    colored.write8(0xFE00, 16);
    colored.write8(0xFE01, 8);
    colored.write8(0xFE02, 1);
    colored.write8(0xFE03, 0);
    colored.write8(0xFE04, 16);
    colored.write8(0xFE05, 16);
    colored.write8(0xFE06, 1);
    colored.write8(0xFE07, 0x10);
    colored.write8(0xFF40, 0x93);
    colored.tick(276);
    check(colored.framebuffer()[0] == 0xFF00FF00 &&
              colored.framebuffer()[8] == 0xFF0000FF &&
              colored.framebuffer()[9] == 0xFFFF0000,
          "DMG rendering routes background, OBJ0, and OBJ1 independently");
}

void test_joypad_matrix_and_interrupts() {
    gameboy::MemoryBus bus{gameboy::Cartridge{test_rom()}};
    check(bus.read8(0xFF00) == 0xFF,
          "joypad starts with neither input group selected");

    bus.set_button(gameboy::Button::a, true);
    check(bus.read8(0xFF00) == 0xFF && (bus.read8(0xFF0F) & 0x10) == 0,
          "an unselected pressed button does not pull an input line low");
    bus.write8(0xFF00, 0x10);
    check(bus.read8(0xFF00) == 0xDE,
          "action selection exposes A on input line zero");
    check((bus.read8(0xFF0F) & 0x10) != 0,
          "selecting an already-pressed button requests the joypad interrupt");

    bus.write8(0xFF0F, 0);
    bus.set_button(gameboy::Button::a, false);
    check((bus.read8(0xFF0F) & 0x10) == 0,
          "button release does not request a joypad interrupt");
    bus.set_button(gameboy::Button::start, true);
    check(bus.read8(0xFF00) == 0xD7 && (bus.read8(0xFF0F) & 0x10) != 0,
          "selected action press updates FF00 and requests interrupt 4");

    bus.write8(0xFF0F, 0);
    bus.write8(0xFF00, 0x20);
    bus.set_button(gameboy::Button::right, true);
    bus.set_button(gameboy::Button::up, true);
    check(bus.read8(0xFF00) == 0xEA,
          "direction selection exposes active-low directional lines");
    check((bus.read8(0xFF0F) & 0x10) != 0,
          "selected direction presses request the joypad interrupt");

    bus.write8(0xFF00, 0x00);
    check((bus.read8(0xFF00) & 0x0F) == 0x02,
          "selecting both groups combines their active-low input lines");
}

void test_oam_dma() {
    gameboy::MemoryBus bus{gameboy::Cartridge{test_rom()}};
    for (unsigned offset = 0; offset < 0xA0; ++offset) {
        bus.write8(static_cast<std::uint16_t>(0xC000 + offset),
                   static_cast<std::uint8_t>(offset + 1));
    }
    bus.write8(0xFF46, 0xC0);
    check(bus.read8(0xFE00) == 0,
          "OAM DMA does not copy its first byte immediately");
    bus.tick(7);
    check(bus.read8(0xFE00) == 0,
          "fresh OAM DMA observes its startup delay");
    bus.tick(1);
    bus.tick(3);
    check(bus.read8(0xFE00) == 0,
          "active OAM DMA waits four cycles to finish its first byte");
    bus.tick(1);
    check(bus.read8(0xFE00) == 1 && bus.read8(0xFE01) == 0,
          "OAM DMA copies one byte every four active cycles");
    bus.tick(4 * 158);
    check(bus.read8(0xFE9E) == 0x9F && bus.read8(0xFE9F) == 0,
          "OAM DMA remains active until the final byte interval");
    bus.tick(4);
    for (unsigned offset = 0; offset < 0xA0; ++offset) {
        check(bus.read8(static_cast<std::uint16_t>(0xFE00 + offset)) ==
                  static_cast<std::uint8_t>(offset + 1),
              "OAM DMA copies all 160 source bytes");
    }
    check(bus.read8(0xFF46) == 0xC0, "DMA register retains its source page");

    gameboy::MemoryBus high_source{gameboy::Cartridge{test_rom()}};
    high_source.write8(0xDE00, 0xA5);
    high_source.write8(0xDF00, 0x5A);
    high_source.write8(0xFF46, 0xFE);
    high_source.tick(648);
    check(high_source.read8(0xFE00) == 0xA5,
          "DMG OAM DMA aliases source page FE to WRAM page DE");
    high_source.write8(0xFF46, 0xFF);
    high_source.tick(648);
    check(high_source.read8(0xFE00) == 0x5A,
          "DMG OAM DMA aliases source page FF to WRAM page DF");

    gameboy::MemoryBus startup{
        gameboy::Cartridge{test_rom({0x3E, 0x42})}}; // LD A,42
    startup.write8(0xFF46, 0xC0);
    gameboy::Cpu startup_cpu;
    check(startup_cpu.step(startup) == 8 &&
              startup_cpu.registers().pc == 0x0102 &&
              startup_cpu.registers().a == 0xFF,
          "fresh DMA permits the first CPU cycle before blocking the bus");

    gameboy::MemoryBus video_dma{
        gameboy::Cartridge{test_rom({0xFA, 0x00, 0xC0})}}; // LD A,(C000)
    video_dma.write8(0x8000, 0x66);
    video_dma.write8(0xC000, 0x42);
    video_dma.write8(0xFF46, 0x80);
    gameboy::Cpu video_dma_cpu;
    check(video_dma_cpu.step(video_dma) == 16 &&
              video_dma_cpu.registers().a == 0x42,
          "VRAM-source DMA leaves the CPU main bus accessible");

    gameboy::MemoryBus split_buses{gameboy::Cartridge{test_rom()}};
    split_buses.write8(0x8000, 0x66);
    split_buses.write8(0xFF80, 0xFA); // LD A,(8000), executing from HRAM.
    split_buses.write8(0xFF81, 0x00);
    split_buses.write8(0xFF82, 0x80);
    split_buses.write8(0xFF46, 0x80);
    split_buses.tick(8);
    gameboy::Cpu split_cpu;
    auto split_registers = split_cpu.registers();
    split_registers.pc = 0xFF80;
    split_cpu.load_registers(split_registers);
    check(split_cpu.step(split_buses) == 16 &&
              split_cpu.registers().a == 0xFF,
          "VRAM-source DMA blocks CPU accesses to the video bus");

    split_buses.write8(0xFF46, 0xC0);
    split_buses.tick(8);
    split_registers = split_cpu.registers();
    split_registers.pc = 0xFF80;
    split_cpu.load_registers(split_registers);
    check(split_cpu.step(split_buses) == 16 &&
              split_cpu.registers().a == 0x66,
          "main-bus DMA leaves CPU VRAM accesses available");

    gameboy::MemoryBus blocked{gameboy::Cartridge{test_rom()}};
    blocked.write8(0xC000, 0x42);
    blocked.write8(0xC100, 0x24);
    blocked.write8(0xD000, 0x99);
    blocked.write8(0xFF80, 0xFA); // LD A,(C000), executing from HRAM.
    blocked.write8(0xFF81, 0x00);
    blocked.write8(0xFF82, 0xC0);
    blocked.write8(0xFF83, 0xE0); // LDH (46),A, restart from page D0.
    blocked.write8(0xFF84, 0x46);
    blocked.write8(0xFF85, 0xEA); // LD (C000),A; blocked during DMA.
    blocked.write8(0xFF86, 0x00);
    blocked.write8(0xFF87, 0xC0);
    blocked.write8(0xFF88, 0xF0); // LDH A,(46); readable during DMA.
    blocked.write8(0xFF89, 0x46);
    blocked.write8(0xFF46, 0xC1);

    gameboy::Cpu cpu;
    auto registers = cpu.registers();
    registers.pc = 0xFF80;
    cpu.load_registers(registers);
    check(cpu.step(blocked) == 16 && cpu.registers().a == 0xFF,
          "OAM DMA blocks CPU reads outside high RAM");
    registers = cpu.registers();
    registers.a = 0xD0;
    cpu.load_registers(registers);
    check(cpu.step(blocked) == 12,
          "the CPU continues executing DMA routines from high RAM");
    check(cpu.step(blocked) == 16 && blocked.read8(0xC000) == 0x42,
          "OAM DMA ignores CPU writes outside high RAM");
    check(blocked.read8(0xFE00) == 0x99 && blocked.read8(0xC000) == 0x42,
          "writing FF46 during OAM DMA restarts the transfer");
    check(cpu.step(blocked) == 12 && cpu.registers().a == 0xD0,
          "the CPU can read the FF46 register during OAM DMA");

    gameboy::Emulator snapshot{gameboy::Cartridge{test_rom()}};
    snapshot.bus().write8(0xFF40, 0); // Keep OAM visible to the test harness.
    snapshot.bus().write8(0xC000, 0x11);
    snapshot.bus().write8(0xC001, 0x22);
    snapshot.bus().write8(0xC002, 0x33);
    snapshot.bus().write8(0xFF46, 0xC0);
    snapshot.bus().tick(18);
    const auto state = snapshot.save_state();
    snapshot.bus().tick(2);
    check(snapshot.bus().read8(0xFE02) == 0x33,
          "an active OAM DMA transfer continues before state restoration");
    snapshot.load_state(state);
    check(snapshot.bus().read8(0xFE02) == 0,
          "save states restore partially copied OAM DMA data");
    snapshot.bus().tick(1);
    check(snapshot.bus().read8(0xFE02) == 0,
          "save states restore the OAM DMA sub-byte cycle");
    snapshot.bus().tick(1);
    check(snapshot.bus().read8(0xFE02) == 0x33,
          "restored OAM DMA resumes on the original cycle");

    gameboy::Emulator pending_snapshot{gameboy::Cartridge{test_rom()}};
    pending_snapshot.bus().write8(0xFF40, 0);
    pending_snapshot.bus().write8(0xC000, 0x77);
    pending_snapshot.bus().write8(0xFF46, 0xC0);
    pending_snapshot.bus().tick(3);
    const auto pending_state = pending_snapshot.save_state();
    pending_snapshot.bus().tick(9);
    check(pending_snapshot.bus().read8(0xFE00) == 0x77,
          "pending OAM DMA reaches its first byte before restoration");
    pending_snapshot.load_state(pending_state);
    pending_snapshot.bus().tick(8);
    check(pending_snapshot.bus().read8(0xFE00) == 0,
          "save states restore the fresh OAM DMA startup delay");
    pending_snapshot.bus().tick(1);
    check(pending_snapshot.bus().read8(0xFE00) == 0x77,
          "restored pending OAM DMA resumes at the original startup cycle");
}


} // namespace

int main() {
    test_cpu_machine_cycle_bus_timing();
    test_ppu_modes_and_memory_access();
    test_ppu_stat_interrupts();
    test_ppu_vblank_and_frame_publication();
    test_ppu_background_window_and_sprites();
    test_joypad_matrix_and_interrupts();
    test_oam_dma();
    return failures == 0 ? 0 : 1;
}
