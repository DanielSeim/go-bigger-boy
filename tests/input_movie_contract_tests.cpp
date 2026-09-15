#include "input_movie.hpp"

#include "gameboy/cartridge.hpp"
#include "gameboy/emulator.hpp"
#include "gbb/core_runtime.hpp"

#include <cstdint>
#include <filesystem>
#include <iostream>
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
    constexpr char title[] = "MOVIE TEST";
    for (std::size_t index = 0; title[index] != '\0'; ++index) {
        rom[0x134 + index] = static_cast<std::uint8_t>(title[index]);
    }
    rom[0x147] = 0x00;
    rom[0x148] = 0x00;
    rom[0x149] = 0x00;
    rom[0x0100] = 0x00; // NOP
    return rom;
}

void test_record_and_replay() {
    const auto path = std::filesystem::temp_directory_path() /
                      "gbb-input-movie-contract" / "recording.gbbm";
    std::filesystem::remove_all(path.parent_path());

    gameboy::Emulator recorder{gameboy::Cartridge{test_rom()}};
    gbb::sdl::InputMovie movie;
    movie.start_recording(recorder);
    movie.set_button(recorder, gameboy::Button::a, true);
    static_cast<void>(recorder.step());
    movie.set_button(recorder, gameboy::Button::a, false);
    movie.stop_and_save(path, recorder);
    check(movie.mode() == gbb::sdl::InputMovie::Mode::idle,
          "recording stops after saving");
    check(movie.event_count() >= 2 && std::filesystem::is_regular_file(path),
          "recording writes button transitions to a movie file");

    gameboy::Emulator replay{gameboy::Cartridge{test_rom()}};
    gbb::sdl::InputMovie loaded;
    loaded.start_replay(replay, path);
    check(loaded.replaying() && loaded.event_count() == movie.event_count(),
          "replay validates the ROM and restores the recorded event stream");
    replay.bus().write8(0xFF00, 0x10); // Select the action-button lines.
    check(!loaded.update_replay(replay) &&
              (replay.bus().read8(0xFF00) & 0x01U) == 0,
          "replay injects the first recorded button transition");
    static_cast<void>(replay.step());
    check(loaded.update_replay(replay) &&
              (replay.bus().read8(0xFF00) & 0x01U) != 0,
          "replay injects the release transition and finishes");
    loaded.stop(&replay);
    check(loaded.mode() == gbb::sdl::InputMovie::Mode::idle,
          "replay can be stopped cleanly");

    std::filesystem::remove_all(path.parent_path());
}

void test_tas_keeps_overlapping_buttons_held_independently() {
    const auto path = std::filesystem::temp_directory_path() /
                      "gbb-input-movie-tas-overlap.gbbm";
    std::filesystem::remove(path);

    gameboy::Emulator recorder{gameboy::Cartridge{test_rom()}};
    const auto start_state = recorder.save_state();
    gbb::sdl::InputMovie movie;
    const std::vector<std::uint8_t> frames{
        1U << 5,       // B
        1U << 5,       // B remains held
        (1U << 5) | (1U << 4), // A joins B
        (1U << 5) | (1U << 4), // A and B remain held
        1U << 5,       // A releases while B remains held
        0,             // B releases
    };
    movie.save_frame_inputs(recorder, path, recorder.rom_fingerprint(),
                            start_state, frames);

    gameboy::Emulator replay{gameboy::Cartridge{test_rom()}};
    gbb::sdl::InputMovie loaded;
    loaded.start_replay(replay, path);
    replay.bus().write8(0xFF00, 0x10); // Select the action-button lines.

    bool saw_b_only = false;
    bool saw_a_and_b = false;
    bool saw_b_after_a_release = false;
    for (unsigned frame = 0; frame < 10 && loaded.replaying(); ++frame) {
        static_cast<void>(loaded.update_replay(replay));
        const auto actions = static_cast<std::uint8_t>(
            replay.bus().read8(0xFF00) & 0x03U);
        saw_b_only = saw_b_only || actions == 0x01U;
        saw_a_and_b = saw_a_and_b || actions == 0x00U;
        if (saw_a_and_b && actions == 0x01U) saw_b_after_a_release = true;
        const auto advanced = gbb::advance_to_frame(replay, 280896U);
        if (advanced.frame_ready) replay.consume_frame();
    }

    check(saw_b_only, "TAS replay holds B before A joins");
    check(saw_a_and_b, "TAS replay holds A and B simultaneously");
    check(saw_b_after_a_release,
          "TAS replay releases A without releasing the still-held B button");
    check(loaded.mode() == gbb::sdl::InputMovie::Mode::idle,
          "TAS replay finishes after the final independent release");
    std::filesystem::remove(path);
}

void test_rejects_wrong_rom() {
    const auto path = std::filesystem::temp_directory_path() /
                      "gbb-input-movie-contract-wrong.gbbm";
    std::filesystem::remove(path);
    gameboy::Emulator recorder{gameboy::Cartridge{test_rom()}};
    gbb::sdl::InputMovie movie;
    movie.start_recording(recorder);
    movie.stop_and_save(path, recorder);

    auto different_rom = test_rom();
    different_rom[0x134] = 'X';
    gameboy::Emulator different{gameboy::Cartridge{std::move(different_rom)}};
    bool rejected = false;
    try {
        gbb::sdl::InputMovie wrong;
        wrong.start_replay(different, path);
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    check(rejected, "replay rejects a recording from a different ROM");
    std::filesystem::remove(path);
}

} // namespace

int main() {
    test_record_and_replay();
    test_tas_keeps_overlapping_buttons_held_independently();
    test_rejects_wrong_rom();
    return failures == 0 ? 0 : 1;
}
