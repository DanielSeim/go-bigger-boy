#include "gbb/audio.hpp"
#include "gbb/core_registry.hpp"
#include "gbb/dashboard_navigation.hpp"
#include "gbb/gameboy_core.hpp"
#include "gbb/log.hpp"
#include "gbb/scene_json.hpp"
#include "gameboy/emulator.hpp"
#include "gbb/touch_control.hpp"
#include "gbb/voxel_profile.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <stdexcept>
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

std::vector<std::uint8_t> test_rom(const std::vector<std::uint8_t>& program = {}) {
    std::vector<std::uint8_t> rom(0x8000, 0);
    constexpr std::string_view title = "FRONTEND TEST";
    std::copy(title.begin(), title.end(), rom.begin() + 0x134);
    std::copy(program.begin(), program.end(), rom.begin() + 0x100);
    return rom;
}

std::vector<std::uint8_t> cgb_test_rom() {
    auto rom = test_rom();
    rom[0x143] = 0x80;
    return rom;
}

void test_core_registry_contract() {
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
    const auto probe_matches = registry.probe_matches(cgb_rom);
    check(probe_matches.size() == registry.factories().size() &&
              probe_matches.front().core_id == "gb" &&
              probe_matches.front().result.confidence > 0,
          "core probe diagnostics report each registered candidate");

    auto core = registry.create(std::move(dmg_rom));
    const auto& descriptor = core->descriptor();
    check(descriptor.core_id == "gb" &&
              descriptor.system == gbb::SystemId::game_boy &&
              descriptor.video_width == 160 && descriptor.video_height == 144 &&
              descriptor.nominal_cycles_per_frame == 70224 &&
              descriptor.audio_channels == 2 && descriptor.input_count == 8,
          "generic core descriptor carries media, timing, system, and input data");
    check(descriptor.software_title == "FRONTEND TEST" &&
              descriptor.rom_size == 0x8000 && descriptor.save_ram_size == 0 &&
              !descriptor.has_battery && !descriptor.supports_color &&
              !descriptor.requires_color,
          "generic core descriptor carries software and cartridge metadata");
    check(core->program_counter().has_value(),
          "optional debugger metadata crosses the generic core boundary");
    check(gbb::has_capability(descriptor.capabilities,
                              gbb::CoreCapability::debugger) &&
              gbb::has_capability(descriptor.capabilities,
                                  gbb::CoreCapability::link_cable) &&
              gbb::has_capability(descriptor.capabilities,
                                  gbb::CoreCapability::printer) &&
              gbb::gameboy_emulator(core.get()) != nullptr,
          "optional GB development tools are capability-gated behind the adapter");
    check(core->take_printer_pages().empty(),
          "printer output is exposed through the generic core contract");
    const auto state_before_printer_toggle = core->save_state();
    core->set_printer_enabled(true);
    core->set_printer_enabled(false);
    check(core->save_state() == state_before_printer_toggle,
          "printer endpoint can be configured through the generic core contract");

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

    bool null_factory_rejected = false;
    try {
        static_cast<void>(extensible.create(std::vector<std::uint8_t>(4)));
    } catch (const std::runtime_error&) {
        null_factory_rejected = true;
    }
    check(null_factory_rejected,
          "registry rejects factories that fail to construct a core");

    gbb::CoreRegistry mismatched;
    mismatched.register_factory({
        "mislabelled", "Mislabelled core",
        [](const std::vector<std::uint8_t>& bytes,
           const gbb::CoreLoadOptions&) noexcept {
            return gbb::CoreProbeResult{bytes.size() >= 0x150 ? 100 : 0,
                                        gbb::SystemId::game_boy};
        },
        [](std::vector<std::uint8_t> rom, const gbb::CoreLoadOptions& options)
            -> std::unique_ptr<gbb::EmulatorCore> {
            return gbb::create_core(std::move(rom), options);
        }});
    bool descriptor_mismatch_rejected = false;
    try {
        static_cast<void>(mismatched.create(test_rom()));
    } catch (const std::runtime_error& error) {
        descriptor_mismatch_rejected =
            std::string_view(error.what()).find("does not match factory") !=
            std::string_view::npos;
    }
    check(descriptor_mismatch_rejected,
          "registry rejects cores whose descriptor identity disagrees with the factory");

    bool invalid_factory_rejected = false;
    try {
        extensible.register_factory({"", "", nullptr, nullptr});
    } catch (const std::invalid_argument&) {
        invalid_factory_rejected = true;
    }
    check(invalid_factory_rejected,
          "registry rejects factories without an id, name, probe, or creator");

    bool unnamed_factory_rejected = false;
    try {
        extensible.register_factory({
            "unnamed", "", nullptr,
            [](std::vector<std::uint8_t>, const gbb::CoreLoadOptions&)
                -> std::unique_ptr<gbb::EmulatorCore> { return {}; }});
    } catch (const std::invalid_argument&) {
        unnamed_factory_rejected = true;
    }
    check(unnamed_factory_rejected,
          "registry rejects factories without display metadata");

    bool duplicate_factory_rejected = false;
    try {
        extensible.register_factory({
            "test-gba", "Duplicate test GBA",
            [](const std::vector<std::uint8_t>&,
               const gbb::CoreLoadOptions&) noexcept {
                return gbb::CoreProbeResult{};
            },
            [](std::vector<std::uint8_t>, const gbb::CoreLoadOptions&)
                -> std::unique_ptr<gbb::EmulatorCore> { return {}; }});
    } catch (const std::invalid_argument&) {
        duplicate_factory_rejected = true;
    }
    check(duplicate_factory_rejected,
          "registry rejects duplicate core identifiers deterministically");

    bool invalid_constructor_rejected = false;
    try {
        gbb::CoreRegistry invalid({gbb::CoreFactory{"", "", nullptr,
                                                    nullptr}});
        static_cast<void>(invalid);
    } catch (const std::invalid_argument&) {
        invalid_constructor_rejected = true;
    }
    check(invalid_constructor_rejected,
          "registry constructor applies the same factory validation guardrail");

    gbb::CoreRegistry tied;
    tied.register_factory({
        "tie-a", "Tie A",
        [](const std::vector<std::uint8_t>&,
           const gbb::CoreLoadOptions&) noexcept {
            return gbb::CoreProbeResult{25, gbb::SystemId::game_boy};
        },
        [](std::vector<std::uint8_t>, const gbb::CoreLoadOptions&)
            -> std::unique_ptr<gbb::EmulatorCore> { return {}; }});
    tied.register_factory({
        "tie-b", "Tie B",
        [](const std::vector<std::uint8_t>&,
           const gbb::CoreLoadOptions&) noexcept {
            return gbb::CoreProbeResult{25, gbb::SystemId::game_boy_color};
        },
        [](std::vector<std::uint8_t>, const gbb::CoreLoadOptions&)
            -> std::unique_ptr<gbb::EmulatorCore> { return {}; }});
    auto& logger = gbb::Logger::instance();
    logger.set_level(gbb::LogLevel::warning);
    logger.set_memory_capacity(8);
    try {
        static_cast<void>(tied.create(test_rom()));
    } catch (const std::runtime_error&) {
        // Both factories deliberately return null; the tie diagnostic is the
        // contract being checked before creation fails.
    }
    const auto records = logger.recent_records();
    check(std::any_of(records.begin(), records.end(), [](const std::string& record) {
              return record.find("probe tie core=tie-b") != std::string::npos;
          }),
          "core registry reports equal-confidence probe selection ties");
    logger.set_memory_capacity(0);
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
    bus.write8(0xFF40, 0x93);
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
          "scene snapshot reports disabled window state without frontend special cases");
    check(scene.schema_version == 1 && scene.producer_id == "gameboy" &&
              scene.layers.empty(),
          "Game Boy scene keeps the versioned optional-layer extension point");
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
              json.find("\"sprites\":[") != std::string::npos &&
              json.find("\"producer\":\"gameboy\"") != std::string::npos &&
              json.find("\"layers\":[]") != std::string::npos,
          "scene JSON includes legacy data and optional core-defined layers");
    auto extended = scene;
    extended.layers.push_back({"gba.depth", "example.core.layer.v1", 2, 1,
                                {0x10, 0x20}});
    const auto extended_json = gbb::scene_snapshot_to_json(extended);
    check(extended_json.find("\"id\":\"gba.depth\"") != std::string::npos &&
              extended_json.find("\"format\":\"example.core.layer.v1\"") !=
                  std::string::npos &&
              extended_json.find("\"payload\":[16,32]") != std::string::npos,
          "scene JSON preserves opaque core-defined layer payloads");

    const auto path = std::filesystem::temp_directory_path() /
                      "gbb-scene-snapshot-test.json";
    check(gbb::write_scene_snapshot_json(scene, path),
          "scene snapshots can be exported to a file");
    std::ifstream input(path, std::ios::binary);
    const std::string saved((std::istreambuf_iterator<char>(input)),
                            std::istreambuf_iterator<char>());
    check(saved == json, "exported scene JSON matches the in-memory document");
    input.close();
    std::filesystem::remove(path);
}

void test_touch_controls() {
    const std::optional<std::size_t> dpad_right{0};
    const std::optional<std::size_t> button_a{4};
    check(gbb::retain_touch_control(dpad_right, std::nullopt) == dpad_right,
          "touch ownership survives motion through neutral space");
    check(gbb::retain_touch_control(dpad_right, button_a) == button_a,
          "touch ownership transfers when another control is entered");
    check(!gbb::retain_touch_control(std::nullopt, std::nullopt).has_value(),
          "neutral touch remains unassigned until it enters a control");
}

void test_dashboard_navigation() {
    using gbb::desktop::DashboardAction;
    const auto without_resume =
        gbb::desktop::dashboard_navigation_items(false, 2);
    check(without_resume.size() == 7 &&
              without_resume.front().action == DashboardAction::open_rom &&
              without_resume.back().action == DashboardAction::quit,
          "dashboard exposes Open ROM and Exit around recent entries");
    check(without_resume[4].action == DashboardAction::recent_rom &&
              without_resume[4].recent_index == 0 &&
              without_resume[5].recent_index == 1,
          "dashboard recents preserve their source order");

    const auto with_resume = gbb::desktop::dashboard_navigation_items(true, 0);
    check(with_resume.size() == 6 &&
              with_resume.front().action == DashboardAction::resume,
          "dashboard puts Resume first when a game can resume");
    check(gbb::desktop::dashboard_move_selection(0, 6, -1) == 5 &&
              gbb::desktop::dashboard_move_selection(5, 6, 1) == 0 &&
              gbb::desktop::dashboard_move_selection(99, 6, -1) == 4,
          "keyboard dashboard selection wraps and clamps safely");
    check(gbb::desktop::dashboard_scroll_selection(0, 6, -1) == 0 &&
              gbb::desktop::dashboard_scroll_selection(5, 6, 1) == 5 &&
              gbb::desktop::dashboard_first_visible(5, 10, 5) == 1,
          "wheel dashboard navigation keeps the selected row visible");
    check(gbb::desktop::dashboard_move_selection(3, 0, 1) == 0 &&
              gbb::desktop::dashboard_first_visible(0, 0, 5) == 0,
          "dashboard navigation is safe before items are populated");
}

void test_voxel_profiles() {
    const auto path = std::filesystem::temp_directory_path() /
                      "gbb-voxel-profile-contract-test.ini";
    std::filesystem::remove(path);
    gbb::ensure_voxel_profile_file(path);
    const auto fingerprint = UINT64_C(0x1234ABCD);
    auto profile = gbb::load_voxel_profile(path, fingerprint);
    check(profile.depth_scale == 1.0F && profile.camera_pitch == 24.0F &&
              profile.zoom == 0.72F && profile.sprite_depth == 8.0F &&
              !profile.framebuffer_facade,
          "voxel profile defaults are loaded");
    const auto super_mario_land =
        gbb::load_voxel_profile(path, UINT64_C(0x7eafc0023b31d850));
    check(super_mario_land.depth_scale == 1.25F &&
              super_mario_land.zoom == 0.74F &&
              super_mario_land.perspective == 0.0012F &&
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
              loaded.sprite_depth_near == 22.0F && !loaded.framebuffer_facade,
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

void test_audio_helpers() {
    const std::vector<std::int16_t> stereo{
        100, -100, 300, -300, 500, -500, 700, -700};
    check(gbb::downsample_audio_box(stereo, 2, 2) ==
              std::vector<std::int16_t>({200, -200, 600, -600}),
          "fast-forward audio uses a stereo-preserving box filter");
    check(gbb::audio_queue_bytes(48000, 2, 200) == 38400,
          "audio queue limits convert milliseconds to interleaved PCM bytes");
}

} // namespace

int main() {
    test_core_registry_contract();
    test_scene_snapshot_contract();
    test_scene_snapshot_json();
    test_touch_controls();
    test_dashboard_navigation();
    test_voxel_profiles();
    test_audio_helpers();
    return failures == 0 ? 0 : 1;
}
