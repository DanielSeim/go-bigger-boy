#include "gameboy/emulator.hpp"
#include "gameboy/sgb_trace.hpp"

#include <array>
#include <iostream>
#include <sstream>
#include <string>
#include <utility>
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
    const char title[] = "SGB TRACE";
    for (std::size_t index = 0; title[index] != '\0'; ++index) {
        rom[0x134 + index] = static_cast<std::uint8_t>(title[index]);
    }
    rom[0x146] = 0x03;
    rom[0x147] = 0x00;
    rom[0x148] = 0x00;
    rom[0x149] = 0x00;
    return rom;
}

void send_command(gameboy::Emulator& emulator, gameboy::SgbTrace::Recorder& recorder,
                  std::uint64_t& cycle,
                  const std::array<std::uint8_t, gameboy::Joypad::sgb_packet_size>&
                      command) {
    const auto write = [&](const std::uint8_t value) {
        emulator.bus().write8(0xFF00, value);
        check(recorder.record_joypad_write(cycle, value),
              "SGB trace accepts JOYP writes");
        cycle += 4;
    };
    write(0x00);
    write(0x30);
    for (std::size_t bit = 0; bit < command.size() * 8; ++bit) {
        write(0x30);
        write((command[bit / 8] & (1U << (bit & 7U))) != 0 ? 0x10 : 0x20);
    }
    write(0x30);
    write(0x20);
}

void test_trace_round_trip_and_replay() {
    gameboy::Emulator source{gameboy::Cartridge{test_rom()}, gameboy::HardwareModel::sgb};
    gameboy::SgbTrace::Recorder recorder(source.rom_fingerprint(),
                                          gameboy::HardwareModel::sgb);
    std::uint64_t cycle{};
    check(recorder.checkpoint(0, 0, source), "SGB trace records the initial checkpoint");
    std::array<std::uint8_t, gameboy::Joypad::sgb_packet_size> command{};
    command[0] = static_cast<std::uint8_t>((0x11U << 3) | 1U);
    command[1] = 0x01;
    send_command(source, recorder, cycle, command);
    check(recorder.checkpoint(cycle, 1, source),
          "SGB trace records a decoded-command checkpoint");

    auto trace = std::move(recorder).finish();
    check(trace.writes.size() == 2 + command.size() * 8 * 2 + 2,
          "SGB trace stores every JOYP write");
    check(trace.checkpoints.size() == 2 && trace.checkpoints[1].commands.size() == 1 &&
              trace.checkpoints[1].commands[0].command == 0x11 &&
              trace.checkpoints[1].commands[0].packet_bytes == command.size(),
          "SGB trace stores decoded command packets");

    std::ostringstream serialized;
    std::string error;
    check(gameboy::SgbTrace::serialize(trace, serialized, &error),
          "SGB trace serializes successfully");
    const auto parsed = gameboy::SgbTrace::parse(serialized.str(), &error);
    check(parsed.has_value() && parsed->writes.size() == trace.writes.size() &&
              parsed->checkpoints.size() == trace.checkpoints.size(),
          "SGB trace parses back with its event counts");
    if (!parsed.has_value()) return;

    gameboy::Emulator replay{gameboy::Cartridge{test_rom()}, gameboy::HardwareModel::sgb};
    const auto result = gameboy::SgbTrace::replay(*parsed, replay);
    check(result.success && result.writes_applied == trace.writes.size() &&
              result.checkpoints_checked == trace.checkpoints.size(),
          "SGB trace replays to identical checkpoints");
}

void test_trace_detects_divergence_and_rejects_bad_input() {
    gameboy::Emulator source{gameboy::Cartridge{test_rom()}, gameboy::HardwareModel::sgb};
    gameboy::SgbTrace::Recorder recorder(source.rom_fingerprint(),
                                          gameboy::HardwareModel::sgb);
    check(recorder.checkpoint(0, 0, source), "SGB trace creates divergence fixture");
    auto trace = std::move(recorder).finish();
    trace.checkpoints[0].state_hash ^= 1;
    gameboy::Emulator replay{gameboy::Cartridge{test_rom()}, gameboy::HardwareModel::sgb};
    const auto result = gameboy::SgbTrace::replay(trace, replay);
    check(!result.success && result.error.find("state checkpoint") != std::string::npos,
          "SGB replay reports state divergence");

    std::string error;
    check(!gameboy::SgbTrace::parse("GBB SGB trace\ntrace_version=1 model=sgb "
                                    "rom_fingerprint=0x1\nwrite cycle=2 value=0x00\n"
                                    "write cycle=1 value=0x00\nend\n",
                                    &error),
          "SGB trace rejects non-monotonic writes");
    check(!error.empty(), "SGB trace reports malformed input details");
}

void test_trace_diff_reports_first_difference() {
    gameboy::SgbTrace::Trace left;
    left.rom_fingerprint = 7;
    left.writes.push_back({4, 0x30});
    left.checkpoints.push_back({0, 0, 1, 2, {}, {}});
    auto right = left;
    check(gameboy::SgbTrace::diff(left, right).equal,
          "identical SGB traces compare equal");
    right.writes[0].value = 0x20;
    const auto write_diff = gameboy::SgbTrace::diff(left, right);
    check(!write_diff.equal && write_diff.index == 0 &&
              write_diff.description.find("JOYP") != std::string::npos,
          "SGB trace diff identifies the first JOYP mismatch");
    right = left;
    right.checkpoints[0].state_hash = 3;
    const auto checkpoint_diff = gameboy::SgbTrace::diff(left, right);
    check(!checkpoint_diff.equal &&
              checkpoint_diff.description.find("state hash") != std::string::npos,
          "SGB trace diff identifies the first state mismatch");
}

void test_trace_diff_ignores_runner_boundary_checkpoints() {
    gameboy::SgbTrace::Trace desktop;
    desktop.rom_fingerprint = 7;
    desktop.writes.push_back({4, 0x30});
    desktop.checkpoints.push_back({0, 0, 1, 2, {}, {}});
    desktop.checkpoints.push_back({10, 1, 3, 4, {}, {}});
    desktop.checkpoints.push_back({20, 0, 5, 6, {}, {}});

    gameboy::SgbTrace::Trace android = desktop;
    android.checkpoints = {desktop.checkpoints[1]};

    check(gameboy::SgbTrace::diff(desktop, android).equal,
          "SGB trace diff ignores desktop runner boundary checkpoints");
}

} // namespace

int main() {
    test_trace_round_trip_and_replay();
    test_trace_detects_divergence_and_rejects_bad_input();
    test_trace_diff_reports_first_difference();
    test_trace_diff_ignores_runner_boundary_checkpoints();
    return failures == 0 ? 0 : 1;
}
