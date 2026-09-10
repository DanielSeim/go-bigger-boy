#include "scenario_trace.hpp"
#include "pokemon_state.hpp"
#include "gbb/trace_format.hpp"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <iostream>
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
    const auto trace_path = std::filesystem::temp_directory_path() /
                            "gbb-link-harness-trace-writer-test.log";
    {
        gbb::link_harness::ScenarioTraceWriter trace(trace_path, "local",
                                                      "trade");
        check(trace.enabled(), "trace writer opens a requested path");
        gbb::write_trace_event_prefix(trace.stream(), "frame", trace.session(),
                                      0, trace.elapsed_ms(), trace.transport(),
                                      trace.role());
        trace.stream() << " marker=session-schema\n";
        trace.stream() << "frame=1 marker=before-checkpoint\n";
        trace.checkpoint_frame(1);
        trace.stream() << "frame=30 marker=checkpoint\n";
        trace.checkpoint_frame(30);
    }

    std::ifstream trace_input(trace_path);
    const std::string trace_contents((std::istreambuf_iterator<char>(trace_input)),
                                      std::istreambuf_iterator<char>());
    check(trace_contents.find("transport=local scenario=trade") !=
              std::string::npos,
          "trace writer emits transport and scenario metadata");
    check(trace_contents.find(
              "event=frame trace_version=1 session_id=1 frame=0 elapsed_ms=") !=
              std::string::npos,
          "harness trace events use the canonical event prefix");
    check(trace_contents.find(
              "transport=local role=harness marker=session-schema") !=
              std::string::npos,
          "harness trace event carries transport and role metadata");
    check(trace_contents.find(
              "session_start id=1 trace_version=1 counters_reset=1 transport=local role=harness scenario=trade") !=
              std::string::npos,
          "trace writer emits the shared session schema");
    check(trace_contents.find("session_end id=1 frames=30 elapsed_ms=") !=
              std::string::npos,
          "trace writer flushes and closes with a shared session record");
    check(trace_contents.find("trace_end frames=30") != std::string::npos,
          "trace writer records the last checkpointed frame");
    check(trace_contents.find("trace_end frames=30") ==
              trace_contents.rfind("trace_end frames=30"),
          "trace writer terminates the trace exactly once");

    const auto checkpoint_path = std::filesystem::temp_directory_path() /
                                 "gbb-link-harness-battle-checkpoint-test.log";
    std::vector<std::uint8_t> rom(0x8000, 0);
    constexpr std::string_view title = "POKEMON G";
    std::copy(title.begin(), title.end(), rom.begin() + 0x134);
    rom[0x143] = 0x80;
    rom[0x14A] = 1;
    gameboy::Emulator first{gameboy::Cartridge{rom}};
    gameboy::Emulator second{gameboy::Cartridge{rom}};
    first.bus().write8(gbb::link_harness::g2_w_link_mode,
                       gbb::link_harness::g2_link_mode_colosseum);
    gbb::link_harness::ScenarioTrace battle_trace(checkpoint_path, "tcp",
                                                   gbb::link_harness::Scenario::battle);
    gbb::link_harness::AutoInputState input;
    battle_trace.write_frame(1, first, second, input);
    battle_trace.write_frame(2, first, second, input);
    second.bus().write8(gbb::link_harness::g2_w_link_mode,
                        gbb::link_harness::g2_link_mode_colosseum);
    battle_trace.write_frame(3, first, second, input);
    std::ifstream checkpoint_input(checkpoint_path);
    const std::string checkpoint_contents(
        (std::istreambuf_iterator<char>(checkpoint_input)),
        std::istreambuf_iterator<char>());
    check(checkpoint_contents.find(
              "event=battle_checkpoint trace_version=1 session_id=") !=
              std::string::npos &&
              checkpoint_contents.find("phase=entry") != std::string::npos,
          "battle trace records Gen II entry checkpoints");
    check(checkpoint_contents.find("phase=first_divergence") !=
              std::string::npos &&
              checkpoint_contents.find("p1_link_mode=0x3") != std::string::npos,
          "battle trace records the first asymmetric entry with raw link mode");
    check(checkpoint_contents.find("p2_generation=2") != std::string::npos,
          "battle trace identifies the Gen II probe generation");
    std::error_code cleanup_error;
    std::filesystem::remove(trace_path, cleanup_error);
    std::filesystem::remove(checkpoint_path, cleanup_error);
    return failures == 0 ? 0 : 1;
}
