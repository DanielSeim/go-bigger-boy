#include "semantic_tracker.hpp"

#include "harness_io.hpp"

#include <iomanip>
#include <sstream>
#include <string>
#include <utility>

namespace gbb::link_harness {
namespace {

constexpr std::uint8_t link_state_battling = 0x04;
constexpr std::uint8_t link_state_trading = 0x32;

bool same_party(const PartySnapshot& first, const PartySnapshot& second) {
    if (!first.valid || !second.valid || first.count != second.count) return false;
    for (std::size_t index = 0; index < first.count; ++index) {
        if (first.signatures[index] != second.signatures[index]) return false;
    }
    return true;
}

bool contains_mon(const PartySnapshot& party, const std::uint64_t signature) {
    if (!party.valid) return false;
    for (std::size_t index = 0; index < party.count; ++index) {
        if (party.signatures[index] == signature) return true;
    }
    return false;
}

bool contains_party_member_from(const PartySnapshot& destination,
                                const PartySnapshot& source) {
    if (!destination.valid || !source.valid) return false;
    for (std::size_t index = 0; index < source.count; ++index) {
        if (contains_mon(destination, source.signatures[index])) return true;
    }
    return false;
}

bool plausible_link_state(const std::uint8_t state) {
    return state <= 0x05 || state == link_state_trading;
}

std::uint8_t effective_link_state(const std::uint8_t primary,
                                  const std::uint8_t alternate) {
    return plausible_link_state(primary) ? primary : alternate;
}

std::string party_text(const PartySnapshot& party) {
    if (!party.valid) return "unavailable";
    std::ostringstream output;
    output << static_cast<unsigned>(party.count) << ':';
    for (std::size_t index = 0; index < party.count; ++index) {
        if (index != 0) output << ',';
        output << std::hex << std::setfill('0') << std::setw(2)
               << static_cast<unsigned>(party.species[index]) << '/'
               << std::setw(4) << party.ot_ids[index];
    }
    return output.str();
}

} // namespace

SemanticTracker::SemanticTracker(PartySnapshot first, PartySnapshot second)
    : initial_first(std::move(first)), initial_second(std::move(second)),
      final_first(initial_first), final_second(initial_second) {}

void SemanticTracker::sample(const SemanticSample& sample) {
    final_first = sample.first_party;
    final_second = sample.second_party;
    first_link_state = sample.first_link_state;
    second_link_state = sample.second_link_state;
    first_link_state_localized = sample.first_link_state_localized;
    second_link_state_localized = sample.second_link_state_localized;
    first_battle_state = sample.first_battle_state;
    second_battle_state = sample.second_battle_state;
    first_battle_state_localized = sample.first_battle_state_localized;
    second_battle_state_localized = sample.second_battle_state_localized;
    first_map_localized = sample.first_map_localized;
    second_map_localized = sample.second_map_localized;
    first_battle_trade_menu_seen =
        first_battle_trade_menu_seen || sample.first_battle_trade_menu;
    second_battle_trade_menu_seen =
        second_battle_trade_menu_seen || sample.second_battle_trade_menu;
    first_trade_selection_menu_seen =
        first_trade_selection_menu_seen || sample.first_trade_selection_menu;
    second_trade_selection_menu_seen =
        second_trade_selection_menu_seen || sample.second_trade_selection_menu;
    first_trade_stats_menu_seen =
        first_trade_stats_menu_seen || sample.first_trade_stats_menu;
    second_trade_stats_menu_seen =
        second_trade_stats_menu_seen || sample.second_trade_stats_menu;
    first_trade_cancel_menu_seen =
        first_trade_cancel_menu_seen || sample.first_trade_cancel_menu;
    second_trade_cancel_menu_seen =
        second_trade_cancel_menu_seen || sample.second_trade_cancel_menu;

    const auto first_effective =
        effective_link_state(first_link_state, first_link_state_localized);
    const auto second_effective =
        effective_link_state(second_link_state, second_link_state_localized);
    first_battle_seen = first_battle_seen || first_effective == link_state_battling ||
                        (first_battle_state != 0 && first_battle_state != 0xFF) ||
                        (first_battle_state_localized != 0 &&
                         first_battle_state_localized != 0xFF);
    second_battle_seen = second_battle_seen || second_effective == link_state_battling ||
                         (second_battle_state != 0 && second_battle_state != 0xFF) ||
                         (second_battle_state_localized != 0 &&
                          second_battle_state_localized != 0xFF);
}

bool SemanticTracker::trade_observed() const {
    return first_party_changed() && second_party_changed() &&
           contains_party_member_from(final_first, initial_second) &&
           contains_party_member_from(final_second, initial_first);
}

bool SemanticTracker::battle_observed() const {
    return first_battle_seen && second_battle_seen;
}

bool SemanticTracker::first_party_changed() const {
    return initial_first.valid && final_first.valid &&
           !same_party(initial_first, final_first);
}

bool SemanticTracker::second_party_changed() const {
    return initial_second.valid && final_second.valid &&
           !same_party(initial_second, final_second);
}

const char* expectation_name(const Expectation expectation) noexcept {
    switch (expectation) {
    case Expectation::trade: return "trade";
    case Expectation::battle: return "battle";
    case Expectation::none: return "none";
    }
    return "none";
}

bool expectation_satisfied(const SemanticTracker& tracker,
                           const Expectation expectation) noexcept {
    switch (expectation) {
    case Expectation::none: return true;
    case Expectation::trade: return tracker.trade_observed();
    case Expectation::battle: return tracker.battle_observed();
    }
    return false;
}

const char* semantic_failure(const SemanticTracker& tracker,
                             const Expectation expectation) noexcept {
    if (expectation_satisfied(tracker, expectation)) return "none";
    if (expectation == Expectation::trade) {
        if (!tracker.initial_first.valid || !tracker.initial_second.valid)
            return "initial_party_snapshot_unavailable";
        if (!tracker.final_first.valid || !tracker.final_second.valid)
            return "final_party_snapshot_unavailable";
        if (!tracker.first_party_changed() || !tracker.second_party_changed()) {
            if (tracker.first_battle_trade_menu_seen ||
                tracker.second_battle_trade_menu_seen ||
                tracker.first_trade_selection_menu_seen ||
                tracker.second_trade_selection_menu_seen ||
                tracker.first_trade_stats_menu_seen ||
                tracker.second_trade_stats_menu_seen ||
                tracker.first_trade_cancel_menu_seen ||
                tracker.second_trade_cancel_menu_seen)
                return "trade_not_completed_after_menu";
            return "party_snapshots_unchanged";
        }
        return "no_cross_party_record_observed";
    }
    if (!tracker.first_battle_seen && !tracker.second_battle_seen) {
        if (tracker.first_battle_trade_menu_seen &&
            tracker.second_battle_trade_menu_seen)
            return "battle_not_started_after_menu";
        return "neither_player_entered_battle";
    }
    if (!tracker.first_battle_seen) return "player1_did_not_enter_battle";
    return "player2_did_not_enter_battle";
}

void append_semantic_report(std::ostream& report,
                            const SemanticTracker& tracker,
                            const Expectation expectation) {
    report << "expectation=" << expectation_name(expectation) << '\n'
           << "party1_before=" << party_text(tracker.initial_first) << '\n'
           << "party1_after=" << party_text(tracker.final_first) << '\n'
           << "party2_before=" << party_text(tracker.initial_second) << '\n'
           << "party2_after=" << party_text(tracker.final_second) << '\n'
           << "party1_changed=" << (tracker.first_party_changed() ? "yes" : "no") << '\n'
           << "party2_changed=" << (tracker.second_party_changed() ? "yes" : "no") << '\n'
           << "trade_observed=" << (tracker.trade_observed() ? "yes" : "no") << '\n'
           << "battle_observed=" << (tracker.battle_observed() ? "yes" : "no") << '\n'
           << "expectation_satisfied="
           << (expectation_satisfied(tracker, expectation) ? "yes" : "no") << '\n'
           << "semantic_failure=" << semantic_failure(tracker, expectation) << '\n'
           << "player1_battle_seen=" << (tracker.first_battle_seen ? "yes" : "no") << '\n'
           << "player2_battle_seen=" << (tracker.second_battle_seen ? "yes" : "no") << '\n'
           << "player1_battle_trade_menu_seen="
           << (tracker.first_battle_trade_menu_seen ? "yes" : "no") << '\n'
           << "player2_battle_trade_menu_seen="
           << (tracker.second_battle_trade_menu_seen ? "yes" : "no") << '\n'
           << "player1_trade_selection_menu_seen="
           << (tracker.first_trade_selection_menu_seen ? "yes" : "no") << '\n'
           << "player2_trade_selection_menu_seen="
           << (tracker.second_trade_selection_menu_seen ? "yes" : "no") << '\n'
           << "player1_trade_stats_menu_seen="
           << (tracker.first_trade_stats_menu_seen ? "yes" : "no") << '\n'
           << "player2_trade_stats_menu_seen="
           << (tracker.second_trade_stats_menu_seen ? "yes" : "no") << '\n'
           << "player1_trade_cancel_menu_seen="
           << (tracker.first_trade_cancel_menu_seen ? "yes" : "no") << '\n'
           << "player2_trade_cancel_menu_seen="
           << (tracker.second_trade_cancel_menu_seen ? "yes" : "no") << '\n'
           << "player1_link_state_final=" << hex(tracker.first_link_state) << '\n'
           << "player2_link_state_final=" << hex(tracker.second_link_state) << '\n'
           << "player1_link_state_localized_final="
           << hex(tracker.first_link_state_localized) << '\n'
           << "player2_link_state_localized_final="
           << hex(tracker.second_link_state_localized) << '\n'
           << "player1_battle_state_final=" << hex(tracker.first_battle_state) << '\n'
           << "player2_battle_state_final=" << hex(tracker.second_battle_state) << '\n'
           << "player1_battle_state_localized_final="
           << hex(tracker.first_battle_state_localized) << '\n'
           << "player2_battle_state_localized_final="
           << hex(tracker.second_battle_state_localized) << '\n'
           << "player1_map_localized_final=" << hex(tracker.first_map_localized) << '\n'
           << "player2_map_localized_final=" << hex(tracker.second_map_localized) << '\n';
}

} // namespace gbb::link_harness
