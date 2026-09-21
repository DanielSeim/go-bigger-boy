#include "fault_channel.hpp"

#include "gbb/trace_parser.hpp"
#include "harness_io.hpp"
#include "scenario_trace.hpp"

#include <sstream>
#include <stdexcept>
#include <string>

namespace gbb::link_harness {
namespace {

const char* packet_name(const gameboy::LinkPacketType packet) noexcept {
    switch (packet) {
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

std::optional<gameboy::LinkPacketType> parse_packet(
    const std::string_view value) noexcept {
    for (const auto packet : {gameboy::LinkPacketType::hello,
                              gameboy::LinkPacketType::bit,
                              gameboy::LinkPacketType::acknowledgement,
                              gameboy::LinkPacketType::clock_release,
                              gameboy::LinkPacketType::byte,
                              gameboy::LinkPacketType::heartbeat,
                              gameboy::LinkPacketType::state_digest}) {
        if (value == packet_name(packet)) return packet;
    }
    return std::nullopt;
}

std::optional<FaultDirection> parse_direction(
    const std::string_view value) noexcept {
    if (value == "host_to_join") return FaultDirection::host_to_join;
    if (value == "join_to_host") return FaultDirection::join_to_host;
    return std::nullopt;
}

} // namespace

const char* fault_direction_name(const FaultDirection direction) noexcept {
    return direction == FaultDirection::host_to_join ? "host_to_join"
                                                     : "join_to_host";
}

void FaultPlan::add(const FaultDirection direction, const FaultKind fault,
                    const gameboy::LinkPacketType packet,
                    const std::optional<std::uint32_t> sequence) {
    rules_.push_back({direction, fault, packet, sequence, false});
}

std::size_t FaultPlan::used_count() const noexcept {
    std::size_t count = 0;
    for (const auto& rule : rules_) {
        if (rule.used) ++count;
    }
    return count;
}

std::optional<FaultKind> FaultPlan::take(
    const FaultDirection direction,
    const gameboy::LinkPacket& packet) noexcept {
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

FaultPlan FaultPlan::from_trace(const std::filesystem::path& path) {
    const auto bytes = read_bytes(path);
    const std::string contents(bytes.begin(), bytes.end());
    const auto report = gbb::parse_trace(contents);
    if (!report.valid()) {
        std::ostringstream message;
        message << "fault replay trace is invalid";
        if (!report.errors.empty()) message << ": " << report.errors.front();
        throw std::invalid_argument(message.str());
    }

    FaultPlan plan;
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
        const auto parsed_fault = parse_fault_kind(*action);
        const auto parsed_packet = parse_packet(*packet);
        if (parsed_direction.has_value() && parsed_fault.has_value() &&
            parsed_packet.has_value()) {
            plan.add(*parsed_direction, *parsed_fault, *parsed_packet,
                     static_cast<std::uint32_t>(*sequence));
        }
    }
    if (plan.empty()) {
        throw std::invalid_argument(
            "fault replay trace contains no replayable fault_injected events");
    }
    return plan;
}

FaultPacketChannel::FaultPacketChannel(
    gameboy::LinkPacketChannel& underlying, const FaultDirection direction,
    FaultPlan& plan, ScenarioTrace& trace) noexcept
    : underlying_(underlying), direction_(direction), plan_(plan), trace_(trace) {}

void FaultPacketChannel::poll() noexcept {
    underlying_.poll();
    if (!delayed_packet_.has_value()) return;
    if (delay_polls_ != 0) {
        --delay_polls_;
        return;
    }
    static_cast<void>(underlying_.send(*delayed_packet_));
    delayed_packet_.reset();
}

void FaultPacketChannel::close() noexcept { underlying_.close(); }

bool FaultPacketChannel::send(const gameboy::LinkPacket& packet) noexcept {
    const auto fault = plan_.take(direction_, packet);
    if (!fault.has_value()) return underlying_.send(packet);

    constexpr unsigned delay_polls = 8;
    trace_.write_fault_event(fault_direction_name(direction_),
                             fault_kind_name(*fault), packet_name(packet.type),
                             packet.sequence,
                             *fault == FaultKind::delay ? delay_polls : 0);
    switch (*fault) {
    case FaultKind::drop:
        return true;
    case FaultKind::delay:
        delayed_packet_ = packet;
        delay_polls_ = delay_polls;
        return true;
    case FaultKind::duplicate: {
        const auto first = underlying_.send(packet);
        const auto second = underlying_.send(packet);
        return first && second;
    }
    case FaultKind::disconnect:
        underlying_.close();
        return false;
    }
    return false;
}

std::optional<gameboy::LinkPacket> FaultPacketChannel::receive() noexcept {
    return underlying_.receive();
}

gameboy::LinkPacketChannel::State FaultPacketChannel::state() const noexcept {
    return underlying_.state();
}

std::size_t FaultPacketChannel::queued_packets() const noexcept {
    return underlying_.queued_packets();
}

std::size_t FaultPacketChannel::buffered_bytes() const noexcept {
    return underlying_.buffered_bytes();
}

std::uint64_t FaultPacketChannel::malformed_packets() const noexcept {
    return underlying_.malformed_packets();
}

} // namespace gbb::link_harness
