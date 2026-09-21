#include "gameboy/link_packet_channel.hpp"
#include "gameboy/memory_bus.hpp"
#include "gameboy/tcp_serial_endpoint.hpp"
#include "gbb/trace_format.hpp"
#include "gbb/trace_parser.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

int failures = 0;

void check(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

enum class Direction { host_to_join, join_to_host };
enum class FaultKind { drop, duplicate, delay, disconnect };

std::string_view direction_name(const Direction direction) {
    return direction == Direction::host_to_join ? "host_to_join" : "join_to_host";
}

std::string_view fault_name(const FaultKind fault) {
    switch (fault) {
    case FaultKind::drop: return "drop";
    case FaultKind::duplicate: return "duplicate";
    case FaultKind::delay: return "delay";
    case FaultKind::disconnect: return "disconnect";
    }
    return "unknown";
}

std::string_view packet_name(const gameboy::LinkPacketType type) {
    switch (type) {
    case gameboy::LinkPacketType::hello: return "hello";
    case gameboy::LinkPacketType::bit: return "bit";
    case gameboy::LinkPacketType::acknowledgement: return "acknowledgement";
    case gameboy::LinkPacketType::clock_release: return "clock_release";
    case gameboy::LinkPacketType::byte: return "byte";
    case gameboy::LinkPacketType::heartbeat: return "heartbeat";
    case gameboy::LinkPacketType::state_digest: return "state_digest";
    }
    return "unknown";
}

std::optional<Direction> parse_direction(const std::string_view value) {
    if (value == "host_to_join") return Direction::host_to_join;
    if (value == "join_to_host") return Direction::join_to_host;
    return std::nullopt;
}

std::optional<FaultKind> parse_fault(const std::string_view value) {
    for (const auto fault : {FaultKind::drop, FaultKind::duplicate,
                             FaultKind::delay, FaultKind::disconnect}) {
        if (fault_name(fault) == value) return fault;
    }
    return std::nullopt;
}

std::optional<gameboy::LinkPacketType> parse_packet(const std::string_view value) {
    for (const auto packet : {gameboy::LinkPacketType::hello,
                              gameboy::LinkPacketType::bit,
                              gameboy::LinkPacketType::acknowledgement,
                              gameboy::LinkPacketType::clock_release,
                              gameboy::LinkPacketType::byte,
                              gameboy::LinkPacketType::heartbeat,
                              gameboy::LinkPacketType::state_digest}) {
        if (packet_name(packet) == value) return packet;
    }
    return std::nullopt;
}

struct FaultRule {
    Direction direction{};
    FaultKind fault{};
    gameboy::LinkPacketType packet{gameboy::LinkPacketType::byte};
    std::optional<std::uint32_t> sequence;
    bool used{};
};

class TraceWriter final {
public:
    explicit TraceWriter(const std::string_view scenario)
        : session_(gbb::next_trace_session_id()), scenario_(scenario) {
        gbb::write_trace_session_start(output_, session_, "fault", "host",
                                       scenario_);
    }

    void event(const std::string_view name, const std::string_view fields = {}) {
        gbb::write_trace_event_prefix(output_, name, session_, frame_, frame_,
                                      "fault", "host");
        if (!fields.empty()) output_ << ' ' << fields;
        output_ << '\n';
        ++frame_;
    }

    [[nodiscard]] std::string finish() {
        gbb::write_trace_session_end(output_, session_, frame_, frame_);
        return output_.str();
    }

private:
    std::ostringstream output_;
    std::uint64_t session_{};
    std::uint64_t frame_{};
    std::string scenario_;
};

class FaultScript final {
public:
    void add(const Direction direction, const FaultKind fault,
             const gameboy::LinkPacketType packet,
             const std::optional<std::uint32_t> sequence = std::nullopt) {
        rules_.push_back({direction, fault, packet, sequence, false});
    }

    [[nodiscard]] std::optional<FaultKind> take(
        const Direction direction, const gameboy::LinkPacket& packet) noexcept {
        for (auto& rule : rules_) {
            if (rule.used || rule.direction != direction ||
                rule.packet != packet.type ||
                (rule.sequence.has_value() && *rule.sequence != packet.sequence)) {
                continue;
            }
            rule.used = true;
            return rule.fault;
        }
        return std::nullopt;
    }

    [[nodiscard]] std::size_t used_count() const noexcept {
        return static_cast<std::size_t>(std::count_if(
            rules_.begin(), rules_.end(), [](const FaultRule& rule) {
                return rule.used;
            }));
    }

private:
    std::vector<FaultRule> rules_;
};

class FaultPacketChannel final : public gameboy::LinkPacketChannel {
public:
    using State = gameboy::LinkPacketChannel::State;

    FaultPacketChannel(const Direction direction, FaultScript& script,
                       TraceWriter& trace)
        : direction_(direction), script_(script), trace_(trace) {}

    void connect_to(FaultPacketChannel& peer) noexcept {
        peer_ = &peer;
        state_ = State::connected;
    }

    void poll() noexcept override {
        if (!delayed_packet_.has_value() || peer_ == nullptr ||
            state_ != State::connected) {
            return;
        }
        if (delay_polls_ != 0) {
            --delay_polls_;
            return;
        }
        peer_->packets_.push_back(*delayed_packet_);
        delayed_packet_.reset();
    }

    void close() noexcept override { state_ = State::disconnected; }

    [[nodiscard]] bool send(const gameboy::LinkPacket& packet) noexcept override {
        if (peer_ == nullptr || state_ != State::connected) return false;
        const auto fault = script_.take(direction_, packet);
        if (fault.has_value()) {
            std::ostringstream fields;
            fields << "direction=" << direction_name(direction_)
                   << " action=" << fault_name(*fault)
                   << " packet=" << packet_name(packet.type)
                   << " sequence=" << packet.sequence;
            if (*fault == FaultKind::delay) fields << " delay_polls=8";
            trace_.event("fault_injected", fields.str());

            switch (*fault) {
            case FaultKind::drop:
                return true;
            case FaultKind::duplicate:
                peer_->packets_.push_back(packet);
                peer_->packets_.push_back(packet);
                return true;
            case FaultKind::delay:
                delayed_packet_ = packet;
                delay_polls_ = 8;
                return true;
            case FaultKind::disconnect:
                state_ = State::failed;
                peer_->state_ = State::failed;
                return false;
            }
        }
        peer_->packets_.push_back(packet);
        return true;
    }

    [[nodiscard]] std::optional<gameboy::LinkPacket> receive() noexcept override {
        if (packets_.empty()) return std::nullopt;
        auto packet = packets_.front();
        packets_.pop_front();
        return packet;
    }

    [[nodiscard]] State state() const noexcept override { return state_; }

private:
    Direction direction_;
    FaultScript& script_;
    TraceWriter& trace_;
    FaultPacketChannel* peer_{};
    State state_{State::disconnected};
    std::deque<gameboy::LinkPacket> packets_;
    std::optional<gameboy::LinkPacket> delayed_packet_;
    unsigned delay_polls_{};
};

std::vector<std::uint8_t> test_rom() {
    std::vector<std::uint8_t> rom(0x8000, 0);
    constexpr std::string_view title = "FAULT HARNESS";
    std::copy(title.begin(), title.end(), rom.begin() + 0x134);
    return rom;
}

struct RunResult {
    bool completed{};
    bool transfer_aborted{};
    bool peer_stayed_connected{};
    bool fault_applied{};
    std::uint64_t retries{};
    std::uint64_t duplicate_requests{};
    std::uint8_t host_received{0xFF};
    std::uint8_t join_received{0xFF};
    std::string trace;
};

void save_trace_if_requested(const std::string_view name,
                             const std::string_view contents) {
    const char* directory = std::getenv("GBB_LINK_FAULT_TRACE_DIR");
    if (directory == nullptr || *directory == '\0') return;
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) return;
    std::ofstream output(std::filesystem::path{directory} /
                         (std::string{name} + ".log"));
    output << contents;
}

RunResult run_scenario(FaultScript& script, const std::string_view scenario) {
    TraceWriter trace(scenario);
    FaultPacketChannel host_channel{Direction::host_to_join, script, trace};
    FaultPacketChannel join_channel{Direction::join_to_host, script, trace};
    host_channel.connect_to(join_channel);
    join_channel.connect_to(host_channel);

    gameboy::MemoryBus host{gameboy::Cartridge{test_rom()}};
    gameboy::MemoryBus join{gameboy::Cartridge{test_rom()}};
    gameboy::TcpSerialEndpoint host_endpoint;
    gameboy::TcpSerialEndpoint join_endpoint;
    host_endpoint.set_arbitration_priority(true);
    join_endpoint.set_arbitration_priority(false);
    constexpr std::uint64_t compatibility_id = UINT64_C(0x4641554C54);
    host_endpoint.attach(host.serial_port(), host_channel, compatibility_id);
    join_endpoint.attach(join.serial_port(), join_channel, compatibility_id);

    for (unsigned attempt = 0; attempt < 300; ++attempt) {
        host_endpoint.poll();
        join_endpoint.poll();
        if (host_endpoint.peer_ready_for_link() &&
            join_endpoint.peer_ready_for_link()) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    host.write8(0xFF01, 0xA5);
    join.write8(0xFF01, 0x3C);
    join.write8(0xFF02, 0x80);
    host.write8(0xFF02, 0x81);
    for (unsigned cycle = 0; cycle < 2500; ++cycle) {
        host.tick(4);
        join.tick(4);
        host_endpoint.poll();
        join_endpoint.poll();
        if (!host.serial_port().transfer_active() &&
            !join.serial_port().transfer_active()) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    host_endpoint.poll();
    join_endpoint.poll();

    RunResult result;
    result.completed = host.serial_port().transfers_completed() == 1 &&
                       join.serial_port().transfers_completed() == 1 &&
                       host.read8(0xFF01) == 0x3C &&
                       join.read8(0xFF01) == 0xA5;
    result.peer_stayed_connected = host_channel.state() ==
                                       gameboy::LinkPacketChannel::State::connected &&
                                   join_channel.state() ==
                                       gameboy::LinkPacketChannel::State::connected;
    result.transfer_aborted = !result.completed &&
                              !host.serial_port().transfer_active() &&
                              !join.serial_port().transfer_active() &&
                              !result.peer_stayed_connected;
    result.fault_applied = script.used_count() != 0;
    result.retries = host_endpoint.request_retries() + join_endpoint.request_retries();
    result.duplicate_requests = host_endpoint.duplicate_requests() +
                                join_endpoint.duplicate_requests();
    result.host_received = host.read8(0xFF01);
    result.join_received = join.read8(0xFF01);

    std::ostringstream outcome;
    outcome << "completed=" << (result.completed ? 1 : 0)
            << " aborted=" << (result.transfer_aborted ? 1 : 0)
            << " retries=" << result.retries;
    trace.event("scenario_result", outcome.str());
    result.trace = trace.finish();
    save_trace_if_requested(scenario, result.trace);

    host_endpoint.detach();
    join_endpoint.detach();
    return result;
}

FaultScript replay_script(const gbb::TraceReport& report) {
    FaultScript script;
    for (const auto& record : report.records) {
        if (record.event != "fault_injected") continue;
        const auto* direction = record.field("direction");
        const auto* action = record.field("action");
        const auto* packet = record.field("packet");
        const auto sequence = record.uint64_field("sequence");
        if (direction == nullptr || action == nullptr || packet == nullptr ||
            !sequence.has_value() || *sequence > UINT32_MAX) {
            continue;
        }
        const auto parsed_direction = parse_direction(*direction);
        const auto parsed_fault = parse_fault(*action);
        const auto parsed_packet = parse_packet(*packet);
        if (parsed_direction.has_value() && parsed_fault.has_value() &&
            parsed_packet.has_value()) {
            script.add(*parsed_direction, *parsed_fault, *parsed_packet,
                       static_cast<std::uint32_t>(*sequence));
        }
    }
    return script;
}

void test_drop_is_replayable() {
    FaultScript original_script;
    original_script.add(Direction::host_to_join, FaultKind::drop,
                        gameboy::LinkPacketType::byte);
    const auto original = run_scenario(original_script, "drop-original");
    const auto report = gbb::parse_trace(original.trace);
    check(report.valid(), "drop scenario emits a valid canonical trace");
    check(report.has_event("fault_injected") &&
              report.has_event("scenario_result"),
          "drop scenario records both the injected fault and outcome");
    check(original.fault_applied && original.completed && original.retries != 0,
          "dropped byte is retransmitted and the transfer completes");

    auto replay_script_instance = replay_script(report);
    const auto replay = run_scenario(replay_script_instance, "drop-replay");
    check(replay.fault_applied && replay.completed && replay.retries != 0,
          "replayed drop trace reproduces the recovered transfer");
    check(replay.host_received == original.host_received &&
              replay.join_received == original.join_received,
          "replayed drop preserves both received bytes");
}

void test_delay_and_duplicate_faults() {
    FaultScript delay_script;
    delay_script.add(Direction::host_to_join, FaultKind::delay,
                     gameboy::LinkPacketType::byte);
    const auto delayed = run_scenario(delay_script, "delay");
    const auto delayed_report = gbb::parse_trace(delayed.trace);
    check(delayed_report.valid() && delayed.fault_applied && delayed.completed,
          "delayed packet is released and the transfer completes");
    check(delayed_report.has_event("fault_injected"),
          "delayed packet is recorded in the trace");

    FaultScript duplicate_script;
    duplicate_script.add(Direction::host_to_join, FaultKind::duplicate,
                         gameboy::LinkPacketType::byte);
    const auto duplicated = run_scenario(duplicate_script, "duplicate");
    check(duplicated.fault_applied && duplicated.completed &&
              duplicated.duplicate_requests != 0,
          "duplicated packet is applied at most once and the transfer completes");
}

void test_disconnect_aborts_transfer() {
    FaultScript script;
    script.add(Direction::host_to_join, FaultKind::disconnect,
               gameboy::LinkPacketType::byte);
    const auto result = run_scenario(script, "disconnect");
    const auto report = gbb::parse_trace(result.trace);
    check(report.valid() && result.fault_applied,
          "disconnect scenario emits a valid fault trace");
    check(!result.completed && result.transfer_aborted &&
              !result.peer_stayed_connected,
          "disconnect aborts the active transfer and fails both peers");
}

} // namespace

int main() {
    test_drop_is_replayable();
    test_delay_and_duplicate_faults();
    test_disconnect_aborts_transfer();
    return failures == 0 ? 0 : 1;
}
