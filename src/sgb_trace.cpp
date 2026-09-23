#include "gameboy/sgb_trace.hpp"

#include "gameboy/emulator.hpp"
#include "gameboy/memory_bus.hpp"

#include <algorithm>
#include <charconv>
#include <iomanip>
#include <limits>
#include <ostream>
#include <sstream>
#include <utility>

namespace gameboy {
namespace {

constexpr std::string_view trace_header = "GBB SGB trace";
constexpr std::string_view trace_end = "end";
constexpr std::size_t max_trace_bytes = 16U * 1024U * 1024U;
constexpr std::size_t max_trace_line_bytes = 1U * 1024U * 1024U;

void set_error(std::string* error, const std::string_view message) {
    if (error != nullptr) *error = std::string{message};
}

std::optional<std::uint64_t> parse_integer(std::string_view value) noexcept {
    if (value.empty()) return std::nullopt;
    auto base = 10;
    if (value.size() > 2 && value[0] == '0' &&
        (value[1] == 'x' || value[1] == 'X')) {
        base = 16;
        value.remove_prefix(2);
    }
    if (value.empty()) return std::nullopt;
    std::uint64_t parsed{};
    const auto result = std::from_chars(value.data(), value.data() + value.size(),
                                        parsed, base);
    if (result.ec != std::errc{} || result.ptr != value.data() + value.size()) {
        return std::nullopt;
    }
    return parsed;
}

std::optional<std::string_view> field(std::string_view line,
                                      const std::string_view name) noexcept {
    const auto prefix = std::string{name} + '=';
    std::size_t offset{};
    while (offset < line.size()) {
        while (offset < line.size() && line[offset] == ' ') ++offset;
        const auto end = line.find(' ', offset);
        const auto token = line.substr(
            offset, end == std::string_view::npos ? line.size() - offset
                                                   : end - offset);
        if (token.size() >= prefix.size() && token.substr(0, prefix.size()) == prefix) {
            return token.substr(prefix.size());
        }
        if (end == std::string_view::npos) break;
        offset = end + 1;
    }
    return std::nullopt;
}

template <typename T>
bool required_integer(std::string_view line, const std::string_view name, T& out,
                      std::string* error) {
    const auto value = field(line, name);
    if (!value.has_value()) {
        set_error(error, "missing SGB trace field: " + std::string{name});
        return false;
    }
    const auto parsed = parse_integer(*value);
    if (!parsed.has_value() || *parsed > std::numeric_limits<T>::max()) {
        set_error(error, "invalid SGB trace field: " + std::string{name});
        return false;
    }
    out = static_cast<T>(*parsed);
    return true;
}

std::optional<HardwareModel> parse_model(const std::string_view value) noexcept {
    for (const auto model : selectable_hardware_models) {
        if (hardware_model_id(model) == value) return model;
    }
    return std::nullopt;
}

std::uint64_t fnv_append(std::uint64_t hash, const std::uint8_t value) noexcept {
    constexpr std::uint64_t prime = UINT64_C(1099511628211);
    return (hash ^ value) * prime;
}

template <typename T>
std::uint64_t fnv_append_integer(std::uint64_t hash, const T value) noexcept {
    for (std::size_t index = 0; index < sizeof(T); ++index) {
        hash = fnv_append(hash, static_cast<std::uint8_t>(value >> (index * 8)));
    }
    return hash;
}

bool parse_hex_packet(const std::string_view value, SgbTrace::Command& command,
                     std::string* error) {
    if (value.size() != static_cast<std::size_t>(command.packet_bytes) * 2U) {
        set_error(error, "SGB trace command packet length does not match bytes");
        return false;
    }
    for (std::size_t index = 0; index < command.packet_bytes; ++index) {
        unsigned parsed_byte{};
        const auto digits = value.substr(index * 2, 2);
        const auto parsed = std::from_chars(digits.data(), digits.data() + 2,
                                            parsed_byte, 16);
        if (parsed.ec != std::errc{} || parsed.ptr != digits.data() + 2) {
            set_error(error, "SGB trace command packet is not hexadecimal");
            return false;
        }
        command.packet[index] = static_cast<std::uint8_t>(parsed_byte);
    }
    return true;
}

bool command_matches(const SgbTrace::Command& expected,
                     const SgbAdapter::CommandRecord& actual) noexcept {
    if (expected.sequence != actual.sequence || expected.command != actual.command ||
        expected.packet_bytes != actual.packet_bytes) {
        return false;
    }
    return std::equal(expected.packet.begin(),
                      expected.packet.begin() + expected.packet_bytes,
                      actual.packet.begin());
}

bool diagnostics_match(const SgbAdapter::Diagnostics& expected,
                       const SgbAdapter::Diagnostics& actual) noexcept {
    return expected.enabled == actual.enabled &&
           expected.packets_completed == actual.packets_completed &&
           expected.commands_applied == actual.commands_applied &&
           expected.malformed_packets == actual.malformed_packets &&
           expected.last_command == actual.last_command &&
           expected.last_packet_bytes == actual.last_packet_bytes &&
           expected.player_count == actual.player_count &&
           expected.current_player == actual.current_player;
}

bool commands_equal(const SgbTrace::Command& left,
                    const SgbTrace::Command& right) noexcept {
    if (left.packet_bytes > left.packet.size() ||
        right.packet_bytes > right.packet.size()) {
        return false;
    }
    return left.sequence == right.sequence && left.command == right.command &&
           left.packet_bytes == right.packet_bytes &&
           std::equal(left.packet.begin(), left.packet.begin() + left.packet_bytes,
                      right.packet.begin());
}

void tick_bus(MemoryBus& bus, std::uint64_t cycles) noexcept {
    while (cycles != 0) {
        const auto chunk = std::min<std::uint64_t>(
            cycles, std::numeric_limits<unsigned>::max());
        bus.tick(static_cast<unsigned>(chunk));
        cycles -= chunk;
    }
}

} // namespace

SgbTrace::Recorder::Recorder(const std::uint64_t rom_fingerprint,
                             const HardwareModel model) noexcept {
    trace_.rom_fingerprint = rom_fingerprint;
    trace_.model = model;
}

bool SgbTrace::Recorder::record_joypad_write(const std::uint64_t cycle,
                                             const std::uint8_t value) {
    if (trace_.writes.size() >= SgbTrace::max_writes ||
        (!trace_.writes.empty() && cycle < trace_.writes.back().cycle)) {
        return false;
    }
    trace_.writes.push_back(JoypadWrite{cycle, value});
    return true;
}

bool SgbTrace::Recorder::checkpoint(const std::uint64_t cycle,
                                    const std::uint64_t frame,
                                    const Emulator& emulator) {
    if (trace_.checkpoints.size() >= SgbTrace::max_checkpoints ||
        (!trace_.checkpoints.empty() && cycle < trace_.checkpoints.back().cycle)) {
        return false;
    }
    Checkpoint checkpoint;
    checkpoint.cycle = cycle;
    checkpoint.frame = frame;
    checkpoint.framebuffer_hash = framebuffer_hash(emulator.sgb_framebuffer());
    checkpoint.state_hash = state_hash(emulator.bus());
    checkpoint.diagnostics = emulator.bus().debug_sgb_diagnostics();
    const auto& adapter = emulator.bus().debug_sgb_adapter();
    const auto& history = adapter.command_history();
    for (std::size_t index = 0; index < adapter.command_history_size(); ++index) {
        const auto history_index =
            (adapter.command_history_oldest() + index) %
            SgbAdapter::command_history_capacity;
        const auto& record = history[history_index];
        if (record.sequence <= last_command_sequence_) continue;
        Command command;
        command.sequence = record.sequence;
        command.command = record.command;
        command.packet_bytes = record.packet_bytes;
        command.packet = record.packet;
        checkpoint.commands.push_back(command);
        if (checkpoint.commands.size() > max_commands_per_checkpoint) return false;
    }
    last_command_sequence_ = checkpoint.diagnostics.commands_applied;
    trace_.checkpoints.push_back(std::move(checkpoint));
    return true;
}

SgbTrace::Trace SgbTrace::Recorder::snapshot() const {
    return trace_;
}

SgbTrace::Trace SgbTrace::Recorder::finish() && {
    return std::move(trace_);
}

std::uint64_t SgbTrace::framebuffer_hash(
    const Ppu::SgbFramebuffer& framebuffer) noexcept {
    auto hash = UINT64_C(14695981039346656037);
    for (const auto pixel : framebuffer) hash = fnv_append_integer(hash, pixel);
    return hash;
}

std::uint64_t SgbTrace::state_hash(const MemoryBus& bus) noexcept {
    return bus.debug_sgb_state_hash();
}

bool SgbTrace::serialize(const Trace& trace, std::ostream& output,
                         std::string* error) {
    if (trace.version != format_version) {
        set_error(error, "unsupported SGB trace version");
        return false;
    }
    if (trace.writes.size() > max_writes ||
        trace.checkpoints.size() > max_checkpoints) {
        set_error(error, "SGB trace exceeds the configured event limits");
        return false;
    }
    output << trace_header << '\n'
           << "trace_version=" << trace.version
           << " model=" << hardware_model_id(trace.model)
           << " rom_fingerprint=0x" << std::hex << trace.rom_fingerprint
           << std::dec << '\n';
    auto previous_cycle = std::uint64_t{};
    for (const auto& write : trace.writes) {
        if (write.cycle < previous_cycle) {
            set_error(error, "SGB trace writes are not monotonic");
            return false;
        }
        previous_cycle = write.cycle;
        output << "write cycle=" << write.cycle << " value=0x" << std::hex
               << std::setw(2) << std::setfill('0')
               << static_cast<unsigned>(write.value) << std::setfill(' ') << std::dec
               << '\n';
    }
    previous_cycle = 0;
    for (const auto& checkpoint : trace.checkpoints) {
        if (checkpoint.cycle < previous_cycle ||
            checkpoint.commands.size() > max_commands_per_checkpoint) {
            set_error(error, "invalid SGB trace checkpoint ordering or size");
            return false;
        }
        previous_cycle = checkpoint.cycle;
        const auto& diagnostics = checkpoint.diagnostics;
        output << "checkpoint cycle=" << checkpoint.cycle
               << " frame=" << checkpoint.frame << " framebuffer_hash=0x"
               << std::hex << checkpoint.framebuffer_hash << " state_hash=0x"
               << checkpoint.state_hash << std::dec
               << " packets=" << diagnostics.packets_completed
               << " commands=" << diagnostics.commands_applied
               << " malformed=" << diagnostics.malformed_packets
               << " last_command=0x" << std::hex << std::setw(2)
               << std::setfill('0') << static_cast<unsigned>(diagnostics.last_command)
               << std::setfill(' ') << std::dec
               << " last_packet_bytes=" << static_cast<unsigned>(diagnostics.last_packet_bytes)
               << " player_count=" << static_cast<unsigned>(diagnostics.player_count)
               << " current_player=" << static_cast<unsigned>(diagnostics.current_player)
               << " enabled=" << (diagnostics.enabled ? 1 : 0)
               << " command_count=" << checkpoint.commands.size() << '\n';
        for (const auto& command : checkpoint.commands) {
            if (command.packet_bytes < Joypad::sgb_packet_size ||
                command.packet_bytes > command.packet.size()) {
                set_error(error, "invalid SGB trace command packet size");
                return false;
            }
            output << "command sequence=" << command.sequence << " command=0x"
                   << std::hex << static_cast<unsigned>(command.command)
                   << std::dec << " bytes=" << static_cast<unsigned>(command.packet_bytes)
                   << " packet=";
            for (std::size_t index = 0; index < command.packet_bytes; ++index) {
                output << std::hex << std::setw(2) << std::setfill('0')
                       << static_cast<unsigned>(command.packet[index]);
            }
            output << std::setfill(' ') << std::dec << '\n';
        }
    }
    output << trace_end << '\n';
    if (!output) {
        set_error(error, "could not write SGB trace");
        return false;
    }
    return true;
}

std::optional<SgbTrace::Trace> SgbTrace::parse(const std::string_view text,
                                               std::string* error) {
    if (text.size() > max_trace_bytes) {
        set_error(error, "SGB trace exceeds the 16 MiB limit");
        return std::nullopt;
    }
    const auto first_end = text.find('\n');
    if (text.substr(0, first_end) != trace_header) {
        set_error(error, "invalid SGB trace header");
        return std::nullopt;
    }
    Trace trace;
    std::size_t offset = first_end == std::string_view::npos ? text.size()
                                                              : first_end + 1;
    const auto header_end = text.find('\n', offset);
    if (header_end == std::string_view::npos) {
        set_error(error, "SGB trace has no metadata header");
        return std::nullopt;
    }
    const auto header = text.substr(offset, header_end - offset);
    if (!required_integer(header, "trace_version", trace.version, error)) return std::nullopt;
    const auto model_field = field(header, "model");
    const auto fingerprint_field = field(header, "rom_fingerprint");
    if (!model_field.has_value() || !fingerprint_field.has_value()) {
        set_error(error, "SGB trace metadata is incomplete");
        return std::nullopt;
    }
    const auto model = parse_model(*model_field);
    const auto fingerprint = parse_integer(*fingerprint_field);
    if (!model.has_value() || !fingerprint.has_value()) {
        set_error(error, "SGB trace metadata is invalid");
        return std::nullopt;
    }
    trace.model = *model;
    trace.rom_fingerprint = *fingerprint;
    if (trace.version != format_version) {
        set_error(error, "unsupported SGB trace version");
        return std::nullopt;
    }
    offset = header_end + 1;
    std::size_t line_number = 2;
    Checkpoint* pending_checkpoint = nullptr;
    std::size_t expected_commands{};
    while (offset <= text.size()) {
        const auto end = text.find('\n', offset);
        const auto line = text.substr(
            offset, end == std::string_view::npos ? text.size() - offset
                                                   : end - offset);
        if (line.size() > max_trace_line_bytes) {
            set_error(error, "SGB trace line exceeds the 1 MiB limit");
            return std::nullopt;
        }
        const auto separator = line.find(' ');
        const auto kind = line.substr(0, separator);
        const auto fields = separator == std::string_view::npos
                                ? std::string_view{}
                                : line.substr(separator + 1);
        if (kind == trace_end) {
            if (pending_checkpoint != nullptr &&
                pending_checkpoint->commands.size() != expected_commands) {
                set_error(error, "SGB trace command count does not match checkpoint");
                return std::nullopt;
            }
            return trace;
        }
        if (kind == "write") {
            if (pending_checkpoint != nullptr &&
                pending_checkpoint->commands.size() != expected_commands) {
                set_error(error, "SGB trace command count does not match checkpoint");
                return std::nullopt;
            }
            pending_checkpoint = nullptr;
            if (trace.writes.size() >= max_writes) {
                set_error(error, "SGB trace contains too many writes");
                return std::nullopt;
            }
            JoypadWrite write;
            if (!required_integer(fields, "cycle", write.cycle, error) ||
                !required_integer(fields, "value", write.value, error)) {
                return std::nullopt;
            }
            if (!trace.writes.empty() && write.cycle < trace.writes.back().cycle) {
                set_error(error, "SGB trace writes are not monotonic");
                return std::nullopt;
            }
            trace.writes.push_back(write);
        } else if (kind == "checkpoint") {
            if (pending_checkpoint != nullptr &&
                pending_checkpoint->commands.size() != expected_commands) {
                set_error(error, "SGB trace command count does not match checkpoint");
                return std::nullopt;
            }
            if (trace.checkpoints.size() >= max_checkpoints) {
                set_error(error, "SGB trace contains too many checkpoints");
                return std::nullopt;
            }
            Checkpoint checkpoint;
            if (!required_integer(fields, "cycle", checkpoint.cycle, error) ||
                !required_integer(fields, "frame", checkpoint.frame, error) ||
                !required_integer(fields, "framebuffer_hash", checkpoint.framebuffer_hash,
                                  error) ||
                !required_integer(fields, "state_hash", checkpoint.state_hash, error)) {
                return std::nullopt;
            }
            auto& diagnostics = checkpoint.diagnostics;
            if (!required_integer(fields, "packets", diagnostics.packets_completed, error) ||
                !required_integer(fields, "commands", diagnostics.commands_applied, error) ||
                !required_integer(fields, "malformed", diagnostics.malformed_packets, error) ||
                !required_integer(fields, "last_command", diagnostics.last_command, error) ||
                !required_integer(fields, "last_packet_bytes", diagnostics.last_packet_bytes,
                                  error) ||
                !required_integer(fields, "player_count", diagnostics.player_count, error) ||
                !required_integer(fields, "current_player", diagnostics.current_player, error)) {
                return std::nullopt;
            }
            std::uint8_t enabled{};
            if (!required_integer(fields, "enabled", enabled, error) || enabled > 1) {
                set_error(error, "invalid SGB trace enabled flag");
                return std::nullopt;
            }
            diagnostics.enabled = enabled != 0;
            std::uint64_t command_count{};
            if (!required_integer(fields, "command_count", command_count, error) ||
                command_count > max_commands_per_checkpoint) {
                set_error(error, "invalid SGB trace command count");
                return std::nullopt;
            }
            if (!trace.checkpoints.empty() &&
                checkpoint.cycle < trace.checkpoints.back().cycle) {
                set_error(error, "SGB trace checkpoints are not monotonic");
                return std::nullopt;
            }
            trace.checkpoints.push_back(std::move(checkpoint));
            pending_checkpoint = &trace.checkpoints.back();
            expected_commands = static_cast<std::size_t>(command_count);
        } else if (kind == "command") {
            if (pending_checkpoint == nullptr ||
                pending_checkpoint->commands.size() >= expected_commands) {
                set_error(error, "SGB trace command has no available checkpoint slot");
                return std::nullopt;
            }
            Command command;
            if (!required_integer(fields, "sequence", command.sequence, error) ||
                !required_integer(fields, "command", command.command, error) ||
                !required_integer(fields, "bytes", command.packet_bytes, error)) {
                return std::nullopt;
            }
            if (command.packet_bytes < Joypad::sgb_packet_size ||
                command.packet_bytes > command.packet.size()) {
                set_error(error, "invalid SGB trace command packet size");
                return std::nullopt;
            }
            const auto packet = field(fields, "packet");
            if (!packet.has_value() || !parse_hex_packet(*packet, command, error)) {
                return std::nullopt;
            }
            pending_checkpoint->commands.push_back(command);
        } else if (!kind.empty()) {
            set_error(error, "unknown SGB trace record at line " +
                              std::to_string(line_number));
            return std::nullopt;
        }
        if (end == std::string_view::npos) break;
        offset = end + 1;
        ++line_number;
    }
    set_error(error, "SGB trace has no end record");
    return std::nullopt;
}

SgbTrace::ReplayResult SgbTrace::replay(const Trace& trace, Emulator& emulator) {
    ReplayResult result;
    if (trace.version != format_version) {
        result.error = "unsupported SGB trace version";
        return result;
    }
    if (trace.rom_fingerprint != 0 && trace.rom_fingerprint != emulator.rom_fingerprint()) {
        result.error = "SGB trace ROM fingerprint does not match emulator";
        return result;
    }
    if (trace.model != HardwareModel::automatic && trace.model != emulator.hardware_model()) {
        result.error = "SGB trace hardware model does not match emulator";
        return result;
    }
    std::size_t write_index{};
    std::size_t checkpoint_index{};
    std::uint64_t cycle{};
    const auto check = [&](const Checkpoint& checkpoint) -> bool {
        if (checkpoint.cycle < cycle) {
            result.error = "SGB trace checkpoint precedes replay cursor";
            return false;
        }
        tick_bus(emulator.bus(), checkpoint.cycle - cycle);
        cycle = checkpoint.cycle;
        if (framebuffer_hash(emulator.sgb_framebuffer()) != checkpoint.framebuffer_hash) {
            result.error = "SGB trace framebuffer checkpoint mismatch";
            return false;
        }
        if (state_hash(emulator.bus()) != checkpoint.state_hash) {
            result.error = "SGB trace state checkpoint mismatch";
            return false;
        }
        if (!diagnostics_match(checkpoint.diagnostics,
                               emulator.bus().debug_sgb_diagnostics())) {
            result.error = "SGB trace diagnostics checkpoint mismatch";
            return false;
        }
        const auto& adapter = emulator.bus().debug_sgb_adapter();
        const auto& history = adapter.command_history();
        for (const auto& expected : checkpoint.commands) {
            bool found = false;
            for (std::size_t index = 0; index < adapter.command_history_size(); ++index) {
                const auto history_index =
                    (adapter.command_history_oldest() + index) %
                    SgbAdapter::command_history_capacity;
                if (command_matches(expected, history[history_index])) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                result.error = "SGB trace decoded command mismatch";
                return false;
            }
        }
        ++result.checkpoints_checked;
        return true;
    };
    while (write_index < trace.writes.size()) {
        const auto& write = trace.writes[write_index];
        while (checkpoint_index < trace.checkpoints.size() &&
               trace.checkpoints[checkpoint_index].cycle < write.cycle) {
            if (!check(trace.checkpoints[checkpoint_index++])) return result;
        }
        if (write.cycle < cycle) {
            result.error = "SGB trace write precedes replay cursor";
            return result;
        }
        tick_bus(emulator.bus(), write.cycle - cycle);
        cycle = write.cycle;
        emulator.bus().write8(0xFF00, write.value);
        ++write_index;
        ++result.writes_applied;
        while (checkpoint_index < trace.checkpoints.size() &&
               trace.checkpoints[checkpoint_index].cycle == cycle) {
            if (!check(trace.checkpoints[checkpoint_index++])) return result;
        }
    }
    while (checkpoint_index < trace.checkpoints.size()) {
        if (!check(trace.checkpoints[checkpoint_index++])) return result;
    }
    result.success = true;
    return result;
}

SgbTrace::DiffResult SgbTrace::diff(const Trace& left, const Trace& right) {
    const auto mismatch = [](const std::size_t index,
                             const std::string& description) {
        return DiffResult{false, index, description};
    };
    if (left.version != right.version) {
        return mismatch(0, "metadata: trace versions differ");
    }
    if (left.model != right.model) {
        return mismatch(0, "metadata: hardware models differ");
    }
    if (left.rom_fingerprint != right.rom_fingerprint) {
        return mismatch(0, "metadata: ROM fingerprints differ");
    }
    if (left.writes.size() != right.writes.size()) {
        return mismatch(std::min(left.writes.size(), right.writes.size()),
                        "writes: event counts differ");
    }
    for (std::size_t index = 0; index < left.writes.size(); ++index) {
        if (left.writes[index].cycle != right.writes[index].cycle) {
            return mismatch(index, "writes: cycle differs");
        }
        if (left.writes[index].value != right.writes[index].value) {
            return mismatch(index, "writes: JOYP value differs");
        }
    }
    if (left.checkpoints.size() != right.checkpoints.size()) {
        return mismatch(std::min(left.checkpoints.size(), right.checkpoints.size()),
                        "checkpoints: event counts differ");
    }
    for (std::size_t index = 0; index < left.checkpoints.size(); ++index) {
        const auto& lhs = left.checkpoints[index];
        const auto& rhs = right.checkpoints[index];
        if (lhs.cycle != rhs.cycle) {
            return mismatch(index, "checkpoints: cycle differs");
        }
        if (lhs.frame != rhs.frame) {
            return mismatch(index, "checkpoints: frame number differs");
        }
        if (lhs.framebuffer_hash != rhs.framebuffer_hash) {
            return mismatch(index, "checkpoints: framebuffer hash differs");
        }
        if (lhs.state_hash != rhs.state_hash) {
            return mismatch(index, "checkpoints: SGB state hash differs");
        }
        if (!diagnostics_match(lhs.diagnostics, rhs.diagnostics)) {
            return mismatch(index, "checkpoints: diagnostics differ");
        }
        if (lhs.commands.size() != rhs.commands.size()) {
            return mismatch(index, "checkpoints: decoded command counts differ");
        }
        for (std::size_t command = 0; command < lhs.commands.size(); ++command) {
            if (!commands_equal(lhs.commands[command], rhs.commands[command])) {
                return mismatch(index, "checkpoints: decoded command differs");
            }
        }
    }
    return DiffResult{true, 0, {}};
}

} // namespace gameboy
