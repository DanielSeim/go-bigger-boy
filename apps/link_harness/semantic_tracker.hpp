#pragma once

#include "options.hpp"

#include <array>
#include <cstdint>
#include <ostream>

namespace gbb::link_harness {

struct PartySnapshot {
    std::uint8_t count{};
    std::array<std::uint8_t, 6> species{};
    std::array<std::uint16_t, 6> ot_ids{};
    std::array<std::uint64_t, 6> signatures{};
    std::uint16_t base_address{};
    bool valid{};
};

// A harness-side semantic sample is deliberately detached from emulator
// memory. The frontend-specific probe code captures one of these records;
// the tracker then classifies outcomes without knowing WRAM addresses.
struct SemanticSample {
    PartySnapshot first_party;
    PartySnapshot second_party;
    std::uint8_t first_link_state{};
    std::uint8_t second_link_state{};
    std::uint8_t first_link_state_localized{};
    std::uint8_t second_link_state_localized{};
    std::uint8_t first_battle_state{};
    std::uint8_t second_battle_state{};
    std::uint8_t first_battle_state_localized{};
    std::uint8_t second_battle_state_localized{};
    std::uint8_t first_map_localized{};
    std::uint8_t second_map_localized{};
    bool first_battle_trade_menu{};
    bool second_battle_trade_menu{};
    bool first_trade_selection_menu{};
    bool second_trade_selection_menu{};
    bool first_trade_stats_menu{};
    bool second_trade_stats_menu{};
    bool first_trade_cancel_menu{};
    bool second_trade_cancel_menu{};
};

class SemanticTracker {
public:
    SemanticTracker(PartySnapshot first, PartySnapshot second);

    void sample(const SemanticSample& sample);

    [[nodiscard]] bool trade_observed() const;
    [[nodiscard]] bool battle_observed() const;
    [[nodiscard]] bool first_party_changed() const;
    [[nodiscard]] bool second_party_changed() const;

    PartySnapshot initial_first;
    PartySnapshot initial_second;
    PartySnapshot final_first;
    PartySnapshot final_second;
    bool first_battle_seen{};
    bool second_battle_seen{};
    bool first_battle_trade_menu_seen{};
    bool second_battle_trade_menu_seen{};
    bool first_trade_selection_menu_seen{};
    bool second_trade_selection_menu_seen{};
    bool first_trade_stats_menu_seen{};
    bool second_trade_stats_menu_seen{};
    bool first_trade_cancel_menu_seen{};
    bool second_trade_cancel_menu_seen{};
    std::uint8_t first_link_state{};
    std::uint8_t second_link_state{};
    std::uint8_t first_link_state_localized{};
    std::uint8_t second_link_state_localized{};
    std::uint8_t first_battle_state{};
    std::uint8_t second_battle_state{};
    std::uint8_t first_battle_state_localized{};
    std::uint8_t second_battle_state_localized{};
    std::uint8_t first_map_localized{};
    std::uint8_t second_map_localized{};
};

[[nodiscard]] const char* expectation_name(Expectation expectation) noexcept;
[[nodiscard]] bool expectation_satisfied(const SemanticTracker& tracker,
                                         Expectation expectation) noexcept;
[[nodiscard]] const char* semantic_failure(const SemanticTracker& tracker,
                                            Expectation expectation) noexcept;
void append_semantic_report(std::ostream& report,
                            const SemanticTracker& tracker,
                            Expectation expectation);

} // namespace gbb::link_harness
