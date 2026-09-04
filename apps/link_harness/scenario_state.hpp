#pragma once

#include "options.hpp"

#include <cstdint>

namespace gbb::link_harness {

// Per-scenario automation state is kept separate from the emulator and
// transport loop. This makes scripted input changes reviewable without
// touching link setup or report generation.
struct AutoInputState {
    bool first_link_choice_seen{};
    bool second_link_choice_seen{};
    bool first_battle_menu_moved{};
    bool second_battle_menu_moved{};
    bool first_battle_confirmed{};
    bool second_battle_confirmed{};
    bool battle_menu_started{};
    std::uint64_t first_battle_menu_frame{};
    std::uint64_t second_battle_menu_frame{};
    std::uint64_t first_battle_down_frame{};
    std::uint64_t second_battle_down_frame{};
    std::uint64_t first_battle_a_frame{};
    std::uint64_t second_battle_a_frame{};
    bool first_table_started{};
    bool second_table_started{};
    bool first_table_positioned{};
    bool second_table_positioned{};
    bool first_table_confirmed{};
    bool second_table_confirmed{};
    bool host_table_retry_sent{};
    bool table_facing_assisted{};
    bool peer_table_facing_assisted{};
    std::uint64_t first_table_frame{};
    std::uint64_t second_table_frame{};
    std::uint64_t table_facing_frame{};
    std::uint64_t first_table_a_frame{};
    std::uint64_t second_table_a_frame{};
    bool first_trade_selected{};
    bool second_trade_selected{};
    bool first_trade_choice_confirmed{};
    bool second_trade_choice_confirmed{};
    bool first_trade_right_sent{};
    bool second_trade_right_sent{};
    bool first_trade_confirmed{};
    bool second_trade_confirmed{};
    std::uint64_t first_trade_selection_frame{};
    std::uint64_t second_trade_selection_frame{};
    std::uint64_t first_trade_choice_frame{};
    std::uint64_t second_trade_choice_frame{};
    std::uint64_t first_trade_right_frame{};
    std::uint64_t second_trade_right_frame{};
    std::uint64_t first_trade_stats_a_frame{};
    std::uint64_t second_trade_stats_a_frame{};
    std::uint64_t first_trade_confirm_frame{};
    std::uint64_t second_trade_confirm_frame{};
};

[[nodiscard]] std::uint32_t trade_input_phase_mask(
    const AutoInputState& input_state) noexcept;

[[nodiscard]] const char* scenario_name(Scenario scenario) noexcept;

} // namespace gbb::link_harness
