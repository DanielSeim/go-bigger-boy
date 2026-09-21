#pragma once

#include "gameboy/link_packet_channel.hpp"
#include "options.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <vector>

namespace gbb::link_harness {

class ScenarioTrace;

enum class FaultDirection { host_to_join, join_to_host };

class FaultPlan final {
  public:
    void add(FaultDirection direction, FaultKind fault,
             gameboy::LinkPacketType packet,
             std::optional<std::uint32_t> sequence = std::nullopt);

    [[nodiscard]] bool empty() const noexcept { return rules_.empty(); }
    [[nodiscard]] std::size_t used_count() const noexcept;

    [[nodiscard]] std::optional<FaultKind> take(
        FaultDirection direction, const gameboy::LinkPacket& packet) noexcept;

    [[nodiscard]] static FaultPlan from_trace(
        const std::filesystem::path& path);

  private:
    struct Rule {
        FaultDirection direction{};
        FaultKind fault{};
        gameboy::LinkPacketType packet{gameboy::LinkPacketType::byte};
        std::optional<std::uint32_t> sequence;
        bool used{};
    };
    std::vector<Rule> rules_;
};

class FaultPacketChannel final : public gameboy::LinkPacketChannel {
  public:
    FaultPacketChannel(gameboy::LinkPacketChannel& underlying,
                       FaultDirection direction, FaultPlan& plan,
                       ScenarioTrace& trace) noexcept;

    void poll() noexcept override;
    void close() noexcept override;
    [[nodiscard]] bool send(const gameboy::LinkPacket& packet) noexcept override;
    [[nodiscard]] std::optional<gameboy::LinkPacket> receive() noexcept override;
    [[nodiscard]] State state() const noexcept override;
    [[nodiscard]] std::size_t queued_packets() const noexcept override;
    [[nodiscard]] std::size_t buffered_bytes() const noexcept override;
    [[nodiscard]] std::uint64_t malformed_packets() const noexcept override;

  private:
    gameboy::LinkPacketChannel& underlying_;
    FaultDirection direction_;
    FaultPlan& plan_;
    ScenarioTrace& trace_;
    std::optional<gameboy::LinkPacket> delayed_packet_;
    unsigned delay_polls_{};
};

[[nodiscard]] const char* fault_direction_name(
    FaultDirection direction) noexcept;

} // namespace gbb::link_harness
