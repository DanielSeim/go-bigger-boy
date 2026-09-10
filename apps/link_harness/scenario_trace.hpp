#pragma once

#include "gameboy/emulator.hpp"
#include "gameboy/tcp_serial_endpoint.hpp"
#include "scenario_state.hpp"
#include "scenario_trace_writer.hpp"

#include <cstdint>
#include <filesystem>
#include <iosfwd>

namespace gbb::link_harness {

struct SerialOwnershipSnapshot {
    bool active{};
    bool internal{};
    std::uint8_t bits{};
    std::uint8_t control{};

    bool operator==(const SerialOwnershipSnapshot& other) const noexcept {
        return active == other.active && internal == other.internal &&
               bits == other.bits && control == other.control;
    }
    bool operator!=(const SerialOwnershipSnapshot& other) const noexcept {
        return !(*this == other);
    }
};

struct SerialProgressWatchdog {
    std::uint64_t last_marker{};
    std::uint64_t stalled_frames{};
    std::uint64_t stall_frame{};
    std::uint64_t ownership_transitions{};
    SerialOwnershipSnapshot first_previous{};
    SerialOwnershipSnapshot second_previous{};
    bool initialized{};
    bool stall_reported{};
};

void append_trace_player(std::ostream& output,
                         gameboy::Emulator& emulator,
                         unsigned player,
                         const gameboy::TcpSerialEndpoint* endpoint);

class ScenarioTrace {
  public:
    ScenarioTrace(const std::filesystem::path& path,
                  const std::string& transport,
                  Scenario scenario);

    void write_frame(std::uint64_t frame,
                     gameboy::Emulator& first,
                     gameboy::Emulator& second,
                     const AutoInputState& input_state,
                     const gameboy::TcpSerialEndpoint* first_endpoint = nullptr,
                     const gameboy::TcpSerialEndpoint* second_endpoint = nullptr,
                     const char* session_state = nullptr,
                     std::uint64_t session_transfers = 0);

    void write_serial_event(std::uint64_t frame,
                            const char* event,
                            gameboy::Emulator& first,
                            gameboy::Emulator& second,
                            const char* session_state,
                            std::uint64_t session_transfers,
                            std::uint64_t stalled_frames = 0);

    void write_trade_phase_event(std::uint64_t frame,
                                 gameboy::Emulator& first,
                                 gameboy::Emulator& second,
                                 const AutoInputState& input_state,
                                 const char* session_state,
                                 std::uint64_t session_transfers);

    void write_battle_checkpoint(std::uint64_t frame,
                                 gameboy::Emulator& first,
                                 gameboy::Emulator& second);

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return writer_.path();
    }

  private:
    ScenarioTraceWriter writer_;
    std::uint32_t last_trade_phase_mask_{};
    bool trade_phase_initialized_{};
    bool first_battle_seen_{};
    bool second_battle_seen_{};
    bool battle_divergence_reported_{};
    std::uint64_t last_battle_checkpoint_frame_{};
};

void update_serial_progress_watchdog(
    ScenarioTrace& trace,
    std::uint64_t frame,
    gameboy::Emulator& first,
    gameboy::Emulator& second,
    SerialProgressWatchdog& watchdog,
    const char* session_state,
    std::uint64_t session_transfers,
    bool watch_trade_stall);

} // namespace gbb::link_harness
