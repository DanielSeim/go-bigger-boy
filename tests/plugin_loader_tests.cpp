#include "gbb/core_contract.hpp"
#include "gbb/plugin_loader.hpp"

#include <cstdint>
#include <iostream>
#include <memory>
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

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: gameboy_plugin_loader_tests FIXTURE_LIBRARY "
                     "[OVERSIZED_FIXTURE]\n";
        return 2;
    }

    std::string error;
    const auto missing = gbb::PluginLoader::load(
        "this-plugin-does-not-exist", error);
    check(missing == nullptr && !error.empty(),
          "missing plugin paths fail with a diagnostic");

    auto loader = gbb::PluginLoader::load(argv[1], error);
    check(loader != nullptr && error.empty(),
          "fixture loads through the production loader");
    if (loader == nullptr) return 1;
    check(std::string(loader->descriptor().core_id) == "fixture",
          "loader exposes the negotiated descriptor");
    for (int index = 2; index < argc; ++index) {
        std::string oversized_error;
        const auto oversized = gbb::PluginLoader::load(argv[index],
                                                       oversized_error);
        check(oversized != nullptr && oversized_error.empty(),
              "loader accepts append-only oversized ABI tables");
    }
    try {
        static_cast<void>(loader->create({}));
        check(false, "failed plugin creation raises an error");
    } catch (const std::exception&) {
        check(true, "failed plugin creation raises an error");
    }

    const std::vector<std::uint8_t> rom{1, 2, 3, 4};
    gbb::CoreLoadOptions options;
    options.source_path = "fixture.gb";
    auto core = loader->create(rom, options);
    check(core != nullptr, "loader creates an EmulatorCore adapter");
    if (core == nullptr) return 1;

    check(gbb::validate_core_contract(*core, error) && error.empty(),
          "plugin adapter satisfies the common core contract");
    check(core->descriptor().core_id == "fixture" &&
              core->descriptor().system == gbb::SystemId::game_boy,
          "adapter maps descriptor identity and system");
    check(core->rom_fingerprint() != 0, "adapter forwards ROM fingerprints");

    check(core->step_instruction() == 4, "adapter forwards stepping");
    check(core->frame_ready(), "adapter forwards frame readiness");
    const auto frame = core->video_frame();
    check(frame.pixels != nullptr && frame.width == 2 && frame.height == 2 &&
              frame.pixel_count == 4 && frame.pixels[0] == 0x11111111,
          "adapter translates the plugin video frame");
    const auto audio = core->take_audio_samples();
    check(audio.size() == 2 && audio[0] == 0,
          "adapter translates plugin audio samples");

    core->set_input(gbb::InputId::a, true);
    const auto pressed_audio = core->take_audio_samples();
    check(pressed_audio.size() == 2 && pressed_audio[1] == 1,
          "adapter maps stable input IDs");

    const auto state = core->save_state();
    check(!state.empty(), "adapter copies plugin-owned save state");
    core->reset();
    core->load_state(state);
    const auto restored_audio = core->take_audio_samples();
    check(restored_audio.size() == 2 && restored_audio[1] == 1,
          "adapter round-trips save state");
    check(!core->has_persistent_data(gbb::PersistentDataKind::battery_ram),
          "adapter reports unsupported persistent data accurately");
    core->flush_persistent_data();

    const std::weak_ptr<const gbb::PluginLoader> weak_loader = loader;
    core.reset();
    loader.reset();
    check(weak_loader.expired(), "plugin library unload waits for core teardown");

    return failures == 0 ? 0 : 1;
}
