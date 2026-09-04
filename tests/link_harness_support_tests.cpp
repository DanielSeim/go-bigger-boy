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

    std::filesystem::remove_all(root);
    return failures == 0 ? 0 : 1;
}
