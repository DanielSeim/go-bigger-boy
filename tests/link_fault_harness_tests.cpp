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
#include <iterator>
#include <optional>
#include <sstream>
#include <stdexcept>
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

    [[nodiscard]] std::size_t size() const noexcept { return rules_.size(); }

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

    [[nodiscard]] bool has_pending_work() const noexcept {
        return !packets_.empty() || delayed_packet_.has_value();
    }

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

const char* channel_state_name(
    const gameboy::LinkPacketChannel::State state) noexcept {
    switch (state) {
    case gameboy::LinkPacketChannel::State::disconnected: return "disconnected";
    case gameboy::LinkPacketChannel::State::listening: return "listening";
    case gameboy::LinkPacketChannel::State::connecting: return "connecting";
    case gameboy::LinkPacketChannel::State::connected: return "connected";
    case gameboy::LinkPacketChannel::State::failed: return "failed";
    }
    return "unknown";
}

std::string endpoint_diagnostic_fields(
    const std::string_view prefix, const gameboy::TcpSerialEndpoint& endpoint,
    const gameboy::SerialPort& port) {
    std::ostringstream fields;
    fields << prefix << "_connected=" << (endpoint.connected() ? 1 : 0)
           << ' ' << prefix << "_peer_ready="
           << (endpoint.peer_ready_for_link() ? 1 : 0)
           << ' ' << prefix << "_peer_hello="
           << (endpoint.peer_hello_seen() ? 1 : 0)
           << ' ' << prefix << "_peer_compatible="
           << (endpoint.peer_compatible() ? 1 : 0)
           << ' ' << prefix << "_state_digest_valid="
           << (endpoint.state_digest_valid() ? 1 : 0)
           << ' ' << prefix << "_peer_request_seen="
           << (endpoint.peer_request_seen() ? 1 : 0)
           << ' ' << prefix << "_transfer_active="
           << (port.transfer_active() ? 1 : 0)
           << ' ' << prefix << "_waiting_for_peer="
           << (endpoint.waiting_for_peer() ? 1 : 0)
           << ' ' << prefix << "_requests_sent=" << endpoint.requests_sent()
           << ' ' << prefix << "_requests_received="
           << endpoint.requests_received()
           << ' ' << prefix << "_responses_sent=" << endpoint.responses_sent()
           << ' ' << prefix << "_responses_received="
           << endpoint.responses_received()
           << ' ' << prefix << "_request_retries="
           << endpoint.request_retries()
           << ' ' << prefix << "_duplicate_requests="
           << endpoint.duplicate_requests()
           << ' ' << prefix << "_protocol_errors="
           << endpoint.protocol_errors()
           << ' ' << prefix << "_transfers_completed="
           << endpoint.transfers_completed();
    return fields.str();
}

std::string channel_diagnostic_fields(const std::string_view prefix,
                                      const FaultPacketChannel& channel) {
    std::ostringstream fields;
    fields << prefix << "_state=" << channel_state_name(channel.state())
           << ' ' << prefix << "_pending_work="
           << (channel.has_pending_work() ? 1 : 0);
    return fields.str();
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

    bool handshake_ready = false;
    // The compatibility handshake intentionally retries on a wall-clock
    // interval. Do not start a transfer after a fixed 300 ms window: that
    // races slower CI runners and makes the fault scenarios scheduler
    // dependent.
    for (unsigned attempt = 0; attempt < 3000; ++attempt) {
        host_endpoint.poll();
        join_endpoint.poll();
        // The join endpoint deliberately stays "not ready" until it sees
        // the host's first request, so waiting for peer_ready_for_link() on
        // both sides would deadlock before the transfer starts.
        if (host_endpoint.peer_ready_for_link() &&
            join_endpoint.peer_hello_seen() &&
            join_endpoint.peer_compatible()) {
            handshake_ready = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    if (!handshake_ready) {
        RunResult result;
        result.fault_applied = script.used_count() != 0;
        result.peer_stayed_connected = host_channel.state() ==
                                           gameboy::LinkPacketChannel::State::connected &&
                                       join_channel.state() ==
                                           gameboy::LinkPacketChannel::State::connected;
        trace.event(
            "link_diagnostic",
            std::string{"kind=handshake_timeout "} +
                endpoint_diagnostic_fields("host", host_endpoint,
                                           host.serial_port()) +
                ' ' + endpoint_diagnostic_fields("join", join_endpoint,
                                                 join.serial_port()) +
                ' ' + channel_diagnostic_fields("host_channel", host_channel) +
                ' ' + channel_diagnostic_fields("join_channel", join_channel));
        trace.event("scenario_result",
                    "completed=0 aborted=0 handshake_ready=0 settled=0 retries=0");
        result.trace = trace.finish();
        save_trace_if_requested(scenario, result.trace);
        host_endpoint.detach();
        join_endpoint.detach();
        return result;
    }

    host.write8(0xFF01, 0xA5);
    join.write8(0xFF01, 0x3C);
    join.write8(0xFF02, 0x80);
    host.write8(0xFF02, 0x81);
    trace.event(
        "link_phase",
        std::string{"phase=handshake_ready "} +
            endpoint_diagnostic_fields("host", host_endpoint,
                                       host.serial_port()) +
            ' ' + endpoint_diagnostic_fields("join", join_endpoint,
                                             join.serial_port()));
    trace.event("link_phase", "phase=transfer_started");
    bool transfer_settled = false;
    for (unsigned cycle = 0; cycle < 2500; ++cycle) {
        host.tick(4);
        join.tick(4);
        host_endpoint.poll();
        join_endpoint.poll();
        // A delayed packet can leave the local serial transfer inactive while
        // its link endpoint still owns an outstanding request. Keep polling
        // until both layers have settled; otherwise a fast Windows scheduler
        // can observe the intermediate state and report a false failure.
        if (!host.serial_port().transfer_active() &&
            !join.serial_port().transfer_active() &&
            !host_endpoint.waiting_for_peer() &&
            !join_endpoint.waiting_for_peer() &&
            !host_channel.has_pending_work() &&
            !join_channel.has_pending_work()) {
            transfer_settled = true;
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

    if (!result.completed) {
        trace.event(
            "link_diagnostic",
            std::string{"kind=terminal_incomplete reason="} +
                (transfer_settled ? "settled_without_completion"
                                  : "transfer_timeout") +
                ' ' + endpoint_diagnostic_fields("host", host_endpoint,
                                                 host.serial_port()) +
                ' ' + endpoint_diagnostic_fields("join", join_endpoint,
                                                 join.serial_port()) +
                ' ' + channel_diagnostic_fields("host_channel", host_channel) +
                ' ' + channel_diagnostic_fields("join_channel", join_channel));
    }

    std::ostringstream outcome;
    outcome << "completed=" << (result.completed ? 1 : 0)
            << " aborted=" << (result.transfer_aborted ? 1 : 0)
            << " handshake_ready=1"
            << " settled=" << (transfer_settled ? 1 : 0)
            << " retries=" << result.retries
            << " host_protocol_errors=" << host_endpoint.protocol_errors()
            << " join_protocol_errors=" << join_endpoint.protocol_errors()
            << " host_pending_work=" << (host_channel.has_pending_work() ? 1 : 0)
            << " join_pending_work=" << (join_channel.has_pending_work() ? 1 : 0);
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
              report.has_event("link_phase") &&
              report.has_event("scenario_result"),
          "drop scenario records the fault, phases, and outcome");
    check(report.has_event("link_diagnostic") == false,
          "successful drop scenario does not emit a terminal failure diagnostic");
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
    check(delayed_report.has_event("link_phase") &&
              delayed_report.has_event("scenario_result"),
          "delayed scenario records transfer phases and outcome");

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
    check(report.has_event("link_phase") &&
              report.has_event("link_diagnostic") &&
              report.has_event("scenario_result"),
          "disconnect trace records phases, terminal diagnostics, and outcome");
    check(!result.completed && result.transfer_aborted &&
              !result.peer_stayed_connected,
          "disconnect aborts the active transfer and fails both peers");
}

void print_cli_usage() {
    std::cout
        << "Usage: gbb_link_fault_harness --scenario "
           "drop|delay|duplicate|disconnect [--trace PATH]\n"
           "       gbb_link_fault_harness --replay TRACE_PATH [--trace PATH]\n"
           "\n"
           "With no arguments, runs the CTest contract suite. The replay mode\n"
           "reuses fault_injected events from a canonical diagnostic trace.\n";
}

struct CliOptions {
    std::optional<FaultKind> scenario;
    std::filesystem::path replay;
    std::filesystem::path trace;
};

CliOptions parse_cli_options(const int argc, char** argv) {
    CliOptions options;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        const auto require_value = [&](const char* name) -> std::string {
            if (index + 1 >= argc) {
                throw std::invalid_argument(std::string{name} +
                                            " requires a value");
            }
            return argv[++index];
        };
        if (argument == "--scenario") {
            const auto value = require_value("--scenario");
            const auto fault = parse_fault(value);
            if (!fault.has_value()) {
                throw std::invalid_argument("unknown fault scenario: " + value);
            }
            options.scenario = *fault;
        } else if (argument == "--replay") {
            options.replay = require_value("--replay");
        } else if (argument == "--trace") {
            options.trace = require_value("--trace");
        } else if (argument == "--help" || argument == "-h") {
            print_cli_usage();
            std::exit(EXIT_SUCCESS);
        } else {
            throw std::invalid_argument("unknown option: " + argument);
        }
    }
    if (options.scenario.has_value() && !options.replay.empty()) {
        throw std::invalid_argument("--scenario and --replay are mutually exclusive");
    }
    if (!options.scenario.has_value() && options.replay.empty()) {
        throw std::invalid_argument("one of --scenario or --replay is required");
    }
    return options;
}

std::string read_text_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::invalid_argument("could not open trace: " + path.string());
    return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

void write_text_file(const std::filesystem::path& path,
                     const std::string_view contents) {
    if (path.has_parent_path()) {
        std::error_code error;
        std::filesystem::create_directories(path.parent_path(), error);
        if (error) throw std::runtime_error("could not create trace directory: " +
                                            error.message());
    }
    std::ofstream output(path, std::ios::binary);
    if (!output) throw std::runtime_error("could not write trace: " + path.string());
    output << contents;
}

std::optional<bool> trace_completed(const gbb::TraceReport& report) {
    for (const auto& record : report.records) {
        if (record.event != "scenario_result") continue;
        const auto value = record.uint64_field("completed");
        if (value.has_value()) return *value != 0;
    }
    return std::nullopt;
}

int run_cli(const int argc, char** argv) {
    const auto options = parse_cli_options(argc, argv);
    FaultScript script;
    std::optional<bool> expected_completed;
    std::string scenario_name_for_output;
    if (options.scenario.has_value()) {
        script.add(Direction::host_to_join, *options.scenario,
                   gameboy::LinkPacketType::byte);
        scenario_name_for_output = std::string{fault_name(*options.scenario)};
        expected_completed = *options.scenario != FaultKind::disconnect;
    } else {
        const auto report = gbb::parse_trace(read_text_file(options.replay));
        if (!report.valid()) {
            for (const auto& error : report.errors) std::cerr << "error=" << error << '\n';
            return 2;
        }
        script = replay_script(report);
        if (script.size() == 0) {
            std::cerr << "error=trace contains no replayable fault_injected event\n";
            return 2;
        }
        expected_completed = trace_completed(report);
        scenario_name_for_output = "replay";
    }

    const auto result = run_scenario(script, scenario_name_for_output);
    if (!options.trace.empty()) {
        write_text_file(options.trace, result.trace);
    }
    std::cout << "scenario=" << scenario_name_for_output
              << " completed=" << (result.completed ? "yes" : "no")
              << " fault_applied=" << (result.fault_applied ? "yes" : "no")
              << " retries=" << result.retries
              << " duplicate_requests=" << result.duplicate_requests;
    if (!options.trace.empty()) std::cout << " trace=" << options.trace.string();
    std::cout << '\n';
    if (options.trace.empty()) std::cout << result.trace;

    if (!result.fault_applied ||
        (expected_completed.has_value() &&
         result.completed != *expected_completed)) {
        std::cerr << "error=fault scenario outcome did not match expectation\n";
        return 1;
    }
    return 0;
}

} // namespace

int main(const int argc, char** argv) {
    if (argc > 1) {
        try {
            return run_cli(argc, argv);
        } catch (const std::exception& error) {
            std::cerr << "error=" << error.what() << '\n';
            print_cli_usage();
            return 2;
        }
    }
    test_drop_is_replayable();
    test_delay_and_duplicate_faults();
    test_disconnect_aborts_transfer();
    return failures == 0 ? 0 : 1;
}
