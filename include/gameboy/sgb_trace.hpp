#pragma once

#include "gameboy/hardware_model.hpp"
#include "gameboy/sgb_adapter.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace gameboy {

class Emulator;
class MemoryBus;

// A deterministic, host-independent trace of the SGB-facing JOYP protocol.
// Traces contain no ROM data and are intended for debugging a user-supplied
// cartridge against a known emulator revision.
class SgbTrace final {
public:
    static constexpr std::uint32_t format_version = 1;
    static constexpr std::size_t max_writes = 1'000'000;
    static constexpr std::size_t max_checkpoints = 100'000;
    static constexpr std::size_t max_commands_per_checkpoint = 32;

    struct JoypadWrite {
        std::uint64_t cycle{};
        std::uint8_t value{};
    };

    struct Command {
        std::uint64_t sequence{};
        std::uint8_t command{};
        std::uint8_t packet_bytes{};
        std::array<std::uint8_t, Joypad::sgb_packet_size * Joypad::sgb_max_packets>
            packet{};
    };

    struct Checkpoint {
        std::uint64_t cycle{};
        std::uint64_t frame{};
        std::uint64_t framebuffer_hash{};
        std::uint64_t state_hash{};
        SgbAdapter::Diagnostics diagnostics{};
        std::vector<Command> commands;
    };

    struct Trace {
        std::uint32_t version{format_version};
        HardwareModel model{HardwareModel::sgb};
        std::uint64_t rom_fingerprint{};
        std::vector<JoypadWrite> writes;
        std::vector<Checkpoint> checkpoints;
    };

    struct ReplayResult {
        bool success{};
        std::size_t writes_applied{};
        std::size_t checkpoints_checked{};
        std::string error;
    };

    class Recorder final {
    public:
        Recorder(std::uint64_t rom_fingerprint, HardwareModel model) noexcept;

        bool record_joypad_write(std::uint64_t cycle,
                                 std::uint8_t value);
        bool checkpoint(std::uint64_t cycle, std::uint64_t frame,
                        const Emulator& emulator);
        [[nodiscard]] Trace finish() &&;

    private:
        Trace trace_;
        std::uint64_t last_command_sequence_{};
    };

    [[nodiscard]] static std::uint64_t framebuffer_hash(
        const Ppu::SgbFramebuffer& framebuffer) noexcept;
    [[nodiscard]] static std::uint64_t state_hash(const MemoryBus& bus) noexcept;

    [[nodiscard]] static bool serialize(const Trace& trace, std::ostream& output,
                                        std::string* error = nullptr);
    [[nodiscard]] static std::optional<Trace> parse(
        std::string_view text, std::string* error = nullptr);
    [[nodiscard]] static ReplayResult replay(const Trace& trace,
                                             Emulator& emulator);
};

} // namespace gameboy
