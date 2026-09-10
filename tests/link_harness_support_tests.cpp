#include "harness_io.hpp"
#include "pokemon_state.hpp"
#include "semantic_tracker.hpp"
#include "scenario_runner.hpp"
#include "scenario_state.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>
#include <algorithm>
#include <string_view>

namespace {

int failures = 0;

void check(const bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

} // namespace

int main() {
    using namespace gbb::link_harness;

    check(fingerprint({}) == UINT64_C(1469598103934665603),
          "harness hashing keeps the FNV-1a empty value");
    check(hex(0x2AU) == "0x000000000000002a",
          "harness hexadecimal formatting is stable");
    check(scenario_name(Scenario::trade) == std::string{"trade"},
          "scenario names are centralized");
    check(plausible_link_state(0x04) && plausible_link_state(link_state_trading),
          "Pokémon link-state probe accepts known guest states");
    check(!plausible_link_state(0xFF) &&
              effective_link_state(0xFF, link_state_trading) == link_state_trading,
          "Pokémon link-state probe falls back to localized state");

    AutoInputState input;
    input.first_trade_selected = true;
    input.second_trade_right_sent = true;
    input.second_trade_confirmed = true;
    check(trade_input_phase_mask(input) == ((1U << 0) | (1U << 3) | (1U << 7)),
          "trade automation state produces the expected phase mask");

    PartySnapshot first_before;
    first_before.valid = true;
    first_before.count = 1;
    first_before.species[0] = 0x01;
    first_before.ot_ids[0] = 0x1001;
    first_before.signatures[0] = 0xA1;
    PartySnapshot second_before;
    second_before.valid = true;
    second_before.count = 1;
    second_before.species[0] = 0x02;
    second_before.ot_ids[0] = 0x1002;
    second_before.signatures[0] = 0xB2;
    SemanticTracker tracker(first_before, second_before);
    SemanticSample semantic_sample;
    semantic_sample.first_party = second_before;
    semantic_sample.second_party = first_before;
    semantic_sample.first_link_state = 0x04;
    semantic_sample.second_link_state = 0x04;
    tracker.sample(semantic_sample);
    check(tracker.trade_observed(),
          "semantic tracker recognizes exchanged party records");
    check(tracker.battle_observed(),
          "semantic tracker recognizes both battle states");

    SemanticTracker gen2_tracker(first_before, second_before);
    SemanticSample gen2_sample;
    gen2_sample.first_battle_active = true;
    gen2_sample.second_battle_active = true;
    gen2_sample.first_battle_link_mode = g2_link_mode_colosseum;
    gen2_sample.second_battle_link_mode = g2_link_mode_colosseum;
    gen2_sample.first_battle_just_started = 1;
    gen2_sample.second_battle_just_started = 1;
    gen2_tracker.sample(gen2_sample);
    check(gen2_tracker.battle_observed(),
          "semantic tracker recognizes Gen II Colosseum markers");
    SemanticTracker gen2_asymmetric(first_before, second_before);
    gen2_sample.second_battle_active = false;
    gen2_asymmetric.sample(gen2_sample);
    check(!gen2_asymmetric.battle_observed() &&
              std::string{semantic_failure(gen2_asymmetric,
                                           Expectation::battle)} ==
                  "player2_did_not_enter_battle",
          "semantic tracker reports an asymmetric Gen II battle entry");

    std::vector<std::uint8_t> gen2_rom(0x8000, 0);
    constexpr std::string_view gen2_title = "POKEMON G";
    std::copy(gen2_title.begin(), gen2_title.end(), gen2_rom.begin() + 0x134);
    gen2_rom[0x143] = 0x80; // CGB-capable cartridge.
    gen2_rom[0x14A] = 1;    // Western destination code.
    gameboy::Emulator gen2_emulator{gameboy::Cartridge{gen2_rom}};
    gen2_emulator.bus().write8(0xFF70, 1);
    gen2_emulator.bus().write8(g2_w_party_count, 1);
    gen2_emulator.bus().write8(g2_w_party_species, 0x19);
    gen2_emulator.bus().write8(g2_w_party_species + 1, 0xFF);
    gen2_emulator.bus().write8(g2_w_party_mon1, 0x19);
    gen2_emulator.bus().write8(g2_w_party_mon1 + 5, 0x12);
    gen2_emulator.bus().write8(g2_w_party_mon1 + 6, 0x34);
    const auto gen2_party = read_party(gen2_emulator);
    check(gen2_party.valid && gen2_party.count == 1 &&
              gen2_party.species[0] == 0x19 &&
              gen2_party.ot_ids[0] == 0x1234,
          "party probe reads the Gen II party layout and trainer ID");
    gen2_emulator.bus().write8(g2_w_link_mode, g2_link_mode_colosseum);
    gen2_emulator.bus().write8(g2_w_battle_just_started, 1);
    gen2_emulator.bus().write8(g2_w_cur_battle_mon, 2);
    gen2_emulator.bus().write8(g2_w_battle_mode, 1);
    gen2_emulator.bus().write8(g2_w_battle_type, 2);
    gen2_emulator.bus().write8(g2_w_battle_mon_hp, 0);
    gen2_emulator.bus().write8(g2_w_battle_mon_hp + 1, 100);
    const auto gen2_battle = probe_battle(gen2_emulator);
    check(gen2_battle.active && gen2_battle.link_mode == g2_link_mode_colosseum &&
              gen2_battle.current_mon == 2 && gen2_battle.battle_mon_hp == 100,
          "battle probe recognizes the Gen II Colosseum marker");

    AutoInputState runner_input;
    unsigned callback_count = 0;
    const auto run_result = run_scenario(
        10, Scenario::trade, runner_input,
        [&](const std::uint64_t, AutoInputState&) { ++callback_count; },
        [&] { return callback_count == 3; });
    check(run_result.frames_run == 3 && callback_count == 3,
          "scenario runner stops at the first satisfied expectation");

    const auto root = std::filesystem::temp_directory_path() /
                      "gbb-link-harness-support-test";
    const auto report_path = root / "nested" / "report.txt";
    const auto frame_path = root / "nested" / "frame.ppm";
    write_report(report_path, "marker=ok\n");
    std::ifstream report(report_path);
    const std::string report_contents((std::istreambuf_iterator<char>(report)),
                                      std::istreambuf_iterator<char>());
    check(report_contents == "marker=ok\n",
          "harness reports create missing parent directories");

    gameboy::Ppu::Framebuffer framebuffer{};
    write_frame(frame_path, framebuffer);
    std::ifstream frame(frame_path, std::ios::binary);
    const std::vector<char> header{std::istreambuf_iterator<char>{frame},
                                   std::istreambuf_iterator<char>{}};
    check(header.size() >= 3 && header[0] == 'P' && header[1] == '6' &&
              header[2] == '\n',
          "harness frame capture writes a PPM header");

    std::error_code cleanup_error;
    std::filesystem::remove_all(root, cleanup_error);
    return failures == 0 ? 0 : 1;
}
