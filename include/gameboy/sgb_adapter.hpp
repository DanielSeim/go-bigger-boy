#pragma once

#include "gameboy/joypad.hpp"
#include "gameboy/ppu.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace gameboy {

class SaveStateBusCodec;

// Clean-room SGB host boundary.
//
// The adapter models the observable ICD2/joypad side of an SGB without
// embedding Nintendo firmware.  Keeping packet framing, controller polling,
// command dispatch, and diagnostics here leaves the Game Boy bus and PPU
// independent from the current HLE implementation and gives a future
// firmware-backed or SNES-host implementation a stable replacement seam.
class SgbAdapter final {
public:
    using Packet = std::array<std::uint8_t,
                              Joypad::sgb_packet_size * Joypad::sgb_max_packets>;

    static constexpr std::size_t command_history_capacity = 32;

    struct CommandRecord {
        std::uint64_t sequence{};
        std::uint8_t command{};
        std::uint8_t packet_bytes{};
        Packet packet{};
    };

    struct Diagnostics {
        bool enabled{};
        std::uint64_t packets_completed{};
        std::uint64_t commands_applied{};
        std::uint64_t malformed_packets{};
        std::uint8_t last_command{};
        std::uint8_t last_packet_bytes{};
        std::uint8_t player_count{1};
        std::uint8_t current_player{};
    };

    void set_enabled(bool enabled) noexcept;
    [[nodiscard]] bool enabled() const noexcept { return enabled_; }

    [[nodiscard]] std::uint8_t read_joypad(const Joypad& joypad) const noexcept;
    [[nodiscard]] bool write_joypad(std::uint8_t value, Joypad& joypad,
                                    Ppu& ppu) noexcept;
    void apply_command(const Packet& packet, std::size_t size,
                       Ppu& ppu) noexcept;

    [[nodiscard]] const Diagnostics& diagnostics() const noexcept {
        return diagnostics_;
    }
    [[nodiscard]] const std::array<CommandRecord, command_history_capacity>&
    command_history() const noexcept {
        return command_history_;
    }
    [[nodiscard]] std::size_t command_history_size() const noexcept {
        return command_history_size_;
    }
    [[nodiscard]] std::size_t command_history_oldest() const noexcept {
        return command_history_size_ == command_history_capacity
                   ? command_history_next_
                   : 0;
    }
    void reset_diagnostics() noexcept;

private:
    friend class SaveStateBusCodec;

    [[nodiscard]] std::size_t command_bits() const noexcept;
    void process_write(std::uint8_t value) noexcept;
    void reset_packet() noexcept;

    bool enabled_{};
    bool ready_for_pulse_{};
    bool ready_for_write_{};
    bool ready_for_stop_{};
    std::size_t bit_count_{};
    Packet command_{};
    Packet packet_{};
    bool packet_ready_{};
    std::size_t packet_bytes_{};
    std::uint8_t player_count_{1};
    std::uint8_t current_player_{};
    Diagnostics diagnostics_{};
    std::array<CommandRecord, command_history_capacity> command_history_{};
    std::size_t command_history_size_{};
    std::size_t command_history_next_{};
};

} // namespace gameboy
