#include "gameboy/cpu.hpp"
#include "gameboy/dmg_palette.hpp"
#include "gameboy/emulator.hpp"
#include "gameboy/gameshark.hpp"
#include "gameboy/link_session.hpp"
#include "gameboy/link_transport.hpp"
#include "gameboy/tcp_link_channel.hpp"
#include "gameboy/tcp_serial_endpoint.hpp"
#include "gameboy/memory_bus.hpp"
#include "gameboy/rom_library.hpp"
#include "gameboy/video_pipeline.hpp"
#include "gbb/core_registry.hpp"
#include "gbb/gameboy_core.hpp"
#include "gbb/scene_json.hpp"
#include "gbb/audio.hpp"
#include "gbb/dashboard_navigation.hpp"
#include "gbb/touch_control.hpp"
#include "gbb/voxel_profile.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
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

void test_touch_control_ownership() {
    const std::optional<std::size_t> dpad_right{0};
    const std::optional<std::size_t> button_a{4};
    check(gbb::retain_touch_control(dpad_right, std::nullopt) == dpad_right,
          "touch ownership survives motion through neutral space");
    check(gbb::retain_touch_control(dpad_right, button_a) == button_a,
          "touch ownership transfers when another control is entered");
    check(!gbb::retain_touch_control(std::nullopt, std::nullopt).has_value(),
          "neutral touch remains unassigned until it enters a control");
}

void test_desktop_dashboard_navigation() {
    using gbb::desktop::DashboardAction;
    const auto without_resume =
        gbb::desktop::dashboard_navigation_items(false, 2);
    check(without_resume.size() == 7,
          "dashboard includes fixed actions, recents, and exit");
    check(without_resume.front().action == DashboardAction::open_rom,
          "dashboard opens with Open ROM when no game can resume");
    check(without_resume[4].action == DashboardAction::recent_rom &&
              without_resume[4].recent_index == 0 &&
              without_resume[5].recent_index == 1,
          "dashboard recents preserve their source order");
    check(without_resume.back().action == DashboardAction::quit,
          "dashboard always ends with Exit GBB");

    const auto with_resume = gbb::desktop::dashboard_navigation_items(true, 0);
    check(with_resume.size() == 6 &&
              with_resume.front().action == DashboardAction::resume,
          "dashboard puts Resume first when a game is active");
    check(gbb::desktop::dashboard_move_selection(0, 6, -1) == 5 &&
              gbb::desktop::dashboard_move_selection(5, 6, 1) == 0,
          "keyboard navigation wraps at both dashboard ends");
    check(gbb::desktop::dashboard_move_selection(99, 6, -1) == 4,
          "keyboard navigation clamps an out-of-range selection");
    check(gbb::desktop::dashboard_scroll_selection(0, 6, -1) == 0 &&
              gbb::desktop::dashboard_scroll_selection(5, 6, 1) == 5,
          "wheel navigation clamps at dashboard ends");
    check(gbb::desktop::dashboard_first_visible(0, 10, 5) == 0 &&
              gbb::desktop::dashboard_first_visible(4, 10, 5) == 0 &&
              gbb::desktop::dashboard_first_visible(5, 10, 5) == 1 &&
              gbb::desktop::dashboard_first_visible(99, 10, 5) == 5,
          "dashboard scroll window keeps the selected row visible");
    check(gbb::desktop::dashboard_move_selection(3, 0, 1) == 0 &&
              gbb::desktop::dashboard_first_visible(0, 0, 5) == 0,
          "dashboard navigation is safe before items are populated");
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

gameboy::CpuRegisters initial_registers() {
    return {
        0x77, 0xB0, 0x11, 0x22, 0x33, 0x44, 0xC0, 0x00, 0xFFFE,
        program_address,
    };
}

std::uint8_t register_value(const gameboy::CpuRegisters& registers,
                            const unsigned index) {
    switch (index) {
    case 0: return registers.b;
    case 1: return registers.c;
    case 2: return registers.d;
    case 3: return registers.e;
    case 4: return registers.h;
    case 5: return registers.l;
    default: return registers.a;
    }
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

void test_sgb_command_path() {
    auto rom = test_rom();
    rom[0x146] = 0x03;
    gameboy::Emulator emulator{gameboy::Cartridge{std::move(rom)}};

    // PAL01 is a one-packet command.  The packet carries a shared color 0
    // followed by three colors for each of palettes 0 and 1.
    std::array<std::uint8_t, 16> command{};
    command[0] = 0x01;
    command[1] = 0x1F; // RGB555 red
    command[2] = 0x00;
    command[3] = 0x00;
    command[4] = 0x7C; // RGB555 blue
    command[5] = 0xE0; // RGB555 green
    command[6] = 0x03;
    command[7] = 0xFF;
    command[8] = 0x7F;
    command[9] = 0x1F;
    command[10] = 0x00;
    command[11] = 0x00;
    command[12] = 0x7C;
    command[13] = 0xE0;
    command[14] = 0x03;

    auto write = [&](const std::uint8_t value) {
        emulator.bus().write8(0xFF00, value);
    };
    write(0x30); // Arm and delimit the command.
    write(0x00);
    for (std::size_t bit = 0; bit < command.size() * 8; ++bit) {
        write(0x30);
        write((command[bit / 8] & (1U << (bit & 7U))) != 0 ? 0x10 : 0x20);
    }
    write(0x30);
    write(0x20); // Zero delimiter commits the complete packet.

    // The command path is end-to-end: JOYP decoding queues the packet and
    // MemoryBus immediately applies it to the SGB PPU state.
    emulator.bus().write8(0xFF40, 0x00);
    emulator.bus().write8(0xFF40, 0x91);
    emulator.bus().tick(70224);
    check(emulator.framebuffer()[0] == 0xFFFF0000,
          "SGB PAL01 packet updates the rendered RGB555 palette");
}

void test_rom_library_metadata_and_deduplication() {
    auto rom = cgb_test_rom();
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
              metadata.cover_name ==
                  "Pokemon Blue (USA) (En,Fr,De)",
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
              restored.entries()[0].metadata.fingerprint ==
                  other.fingerprint &&
              restored.entries()[0].metadata.crc32 == other.crc32 &&
              restored.entries()[1].metadata.fingerprint ==
                  metadata.fingerprint,
          "ROM library metadata persists in recency order");
    auto removable = restored;
    check(removable.remove(other.fingerprint) &&
              removable.entries().size() == 1 &&
              !removable.remove(other.fingerprint),
          "ROM library entries can be removed by stable fingerprint");
    std::filesystem::remove_all(directory);
}

void test_multicore_frontend_contract() {
    const auto& registry = gbb::built_in_core_registry();
    check(registry.factories().size() == 1 &&
              registry.factories().front().core_id == "gb",
          "built-in core registry exposes the GB/GBC adapter");

    auto dmg_rom = test_rom({0x00});
    const auto dmg_probe = registry.probe(dmg_rom);
    check(dmg_probe.confidence > 0 &&
              dmg_probe.system == gbb::SystemId::game_boy,
          "core registry identifies Game Boy ROMs without frontend knowledge");
    auto cgb_rom = cgb_test_rom();
    check(registry.probe(cgb_rom).system == gbb::SystemId::game_boy_color,
          "core registry distinguishes Game Boy Color software");

    auto core = registry.create(std::move(dmg_rom));
    const auto& descriptor = core->descriptor();
    check(descriptor.core_id == "gb" &&
              descriptor.system == gbb::SystemId::game_boy &&
              descriptor.video_width == 160 && descriptor.video_height == 144 &&
              descriptor.nominal_cycles_per_frame == 70224 &&
              descriptor.audio_channels == 2 && descriptor.input_count == 8,
          "generic core descriptor carries media, timing, system, and input data");
    check(gbb::has_capability(descriptor.capabilities,
                              gbb::CoreCapability::debugger) &&
              gbb::has_capability(descriptor.capabilities,
                                  gbb::CoreCapability::link_cable) &&
              gbb::gameboy_emulator(core.get()) != nullptr,
          "optional GB development tools are capability-gated behind the adapter");

    const auto state = core->save_state();
    core->set_input(gbb::InputId::start, true);
    static_cast<void>(core->step_instruction());
    core->load_state(state);
    check(core->save_state() == state,
          "generic core state operations round trip deterministically");
    const auto frame = core->video_frame();
    check(frame.pixels != nullptr && frame.pixel_count == 160 * 144 &&
              frame.pitch == 160 * sizeof(std::uint32_t),
          "generic video frames describe their own dimensions and pitch");

    bool unsupported_rejected = false;
    try {
        static_cast<void>(registry.create(std::vector<std::uint8_t>(32)));
    } catch (const std::runtime_error&) {
        unsupported_rejected = true;
    }
    check(unsupported_rejected,
          "registry rejects ROMs for which no installed core claims support");

    gbb::CoreRegistry extensible;
    extensible.register_factory({
        "test-gba", "Test GBA",
        [](const std::vector<std::uint8_t>& bytes,
           const gbb::CoreLoadOptions&) noexcept {
            return gbb::CoreProbeResult{
                bytes.size() >= 4 ? 100 : 0,
                bytes.size() >= 4 ? gbb::SystemId::game_boy_advance
                                  : gbb::SystemId::unknown};
        },
        [](std::vector<std::uint8_t>, const gbb::CoreLoadOptions&)
            -> std::unique_ptr<gbb::EmulatorCore> { return {}; }});
    check(extensible.probe(std::vector<std::uint8_t>(4)).system ==
              gbb::SystemId::game_boy_advance,
          "new systems can register a core without changing frontend code");
}

void test_scene_snapshot_contract() {
    auto core = gbb::create_core(cgb_test_rom());
    check(gbb::has_capability(core->descriptor().capabilities,
                              gbb::CoreCapability::scene_layers),
          "GB core advertises optional scene-layer data");

    auto* emulator = gbb::gameboy_emulator(core.get());
    check(emulator != nullptr, "scene test can access the GB development adapter");
    if (emulator == nullptr) return;

    auto& bus = emulator->bus();
    bus.debug_write_vram(0, 0x1800, 0x2A);
    bus.debug_write_vram(1, 0x1800, 0x60);
    bus.debug_write_vram(0, static_cast<std::uint16_t>(0x2A * 16), 0xAB);
    bus.write8(0xFF40, 0x93); // Enable LCD, background, and OBJ rendering.
    bus.debug_write_oam(0, 48);
    bus.debug_write_oam(1, 40);
    bus.debug_write_oam(2, 0x2A);
    bus.debug_write_oam(3, 0x20);

    const auto& scene = core->scene_snapshot();
    check(scene.width == 160 && scene.height == 144 && scene.cgb_mode,
          "scene snapshot carries core dimensions and hardware mode");
    check(scene.background.enabled && scene.background.map_address == 0x9800 &&
              scene.background.tile_ids[0] == 0x2A &&
              scene.background.attributes[0] == 0x60,
          "scene snapshot exposes the selected background tile and CGB attributes");
    check(scene.tile_data[0x2A * 16] == 0xAB,
          "scene snapshot exposes banked tile graphics");
    check(scene.sprites[0].visible && scene.sprites[0].screen_x == 32 &&
              scene.sprites[0].screen_y == 32 && scene.sprites[0].tile == 0x2A &&
              scene.sprites[0].attributes == 0x20,
          "scene snapshot decodes visible OAM coordinates and attributes");
    check(scene.window.map_address == 0x9800 && !scene.window.enabled,
          "scene snapshot reports disabled window state without special casing the frontend");
}

void test_scene_snapshot_json() {
    auto core = gbb::create_core(cgb_test_rom());
    const auto& scene = core->scene_snapshot();
    const auto json = gbb::scene_snapshot_to_json(scene);
    check(json.rfind("{\"schema\":\"gbb.scene.v1\"", 0) == 0 &&
              json.back() == '\n' && json[json.size() - 2] == '}',
          "scene snapshots serialize with a versioned JSON envelope");
    check(json.find("\"background\":{") != std::string::npos &&
              json.find("\"tile_data\":[") != std::string::npos &&
              json.find("\"sprites\":[") != std::string::npos,
          "scene JSON includes tile layers, graphics data, and sprites");

    const auto path = std::filesystem::temp_directory_path() /
                      "gbb-scene-snapshot-test.json";
    check(gbb::write_scene_snapshot_json(scene, path),
          "scene snapshots can be exported to a file");
    std::ifstream input(path, std::ios::binary);
    const std::string saved((std::istreambuf_iterator<char>(input)),
                            std::istreambuf_iterator<char>());
    check(saved == json, "exported scene JSON matches the in-memory document");
    // Windows keeps an open input stream locked, so close it before removing
    // the temporary export. POSIX systems permit the unlink while open, which
    // previously masked this portability issue in local/Linux test runs.
    input.close();
    std::filesystem::remove(path);
}

void test_gameshark_cheats() {
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

void test_video_pipeline_modes() {
    check(gameboy::video_mode_from_id("nearest") == gameboy::VideoMode::nearest &&
              gameboy::video_mode_from_id("bilinear") == gameboy::VideoMode::bilinear &&
              gameboy::video_mode_from_id("sharp") ==
                  gameboy::VideoMode::sharp_smoothing &&
              gameboy::video_mode_from_id("integer") == gameboy::VideoMode::integer &&
              gameboy::video_mode_from_id("lcd") == gameboy::VideoMode::lcd_shader &&
              gameboy::video_mode_from_id("voxel") == gameboy::VideoMode::voxel_diorama &&
              gameboy::video_mode_from_id("voxel_shape") ==
                  gameboy::VideoMode::voxel_shape &&
              gameboy::video_mode_from_id("voxel_popup") ==
                  gameboy::VideoMode::voxel_popup &&
              gameboy::video_modes.size() == 8,
          "video pipeline settings map stable ids to presentation modes");
    check(gameboy::video_mode_from_id("unknown") == gameboy::default_video_mode,
          "unknown video pipeline settings use the nearest default");
    constexpr auto source = UINT32_C(0xFFCC8844);
    check(gameboy::apply_lcd_shader(source, 1, 0) != source &&
              (gameboy::apply_lcd_shader(source, 1, 0) & UINT32_C(0xFF000000)) ==
                  UINT32_C(0xFF000000),
          "LCD shader changes RGB channels while preserving alpha");
    constexpr auto left = UINT32_C(0xFF202020);
    constexpr auto right = UINT32_C(0xFFE0E0E0);
    check(gameboy::apply_sharp_smoothing(source, left, right, source, source) !=
              source,
          "sharp smoothing adjusts high-contrast edges");
}

void test_voxel_profiles() {
    const auto path = std::filesystem::temp_directory_path() /
                      "gbb-voxel-profile-test.ini";
    std::filesystem::remove(path);
    gbb::ensure_voxel_profile_file(path);
    const auto fingerprint = UINT64_C(0x1234ABCD);
    auto profile = gbb::load_voxel_profile(path, fingerprint);
    check(profile.depth_scale == 1.0F && profile.camera_pitch == 24.0F &&
              profile.camera_yaw == 0.0F && profile.zoom == 0.72F &&
              profile.perspective == 0.0015F && profile.sprite_depth == 8.0F &&
              profile.background_depth_far == 100.0F &&
              profile.background_depth_near == 20.0F &&
              profile.background_transparent_depth == 95.0F &&
              profile.window_depth_far == 90.0F &&
              profile.window_depth_near == 50.0F &&
              profile.sprite_depth_far == 45.0F &&
              profile.sprite_depth_near == 25.0F &&
              !profile.framebuffer_facade,
          "voxel profile defaults are loaded");
    const auto super_mario_land =
        gbb::load_voxel_profile(path, UINT64_C(0x7eafc0023b31d850));
    check(super_mario_land.depth_scale == 1.25F &&
              super_mario_land.zoom == 0.74F &&
              super_mario_land.perspective == 0.0012F &&
              super_mario_land.sprite_depth == 10.0F &&
              super_mario_land.lighting == 1.08F,
          "Super Mario Land receives its specialized voxel profile");
    profile.depth_scale = 2.25F;
    profile.camera_yaw = -12.0F;
    profile.background_depth_near = 18.0F;
    profile.window_depth_near = 48.0F;
    profile.sprite_depth_near = 22.0F;
    profile.framebuffer_facade = false;
    check(gbb::save_voxel_profile(path, fingerprint, profile),
          "voxel profile can be saved");
    const auto loaded = gbb::load_voxel_profile(path, fingerprint);
    check(loaded.depth_scale == 2.25F && loaded.camera_yaw == -12.0F &&
              loaded.background_depth_near == 18.0F &&
              loaded.window_depth_near == 48.0F &&
              loaded.sprite_depth_near == 22.0F &&
              !loaded.framebuffer_facade,
          "voxel profile round-trips per-ROM values");
    auto second = gbb::VoxelProfile{};
    second.depth_scale = 3.5F;
    const auto second_fingerprint = UINT64_C(0xFEDCBA98);
    check(gbb::save_voxel_profile(path, second_fingerprint, second),
          "voxel profile saves a second ROM section");
    check(gbb::load_voxel_profile(path, fingerprint).depth_scale == 2.25F &&
              gbb::load_voxel_profile(path, second_fingerprint).depth_scale == 3.5F,
          "saving one voxel profile preserves other ROM sections");
    std::filesystem::remove(path);
}

void test_audio_frontend_helpers() {
    const std::vector<std::int16_t> stereo{
        100, -100, 300, -300, 500, -500, 700, -700};
    check(gbb::downsample_audio_box(stereo, 2, 2) ==
              std::vector<std::int16_t>({200, -200, 600, -600}),
          "fast-forward audio uses a stereo-preserving box filter");
    check(gbb::audio_queue_bytes(48000, 2, 200) == 38400,
          "audio queue limits convert milliseconds to interleaved PCM bytes");
}

std::uint64_t audio_waveform_signature(
    const std::vector<std::int16_t>& samples) noexcept {
    // Quantize away six low bits before hashing. This keeps the regression
    // stable across platforms whose floating-point mixers round a boundary
    // sample one unit differently, while still catching waveform/timing
    // changes across the complete stereo buffer.
    auto hash = UINT64_C(1469598103934665603);
    for (const auto sample : samples) {
        const auto quantized = static_cast<std::int16_t>(sample / 64);
        const auto bits = static_cast<std::uint16_t>(quantized);
        hash ^= static_cast<std::uint8_t>(bits);
        hash *= UINT64_C(1099511628211);
        hash ^= static_cast<std::uint8_t>(bits >> 8);
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

struct AudioWaveformReference {
    bool format_valid{};
    std::string name;
    std::string model;
    unsigned sample_rate{};
    unsigned channels{};
    unsigned quantization{};
    std::size_t sample_count{};
    std::int16_t max_abs_error{};
    std::int16_t rms_error{};
    std::vector<std::int16_t> samples;
};

std::optional<AudioWaveformReference> load_audio_waveform_reference(
    const std::filesystem::path& path) {
    std::ifstream input(path);
    if (!input) return std::nullopt;

    AudioWaveformReference reference;
    std::string line;
    bool data_started = false;
    while (std::getline(input, line)) {
        if (line.empty() || line.front() == '#') continue;
        if (line == "data=") {
            data_started = true;
            continue;
        }
        if (data_started) {
            std::istringstream values(line);
            int sample = 0;
            while (values >> sample) {
                if (sample < std::numeric_limits<std::int16_t>::min() ||
                    sample > std::numeric_limits<std::int16_t>::max()) {
                    return std::nullopt;
                }
                reference.samples.push_back(static_cast<std::int16_t>(sample));
            }
            continue;
        }

        const auto separator = line.find('=');
        if (separator == std::string::npos) return std::nullopt;
        const auto key = line.substr(0, separator);
        const auto value = line.substr(separator + 1);
        try {
            if (key == "name") reference.name = value;
            else if (key == "model") reference.model = value;
            else if (key == "format") reference.format_valid =
                value == "gbb-audio-waveform-v1";
            else if (key == "sample_rate") reference.sample_rate =
                static_cast<unsigned>(std::stoul(value));
            else if (key == "channels") reference.channels =
                static_cast<unsigned>(std::stoul(value));
            else if (key == "quantization") reference.quantization =
                static_cast<unsigned>(std::stoul(value));
            else if (key == "samples") reference.sample_count =
                static_cast<std::size_t>(std::stoull(value));
            else if (key == "max_abs_error") reference.max_abs_error =
                static_cast<std::int16_t>(std::stoi(value));
            else if (key == "rms_error") reference.rms_error =
                static_cast<std::int16_t>(std::stoi(value));
        } catch (const std::exception&) {
            return std::nullopt;
        }
    }
    if (!reference.format_valid || reference.sample_rate == 0 ||
        reference.channels == 0 ||
        reference.quantization == 0 || reference.sample_count == 0 ||
        reference.samples.size() != reference.sample_count) {
        return std::nullopt;
    }
    return reference;
}

bool write_audio_waveform_reference(const std::filesystem::path& path,
                                    const std::string_view name,
                                    const std::string_view model,
                                    const std::vector<std::int16_t>& samples) {
    std::ofstream output(path);
    if (!output) return false;
    output << "# GBB audio waveform reference v1\n"
           << "format=gbb-audio-waveform-v1\n"
           << "name=" << name << '\n'
           << "model=" << model << '\n'
           << "sample_rate=48000\n"
           << "channels=2\n"
           << "quantization=64\n"
           << "samples=" << samples.size() << '\n'
           << "max_abs_error=0\n"
           << "rms_error=0\n"
           << "data=\n";
    for (std::size_t index = 0; index < samples.size(); ++index) {
        output << samples[index] / 64;
        if (index + 1 == samples.size() || (index + 1) % 16 == 0) {
            output << '\n';
        } else {
            output << ' ';
        }
    }
    return static_cast<bool>(output);
}

void compare_audio_waveform_reference(
    const std::filesystem::path& path, const std::string_view name,
    const std::string_view model,
    const std::vector<std::int16_t>& rendered) {
    const auto reference = load_audio_waveform_reference(path);
    check(reference.has_value(),
          "audio reference " + std::string{name} + " has a valid format");
    if (!reference) return;

    const auto quantized = [](const std::int16_t sample) {
        return static_cast<std::int16_t>(sample / 64);
    };
    check(reference->name == name && reference->model == model &&
              reference->sample_rate == 48000 &&
              reference->channels == 2 &&
              reference->quantization == 64 &&
              rendered.size() == reference->samples.size(),
          "audio reference " + std::string{name} +
              " matches the 48 kHz stereo render shape");
    if (rendered.size() != reference->samples.size()) return;

    std::int32_t max_error = 0;
    long double squared_error = 0;
    std::size_t first_mismatch = rendered.size();
    for (std::size_t index = 0; index < rendered.size(); ++index) {
        const auto error = static_cast<std::int32_t>(quantized(rendered[index])) -
                           static_cast<std::int32_t>(reference->samples[index]);
        max_error = std::max(max_error, std::abs(error));
        squared_error += static_cast<long double>(error) * error;
        if (first_mismatch == rendered.size() && error != 0) {
            first_mismatch = index;
        }
    }
    const auto rms = static_cast<std::int32_t>(std::sqrt(
        squared_error / static_cast<long double>(rendered.size())));
    const auto within_tolerance = max_error <= reference->max_abs_error &&
                                  rms <= reference->rms_error;
    std::string detail = "audio reference " + std::string{name} +
                         " stays within waveform tolerance";
    if (!within_tolerance) {
        detail += " (max=" + std::to_string(max_error) +
                  ", rms=" + std::to_string(rms);
        if (first_mismatch != rendered.size()) {
            detail += ", first sample=" + std::to_string(first_mismatch);
        }
        detail += ')';
    }
    check(within_tolerance, detail);
}

void test_apu_waveform_regressions() {
    const auto render = [](const gameboy::HardwareModel model, const auto configure) {
        gameboy::MemoryBus bus{gameboy::Cartridge{test_rom()}};
        bus.initialize_post_boot(model);
        bus.write8(0xFF24, 0x77);
        configure(bus);
        bus.tick(8192);
        return bus.take_audio_samples();
    };

    const auto pulse_setup = [](gameboy::MemoryBus& bus) {
        bus.write8(0xFF25, 0x11); // Channel 1 to both terminals.
        bus.write8(0xFF11, 0x80); // 50% duty.
        bus.write8(0xFF12, 0xF0); // Volume 15, envelope disabled.
        bus.write8(0xFF13, 0xE8);
        bus.write8(0xFF14, 0x87); // Trigger at frequency 1000.
    };
    const auto wave_setup = [](gameboy::MemoryBus& bus) {
        bus.write8(0xFF25, 0x44); // Channel 3 to both terminals.
        for (unsigned index = 0; index < 16; ++index) {
            bus.write8(static_cast<std::uint16_t>(0xFF30 + index),
                       static_cast<std::uint8_t>(0xF0 - index * 7));
        }
        bus.write8(0xFF1A, 0x80);
        bus.write8(0xFF1C, 0x20); // Full output level.
        bus.write8(0xFF1D, 0x00);
        bus.write8(0xFF1E, 0x87);
    };
    const auto noise_setup = [](gameboy::MemoryBus& bus) {
        bus.write8(0xFF25, 0x88); // Channel 4 to both terminals.
        bus.write8(0xFF21, 0xF0);
        bus.write8(0xFF22, 0x08); // Fast 7-bit LFSR mode.
        bus.write8(0xFF23, 0x80);
    };
    const auto pulse = render(gameboy::HardwareModel::dmg, pulse_setup);
    const auto wave = render(gameboy::HardwareModel::dmg, wave_setup);
    const auto noise = render(gameboy::HardwareModel::dmg, noise_setup);
    const auto cgb_pulse = render(gameboy::HardwareModel::cgb, pulse_setup);
    const auto cgb_wave = render(gameboy::HardwareModel::cgb, wave_setup);
    const auto cgb_noise = render(gameboy::HardwareModel::cgb, noise_setup);

    check(pulse.size() == 186 && wave.size() == 186 &&
              noise.size() == 186,
          "waveform regression fixtures produce a deterministic PCM window");
    check(audio_waveform_signature(pulse) == UINT64_C(0x55e406d59a987d8b),
          "pulse waveform regression signature");
    check(audio_waveform_signature(wave) == UINT64_C(0x5b853b4bcc531d67),
          "wave waveform regression signature");
    check(audio_waveform_signature(noise) == UINT64_C(0x224d45db0d6b5c33),
          "noise waveform regression signature");

    struct Fixture {
        std::string_view name;
        std::string_view model;
        const std::vector<std::int16_t>* samples;
    };
    const std::array<Fixture, 6> fixtures{{
        {"pulse", "dmg", &pulse}, {"wave", "dmg", &wave},
        {"noise", "dmg", &noise}, {"pulse", "cgb", &cgb_pulse},
        {"wave", "cgb", &cgb_wave}, {"noise", "cgb", &cgb_noise}}};
    const auto reference_directory = [] {
        if (const auto* value = std::getenv("GBB_AUDIO_REFERENCE_DIR");
            value != nullptr && *value != '\0') {
            return std::filesystem::path{value};
        }
        const auto executable_fixtures = executable_directory / "audio-fixtures";
        if (std::filesystem::exists(executable_fixtures)) {
            return executable_fixtures;
        }
        return std::filesystem::path{"tests"} / "fixtures" / "audio";
    }();
    const auto capture_directory = [] {
        if (const auto* value = std::getenv("GBB_AUDIO_REFERENCE_CAPTURE_DIR");
            value != nullptr && *value != '\0') {
            return std::filesystem::path{value};
        }
        return std::filesystem::path{};
    }();
    if (!capture_directory.empty()) {
        std::error_code error;
        std::filesystem::create_directories(capture_directory, error);
        check(!error, "audio reference capture directory is writable");
        if (!error) {
            for (const auto& fixture : fixtures) {
                const auto name = fixture.name;
                const auto model = fixture.model;
                const auto samples = fixture.samples;
                check(write_audio_waveform_reference(
                          capture_directory /
                              (std::string{model} + "-" + std::string{name} + ".txt"),
                          name, model, *samples),
                      "audio reference capture writes " + std::string{model} +
                          " " + std::string{name});
            }
        }
    }
    for (const auto& fixture : fixtures) {
        const auto name = fixture.name;
        const auto model = fixture.model;
        const auto samples = fixture.samples;
        compare_audio_waveform_reference(
            reference_directory /
                (std::string{model} + "-" + std::string{name} + ".txt"),
            name, model, *samples);
    }
}

void test_apu_cycle_integrated_resampling() {
    gameboy::MemoryBus bus{gameboy::Cartridge{test_rom()}};
    bus.initialize_post_boot();
    bus.write8(0xFF24, 0x77); // Full left and right master volume.
    bus.write8(0xFF25, 0x11); // Route channel 1 to both terminals.

    // Let one active channel cycle contribute for a single master-clock tick,
    // then remove its routing before the 48 kHz sample boundary. A sampler
    // that only observes the final cycle would output silence; integration
    // must preserve the short transition as a small non-zero sample.
    bus.tick(1);
    bus.write8(0xFF25, 0x00);
    bus.tick(87);
    const auto samples = bus.take_audio_samples();
    check(samples.size() == 2 && samples[0] != 0 && samples[1] != 0,
          "audio resampling preserves transitions shorter than one output sample");
    check(samples[0] == samples[1],
          "cycle-integrated mixer keeps both routed terminals phase-aligned");
}

void test_cgb_memory_and_rendering() {
    gameboy::Emulator speed{gameboy::Cartridge{cgb_test_rom({
        0x3E, 0x01, // LD A,1
        0xE0, 0x4D, // LDH (KEY1),A
        0x10, 0x00, // STOP: switch to double speed
        0x3E, 0x01, 0xE0, 0x4D, 0x10, 0x00,
    })}};
    static_cast<void>(speed.step());
    static_cast<void>(speed.step());
    check(speed.bus().read8(0xFF4D) == 0x7F,
          "KEY1 exposes a requested CGB speed switch");
    static_cast<void>(speed.step());
    check(speed.bus().double_speed() && !speed.cpu().stopped() &&
              speed.bus().read8(0xFF4D) == 0xFE,
          "CGB STOP performs an armed double-speed switch");
    static_cast<void>(speed.step());
    static_cast<void>(speed.step());
    static_cast<void>(speed.step());
    check(!speed.bus().double_speed() && !speed.cpu().stopped(),
          "a second armed STOP returns the CGB CPU to normal speed");

    gameboy::Emulator emulator{gameboy::Cartridge{cgb_test_rom()}};
    auto& bus = emulator.bus();
    check(bus.cgb_mode() && emulator.cpu().registers().a == 0x11 &&
              emulator.cpu().registers().f == 0x80 &&
              emulator.cpu().registers().e == 0x08 &&
              emulator.cpu().registers().l == 0x7C,
          "CGB cartridges start with CGB hardware detection state");
    auto edited_registers = emulator.cpu().registers();
    edited_registers.a = 0x42;
    edited_registers.f = 0xAF;
    edited_registers.sp = 0xC123;
    edited_registers.pc = 0x4567;
    emulator.set_cpu_registers(edited_registers);
    check(emulator.cpu().registers().a == 0x42 &&
              emulator.cpu().registers().f == 0xA0 &&
              emulator.cpu().registers().sp == 0xC123 &&
              emulator.cpu().registers().pc == 0x4567,
          "debugger register edits update CPU state and sanitize F");

    bus.write8(0xFF40, 0);
    bus.write8(0x8000, 0x12);
    bus.write8(0xFF4F, 1);
    bus.write8(0x8000, 0x34);
    check(bus.read8(0xFF4F) == 0xFF && bus.read8(0x8000) == 0x34,
          "VBK selects the second CGB VRAM bank");
    bus.write8(0xFF4F, 0);
    check(bus.read8(0x8000) == 0x12,
          "switching VBK restores the first CGB VRAM bank");
    bus.debug_write_vram(0, 0x0020, 0x56);
    bus.debug_write_vram(1, 0x0020, 0x78);
    check(bus.debug_read_vram(0, 0x0020) == 0x56 &&
              bus.debug_read_vram(1, 0x0020) == 0x78,
          "debugger VRAM access reads and edits either CGB bank directly");

    bus.write8(0xD000, 0x11);
    bus.write8(0xFF70, 2);
    bus.write8(0xD000, 0x22);
    bus.write8(0xFF70, 0);
    check(bus.read8(0xFF70) == 0xF9 && bus.read8(0xD000) == 0x11,
          "SVBK zero aliases bank one and preserves switched WRAM banks");
    bus.write8(0xFF70, 2);
    check(bus.read8(0xD000) == 0x22 && bus.read8(0xF000) == 0x22,
          "CGB switched WRAM is mirrored through echo RAM");

    bus.write8(0xFF4F, 1);
    bus.write8(0x8000, 0x80);
    bus.write8(0x8001, 0x00); // Bank 1 tile 0, first pixel color 1.
    bus.write8(0x9800, 0x08); // Tile attribute selects VRAM bank 1.
    bus.write8(0xFF4F, 0);
    bus.write8(0x9800, 0);
    bus.write8(0xFF68, 0x82); // Palette 0, color 1, auto-increment.
    bus.write8(0xFF69, 0x1F);
    bus.write8(0xFF69, 0x00); // RGB555 red.
    check(bus.read8(0xFF68) == 0xC4,
          "CGB palette writes auto-increment their six-bit index");
    bus.write8(0xFF40, 0x91);
    bus.tick(254);
    check(bus.framebuffer()[0] == 0xFFFF0000,
          "CGB tile attributes select VRAM banks and RGB555 palettes");

    bus.write8(0xFF40, 0);
    for (unsigned byte = 0; byte < 0x30; ++byte) {
        bus.write8(static_cast<std::uint16_t>(0xC000 + byte),
                   static_cast<std::uint8_t>(0x40 + byte));
    }
    bus.write8(0xFF4F, 1);
    bus.write8(0xFF51, 0xC0);
    bus.write8(0xFF52, 0x00);
    bus.write8(0xFF53, 0x01);
    bus.write8(0xFF54, 0x00);
    bus.write8(0xFF55, 0x00);
    check(bus.read8(0x8100) == 0x40 && bus.read8(0x810F) == 0x4F &&
              bus.read8(0xFF55) == 0xFF,
          "CGB general-purpose VRAM DMA copies complete 16-byte blocks");

    bus.write8(0xFF51, 0xC0);
    bus.write8(0xFF52, 0x10);
    bus.write8(0xFF53, 0x01);
    bus.write8(0xFF54, 0x20);
    bus.write8(0xFF40, 0x91);
    bus.write8(0xFF55, 0x81);
    bus.tick(254);
    check(bus.read8(0xFF55) == 0x00 && bus.read8(0x8120) == 0x50,
          "CGB HBlank DMA transfers one block at each HBlank");
    bus.tick(456);
    check(bus.read8(0xFF55) == 0xFF && bus.read8(0x8130) == 0x60,
          "CGB HBlank DMA completes after its requested block count");

    gameboy::MemoryBus batched_hdma{gameboy::Cartridge{cgb_test_rom()}};
    for (unsigned byte = 0; byte < 0x30; ++byte) {
        batched_hdma.write8(static_cast<std::uint16_t>(0xC000 + byte),
                            static_cast<std::uint8_t>(0x70 + byte));
    }
    batched_hdma.write8(0xFF51, 0xC0);
    batched_hdma.write8(0xFF52, 0x00);
    batched_hdma.write8(0xFF53, 0x01);
    batched_hdma.write8(0xFF54, 0x40);
    batched_hdma.write8(0xFF40, 0x91);
    batched_hdma.write8(0xFF55, 0x82); // Three HBlank blocks.
    batched_hdma.tick(720); // Crosses two HBlanks in one bus batch.
    check(batched_hdma.read8(0x8140) == 0x70 &&
              batched_hdma.read8(0x8150) == 0x80 &&
              batched_hdma.read8(0xFF55) == 0x00,
          "CGB HBlank DMA services every HBlank crossed by a batched tick");

    const auto cgb_state = emulator.save_state();
    bus.write8(0xFF40, 0);
    bus.write8(0xFF4F, 1);
    bus.write8(0x8100, 0);
    bus.write8(0xFF70, 2);
    bus.write8(0xD000, 0);
    emulator.load_state(cgb_state);
    check(bus.read8(0xFF4F) == 0xFF && bus.read8(0x8100) == 0x40 &&
              bus.read8(0xD000) == 0x22,
          "save states preserve CGB VRAM, WRAM, palettes, and bank selection");

    gameboy::MemoryBus dmg{gameboy::Cartridge{test_rom()}};
    check(!dmg.cgb_mode() && dmg.read8(0xFF4F) == 0xFF &&
              dmg.read8(0xFF68) == 0xFF && dmg.read8(0xFF70) == 0xFF,
          "CGB-only registers remain unavailable to monochrome cartridges");

    gameboy::Emulator compatibility{
        gameboy::Cartridge{test_rom()}, gameboy::HardwareModel::cgb};
    auto& compatibility_bus = compatibility.bus();
    compatibility_bus.write8(0xFF72, 0x12);
    compatibility_bus.write8(0xFF73, 0x34);
    compatibility_bus.write8(0xFF75, 0xFF);
    check(!compatibility_bus.cgb_mode() &&
              compatibility_bus.read8(0xFF4F) == 0xFE &&
              compatibility_bus.read8(0xFF68) == 0xC8 &&
              compatibility_bus.read8(0xFF69) == 0xFF &&
              compatibility_bus.read8(0xFF72) == 0x12 &&
              compatibility_bus.read8(0xFF73) == 0x34 &&
              compatibility_bus.read8(0xFF75) == 0xFF &&
              compatibility_bus.read8(0xFF76) == 0,
          "DMG software on CGB hardware exposes compatibility-mode registers");
}

void test_cartridge_file_loading() {
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

void test_apu_power_registers_and_wave_ram() {
    gameboy::MemoryBus bus{gameboy::Cartridge{test_rom()}};
    bus.initialize_post_boot();
    check((bus.read8(0xFF26) & 0xF0) == 0xF0,
          "post-boot APU state has master power enabled");
    check(bus.read8(0xFF24) == 0x77 && bus.read8(0xFF25) == 0xF3,
          "post-boot APU mixer registers have their DMG values");
    bus.write8(0xFF27, 0);
    bus.write8(0xFF2F, 0);
    check(bus.read8(0xFF27) == 0xFF && bus.read8(0xFF2F) == 0xFF,
          "unused APU register space ignores writes and reads as FF");

    bus.write8(0xFF30, 0xA5);
    bus.write8(0xFF26, 0);
    check(bus.read8(0xFF26) == 0x70,
          "clearing NR52 powers down the APU and its channels");
    bus.write8(0xFF17, 0xF0);
    check(bus.read8(0xFF17) == 0,
          "powered-down APU registers ignore ordinary writes");
    check(bus.read8(0xFF30) == 0xA5,
          "wave RAM remains accessible while the APU is powered down");
    bus.write8(0xFF16, 0xBF); // DMG permits length writes while powered down.
    bus.write8(0xFF26, 0x80);
    check((bus.read8(0xFF26) & 0x80) != 0,
          "setting NR52 restores APU master power");
    bus.write8(0xFF17, 0xF0);
    bus.write8(0xFF19, 0xC0);
    bus.tick(4096);
    bus.write8(0xFF04, 0);
    check((bus.read8(0xFF26) & 0x02) == 0,
          "DMG length-counter writes survive an APU power cycle");

    gameboy::MemoryBus cgb{gameboy::Cartridge{cgb_test_rom()}};
    cgb.initialize_post_boot(gameboy::HardwareModel::cgb);
    cgb.write8(0xFF26, 0);
    cgb.write8(0xFF16, 0xBF); // CGB ignores length writes while powered off.
    cgb.write8(0xFF26, 0x80);
    cgb.write8(0xFF17, 0xF0);
    cgb.write8(0xFF19, 0xC0);
    cgb.tick(4096);
    cgb.write8(0xFF04, 0);
    check((cgb.read8(0xFF26) & 0x02) != 0,
          "CGB power-off clears lengths and ignores powered-down length writes");
    cgb.write8(0xFF11, 0x80);
    cgb.write8(0xFF12, 0xF0);
    cgb.write8(0xFF13, 0xFC);
    cgb.write8(0xFF14, 0x87);
    auto saw_pcm_pulse = false;
    for (unsigned cycle = 0; cycle < 128 && !saw_pcm_pulse; ++cycle) {
        saw_pcm_pulse = (cgb.read8(0xFF76) & 0x0F) == 0x0F;
        cgb.tick(1);
    }
    check(saw_pcm_pulse && bus.read8(0xFF76) == 0xFF,
          "CGB PCM12 exposes live channel output and remains unmapped on DMG");

    const auto pcm_after_trigger = [](const unsigned idle_cycles) {
        gameboy::MemoryBus probe{gameboy::Cartridge{cgb_test_rom()}};
        probe.initialize_post_boot(gameboy::HardwareModel::cgb);
        probe.write8(0xFF11, 0x40); // 25% duty, initially high.
        probe.write8(0xFF12, 0x80); // DAC enabled, volume 8.
        probe.write8(0xFF13, 0xFF);
        probe.write8(0xFF14, 0x07); // Leave channel disabled.
        probe.tick(idle_cycles);
        probe.write8(0xFF14, 0x87); // Trigger after the idle interval.
        return probe.read8(0xFF76) & 0x0F;
    };
    check(pcm_after_trigger(0) == pcm_after_trigger(256),
          "disabled pulse timers do not advance the duty phase");
}

void test_active_wave_ram_timing() {
    gameboy::MemoryBus bus{gameboy::Cartridge{test_rom()}};
    bus.initialize_post_boot();
    for (unsigned index = 0; index < 16; ++index) {
        bus.write8(static_cast<std::uint16_t>(0xFF30 + index),
                   static_cast<std::uint8_t>(0x40 + index));
    }
    bus.write8(0xFF1A, 0x80);
    bus.write8(0xFF1D, 0xFC); // Frequency 2044: one sample every 8 clocks.
    bus.write8(0xFF1E, 0x87);

    check(bus.read8(0xFF30) == 0xFF,
          "active wave RAM is blocked between channel 3 fetches");
    bus.tick(14); // Trigger adds a six-clock startup delay.
    check(bus.read8(0xFF3F) == 0x40,
          "active wave RAM addresses expose the byte channel 3 just fetched");
    bus.write8(0xFF3F, 0xA5);
    bus.tick(2);
    bus.write8(0xFF30, 0x99);
    check(bus.read8(0xFF30) == 0xFF,
          "the channel 3 wave RAM access window lasts two clocks");
    bus.write8(0xFF1A, 0);
    check(bus.read8(0xFF30) == 0xA5,
          "active wave writes target the fetched byte and blocked writes are ignored");

    gameboy::MemoryBus corruption{gameboy::Cartridge{test_rom()}};
    corruption.initialize_post_boot();
    for (unsigned index = 0; index < 16; ++index) {
        corruption.write8(static_cast<std::uint16_t>(0xFF30 + index),
                          static_cast<std::uint8_t>(index));
    }
    corruption.write8(0xFF1A, 0x80);
    corruption.write8(0xFF1D, 0xFC);
    corruption.write8(0xFF1E, 0x87);
    corruption.tick(68); // Channel 3 is about to fetch wave byte 4.
    corruption.write8(0xFF1E, 0x87);
    corruption.write8(0xFF1A, 0);
    check(corruption.read8(0xFF30) == 4 &&
              corruption.read8(0xFF31) == 5 &&
              corruption.read8(0xFF32) == 6 &&
              corruption.read8(0xFF33) == 7,
          "retriggering channel 3 during a fetch reproduces DMG wave corruption");

    gameboy::MemoryBus cgb{gameboy::Cartridge{cgb_test_rom()}};
    cgb.initialize_post_boot(gameboy::HardwareModel::cgb);
    for (unsigned index = 0; index < 16; ++index) {
        cgb.write8(static_cast<std::uint16_t>(0xFF30 + index),
                   static_cast<std::uint8_t>(index));
    }
    cgb.write8(0xFF1A, 0x80);
    cgb.write8(0xFF1D, 0xFC);
    cgb.write8(0xFF1E, 0x87);
    check(cgb.read8(0xFF3F) == 0,
          "active CGB wave RAM reads are redirected to the current byte");
    cgb.write8(0xFF3F, 0xA5);
    cgb.tick(68);
    cgb.write8(0xFF1E, 0x87);
    cgb.write8(0xFF1A, 0);
    check(cgb.read8(0xFF30) == 0xA5 && cgb.read8(0xFF31) == 1 &&
              cgb.read8(0xFF32) == 2 && cgb.read8(0xFF33) == 3,
          "CGB wave writes redirect while active and retriggering does not corrupt RAM");
}

void test_apu_high_pass_filter() {
    gameboy::MemoryBus bus{gameboy::Cartridge{test_rom()}};
    bus.initialize_post_boot();
    bus.write8(0xFF24, 0x77);
    bus.write8(0xFF25, 0x11); // Inactive channel 1 DAC routed both ways.
    bus.tick(41943);
    const auto filtered = bus.take_audio_samples();
    const auto magnitude = [](const std::int16_t sample) {
        return sample < 0 ? -static_cast<int>(sample) : static_cast<int>(sample);
    };
    check(filtered.size() > 4 &&
              magnitude(filtered.front()) >
                  magnitude(filtered[filtered.size() - 2]),
          "the DMG high-pass filter removes an inactive DAC's DC bias");

    bus.write8(0xFF12, 0); // Disconnect the final active DAC.
    bus.tick(4096);
    const auto disconnected = bus.take_audio_samples();
    check(std::all_of(disconnected.begin(), disconnected.end(),
                      [](const std::int16_t sample) { return sample == 0; }),
          "disconnecting every DAC forces the mixed output to silence");

    gameboy::MemoryBus cgb{gameboy::Cartridge{cgb_test_rom()}};
    cgb.initialize_post_boot(gameboy::HardwareModel::cgb);
    cgb.write8(0xFF24, 0x77);
    cgb.write8(0xFF25, 0x11);
    cgb.tick(41943);
    const auto cgb_filtered = cgb.take_audio_samples();
    check(!cgb_filtered.empty() &&
              magnitude(cgb_filtered[cgb_filtered.size() - 2]) <
                  magnitude(filtered[filtered.size() - 2]),
          "CGB output uses its faster hardware high-pass response");
}

void test_apu_pulse2_samples_and_length() {
    gameboy::MemoryBus bus{gameboy::Cartridge{test_rom()}};
    bus.initialize_post_boot();
    bus.write8(0xFF24, 0x77); // Full left and right master volume.
    bus.write8(0xFF25, 0x22); // Route channel 2 to both outputs.
    bus.write8(0xFF16, 0x80); // 50% duty.
    bus.write8(0xFF17, 0xF0); // Initial volume 15, envelope disabled.
    bus.write8(0xFF18, 0x00);
    bus.write8(0xFF19, 0x87); // Trigger at frequency 1792.
    check((bus.read8(0xFF26) & 0x02) != 0,
          "triggering pulse channel 2 marks it active in NR52");

    bus.tick(41943); // Approximately 10 ms at the DMG master clock.
    const auto samples = bus.take_audio_samples();
    check(samples.size() >= 950 && samples.size() <= 970 &&
              samples.size() % 2 == 0,
          "the APU resamples master-clock cycles into 48 kHz stereo frames");
    check(std::any_of(samples.begin(), samples.end(),
                      [](const std::int16_t sample) { return sample != 0; }),
          "an active pulse channel produces audible non-zero PCM samples");
    auto stereo_matches = true;
    for (std::size_t index = 0; index + 1 < samples.size(); index += 2) {
        stereo_matches = stereo_matches && samples[index] == samples[index + 1];
    }
    check(stereo_matches,
          "routing pulse channel 2 to both terminals produces matching stereo");

    gameboy::MemoryBus length_bus{gameboy::Cartridge{test_rom()}};
    length_bus.initialize_post_boot();
    length_bus.write8(0xFF16, 0xBF); // 50% duty and a one-tick length.
    length_bus.write8(0xFF17, 0xF0);
    length_bus.write8(0xFF19, 0xC0); // Trigger with length enabled.
    length_bus.tick(4096);           // Raise the internal DIV-APU bit.
    length_bus.write8(0xFF04, 0);    // Its falling edge clocks length.
    check((length_bus.read8(0xFF26) & 0x02) == 0,
          "resetting DIV on its APU edge clocks and expires channel length");

    gameboy::MemoryBus extra_clock_bus{gameboy::Cartridge{test_rom()}};
    extra_clock_bus.initialize_post_boot();
    extra_clock_bus.write8(0xFF16, 0xBF);
    extra_clock_bus.write8(0xFF17, 0xF0);
    extra_clock_bus.write8(0xFF19, 0x80); // Trigger with length disabled.
    extra_clock_bus.tick(8192);           // Next sequencer step skips length.
    extra_clock_bus.write8(0xFF19, 0x40); // Enabling length adds a clock.
    check((extra_clock_bus.read8(0xFF26) & 0x02) == 0,
          "enabling length before a non-length step performs the DMG extra clock");

    gameboy::MemoryBus apu_start_phase{gameboy::Cartridge{cgb_test_rom()}};
    apu_start_phase.initialize_post_boot(gameboy::HardwareModel::cgb);
    apu_start_phase.write8(0xFF26, 0x00); // Power the APU off.
    apu_start_phase.write8(0xFF04, 0x00);
    apu_start_phase.tick(4096); // Leave DIV/APU high before re-enabling.
    apu_start_phase.write8(0xFF26, 0x80);
    apu_start_phase.write8(0xFF16, 0xBF); // One-tick channel-2 length.
    apu_start_phase.write8(0xFF17, 0xF0);
    apu_start_phase.write8(0xFF19, 0xC0);
    apu_start_phase.tick(4096); // The first falling edge is skipped.
    check((apu_start_phase.read8(0xFF26) & 0x02) != 0,
          "enabling the APU on a high DIV/APU phase skips its first edge");
    apu_start_phase.tick(8192); // The next falling edge clocks the length.
    check((apu_start_phase.read8(0xFF26) & 0x02) == 0,
          "the skipped APU edge does not shift later frame-sequencer clocks");
}

void test_apu_pulse1_sweep_wave_and_noise() {
    gameboy::MemoryBus pulse_bus{gameboy::Cartridge{test_rom()}};
    pulse_bus.initialize_post_boot();
    pulse_bus.write8(0xFF24, 0x77);
    pulse_bus.write8(0xFF25, 0x01); // Channel 1 to right output only.
    pulse_bus.write8(0xFF10, 0x11); // Sweep pace 1, add, shift 1.
    pulse_bus.write8(0xFF11, 0x80);
    pulse_bus.write8(0xFF12, 0xF0);
    pulse_bus.write8(0xFF13, 0xE8); // Frequency 1000.
    pulse_bus.write8(0xFF14, 0x83);
    check((pulse_bus.read8(0xFF26) & 0x01) != 0,
          "triggering pulse channel 1 marks it active in NR52");
    pulse_bus.tick(4096);
    const auto pulse_samples = pulse_bus.take_audio_samples();
    auto right_only_audio = false;
    for (std::size_t index = 0; index + 1 < pulse_samples.size(); index += 2) {
        right_only_audio = right_only_audio ||
                           (pulse_samples[index] == 0 &&
                            pulse_samples[index + 1] != 0);
    }
    check(right_only_audio,
          "NR51 can route pulse channel 1 exclusively to the right terminal");
    pulse_bus.tick(3 * 8192);
    check((pulse_bus.read8(0xFF26) & 0x01) == 0,
          "pulse channel 1 sweep disables the channel on frequency overflow");

    gameboy::MemoryBus overflow_bus{gameboy::Cartridge{test_rom()}};
    overflow_bus.initialize_post_boot();
    overflow_bus.write8(0xFF10, 0x01); // Pace zero, add, shift one.
    overflow_bus.write8(0xFF11, 0x80);
    overflow_bus.write8(0xFF12, 0xF0);
    overflow_bus.write8(0xFF13, 0xDC); // Frequency 1500.
    overflow_bus.write8(0xFF14, 0x85);
    check((overflow_bus.read8(0xFF26) & 0x01) == 0,
          "pulse channel 1 checks sweep overflow even when pace is zero");

    gameboy::MemoryBus wave_bus{gameboy::Cartridge{test_rom()}};
    wave_bus.initialize_post_boot();
    wave_bus.write8(0xFF24, 0x77);
    wave_bus.write8(0xFF25, 0x44); // Channel 3 to both terminals.
    for (unsigned index = 0; index < 16; ++index) {
        wave_bus.write8(static_cast<std::uint16_t>(0xFF30 + index),
                        index % 2 == 0 ? 0xF0 : 0x1E);
    }
    wave_bus.write8(0xFF1A, 0x80);
    wave_bus.write8(0xFF1C, 0x20); // Full output level.
    wave_bus.write8(0xFF1D, 0x00);
    wave_bus.write8(0xFF1E, 0x87);
    wave_bus.tick(4096);
    const auto wave_samples = wave_bus.take_audio_samples();
    check((wave_bus.read8(0xFF26) & 0x04) != 0 &&
              std::any_of(wave_samples.begin(), wave_samples.end(),
                          [](const std::int16_t sample) { return sample != 0; }),
          "wave channel 3 plays packed four-bit samples from wave RAM");

    gameboy::MemoryBus noise_bus{gameboy::Cartridge{test_rom()}};
    noise_bus.initialize_post_boot();
    noise_bus.write8(0xFF24, 0x77);
    noise_bus.write8(0xFF25, 0x88); // Channel 4 to both terminals.
    noise_bus.write8(0xFF21, 0xF0);
    noise_bus.write8(0xFF22, 0x08); // Fast 7-bit LFSR mode.
    noise_bus.write8(0xFF23, 0x80);
    noise_bus.tick(4096);
    const auto noise_samples = noise_bus.take_audio_samples();
    check((noise_bus.read8(0xFF26) & 0x08) != 0 &&
              std::any_of(noise_samples.begin(), noise_samples.end(),
                          [](const std::int16_t sample) { return sample != 0; }),
          "noise channel 4 produces PCM through its short-mode LFSR");

    noise_bus.write8(0xFF21, 0);
    check((noise_bus.read8(0xFF26) & 0x08) == 0,
          "disabling a channel DAC immediately clears its NR52 status bit");
}

void test_serial_transfer() {
    gameboy::MemoryBus bus{gameboy::Cartridge{test_rom()}};
    bus.write8(0xFF0F, 0);
    bus.write8(0xFF01, 'A');
    bus.write8(0xFF02, 0x81);
    check(bus.read8(0xFF01) == 'A' && (bus.read8(0xFF02) & 0x80) != 0,
          "internal-clock serial transfer starts through SB/SC");
    bus.tick(4095);
    check(bus.take_serial_output().empty() &&
              (bus.read8(0xFF02) & 0x80) != 0,
          "serial transfer remains active before its eighth bit");
    bus.tick(1);
    check(bus.take_serial_output() == "A" &&
              (bus.read8(0xFF02) & 0x80) == 0 &&
              (bus.read8(0xFF0F) & 0x08) != 0,
          "serial transfer publishes its byte after eight clock edges");
    check(bus.take_serial_output().empty(),
          "taking serial output drains the observation buffer");

    bus.write8(0xFF0F, 0);
    bus.write8(0xFF01, 'B');
    bus.write8(0xFF02, 0x80); // External clock selected.
    bus.tick(8192);
    check(bus.take_serial_output().empty() &&
              (bus.read8(0xFF02) & 0x80) != 0 &&
              (bus.read8(0xFF0F) & 0x08) == 0,
          "external-clock serial transfer waits for an external peer");
}

void test_serial_link_cable() {
    gameboy::MemoryBus first{gameboy::Cartridge{test_rom()}};
    gameboy::MemoryBus second{gameboy::Cartridge{test_rom()}};
    gameboy::SerialCable cable;
    cable.connect(first.serial_port(), second.serial_port());

    first.write8(0xFF01, 0xA5);
    second.write8(0xFF01, 0x3C);
    first.write8(0xFF02, 0x81);  // Player one supplies the clock.
    second.write8(0xFF02, 0x80); // Player two uses the external clock.

    first.tick(511);
    check(first.read8(0xFF01) == 0xA5 && second.read8(0xFF01) == 0x3C,
          "linked serial ports wait for the first clock edge");
    first.tick(512);
    check(first.read8(0xFF01) == 0x4A && second.read8(0xFF01) == 0x79,
          "linked serial ports exchange one bit on each clock edge");
    first.tick(7 * 512);
    check(first.read8(0xFF01) == 0x3C && second.read8(0xFF01) == 0xA5 &&
              (first.read8(0xFF02) & 0x80) == 0 &&
              (second.read8(0xFF02) & 0x80) == 0 &&
              (first.read8(0xFF0F) & 0x08) != 0 &&
              (second.read8(0xFF0F) & 0x08) != 0,
          "linked serial transfers complete on both consoles");

    check(first.serial_port().transfers_completed() == 1 &&
              second.serial_port().transfers_completed() == 1,
          "linked serial diagnostics count completed transfers");
    first.serial_port().reset_diagnostics();
    second.serial_port().reset_diagnostics();
    check(first.serial_port().transfers_completed() == 0 &&
              second.serial_port().transfers_completed() == 0 &&
              first.serial_port().last_transmitted() == 0xFF &&
              first.serial_port().last_received() == 0xFF &&
              second.serial_port().last_transmitted() == 0xFF &&
              second.serial_port().last_received() == 0xFF,
          "serial diagnostics reset without changing link state");

    // Pokémon's Cable Club can have both consoles request the internal clock
    // during the same handshake window. A real cable has one clock source; the
    // first deterministic request wins, while the loser keeps its preceding
    // external probe byte available for the winning edge.
    first.write8(0xFF01, 0x02);
    second.write8(0xFF01, 0x02);
    first.write8(0xFF02, 0x80);
    second.write8(0xFF02, 0x80);
    first.write8(0xFF01, 0x01);
    second.write8(0xFF01, 0x01);
    first.write8(0xFF02, 0x81);
    second.write8(0xFF02, 0x81);
    check(first.serial_port().internal_clock() &&
              !second.serial_port().internal_clock(),
          "a linked cable arbitrates simultaneous internal-clock requests");
    first.tick(4096);
    check(first.read8(0xFF01) == 0x02 && second.read8(0xFF01) == 0x01 &&
              (first.read8(0xFF0F) & 0x08) != 0 &&
              (second.read8(0xFF0F) & 0x08) != 0,
          "arbitrated probe exchanges the preceding external byte");

    // Mirror the Gen I probe sequence: both sides arm external first, then
    // switch to internal to discover a peer that is already waiting.
    first.write8(0xFF01, 0x02);
    second.write8(0xFF01, 0x02);
    first.write8(0xFF02, 0x80);
    second.write8(0xFF02, 0x80);
    first.write8(0xFF02, 0x81);
    second.write8(0xFF02, 0x81);
    first.tick(4096);
    second.tick(4096);
    check(first.read8(0xFF01) == 0x02 && second.read8(0xFF01) == 0x02 &&
              (first.read8(0xFF0F) & 0x08) != 0 &&
              (second.read8(0xFF0F) & 0x08) != 0,
          "Gen I external-then-internal probe exchanges both role bytes");

    // A connected cable holds the first edge until its peer arms the receiver;
    // this prevents a startup probe from being consumed as pull-up bits.
    first.write8(0xFF0F, 0);
    second.write8(0xFF0F, 0);
    first.write8(0xFF01, 0x12);
    second.write8(0xFF01, 0x34);
    first.write8(0xFF02, 0x81);
    first.tick(4096);
    check(first.serial_port().transfer_active() &&
              first.serial_port().bits_shifted() == 0 &&
              (first.read8(0xFF0F) & 0x08) == 0,
          "connected internal clock waits for an unarmed external peer");
    second.write8(0xFF02, 0x80);
    // Arming the receiver releases only the next edge. Advance one bit at a
    // time so the test also guards against replaying the time spent waiting.
    for (unsigned bit = 0; bit < 8; ++bit) first.tick(512);
    check(!first.serial_port().transfer_active() &&
              !second.serial_port().transfer_active() &&
              first.read8(0xFF01) == 0x34 &&
              second.read8(0xFF01) == 0x12 &&
              (first.read8(0xFF0F) & 0x08) != 0 &&
              (second.read8(0xFF0F) & 0x08) != 0,
          "armed external peer receives the held transfer");

    // A peer edge is ignored while a port is the internal clock source; only
    // the cable owner's edge shifts both ports.
    second.write8(0xFF01, 0x80);
    second.write8(0xFF02, 0x81);
    static_cast<void>(second.serial_port().clock_external_bit(false));
    check(second.read8(0xFF01) == 0x80,
          "a cable edge does not shift an internal clock source");

    cable.disconnect();
    first.write8(0xFF0F, 0);
    first.write8(0xFF01, 0x00);
    first.write8(0xFF02, 0x81);
    first.tick(4096);
    check(first.read8(0xFF01) == 0xFF,
          "a disconnected link supplies pull-up one bits");

    gameboy::MemoryBus cgb{gameboy::Cartridge{cgb_test_rom()}};
    cgb.write8(0xFF01, 0x00);
    cgb.write8(0xFF02, 0x83);
    cgb.tick(127);
    check((cgb.read8(0xFF02) & 0x80) != 0,
          "CGB fast serial remains active before eight fast edges");
    cgb.tick(1);
    check((cgb.read8(0xFF02) & 0x80) == 0,
          "CGB fast serial completes after 128 CPU cycles");
}

void test_serial_link_interrupt_handshake() {
    // A tiny ROM-level probe matching Pokémon's external-then-internal
    // connection routine. The ISR copies the received SB byte into the HRAM
    // connection marker, exactly as the game does.
    const std::vector<std::uint8_t> program{
        0x31, 0xFE, 0xFF,       // LD SP,$FFFE
        0x3E, 0x08, 0xEA, 0xFF, 0xFF, // LD A,$08; LD ($FFFF),A
        0xFB,                   // EI
        0x3E, 0xFF, 0xEA, 0xAA, 0xFF, // marker = connection pending
        0x3E, 0x02, 0xEA, 0x01, 0xFF, // SB = external probe
        0x3E, 0x80, 0xEA, 0x02, 0xFF, // SC = external start
        0x3E, 0x01, 0xEA, 0x01, 0xFF, // SB = internal probe
        0x3E, 0x81, 0xEA, 0x02, 0xFF, // SC = internal start
        0x18, 0xFE};            // spin
    auto first_rom = test_rom(program);
    auto second_rom = first_rom;
    first_rom[0x58] = 0xF0;     // LDH A,($01)
    first_rom[0x59] = 0x01;
    first_rom[0x5A] = 0xEA;     // LD ($FFAA),A
    first_rom[0x5B] = 0xAA;
    first_rom[0x5C] = 0xFF;
    first_rom[0x5D] = 0xD9;     // RETI
    second_rom[0x58] = 0xF0;
    second_rom[0x59] = 0x01;
    second_rom[0x5A] = 0xEA;
    second_rom[0x5B] = 0xAA;
    second_rom[0x5C] = 0xFF;
    second_rom[0x5D] = 0xD9;
    gameboy::Emulator first{gameboy::Cartridge{std::move(first_rom)}};
    gameboy::Emulator second{gameboy::Cartridge{std::move(second_rom)}};
    gameboy::SerialCable cable;
    cable.connect(first.bus().serial_port(), second.bus().serial_port());
    for (unsigned instruction = 0; instruction < 20000; ++instruction) {
        static_cast<void>(first.step());
        static_cast<void>(second.step());
    }
    const auto first_status = first.bus().read8(0xFFAA);
    const auto second_status = second.bus().read8(0xFFAA);
    if (first_status != 0x02 || second_status != 0x01) {
        std::cerr << "probe status first=" << unsigned(first_status)
                  << " second=" << unsigned(second_status)
                  << " sb=" << unsigned(first.bus().read8(0xFF01)) << "/"
                  << unsigned(second.bus().read8(0xFF01)) << " sc="
                  << unsigned(first.bus().read8(0xFF02)) << "/"
                  << unsigned(second.bus().read8(0xFF02)) << '\n';
    }
    check(first_status == 0x02 && second_status == 0x01,
          "serial cable delivers probe bytes through ROM interrupt handlers");
}

void test_serial_link_interrupt_rearm() {
    // Continue the probe with repeated master/slave transfers. This models
    // the Cable Club's byte exchange where the external side must re-arm SC
    // from its serial ISR before the next internal edge.
    const std::vector<std::uint8_t> program{
        0x31, 0xFE, 0xFF, 0x3E, 0x08, 0xEA, 0xFF, 0xFF, 0xFB,
        0x3E, 0xFF, 0xEA, 0xAA, 0xFF, // pending marker
        0x3E, 0x02, 0xEA, 0x01, 0xFF, 0x3E, 0x80, 0xEA, 0x02, 0xFF,
        0x3E, 0x01, 0xEA, 0x01, 0xFF, 0x3E, 0x81, 0xEA, 0x02, 0xFF,
        0xF0, 0xAA, 0xFE, 0x02, 0x20, 0xFA, // wait for internal role
        0x3E, 0x60, 0xEA, 0x01, 0xFF, 0x3E, 0x81, 0xEA, 0x02, 0xFF,
        0x18, 0xEB}; // repeat internal transfers
    auto first_rom = test_rom(program);
    auto second_rom = first_rom;
    const std::array<std::uint8_t, 0x1C> isr{{
        0xF0, 0xAA, 0xFE, 0xFF, 0x20, 0x06, // if pending, capture SB
        0xF0, 0x01, 0xEA, 0xAA, 0xFF,       // hstatus = received probe
        0xF0, 0xAA, 0xFE, 0x01, 0x20, 0x0A, // if external, re-arm below
        0x3E, 0x60, 0xEA, 0x01, 0xFF, 0x3E, 0x80, 0xEA, 0x02, 0xFF,
        0xD9}};
    std::copy(isr.begin(), isr.end(), first_rom.begin() + 0x58);
    std::copy(isr.begin(), isr.end(), second_rom.begin() + 0x58);
    gameboy::Emulator first{gameboy::Cartridge{std::move(first_rom)}};
    gameboy::Emulator second{gameboy::Cartridge{std::move(second_rom)}};
    gameboy::SerialCable cable;
    cable.connect(first.bus().serial_port(), second.bus().serial_port());
    for (unsigned instruction = 0; instruction < 50000; ++instruction) {
        static_cast<void>(first.step());
        static_cast<void>(second.step());
    }
    check(first.bus().read8(0xFF01) == 0x60 &&
              second.bus().read8(0xFF01) == 0x60,
          "external serial ISR re-arms repeated linked transfers");
}

void test_serial_link_asymmetric_scheduling() {
    // The desktop frontend advances both machines in cycle-sized slices, but
    // host scheduling can still let one CPU run several instructions ahead.
    // Keep the same repeated-transfer probe as above while deliberately
    // starving each side in alternating bursts. The cable must hold an edge
    // until the peer arms its receiver and must not lose the next transfer.
    const std::vector<std::uint8_t> program{
        0x31, 0xFE, 0xFF, 0x3E, 0x08, 0xEA, 0xFF, 0xFF, 0xFB,
        0x3E, 0xFF, 0xEA, 0xAA, 0xFF, // pending marker
        0x3E, 0x02, 0xEA, 0x01, 0xFF, 0x3E, 0x80, 0xEA, 0x02, 0xFF,
        0x3E, 0x01, 0xEA, 0x01, 0xFF, 0x3E, 0x81, 0xEA, 0x02, 0xFF,
        0xF0, 0xAA, 0xFE, 0x02, 0x20, 0xFA, // wait for internal role
        0x3E, 0x60, 0xEA, 0x01, 0xFF, 0x3E, 0x81, 0xEA, 0x02, 0xFF,
        0x18, 0xEB};
    auto first_rom = test_rom(program);
    auto second_rom = first_rom;
    const std::array<std::uint8_t, 0x1C> isr{{
        0xF0, 0xAA, 0xFE, 0xFF, 0x20, 0x06,
        0xF0, 0x01, 0xEA, 0xAA, 0xFF,
        0xF0, 0xAA, 0xFE, 0x01, 0x20, 0x0A,
        0x3E, 0x60, 0xEA, 0x01, 0xFF, 0x3E, 0x80, 0xEA, 0x02, 0xFF,
        0xD9}};
    std::copy(isr.begin(), isr.end(), first_rom.begin() + 0x58);
    std::copy(isr.begin(), isr.end(), second_rom.begin() + 0x58);
    gameboy::Emulator first{gameboy::Cartridge{std::move(first_rom)}};
    gameboy::Emulator second{gameboy::Cartridge{std::move(second_rom)}};
    gameboy::SerialCable cable;
    cable.connect(first.bus().serial_port(), second.bus().serial_port());

    for (unsigned instruction = 0; instruction < 100000; ++instruction) {
        // Alternate which side gets a 4:1 share of host time. Both consoles
        // continue to make progress, but neither has lockstep instruction
        // timing to hide a peer-readiness bug.
        const auto first_ahead = (instruction / 32U) % 2U == 0;
        if (first_ahead) {
            static_cast<void>(first.step());
            if (instruction % 4U == 0) static_cast<void>(second.step());
        } else {
            if (instruction % 4U == 0) static_cast<void>(first.step());
            static_cast<void>(second.step());
        }
    }
    check(first.bus().read8(0xFF01) == 0x60 &&
              second.bus().read8(0xFF01) == 0x60,
          "linked transfers survive asymmetric emulator scheduling");
}

void test_link_session_lifecycle() {
    gameboy::Emulator first{gameboy::Cartridge{test_rom()}};
    gameboy::Emulator second{gameboy::Cartridge{test_rom()}};
    gameboy::LocalLinkTransport transport;
    gameboy::LinkSession session{transport};

    check(session.state() == gameboy::LinkSession::State::disconnected &&
              !session.active(),
          "link session starts disconnected");

    session.start(first, second);
    check(session.state() == gameboy::LinkSession::State::connected &&
              session.active() && first.bus().serial_port().has_endpoint() &&
              second.bus().serial_port().has_endpoint() &&
              transport.connected(),
          "link session attaches both serial endpoints");

    first.bus().write8(0xFF01, 0xA5);
    second.bus().write8(0xFF01, 0x5A);
    first.bus().write8(0xFF02, 0x81);
    second.bus().write8(0xFF02, 0x80);
    check(session.state() == gameboy::LinkSession::State::transferring,
          "link session reports an active serial transfer");
    session.advance(4096);
    check(first.bus().read8(0xFF01) == 0x5A &&
              second.bus().read8(0xFF01) == 0xA5 &&
              session.state() == gameboy::LinkSession::State::connected &&
              session.transfers_completed() == 2,
          "link session scheduler completes a transfer on both consoles");

    session.mark_timeout();
    check(session.state() == gameboy::LinkSession::State::timed_out &&
              !session.active() && first.bus().serial_port().has_endpoint() &&
              second.bus().serial_port().has_endpoint(),
          "link session exposes an explicit timeout state");
    session.stop();
    check(session.state() == gameboy::LinkSession::State::disconnected &&
              !first.bus().serial_port().has_endpoint() &&
              !second.bus().serial_port().has_endpoint() &&
              !transport.connected(),
          "link session detaches both endpoints on stop");

    session.start(first, second);
    check(session.active() && session.state() ==
              gameboy::LinkSession::State::connected,
          "link session can reconnect after stopping");
    session.stop();
}

void test_link_session_timeout_and_retry() {
    gameboy::Emulator first{gameboy::Cartridge{test_rom()}};
    gameboy::Emulator second{gameboy::Cartridge{test_rom()}};
    // Use a short watchdog budget so a stalled external receiver is covered
    // without spending seconds emulating an entire production timeout.
    gameboy::LinkSession session{1024};
    session.start(first, second);
    first.bus().write8(0xFF01, 0xA5);
    first.bus().write8(0xFF02, 0x81);
    session.advance(1024);
    check(session.state() == gameboy::LinkSession::State::timed_out &&
              !session.active(),
          "link session watchdog detects a stalled transfer");

    session.retry();
    check(session.state() == gameboy::LinkSession::State::connected &&
              session.active() && first.bus().serial_port().has_endpoint() &&
              second.bus().serial_port().has_endpoint() &&
              !first.bus().serial_port().transfer_active(),
          "link retry clears protocol state without replacing emulators");

    // A recovered session must be usable for the next byte, not merely report
    // a connected lifecycle state. This protects the trade/battle retry path
    // from regressing into a second deadlock after the watchdog fires.
    first.bus().write8(0xFF01, 0xA5);
    second.bus().write8(0xFF01, 0x5A);
    first.bus().write8(0xFF02, 0x81);
    second.bus().write8(0xFF02, 0x80);
    session.advance(4096);
    check(first.bus().read8(0xFF01) == 0x5A &&
              second.bus().read8(0xFF01) == 0xA5 &&
              session.state() == gameboy::LinkSession::State::connected &&
              session.transfers_completed() == 2,
          "link retry permits a fresh transfer after a timeout");
    session.stop();
}

void test_link_transport_framing() {
    const gameboy::LinkPacket packet{gameboy::LinkPacketType::bit,
                                     UINT32_C(0xA1B2C3D4), 0x5A, 0x03};
    const auto wire = gameboy::LinkPacketCodec::encode(packet);
    const auto decoded = gameboy::LinkPacketCodec::decode(wire.data(),
                                                            wire.size());
    check(decoded && decoded->type == packet.type &&
              decoded->sequence == packet.sequence &&
              decoded->value == packet.value && decoded->flags == packet.flags,
          "link transport framing round-trips packet fields");

    auto corrupted = wire;
    corrupted[8] ^= 0x01;
    check(!gameboy::LinkPacketCodec::decode(corrupted.data(), corrupted.size()),
          "link transport framing rejects a corrupted packet");
    check(!gameboy::LinkPacketCodec::decode(wire.data(), wire.size() - 1),
          "link transport framing rejects a truncated packet");
}

void test_tcp_link_channel_loopback() {
    gameboy::TcpLinkChannel server;
    gameboy::TcpLinkChannel client;
    const auto listening = server.listen(0) && server.local_port() != 0;
    // Some hermetic runners disable AF_INET sockets entirely. Keep the core
    // suite useful there; codec coverage still runs and desktop CI exercises
    // the channel with real sockets.
    if (!listening) return;
    check(server.state() == gameboy::TcpLinkChannel::State::listening,
          "TCP link channel listens on an ephemeral loopback port");
    check(client.connect("127.0.0.1", server.local_port()),
          "TCP link channel starts a non-blocking loopback connect");
    for (unsigned attempt = 0;
         attempt < 100 &&
         (server.state() != gameboy::TcpLinkChannel::State::connected ||
          client.state() != gameboy::TcpLinkChannel::State::connected);
         ++attempt) {
        server.poll();
        client.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    check(server.state() == gameboy::TcpLinkChannel::State::connected &&
              client.state() == gameboy::TcpLinkChannel::State::connected,
          "TCP link channel establishes a loopback peer without blocking");
    const gameboy::LinkPacket packet{gameboy::LinkPacketType::bit, 7, 0xA5,
                                     1};
    check(client.send(packet), "TCP link channel queues a framed packet");
    for (unsigned attempt = 0; attempt < 20; ++attempt) {
        client.poll();
        server.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const auto received = server.receive();
    check(received && received->sequence == packet.sequence &&
              received->value == packet.value && received->flags == packet.flags,
          "TCP link channel delivers framed packets over loopback");
}

void test_tcp_serial_endpoint_loopback() {
    gameboy::TcpLinkChannel server;
    gameboy::TcpLinkChannel client;
    if (!server.listen(0) || server.local_port() == 0) return;
    if (!client.connect("127.0.0.1", server.local_port())) return;
    for (unsigned attempt = 0;
         attempt < 100 &&
         (server.state() != gameboy::TcpLinkChannel::State::connected ||
          client.state() != gameboy::TcpLinkChannel::State::connected);
         ++attempt) {
        server.poll();
        client.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (server.state() != gameboy::TcpLinkChannel::State::connected ||
        client.state() != gameboy::TcpLinkChannel::State::connected) {
        return;
    }

    gameboy::MemoryBus first{gameboy::Cartridge{test_rom()}};
    gameboy::MemoryBus second{gameboy::Cartridge{test_rom()}};
    gameboy::TcpSerialEndpoint first_endpoint;
    gameboy::TcpSerialEndpoint second_endpoint;
    // Match the desktop TCP roles: the host (first endpoint) owns the
    // initial clock and the join side starts as the external receiver.
    first_endpoint.set_arbitration_priority(true);
    second_endpoint.set_arbitration_priority(false);
    first_endpoint.attach(first.serial_port(), client);
    second_endpoint.attach(second.serial_port(), server);

    // Exercise the reset path: leave one host bit pending, then have the
    // guest rewrite SC before the response arrives. A normal rewrite keeps
    // the request alive; an explicit link reset must cancel it cleanly.
    for (unsigned attempt = 0;
         attempt < 100 &&
         !first_endpoint.peer_ready_for_link();
         ++attempt) {
        first_endpoint.poll();
        second_endpoint.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    first.write8(0xFF01, 0xA5);
    second.write8(0xFF01, 0x5A);
    first.write8(0xFF02, 0x81);
    second.write8(0xFF02, 0x80);
    first.tick(512);
    check(first_endpoint.waiting_for_peer(),
          "TCP endpoint records an outstanding bit before serial reset");
    first.write8(0xFF02, 0x81);
    check(first_endpoint.waiting_for_peer(),
          "a normal SC rewrite preserves an in-flight TCP bit request");
    first.serial_port().reset_link();
    check(!first_endpoint.waiting_for_peer(),
          "explicit link reset cancels an obsolete TCP bit request");
    second.serial_port().reset_link();
    check(!first.serial_port().transfer_active(),
          "peer serial reset discards a partial external byte");
    // Let both channels consume the abandoned response and ordered reset
    // markers before the next guest arms SC. This is the same idle polling
    // cadence used by the desktop frontend between retries.
    for (unsigned attempt = 0; attempt < 20; ++attempt) {
        first_endpoint.poll();
        second_endpoint.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    first.write8(0xFF01, 0xA5);
    second.write8(0xFF01, 0x5A);
    first.write8(0xFF02, 0x81);
    second.write8(0xFF02, 0x81);
    for (unsigned cycle = 0; cycle < 20000; ++cycle) {
        first.tick(4);
        second.tick(4);
        // Match the desktop remote-link cadence (one network poll roughly
        // every 64 CPU cycles) instead of accidentally masking timing races
        // with a poll on every four-cycle tick.
        if ((cycle & 15U) == 0) {
            first_endpoint.poll();
            second_endpoint.poll();
        }
        if (!first.serial_port().transfer_active() &&
            !second.serial_port().transfer_active()) {
            break;
        }
    }
    check(first.read8(0xFF01) == 0x5A && second.read8(0xFF01) == 0xA5 &&
              !first.serial_port().transfer_active() &&
              !second.serial_port().transfer_active(),
          "TCP serial endpoint bridges a non-blocking loopback transfer");

    // A completed byte releases clock ownership. The join side must be able
    // to clock the following byte after receiving that release, which is the
    // sequence used by Pokémon when it re-enters the Cable Club after a
    // reset. Keep the host external for this transfer so the direction is
    // unambiguous.
    first.write8(0xFF01, 0x3C);
    second.write8(0xFF01, 0xC3);
    second.write8(0xFF02, 0x80);
    first.write8(0xFF02, 0x81);
    for (unsigned cycle = 0; cycle < 20000; ++cycle) {
        first.tick(4);
        second.tick(4);
        if ((cycle & 15U) == 0) {
            first_endpoint.poll();
            second_endpoint.poll();
        }
        if (!first.serial_port().transfer_active() &&
            !second.serial_port().transfer_active()) {
            break;
        }
    }
    check(first.read8(0xFF01) == 0xC3 && second.read8(0xFF01) == 0x3C &&
              !first.serial_port().transfer_active() &&
              !second.serial_port().transfer_active(),
          "TCP serial endpoint alternates clock ownership after release");

    // A Pokémon probe can rewrite SC while the first TCP response is still
    // in flight. Keep the transport cadence deliberately coarse here; the
    // re-armed transfer must consume that response instead of spinning on an
    // unmatched packet.
    first.write8(0xFF01, 0x96);
    second.write8(0xFF01, 0x69);
    second.write8(0xFF02, 0x80);
    first.write8(0xFF02, 0x81);
    first.tick(512);
    first.write8(0xFF02, 0x81);
    for (unsigned cycle = 0; cycle < 20000; ++cycle) {
        first.tick(4);
        second.tick(4);
        if ((cycle & 15U) == 0) {
            first_endpoint.poll();
            second_endpoint.poll();
        }
        if (!first.serial_port().transfer_active() &&
            !second.serial_port().transfer_active()) {
            break;
        }
    }
    check(first.read8(0xFF01) == 0x69 && second.read8(0xFF01) == 0x96 &&
              !first.serial_port().transfer_active() &&
              !second.serial_port().transfer_active(),
          "TCP endpoint recovers when SC is rewritten before a response");

    // Exercise a short alternating payload, which is the pattern used by
    // Pokémon's trade/battle data exchange after the initial probe. The
    // owner changes every byte and the network is still polled only once per
    // 64 CPU cycles.
    for (unsigned byte = 0; byte < 12; ++byte) {
        // The previous owner's completion callback queues clock_release on
        // its TCP channel. Give both endpoints a normal idle polling window
        // before arming the next byte; otherwise a fast runner can have both
        // guests observe the old peer_clock_busy state and become external
        // receivers, leaving the alternating transfer stalled.
        for (unsigned attempt = 0; attempt < 4; ++attempt) {
            first_endpoint.poll();
            second_endpoint.poll();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        const auto host_owns_clock = (byte & 1U) == 0;
        const auto first_value = static_cast<std::uint8_t>(0x30U + byte);
        const auto second_value = static_cast<std::uint8_t>(0xC0U + byte);
        first.write8(0xFF01, first_value);
        second.write8(0xFF01, second_value);
        if (host_owns_clock) {
            second.write8(0xFF02, 0x80);
            first.write8(0xFF02, 0x81);
        } else {
            first.write8(0xFF02, 0x80);
            second.write8(0xFF02, 0x81);
        }
        for (unsigned cycle = 0; cycle < 20000; ++cycle) {
            first.tick(4);
            second.tick(4);
            if ((cycle & 15U) == 0) {
                first_endpoint.poll();
                second_endpoint.poll();
            }
            if (!first.serial_port().transfer_active() &&
                !second.serial_port().transfer_active()) {
                break;
            }
        }
        check(first.read8(0xFF01) == second_value &&
                  second.read8(0xFF01) == first_value &&
                  !first.serial_port().transfer_active() &&
                  !second.serial_port().transfer_active(),
              "TCP endpoint preserves alternating link payload bytes");
    }
    first_endpoint.detach();
    second_endpoint.detach();
}

void test_gameboy_printer() {
    gameboy::GameBoyPrinter printer;
    const auto send_packet = [&printer](const std::uint8_t command,
                                        const std::uint8_t compression,
                                        const std::vector<std::uint8_t>& data,
                                        const bool valid_checksum = true) {
        const auto length = static_cast<std::uint16_t>(data.size());
        auto checksum = static_cast<std::uint16_t>(
            command + compression + (length & 0xFF) + (length >> 8));
        static_cast<void>(printer.transfer(0x88));
        static_cast<void>(printer.transfer(0x33));
        static_cast<void>(printer.transfer(command));
        static_cast<void>(printer.transfer(compression));
        static_cast<void>(printer.transfer(static_cast<std::uint8_t>(length)));
        static_cast<void>(
            printer.transfer(static_cast<std::uint8_t>(length >> 8)));
        for (const auto byte : data) {
            checksum = static_cast<std::uint16_t>(checksum + byte);
            static_cast<void>(printer.transfer(byte));
        }
        if (!valid_checksum) ++checksum;
        static_cast<void>(
            printer.transfer(static_cast<std::uint8_t>(checksum)));
        static_cast<void>(
            printer.transfer(static_cast<std::uint8_t>(checksum >> 8)));
        const auto acknowledgement = printer.transfer(0);
        const auto status = printer.transfer(0);
        check(acknowledgement == (valid_checksum ? 0x81 : 0),
              "Game Boy Printer only acknowledges valid complete packets");
        return status;
    };

    check(send_packet(0x01, 0, {}) == 0,
          "Game Boy Printer initialization clears its status");
    std::vector<std::uint8_t> tile(16, 0);
    tile[0] = 0xFF; // Eight color-1 pixels on the first row.
    check(send_packet(0x04, 0, tile) == 0,
          "Game Boy Printer accepts uncompressed tile data");
    check(send_packet(0x04, 1, {0x8E, 0x00}) == 0x08,
          "Game Boy Printer expands compressed run-length tile data");
    check(send_packet(0x02, 0, {1, 0, 0xE4, 0x40}) == 0x08,
          "Game Boy Printer completes a print command");
    auto images = printer.take_images();
    check(images.size() == 1 && images[0].height == 8 &&
              images[0].pixels.size() == gameboy::PrinterImage::width * 8 &&
              images[0].pixels[0] == 1 && images[0].pixels[7] == 1 &&
              images[0].pixels[8] == 0,
          "Game Boy Printer converts 2bpp tiles and print palettes into an image");
    const auto bitmap = gameboy::encode_printer_bmp(images[0]);
    constexpr auto bitmap_row_size = gameboy::PrinterImage::width * 4 * 3;
    const auto top_row_offset = 54 + (images[0].height * 4 - 1) *
                                         bitmap_row_size;
    check(bitmap.size() == 54 + images[0].height * 4 * bitmap_row_size &&
              bitmap[0] == 'B' && bitmap[1] == 'M' &&
              bitmap[18] == 0x80 && bitmap[19] == 0x02 &&
              bitmap[22] == 0x20 && bitmap[top_row_offset] == 170,
          "printer images encode as scaled lossless 24-bit BMP files");
    check(printer.take_images().empty(),
          "taking printer images drains the completed print queue");
    check(send_packet(0x0F, 0, {}) == 0x04,
          "Game Boy Printer reports a completed page to status inquiries");
    check(send_packet(0x0F, 0, {}, false) == 0,
          "Game Boy Printer rejects invalid packet checksums without an acknowledgement");

    gameboy::MemoryBus bus{gameboy::Cartridge{test_rom()}};
    bus.connect_printer();
    bus.write8(0xFF01, 0x88);
    bus.write8(0xFF02, 0x81);
    bus.tick(4096);
    check(bus.read8(0xFF01) == 0 && bus.take_serial_output().empty(),
          "a connected printer exchanges serial bytes without buffering a debug transcript");
}

void test_cpu_state_normalization() {
    auto registers = initial_registers();
    registers.f = 0xFF;
    gameboy::Cpu cpu;
    cpu.load_registers(registers);
    check(cpu.registers().f == 0xF0, "CPU state masks unavailable flag bits");
}

void test_register_load_matrix() {
    const std::array<std::uint8_t, 8> values{
        0x11, 0x22, 0x33, 0x44, 0xC0, 0x00, 0x66, 0x77,
    };

    for (unsigned destination = 0; destination < 8; ++destination) {
        for (unsigned source = 0; source < 8; ++source) {
            if (destination == 6 && source == 6) {
                continue; // Opcode 76 is HALT, not LD (HL),(HL).
            }
            const auto opcode = static_cast<std::uint8_t>(
                0x40 | (destination << 3) | source);
            gameboy::MemoryBus bus{
                gameboy::Cartridge{test_rom({opcode})}};
            bus.write8(0xC000, values[6]);
            gameboy::Cpu cpu;
            cpu.load_registers(initial_registers());

            const auto cycles = cpu.step(bus);
            const auto label = "LD matrix opcode " + std::to_string(opcode);
            const auto actual = destination == 6
                                    ? bus.read8(0xC000)
                                    : register_value(cpu.registers(), destination);
            check(actual == values[source], label + " moves the correct value");
            check(cycles == (destination == 6 || source == 6 ? 8U : 4U),
                  label + " has the correct cycle count");
            check(cpu.registers().pc == 0x0101, label + " advances PC once");
            check(cpu.registers().f == 0xB0, label + " preserves flags");
        }
    }
}

void test_immediate_load_table() {
    struct LoadCase {
        std::uint8_t opcode;
        unsigned destination;
        unsigned cycles;
    };
    constexpr std::array<LoadCase, 8> cases{{
        {0x06, 0, 8}, {0x0E, 1, 8}, {0x16, 2, 8}, {0x1E, 3, 8},
        {0x26, 4, 8}, {0x2E, 5, 8}, {0x36, 6, 12}, {0x3E, 7, 8},
    }};

    for (const auto& test : cases) {
        gameboy::MemoryBus bus{
            gameboy::Cartridge{test_rom({test.opcode, 0xA5})}};
        gameboy::Cpu cpu;
        cpu.load_registers(initial_registers());
        const auto cycles = cpu.step(bus);
        const auto actual = test.destination == 6
                                ? bus.read8(0xC000)
                                : register_value(cpu.registers(), test.destination);
        check(actual == 0xA5, "LD r,d8 writes its immediate operand");
        check(cycles == test.cycles, "LD r,d8 has the correct cycle count");
        check(cpu.registers().pc == 0x0102, "LD r,d8 consumes two bytes");
        check(cpu.registers().f == 0xB0, "LD r,d8 preserves flags");
    }
}

void test_arithmetic_table() {
    struct ArithmeticCase {
        const char* name;
        std::uint8_t opcode;
        std::uint8_t a;
        std::uint8_t operand;
        std::uint8_t flags;
        std::uint8_t expected_a;
        std::uint8_t expected_flags;
    };
    constexpr std::array<ArithmeticCase, 12> cases{{
        {"ADD half carry", 0x80, 0x0F, 0x01, 0x00, 0x10, 0x20},
        {"ADD zero carry", 0x80, 0xFF, 0x01, 0x00, 0x00, 0xB0},
        {"ADC carry input", 0x88, 0x0F, 0x00, 0x10, 0x10, 0x20},
        {"ADC ignores stale flags", 0x88, 0x01, 0x01, 0xE0, 0x02, 0x00},
        {"SUB half borrow", 0x90, 0x10, 0x01, 0x00, 0x0F, 0x60},
        {"SUB full borrow", 0x90, 0x00, 0x01, 0x00, 0xFF, 0x70},
        {"SBC carry input", 0x98, 0x10, 0x00, 0x10, 0x0F, 0x60},
        {"SBC zero", 0x98, 0x01, 0x00, 0x10, 0x00, 0xC0},
        {"AND", 0xA0, 0xF0, 0x0F, 0xF0, 0x00, 0xA0},
        {"XOR", 0xA8, 0xAA, 0xAA, 0xF0, 0x00, 0x80},
        {"OR", 0xB0, 0x80, 0x01, 0xF0, 0x81, 0x00},
        {"CP", 0xB8, 0x10, 0x20, 0x00, 0x10, 0x50},
    }};

    for (const auto& test : cases) {
        gameboy::MemoryBus bus{
            gameboy::Cartridge{test_rom({test.opcode})}};
        auto registers = initial_registers();
        registers.a = test.a;
        registers.b = test.operand;
        registers.f = test.flags;
        gameboy::Cpu cpu;
        cpu.load_registers(registers);

        const auto cycles = cpu.step(bus);
        check(cpu.registers().a == test.expected_a,
              std::string(test.name) + " produces the expected accumulator");
        check(cpu.registers().f == test.expected_flags,
              std::string(test.name) + " produces the expected flags");
        check(cpu.registers().pc == 0x0101,
              std::string(test.name) + " advances PC once");
        check(cycles == 4, std::string(test.name) + " takes four cycles");
    }
}

void test_immediate_arithmetic_table() {
    struct ImmediateCase {
        std::uint8_t opcode;
        std::uint8_t flags;
        std::uint8_t expected_a;
        std::uint8_t expected_flags;
    };
    constexpr std::array<ImmediateCase, 8> cases{{
        {0xC6, 0x00, 0x11, 0x00}, // ADD
        {0xCE, 0x10, 0x12, 0x00}, // ADC
        {0xD6, 0x00, 0x0F, 0x60}, // SUB
        {0xDE, 0x10, 0x0E, 0x60}, // SBC
        {0xE6, 0xF0, 0x00, 0xA0}, // AND
        {0xEE, 0xF0, 0x11, 0x00}, // XOR
        {0xF6, 0xF0, 0x11, 0x00}, // OR
        {0xFE, 0x00, 0x10, 0x60}, // CP preserves A
    }};

    for (const auto& test : cases) {
        gameboy::MemoryBus bus{
            gameboy::Cartridge{test_rom({test.opcode, 0x01})}};
        auto registers = initial_registers();
        registers.a = 0x10;
        registers.f = test.flags;
        gameboy::Cpu cpu;
        cpu.load_registers(registers);
        const auto cycles = cpu.step(bus);

        check(cpu.registers().a == test.expected_a,
              "immediate ALU instruction produces the expected accumulator");
        check(cpu.registers().f == test.expected_flags,
              "immediate ALU instruction produces the expected flags");
        check(cpu.registers().pc == 0x0102,
              "immediate ALU instruction consumes two bytes");
        check(cycles == 8, "immediate ALU instruction takes eight cycles");
    }
}

void test_memory_arithmetic_table() {
    struct MemoryCase {
        std::uint8_t opcode;
        std::uint8_t a;
        std::uint8_t operand;
        std::uint8_t flags;
        std::uint8_t expected_a;
        std::uint8_t expected_flags;
    };
    constexpr std::array<MemoryCase, 8> cases{{
        {0x86, 0x01, 0x02, 0x00, 0x03, 0x00},
        {0x8E, 0x01, 0x02, 0x10, 0x04, 0x00},
        {0x96, 0x03, 0x02, 0x00, 0x01, 0x40},
        {0x9E, 0x03, 0x01, 0x10, 0x01, 0x40},
        {0xA6, 0xF0, 0x0F, 0x00, 0x00, 0xA0},
        {0xAE, 0xAA, 0xAA, 0x00, 0x00, 0x80},
        {0xB6, 0x80, 0x01, 0x00, 0x81, 0x00},
        {0xBE, 0x10, 0x20, 0x00, 0x10, 0x50},
    }};

    for (const auto& test : cases) {
        gameboy::MemoryBus bus{
            gameboy::Cartridge{test_rom({test.opcode})}};
        bus.write8(0xC000, test.operand);
        auto registers = initial_registers();
        registers.a = test.a;
        registers.f = test.flags;
        gameboy::Cpu cpu;
        cpu.load_registers(registers);
        check(cpu.step(bus) == 8, "ALU A,(HL) takes eight cycles");
        check(cpu.registers().a == test.expected_a,
              "ALU A,(HL) produces the expected accumulator");
        check(cpu.registers().f == test.expected_flags,
              "ALU A,(HL) produces the expected flags");
    }
}

void test_increment_decrement_table() {
    struct IncDecCase {
        const char* name;
        std::uint8_t opcode;
        std::uint8_t input;
        std::uint8_t flags;
        std::uint8_t expected;
        std::uint8_t expected_flags;
    };
    constexpr std::array<IncDecCase, 6> cases{{
        {"INC normal", 0x04, 0x01, 0x10, 0x02, 0x10},
        {"INC half carry", 0x04, 0x0F, 0x10, 0x10, 0x30},
        {"INC wrap", 0x04, 0xFF, 0x10, 0x00, 0xB0},
        {"DEC normal", 0x05, 0x02, 0x10, 0x01, 0x50},
        {"DEC half borrow", 0x05, 0x10, 0x10, 0x0F, 0x70},
        {"DEC zero", 0x05, 0x01, 0x10, 0x00, 0xD0},
    }};

    for (const auto& test : cases) {
        gameboy::MemoryBus bus{
            gameboy::Cartridge{test_rom({test.opcode})}};
        auto registers = initial_registers();
        registers.b = test.input;
        registers.f = test.flags;
        gameboy::Cpu cpu;
        cpu.load_registers(registers);
        const auto cycles = cpu.step(bus);
        check(cpu.registers().b == test.expected,
              std::string(test.name) + " produces the expected value");
        check(cpu.registers().f == test.expected_flags,
              std::string(test.name) + " produces the expected flags");
        check(cycles == 4, std::string(test.name) + " takes four cycles");
    }

    for (const auto opcode : {std::uint8_t{0x34}, std::uint8_t{0x35}}) {
        gameboy::MemoryBus bus{gameboy::Cartridge{test_rom({opcode})}};
        bus.write8(0xC000, opcode == 0x34 ? 0x0F : 0x10);
        gameboy::Cpu cpu;
        cpu.load_registers(initial_registers());
        const auto cycles = cpu.step(bus);
        check(bus.read8(0xC000) == (opcode == 0x34 ? 0x10 : 0x0F),
              "INC/DEC (HL) updates memory");
        check(cycles == 12, "INC/DEC (HL) takes twelve cycles");
    }
}

void test_sixteen_bit_operations() {
    struct PairLoadCase {
        std::uint8_t opcode;
        std::uint16_t expected;
    };
    constexpr std::array<PairLoadCase, 4> loads{{
        {0x01, 0xA1B2}, {0x11, 0xA1B2}, {0x21, 0xA1B2}, {0x31, 0xA1B2},
    }};
    for (unsigned index = 0; index < loads.size(); ++index) {
        gameboy::MemoryBus bus{
            gameboy::Cartridge{test_rom({loads[index].opcode, 0xB2, 0xA1})}};
        gameboy::Cpu cpu;
        cpu.load_registers(initial_registers());
        check(cpu.step(bus) == 12, "LD rr,d16 takes twelve cycles");
        const auto& r = cpu.registers();
        const std::array<std::uint16_t, 4> actual{
            static_cast<std::uint16_t>((r.b << 8) | r.c),
            static_cast<std::uint16_t>((r.d << 8) | r.e),
            static_cast<std::uint16_t>((r.h << 8) | r.l), r.sp,
        };
        check(actual[index] == loads[index].expected,
              "LD rr,d16 loads the selected register pair");
        check(r.pc == 0x0103, "LD rr,d16 consumes three bytes");
    }

    gameboy::MemoryBus add_bus{gameboy::Cartridge{test_rom({0x09})}};
    auto add_registers = initial_registers();
    add_registers.b = 0x00;
    add_registers.c = 0x01;
    add_registers.h = 0xFF;
    add_registers.l = 0xFF;
    add_registers.f = 0x80;
    gameboy::Cpu add_cpu;
    add_cpu.load_registers(add_registers);
    check(add_cpu.step(add_bus) == 8, "ADD HL,rr takes eight cycles");
    check(add_cpu.registers().h == 0 && add_cpu.registers().l == 0,
          "ADD HL,rr wraps at sixteen bits");
    check(add_cpu.registers().f == 0xB0,
          "ADD HL,rr sets H/C, clears N, and preserves Z");

    gameboy::MemoryBus offset_bus{
        gameboy::Cartridge{test_rom({0xF8, 0xFF})}};
    auto offset_registers = initial_registers();
    offset_registers.sp = 0x0001;
    gameboy::Cpu offset_cpu;
    offset_cpu.load_registers(offset_registers);
    check(offset_cpu.step(offset_bus) == 12, "LD HL,SP+e8 takes twelve cycles");
    check(offset_cpu.registers().h == 0 && offset_cpu.registers().l == 0,
          "LD HL,SP+e8 sign-extends negative offsets");
    check(offset_cpu.registers().f == 0x30,
          "LD HL,SP+e8 derives H/C from the low unsigned byte addition");
}

void test_special_loads_and_decimal_adjust() {
    gameboy::MemoryBus store_bus{
        gameboy::Cartridge{test_rom({0x22, 0x3A})}};
    auto registers = initial_registers();
    registers.a = 0x42;
    gameboy::Cpu cpu;
    cpu.load_registers(registers);
    check(cpu.step(store_bus) == 8, "LD (HL+),A takes eight cycles");
    check(store_bus.read8(0xC000) == 0x42, "LD (HL+),A stores A");
    check(cpu.registers().l == 0x01, "LD (HL+),A increments HL");
    store_bus.write8(0xC001, 0x99);
    check(cpu.step(store_bus) == 8, "LD A,(HL-) takes eight cycles");
    check(cpu.registers().a == 0x99 && cpu.registers().l == 0x00,
          "LD A,(HL-) loads A and decrements HL");

    struct DaaCase {
        std::uint8_t a;
        std::uint8_t flags;
        std::uint8_t expected_a;
        std::uint8_t expected_flags;
    };
    constexpr std::array<DaaCase, 4> daa_cases{{
        {0x3C, 0x00, 0x42, 0x00},
        {0x32, 0x20, 0x38, 0x00},
        {0x9A, 0x00, 0x00, 0x90},
        {0x0F, 0x60, 0x09, 0x40},
    }};
    for (const auto& test : daa_cases) {
        gameboy::MemoryBus bus{gameboy::Cartridge{test_rom({0x27})}};
        auto state = initial_registers();
        state.a = test.a;
        state.f = test.flags;
        gameboy::Cpu daa_cpu;
        daa_cpu.load_registers(state);
        check(daa_cpu.step(bus) == 4, "DAA takes four cycles");
        check(daa_cpu.registers().a == test.expected_a,
              "DAA produces the expected BCD result");
        check(daa_cpu.registers().f == test.expected_flags,
              "DAA produces the expected flags");
    }
}

void test_addressed_loads() {
    gameboy::MemoryBus bus{gameboy::Cartridge{test_rom({
        0xEA, 0x00, 0xC1, // LD (C100),A
        0xFA, 0x00, 0xC2, // LD A,(C200)
        0xE0, 0x80,       // LDH (FF80),A
        0xF0, 0x81,       // LDH A,(FF81)
        0xE2,             // LD (FF00+C),A
        0xF2,             // LD A,(FF00+C)
        0x08, 0x00, 0xC3, // LD (C300),SP
    })}};
    bus.write8(0xC200, 0x55);
    bus.write8(0xFF81, 0x66);
    auto state = initial_registers();
    state.a = 0x42;
    state.c = 0x82;
    state.sp = 0xBEEF;
    gameboy::Cpu cpu;
    cpu.load_registers(state);

    check(cpu.step(bus) == 16 && bus.read8(0xC100) == 0x42,
          "LD (a16),A stores through an absolute address");
    check(cpu.step(bus) == 16 && cpu.registers().a == 0x55,
          "LD A,(a16) loads through an absolute address");
    check(cpu.step(bus) == 12 && bus.read8(0xFF80) == 0x55,
          "LDH (a8),A stores in high memory");
    check(cpu.step(bus) == 12 && cpu.registers().a == 0x66,
          "LDH A,(a8) loads from high memory");
    check(cpu.step(bus) == 8 && bus.read8(0xFF82) == 0x66,
          "LD (C),A uses C as a high-memory offset");
    bus.write8(0xFF82, 0x77);
    check(cpu.step(bus) == 8 && cpu.registers().a == 0x77,
          "LD A,(C) uses C as a high-memory offset");
    check(cpu.step(bus) == 20 && bus.read16(0xC300) == 0xBEEF,
          "LD (a16),SP stores little-endian SP");
    check(cpu.registers().pc == 0x010F,
          "addressed load sequence consumes every operand byte");

    gameboy::MemoryBus sp_bus{
        gameboy::Cartridge{test_rom({0xE8, 0xFF, 0xF9})}};
    auto sp_state = initial_registers();
    sp_state.sp = 0x0001;
    sp_state.h = 0xC1;
    sp_state.l = 0x23;
    gameboy::Cpu sp_cpu;
    sp_cpu.load_registers(sp_state);
    check(sp_cpu.step(sp_bus) == 16 && sp_cpu.registers().sp == 0,
          "ADD SP,e8 sign-extends a negative offset");
    check(sp_cpu.registers().f == 0x30,
          "ADD SP,e8 sets the expected low-byte carry flags");
    check(sp_cpu.step(sp_bus) == 8 && sp_cpu.registers().sp == 0xC123,
          "LD SP,HL copies the register pair");
}

void test_stack_load_table() {
    struct StackCase {
        std::uint8_t push_opcode;
        std::uint8_t pop_opcode;
        std::uint16_t value;
        std::uint16_t expected;
    };
    constexpr std::array<StackCase, 4> cases{{
        {0xC5, 0xC1, 0x1122, 0x1122},
        {0xD5, 0xD1, 0x3344, 0x3344},
        {0xE5, 0xE1, 0xC000, 0xC000},
        {0xF5, 0xF1, 0x77BF, 0x77B0},
    }};

    for (unsigned index = 0; index < cases.size(); ++index) {
        const auto& test = cases[index];
        gameboy::MemoryBus bus{gameboy::Cartridge{
            test_rom({test.push_opcode, test.pop_opcode})}};
        auto state = initial_registers();
        state.sp = 0xD000;
        gameboy::Cpu cpu;
        cpu.load_registers(state);

        check(cpu.step(bus) == 16, "PUSH qq takes sixteen cycles");
        check(cpu.registers().sp == 0xCFFE, "PUSH qq decrements SP by two");
        check(bus.read16(0xCFFE) ==
                  ((test.value & 0xFFF0U) |
                   (index == 3 ? 0U : (test.value & 0x000FU))),
              "PUSH qq writes the selected pair in little-endian order");

        bus.write16(0xCFFE, test.value);
        check(cpu.step(bus) == 12, "POP qq takes twelve cycles");
        check(cpu.registers().sp == 0xD000, "POP qq increments SP by two");
        const auto& result = cpu.registers();
        const std::array<std::uint16_t, 4> actual{
            static_cast<std::uint16_t>((result.b << 8) | result.c),
            static_cast<std::uint16_t>((result.d << 8) | result.e),
            static_cast<std::uint16_t>((result.h << 8) | result.l),
            static_cast<std::uint16_t>((result.a << 8) | result.f),
        };
        check(actual[index] == test.expected,
              "POP qq restores the selected pair and normalizes AF");
    }
}

void test_conditional_branch_tables() {
    struct ConditionCase {
        std::uint8_t jr_opcode;
        std::uint8_t jp_opcode;
        std::uint8_t flags_when_true;
        std::uint8_t flags_when_false;
    };
    constexpr std::array<ConditionCase, 4> conditions{{
        {0x20, 0xC2, 0x00, 0x80}, // NZ
        {0x28, 0xCA, 0x80, 0x00}, // Z
        {0x30, 0xD2, 0x00, 0x10}, // NC
        {0x38, 0xDA, 0x10, 0x00}, // C
    }};

    for (const auto& test : conditions) {
        for (const auto taken : {false, true}) {
            gameboy::MemoryBus jr_bus{gameboy::Cartridge{
                test_rom({test.jr_opcode, 0x02})}};
            auto state = initial_registers();
            state.f = taken ? test.flags_when_true : test.flags_when_false;
            gameboy::Cpu jr_cpu;
            jr_cpu.load_registers(state);
            check(jr_cpu.step(jr_bus) == (taken ? 12U : 8U),
                  "JR cc,e8 uses taken/not-taken timing");
            check(jr_cpu.registers().pc == (taken ? 0x0104 : 0x0102),
                  "JR cc,e8 applies the selected condition");

            gameboy::MemoryBus jp_bus{gameboy::Cartridge{
                test_rom({test.jp_opcode, 0x00, 0xC0})}};
            gameboy::Cpu jp_cpu;
            jp_cpu.load_registers(state);
            check(jp_cpu.step(jp_bus) == (taken ? 16U : 12U),
                  "JP cc,a16 uses taken/not-taken timing");
            check(jp_cpu.registers().pc == (taken ? 0xC000 : 0x0103),
                  "JP cc,a16 applies the selected condition");
        }
    }

    gameboy::MemoryBus relative_bus{
        gameboy::Cartridge{test_rom({0x18, 0xFE})}};
    gameboy::Cpu relative_cpu;
    relative_cpu.load_registers(initial_registers());
    check(relative_cpu.step(relative_bus) == 12 &&
              relative_cpu.registers().pc == 0x0100,
          "JR sign-extends negative offsets");

    gameboy::MemoryBus indirect_bus{
        gameboy::Cartridge{test_rom({0xE9})}};
    gameboy::Cpu indirect_cpu;
    indirect_cpu.load_registers(initial_registers());
    check(indirect_cpu.step(indirect_bus) == 4 &&
              indirect_cpu.registers().pc == 0xC000,
          "JP HL jumps without reading an immediate operand");
}

void test_calls_returns_and_restarts() {
    auto rom = test_rom({0xCD, 0x00, 0xC0});
    gameboy::MemoryBus bus{gameboy::Cartridge{std::move(rom)}};
    bus.write8(0xC000, 0xC9);
    auto state = initial_registers();
    state.sp = 0xD000;
    gameboy::Cpu cpu;
    cpu.load_registers(state);
    check(cpu.step(bus) == 24, "CALL a16 takes twenty-four cycles");
    check(cpu.registers().pc == 0xC000 && cpu.registers().sp == 0xCFFE,
          "CALL a16 jumps and reserves a stack word");
    check(bus.read16(0xCFFE) == 0x0103,
          "CALL a16 pushes the return address");
    check(cpu.step(bus) == 16 && cpu.registers().pc == 0x0103,
          "RET restores the return address");
    check(cpu.registers().sp == 0xD000, "RET releases its stack word");

    struct CallCase {
        std::uint8_t call_opcode;
        std::uint8_t return_opcode;
        std::uint8_t flags_when_true;
        std::uint8_t flags_when_false;
    };
    constexpr std::array<CallCase, 4> conditions{{
        {0xC4, 0xC0, 0x00, 0x80}, {0xCC, 0xC8, 0x80, 0x00},
        {0xD4, 0xD0, 0x00, 0x10}, {0xDC, 0xD8, 0x10, 0x00},
    }};
    for (const auto& test : conditions) {
        for (const auto taken : {false, true}) {
            gameboy::MemoryBus call_bus{gameboy::Cartridge{
                test_rom({test.call_opcode, 0x00, 0xC0})}};
            auto call_state = initial_registers();
            call_state.sp = 0xD000;
            call_state.f = taken ? test.flags_when_true : test.flags_when_false;
            gameboy::Cpu call_cpu;
            call_cpu.load_registers(call_state);
            check(call_cpu.step(call_bus) == (taken ? 24U : 12U),
                  "CALL cc,a16 uses taken/not-taken timing");
            check(call_cpu.registers().pc == (taken ? 0xC000 : 0x0103),
                  "CALL cc,a16 applies the selected condition");

            gameboy::MemoryBus return_bus{gameboy::Cartridge{
                test_rom({test.return_opcode})}};
            auto return_state = call_state;
            return_state.sp = 0xCFFE;
            return_bus.write16(0xCFFE, 0xC123);
            gameboy::Cpu return_cpu;
            return_cpu.load_registers(return_state);
            check(return_cpu.step(return_bus) == (taken ? 20U : 8U),
                  "RET cc uses taken/not-taken timing");
            check(return_cpu.registers().pc == (taken ? 0xC123 : 0x0101),
                  "RET cc applies the selected condition");
        }
    }

    constexpr std::array<std::uint8_t, 8> restarts{
        0xC7, 0xCF, 0xD7, 0xDF, 0xE7, 0xEF, 0xF7, 0xFF,
    };
    for (const auto opcode : restarts) {
        gameboy::MemoryBus rst_bus{
            gameboy::Cartridge{test_rom({opcode})}};
        auto rst_state = initial_registers();
        rst_state.sp = 0xD000;
        gameboy::Cpu rst_cpu;
        rst_cpu.load_registers(rst_state);
        check(rst_cpu.step(rst_bus) == 16, "RST vec takes sixteen cycles");
        check(rst_cpu.registers().pc == (opcode & 0x38),
              "RST vec selects the encoded vector");
        check(rst_bus.read16(0xCFFE) == 0x0101,
              "RST vec pushes the return address");
    }
}

void test_interrupt_and_low_power_states() {
    auto interrupt_rom = test_rom({0xFB, 0x00, 0x00});
    interrupt_rom[0x0040] = 0xD9; // RETI
    gameboy::MemoryBus interrupt_bus{
        gameboy::Cartridge{std::move(interrupt_rom)}};
    interrupt_bus.write8(0xFFFF, 0x01);
    interrupt_bus.write8(0xFF0F, 0x01);
    auto state = initial_registers();
    state.sp = 0xD000;
    gameboy::Cpu interrupt_cpu;
    interrupt_cpu.load_registers(state);

    check(interrupt_cpu.step(interrupt_bus) == 4 &&
              !interrupt_cpu.interrupts_enabled(),
          "EI does not enable interrupts immediately");
    check(interrupt_cpu.step(interrupt_bus) == 4 &&
              interrupt_cpu.interrupts_enabled(),
          "EI enables interrupts after the following instruction");
    check(interrupt_cpu.step(interrupt_bus) == 20,
          "interrupt dispatch takes twenty cycles");
    check(interrupt_cpu.registers().pc == 0x0040 &&
              interrupt_cpu.registers().sp == 0xCFFE,
          "interrupt dispatch pushes PC and selects the highest-priority vector");
    check(interrupt_bus.read16(0xCFFE) == 0x0102 &&
              (interrupt_bus.read8(0xFF0F) & 1) == 0,
          "interrupt dispatch stores PC and acknowledges IF");
    check(interrupt_cpu.step(interrupt_bus) == 16 &&
              interrupt_cpu.registers().pc == 0x0102 &&
              interrupt_cpu.interrupts_enabled(),
          "RETI restores PC and enables interrupts");
    check(interrupt_cpu.total_cycles() == 44,
          "CPU accumulates instruction, idle, and interrupt cycles");

    gameboy::MemoryBus repeated_ei_bus{
        gameboy::Cartridge{test_rom({0xFB, 0xFB, 0x00})}};
    repeated_ei_bus.write8(0xFFFF, 1);
    repeated_ei_bus.write8(0xFF0F, 1);
    gameboy::Cpu repeated_ei_cpu;
    repeated_ei_cpu.load_registers(initial_registers());
    static_cast<void>(repeated_ei_cpu.step(repeated_ei_bus));
    static_cast<void>(repeated_ei_cpu.step(repeated_ei_bus));
    check(repeated_ei_cpu.interrupts_enabled() &&
              repeated_ei_cpu.step(repeated_ei_bus) == 20,
          "repeating EI does not postpone an already scheduled IME enable");

    gameboy::MemoryBus interrupt_register_bus{
        gameboy::Cartridge{test_rom()}};
    interrupt_register_bus.write8(0xFF0F, 0x08);
    check(interrupt_register_bus.read8(0xFF0F) == 0xE8,
          "unused IF bits read high while writable interrupt flags are retained");

    gameboy::MemoryBus di_bus{
        gameboy::Cartridge{test_rom({0xFB, 0xF3, 0x00})}};
    di_bus.write8(0xFFFF, 1);
    di_bus.write8(0xFF0F, 1);
    gameboy::Cpu di_cpu;
    di_cpu.load_registers(initial_registers());
    static_cast<void>(di_cpu.step(di_bus));
    static_cast<void>(di_cpu.step(di_bus));
    check(!di_cpu.interrupts_enabled(), "DI cancels a pending EI enable");
    check(di_cpu.step(di_bus) == 4 && di_cpu.registers().pc == 0x0103,
          "DI prevents pending interrupts from dispatching");

    gameboy::MemoryBus halt_bus{
        gameboy::Cartridge{test_rom({0x76, 0x00})}};
    gameboy::Cpu halt_cpu;
    halt_cpu.load_registers(initial_registers());
    check(halt_cpu.step(halt_bus) == 4 && halt_cpu.halted(),
          "HALT enters the halted state");
    check(halt_cpu.step(halt_bus) == 4 && halt_cpu.registers().pc == 0x0101,
          "HALT idles without advancing PC");
    halt_bus.write8(0xFFFF, 1);
    halt_bus.write8(0xFF0F, 1);
    check(halt_cpu.step(halt_bus) == 4 && !halt_cpu.halted() &&
              halt_cpu.registers().pc == 0x0102,
          "a pending interrupt wakes HALT even when IME is clear");

    gameboy::MemoryBus bug_bus{
        gameboy::Cartridge{test_rom({0x76, 0x00, 0x00})}};
    bug_bus.write8(0xFFFF, 1);
    bug_bus.write8(0xFF0F, 1);
    gameboy::Cpu bug_cpu;
    bug_cpu.load_registers(initial_registers());
    static_cast<void>(bug_cpu.step(bug_bus));
    check(!bug_cpu.halted(), "HALT bug does not enter the halted state");
    static_cast<void>(bug_cpu.step(bug_bus));
    check(bug_cpu.registers().pc == 0x0101,
          "HALT bug suppresses the next opcode-fetch increment");
    static_cast<void>(bug_cpu.step(bug_bus));
    check(bug_cpu.registers().pc == 0x0102,
          "execution resumes normally after the HALT bug fetch");

    gameboy::MemoryBus stop_bus{
        gameboy::Cartridge{test_rom({0x10, 0x00, 0x00})}};
    gameboy::Cpu stop_cpu;
    stop_cpu.load_registers(initial_registers());
    check(stop_cpu.step(stop_bus) == 4 && stop_cpu.stopped() &&
              stop_cpu.registers().pc == 0x0102,
          "STOP consumes its padding byte and enters stopped state");
    check(stop_cpu.step(stop_bus) == 4 && stop_cpu.registers().pc == 0x0102,
          "STOP idles without advancing PC");
}

void test_base_rotate_table() {
    struct RotateCase {
        std::uint8_t opcode;
        std::uint8_t value;
        std::uint8_t flags;
        std::uint8_t expected;
        std::uint8_t expected_flags;
    };
    constexpr std::array<RotateCase, 4> cases{{
        {0x07, 0x80, 0xF0, 0x01, 0x10},
        {0x0F, 0x01, 0xF0, 0x80, 0x10},
        {0x17, 0x80, 0x10, 0x01, 0x10},
        {0x1F, 0x01, 0x10, 0x80, 0x10},
    }};
    for (const auto& test : cases) {
        gameboy::MemoryBus bus{
            gameboy::Cartridge{test_rom({test.opcode})}};
        auto state = initial_registers();
        state.a = test.value;
        state.f = test.flags;
        gameboy::Cpu cpu;
        cpu.load_registers(state);
        check(cpu.step(bus) == 4, "base accumulator rotate takes four cycles");
        check(cpu.registers().a == test.expected &&
                  cpu.registers().f == test.expected_flags,
              "base accumulator rotate produces the expected result and flags");
    }
}

void test_cb_rotate_shift_table() {
    struct CbCase {
        std::uint8_t opcode;
        std::uint8_t value;
        std::uint8_t flags;
        std::uint8_t expected;
        std::uint8_t expected_flags;
    };
    constexpr std::array<CbCase, 8> cases{{
        {0x00, 0x81, 0x00, 0x03, 0x10}, // RLC
        {0x08, 0x01, 0x00, 0x80, 0x10}, // RRC
        {0x10, 0x80, 0x10, 0x01, 0x10}, // RL
        {0x18, 0x01, 0x10, 0x80, 0x10}, // RR
        {0x20, 0x81, 0x00, 0x02, 0x10}, // SLA
        {0x28, 0x81, 0x00, 0xC0, 0x10}, // SRA
        {0x30, 0xF0, 0xF0, 0x0F, 0x00}, // SWAP
        {0x38, 0x01, 0x00, 0x00, 0x90}, // SRL
    }};

    for (const auto& test : cases) {
        gameboy::MemoryBus bus{
            gameboy::Cartridge{test_rom({0xCB, test.opcode})}};
        auto state = initial_registers();
        state.b = test.value;
        state.f = test.flags;
        gameboy::Cpu cpu;
        cpu.load_registers(state);
        check(cpu.step(bus) == 8, "CB register rotate/shift takes eight cycles");
        check(cpu.registers().b == test.expected &&
                  cpu.registers().f == test.expected_flags,
              "CB register rotate/shift produces expected result and flags");

        const auto memory_opcode = static_cast<std::uint8_t>(test.opcode | 0x06);
        gameboy::MemoryBus memory_bus{
            gameboy::Cartridge{test_rom({0xCB, memory_opcode})}};
        memory_bus.write8(0xC000, test.value);
        gameboy::Cpu memory_cpu;
        memory_cpu.load_registers(state);
        check(memory_cpu.step(memory_bus) == 16,
              "CB (HL) rotate/shift takes sixteen cycles");
        check(memory_bus.read8(0xC000) == test.expected &&
                  memory_cpu.registers().f == test.expected_flags,
              "CB (HL) rotate/shift updates memory and flags");
    }
}

void test_cb_bit_reset_set_matrix() {
    for (unsigned bit = 0; bit < 8; ++bit) {
        const auto mask = static_cast<std::uint8_t>(1U << bit);
        for (const auto target : {0U, 6U}) {
            const auto bit_opcode = static_cast<std::uint8_t>(
                0x40 | (bit << 3) | target);
            gameboy::MemoryBus bit_bus{
                gameboy::Cartridge{test_rom({0xCB, bit_opcode})}};
            auto state = initial_registers();
            state.b = mask;
            state.f = 0x10;
            bit_bus.write8(0xC000, mask);
            gameboy::Cpu bit_cpu;
            bit_cpu.load_registers(state);
            check(bit_cpu.step(bit_bus) == (target == 6 ? 12U : 8U),
                  "BIT b,r uses register/(HL) timing");
            check(bit_cpu.registers().f == 0x30,
                  "BIT b,r preserves C, sets H, and reports a set bit");

            const auto res_opcode = static_cast<std::uint8_t>(
                0x80 | (bit << 3) | target);
            gameboy::MemoryBus res_bus{
                gameboy::Cartridge{test_rom({0xCB, res_opcode})}};
            res_bus.write8(0xC000, 0xFF);
            state.b = 0xFF;
            state.f = 0xB0;
            gameboy::Cpu res_cpu;
            res_cpu.load_registers(state);
            check(res_cpu.step(res_bus) == (target == 6 ? 16U : 8U),
                  "RES b,r uses register/(HL) timing");
            const auto res_value = target == 6 ? res_bus.read8(0xC000)
                                               : res_cpu.registers().b;
            check(res_value == static_cast<std::uint8_t>(0xFF & ~mask) &&
                      res_cpu.registers().f == 0xB0,
                  "RES b,r clears the selected bit and preserves flags");

            const auto set_opcode = static_cast<std::uint8_t>(
                0xC0 | (bit << 3) | target);
            gameboy::MemoryBus set_bus{
                gameboy::Cartridge{test_rom({0xCB, set_opcode})}};
            set_bus.write8(0xC000, 0x00);
            state.b = 0x00;
            gameboy::Cpu set_cpu;
            set_cpu.load_registers(state);
            check(set_cpu.step(set_bus) == (target == 6 ? 16U : 8U),
                  "SET b,r uses register/(HL) timing");
            const auto set_value = target == 6 ? set_bus.read8(0xC000)
                                               : set_cpu.registers().b;
            check(set_value == mask && set_cpu.registers().f == 0xB0,
                  "SET b,r sets the selected bit and preserves flags");
        }
    }
}

void test_base_opcode_completeness() {
    const auto invalid = [](const std::uint8_t opcode) {
        switch (opcode) {
        case 0xD3: case 0xDB: case 0xDD:
        case 0xE3: case 0xE4: case 0xEB: case 0xEC: case 0xED:
        case 0xF4: case 0xFC: case 0xFD:
            return true;
        default:
            return false;
        }
    };

    for (unsigned value = 0; value <= 0xFF; ++value) {
        const auto opcode = static_cast<std::uint8_t>(value);
        gameboy::MemoryBus bus{
            gameboy::Cartridge{test_rom({opcode, 0x00, 0xC0})}};
        auto state = initial_registers();
        state.sp = 0xD000;
        gameboy::Cpu cpu;
        cpu.load_registers(state);
        auto rejected = false;
        try {
            static_cast<void>(cpu.step(bus));
        } catch (const gameboy::UnsupportedOpcode&) {
            rejected = true;
        }
        check(rejected == invalid(opcode),
              "base decoder accepts every legal opcode and rejects only invalid ones");
    }
}

void test_divider_and_timer_frequencies() {
    gameboy::MemoryBus divider_bus{gameboy::Cartridge{test_rom()}};
    divider_bus.tick(255);
    check(divider_bus.read8(0xFF04) == 0,
          "DIV exposes the upper byte of the internal divider");
    divider_bus.tick(1);
    check(divider_bus.read8(0xFF04) == 1,
          "DIV advances once per 256 T-cycles");
    divider_bus.write8(0xFF04, 0xAB);
    check(divider_bus.read8(0xFF04) == 0,
          "writing any value to DIV resets the internal divider");

    struct FrequencyCase {
        std::uint8_t selection;
        unsigned period;
    };
    constexpr std::array<FrequencyCase, 4> frequencies{{
        {0, 1024}, {1, 16}, {2, 64}, {3, 256},
    }};
    for (const auto& test : frequencies) {
        gameboy::MemoryBus bus{gameboy::Cartridge{test_rom()}};
        bus.write8(0xFF07, static_cast<std::uint8_t>(0x04 | test.selection));
        check(bus.read8(0xFF07) ==
                  static_cast<std::uint8_t>(0xFC | test.selection),
              "TAC stores its low bits and reads unused bits high");
        bus.tick(test.period - 1);
        check(bus.read8(0xFF05) == 0,
              "TIMA waits for the selected divider falling edge");
        bus.tick(1);
        check(bus.read8(0xFF05) == 1,
              "TIMA increments at the selected TAC frequency");
    }

    gameboy::MemoryBus disabled_bus{gameboy::Cartridge{test_rom()}};
    disabled_bus.write8(0xFF07, 0x01);
    disabled_bus.tick(1024);
    check(disabled_bus.read8(0xFF05) == 0,
          "TIMA does not advance while TAC is disabled");
}

void test_timer_overflow_pipeline() {
    gameboy::MemoryBus bus{gameboy::Cartridge{test_rom()}};
    bus.write8(0xFF05, 0xFF);
    bus.write8(0xFF06, 0x42);
    bus.write8(0xFF07, 0x05);
    bus.tick(16);
    check(bus.read8(0xFF05) == 0 && (bus.read8(0xFF0F) & 0x04) == 0,
          "TIMA overflow enters its four-cycle pending state");
    bus.tick(3);
    check(bus.read8(0xFF05) == 0 && (bus.read8(0xFF0F) & 0x04) == 0,
          "TIMA remains zero during the reload delay");
    bus.tick(1);
    check(bus.read8(0xFF05) == 0x42 && (bus.read8(0xFF0F) & 0x04) != 0,
          "TIMA reloads TMA and requests interrupt 2 after four cycles");

    gameboy::MemoryBus cancel_bus{gameboy::Cartridge{test_rom()}};
    cancel_bus.write8(0xFF05, 0xFF);
    cancel_bus.write8(0xFF06, 0x42);
    cancel_bus.write8(0xFF07, 0x05);
    cancel_bus.tick(16);
    cancel_bus.write8(0xFF05, 0x99);
    cancel_bus.tick(4);
    check(cancel_bus.read8(0xFF05) == 0x99 &&
              (cancel_bus.read8(0xFF0F) & 0x04) == 0,
          "writing TIMA during overflow cancels reload and interrupt");

    gameboy::MemoryBus modulo_bus{gameboy::Cartridge{test_rom()}};
    modulo_bus.write8(0xFF05, 0xFF);
    modulo_bus.write8(0xFF06, 0x42);
    modulo_bus.write8(0xFF07, 0x05);
    modulo_bus.tick(16);
    modulo_bus.tick(2);
    modulo_bus.write8(0xFF06, 0x55);
    modulo_bus.tick(2);
    check(modulo_bus.read8(0xFF05) == 0x55,
          "a TMA write during overflow changes the pending reload value");
}

void test_timer_write_edges() {
    gameboy::MemoryBus divider_bus{gameboy::Cartridge{test_rom()}};
    divider_bus.write8(0xFF07, 0x05);
    divider_bus.tick(8); // Selected divider bit 3 is now high.
    divider_bus.write8(0xFF04, 0);
    check(divider_bus.read8(0xFF05) == 1,
          "resetting DIV across a timer-input falling edge increments TIMA");

    gameboy::MemoryBus control_bus{gameboy::Cartridge{test_rom()}};
    control_bus.write8(0xFF07, 0x05);
    control_bus.tick(8);
    control_bus.write8(0xFF07, 0x00);
    check(control_bus.read8(0xFF05) == 1,
          "changing TAC across a timer-input falling edge increments TIMA");
}

void test_emulator_timer_integration() {
    gameboy::Emulator emulator{
        gameboy::Cartridge{test_rom({0x00, 0x00, 0x00, 0x00})}};
    emulator.bus().write8(0xFF07, 0x05);
    for (unsigned instruction = 0; instruction < 4; ++instruction) {
        check(emulator.step() == 4, "NOP takes four cycles during timer integration");
    }
    check(emulator.bus().read8(0xFF05) == 1,
          "emulator steps forward timer hardware by CPU cycles");

    gameboy::Emulator halted{
        gameboy::Cartridge{test_rom({0x76, 0x00})}};
    halted.bus().write8(0xFF04, 0);
    halted.bus().write8(0xFFFF, 0x04);
    halted.bus().write8(0xFF05, 0xFF);
    halted.bus().write8(0xFF06, 0x00);
    halted.bus().write8(0xFF07, 0x05);
    for (unsigned step = 0; step < 5; ++step) {
        static_cast<void>(halted.step());
    }
    check(halted.cpu().halted() && (halted.bus().read8(0xFF0F) & 0x04) != 0,
          "timer continues running and requests interrupts while CPU is halted");
    check(halted.step() == 4 && !halted.cpu().halted() &&
              halted.cpu().registers().pc == 0x0102,
          "a timer interrupt wakes HALT when IME is disabled");

    gameboy::Emulator stopped{
        gameboy::Cartridge{test_rom({0x10, 0x00, 0x00})}};
    stopped.bus().tick(300);
    check(stopped.bus().read8(0xFF04) != 0,
          "STOP integration begins with an advanced divider");
    static_cast<void>(stopped.step());
    check(stopped.cpu().stopped() && stopped.bus().read8(0xFF04) == 0,
          "STOP resets the system divider");
    for (unsigned idle = 0; idle < 100; ++idle) {
        static_cast<void>(stopped.step());
    }
    check(stopped.bus().read8(0xFF04) == 0,
          "the system divider remains frozen while STOP is active");
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
    // Version 22 adds SGB joypad packet parser state and PPU palettes/
    // attribute memory.  Strip it when constructing the legacy v1-v21
    // fixtures below, just like the earlier version deltas.
    constexpr std::size_t version_twenty_two_sgb_size = 237 + 393;
    constexpr std::size_t version_nine_fetcher_size =
        737 + version_ten_window_latch_size + version_eleven_fetcher_size +
        version_twelve_sprite_size + version_thirteen_sprite_fetch_size +
        version_fourteen_sprite_deadline_size + version_fifteen_sprite_render_size;
    auto legacy_saved = saved;
    legacy_saved.resize(legacy_saved.size() -
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
    future_version[8] = 23;
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

int main(const int argc, char** argv) {
    try {
        if (argc > 0 && argv != nullptr && argv[0] != nullptr) {
            std::error_code error;
            executable_directory =
                std::filesystem::absolute(argv[0], error).parent_path();
            if (error) executable_directory.clear();
        }
        test_cartridge_header();
        test_sgb_command_path();
        test_touch_control_ownership();
        test_rom_library_metadata_and_deduplication();
        test_multicore_frontend_contract();
        test_scene_snapshot_contract();
        test_scene_snapshot_json();
        test_desktop_dashboard_navigation();
        test_gameshark_cheats();
        test_video_pipeline_modes();
        test_voxel_profiles();
        test_audio_frontend_helpers();
        test_apu_waveform_regressions();
        test_apu_cycle_integrated_resampling();
        test_cartridge_file_loading();
        test_cgb_memory_and_rendering();
        test_mbc1_rom_banking();
        test_mbc1_ram_banking();
        test_mbc2_banking_and_ram();
        test_battery_ram_persistence();
        test_battery_data_import_export();
        test_mbc3_banking_and_rtc();
        test_mbc3_rtc_persistence();
        test_mbc5_banking_and_rumble();
        test_gameboy_camera();
        test_memory_map();
        test_apu_power_registers_and_wave_ram();
        test_active_wave_ram_timing();
        test_apu_high_pass_filter();
        test_apu_pulse2_samples_and_length();
        test_apu_pulse1_sweep_wave_and_noise();
        test_serial_transfer();
        test_serial_link_cable();
        test_serial_link_interrupt_handshake();
        test_serial_link_interrupt_rearm();
        test_serial_link_asymmetric_scheduling();
        test_link_session_lifecycle();
        test_link_session_timeout_and_retry();
        test_link_transport_framing();
        test_tcp_link_channel_loopback();
        test_tcp_serial_endpoint_loopback();
        test_gameboy_printer();
        test_cpu_state_normalization();
        test_register_load_matrix();
        test_immediate_load_table();
        test_arithmetic_table();
        test_immediate_arithmetic_table();
        test_memory_arithmetic_table();
        test_increment_decrement_table();
        test_sixteen_bit_operations();
        test_special_loads_and_decimal_adjust();
        test_addressed_loads();
        test_stack_load_table();
        test_conditional_branch_tables();
        test_calls_returns_and_restarts();
        test_interrupt_and_low_power_states();
        test_base_rotate_table();
        test_cb_rotate_shift_table();
        test_cb_bit_reset_set_matrix();
        test_base_opcode_completeness();
        test_divider_and_timer_frequencies();
        test_timer_overflow_pipeline();
        test_timer_write_edges();
        test_emulator_timer_integration();
        test_cpu_machine_cycle_bus_timing();
        test_ppu_modes_and_memory_access();
        test_ppu_stat_interrupts();
        test_ppu_vblank_and_frame_publication();
        test_ppu_background_window_and_sprites();
        test_joypad_matrix_and_interrupts();
        test_oam_dma();
        test_save_state_round_trip_and_validation();
    } catch (const std::exception& error) {
        std::cerr << "Unexpected exception: " << error.what() << '\n';
        return 1;
    }

    if (failures == 0) {
        std::cout << "All tests passed\n";
    }
    return failures == 0 ? 0 : 1;
}
