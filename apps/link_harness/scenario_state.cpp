#include "scenario_state.hpp"

namespace gbb::link_harness {

std::uint32_t trade_input_phase_mask(
    const AutoInputState& input_state) noexcept {
    return (input_state.first_trade_selected ? 1U : 0U) |
           (input_state.second_trade_selected ? 1U << 1 : 0U) |
           (input_state.first_trade_right_sent ? 1U << 2 : 0U) |
           (input_state.second_trade_right_sent ? 1U << 3 : 0U) |
           (input_state.first_trade_stats_a_frame ? 1U << 4 : 0U) |
           (input_state.second_trade_stats_a_frame ? 1U << 5 : 0U) |
           (input_state.first_trade_confirmed ? 1U << 6 : 0U) |
           (input_state.second_trade_confirmed ? 1U << 7 : 0U);
}

const char* scenario_name(const Scenario scenario) noexcept {
    switch (scenario) {
    case Scenario::trade: return "trade";
    case Scenario::battle: return "battle";
    case Scenario::none: return "none";
    }
    return "none";
}

} // namespace gbb::link_harness
