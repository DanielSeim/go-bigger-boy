#include "pokemon_automation.hpp"

#include "pokemon_state.hpp"

#include <algorithm>
#include <cstdint>

namespace gbb::link_harness {

void apply_auto_inputs(const Options& options, const std::uint64_t frame,
                       gameboy::Emulator& first, gameboy::Emulator& second,
                       AutoInputState& input_state) {
    if (!options.auto_confirm && options.scenario == Scenario::none) return;
    WramBank1Guard first_bank(first);
    WramBank1Guard second_bank(second);
    constexpr std::uint64_t confirm_interval = 24;
    if (!options.state1.empty()) {
        const auto scenario_start_delay = options.scenario == Scenario::none ? 24 : 120;
        if (options.scenario != Scenario::none) {
            constexpr std::uint64_t peer_table_delay = 120;
            input_state.first_link_choice_seen =
                input_state.first_link_choice_seen || at_battle_trade_menu(first);
            input_state.second_link_choice_seen =
                input_state.second_link_choice_seen || at_battle_trade_menu(second);
            if (options.scenario == Scenario::trade) {
                // States captured with the Cable Club trade/battle choice
                // already open must be confirmed before any movement input.
                // Otherwise the generic table bootstrap below can consume the
                // menu's first frame and desynchronise the two guests.
                const auto first_choice = !input_state.first_table_started &&
                                          at_ready_link_choice(first);
                const auto second_choice = !input_state.second_table_started &&
                                           at_ready_link_choice(second);
                if (first_choice && !input_state.first_trade_choice_confirmed) {
                    select_joypad_lines(first, true);
                    first.set_button(gameboy::Button::a, true);
                    input_state.first_trade_choice_confirmed = true;
                    input_state.first_trade_choice_frame = frame;
                }
                if (second_choice && !input_state.second_trade_choice_confirmed) {
                    select_joypad_lines(second, true);
                    second.set_button(gameboy::Button::a, true);
                    input_state.second_trade_choice_confirmed = true;
                    input_state.second_trade_choice_frame = frame;
                }
                if ((input_state.first_trade_choice_confirmed &&
                     frame < input_state.first_trade_choice_frame + 6) ||
                    (input_state.second_trade_choice_confirmed &&
                     frame < input_state.second_trade_choice_frame + 6)) {
                    if (input_state.first_trade_choice_confirmed &&
                        frame < input_state.first_trade_choice_frame + 6) {
                        select_joypad_lines(first, true);
                        first.set_button(gameboy::Button::a, true);
                    }
                    if (input_state.second_trade_choice_confirmed &&
                        frame < input_state.second_trade_choice_frame + 6) {
                        select_joypad_lines(second, true);
                        second.set_button(gameboy::Button::a, true);
                    }
                    return;
                }
            }
            const auto first_club = at_cable_club_map(first);
            const auto second_club = at_cable_club_map(second);
            const auto choice_flow_started =
                input_state.first_trade_choice_confirmed ||
                input_state.second_trade_choice_confirmed;
            const auto link_choice_seen = input_state.first_link_choice_seen ||
                                          input_state.second_link_choice_seen;
            if (first_club && !link_choice_seen && !choice_flow_started &&
                !input_state.first_table_started) {
                select_joypad_lines(first, false);
                first.set_button(gameboy::Button::right, true);
                input_state.first_table_started = true;
                input_state.first_table_frame = frame;
                return;
            }
            if (second_club && !link_choice_seen && !choice_flow_started &&
                !input_state.second_table_started &&
                input_state.first_table_started &&
                frame >= input_state.first_table_frame + peer_table_delay) {
                select_joypad_lines(second, false);
                second.set_button(gameboy::Button::left, true);
                input_state.second_table_started = true;
                input_state.second_table_frame = frame;
                return;
            }
            if (input_state.first_table_started &&
                !input_state.first_table_positioned) {
                const auto x = player_map_x(first);
                if (x == 0x03) {
                    input_state.first_table_positioned = true;
                } else {
                    first.set_button(gameboy::Button::right, false);
                    first.set_button(gameboy::Button::left, false);
                    select_joypad_lines(first, false);
                    first.set_button(x < 0x03 ? gameboy::Button::right
                                              : gameboy::Button::left,
                                     true);
                    return;
                }
            }
            if (input_state.second_table_started &&
                !input_state.second_table_positioned) {
                const auto x = player_map_x(second);
                if (x == 0x06) {
                    input_state.second_table_positioned = true;
                } else {
                    second.set_button(gameboy::Button::right, false);
                    second.set_button(gameboy::Button::left, false);
                    select_joypad_lines(second, false);
                    second.set_button(x < 0x06 ? gameboy::Button::right
                                               : gameboy::Button::left,
                                      true);
                    return;
                }
            }
            first.set_button(gameboy::Button::right, false);
            second.set_button(gameboy::Button::left, false);
            first.set_button(gameboy::Button::a, false);
            second.set_button(gameboy::Button::a, false);
            const auto link_ready =
                effective_link_state(first.bus().read8(w_link_state),
                                     first.bus().read8(w_link_state_localized)) == 1 &&
                effective_link_state(second.bus().read8(w_link_state),
                                     second.bus().read8(w_link_state_localized)) == 1;
            if (input_state.first_table_started &&
                input_state.first_table_positioned &&
                !input_state.first_table_confirmed &&
                link_ready &&
                frame >= input_state.first_table_frame + 4) {
                set_facing_direction(first, 0x0C);
                if (input_state.second_table_positioned) {
                    set_facing_direction(second, 0x08);
                }
                if (!input_state.table_facing_assisted) {
                    input_state.table_facing_assisted = true;
                    input_state.table_facing_frame = frame;
                    return;
                }
                if (frame < input_state.table_facing_frame + 2) return;
                select_joypad_lines(first, true);
                first.set_button(gameboy::Button::a, true);
                input_state.first_table_confirmed = true;
                input_state.first_table_a_frame = frame;
                return;
            }
            if (input_state.first_table_confirmed &&
                !input_state.second_table_confirmed) {
                if (frame < input_state.first_table_a_frame + 6) {
                    select_joypad_lines(first, true);
                    first.set_button(gameboy::Button::a, true);
                    return;
                }
                if (input_state.second_table_positioned &&
                    frame >= input_state.first_table_a_frame + 8) {
                    set_facing_direction(second, 0x08);
                    if (!input_state.peer_table_facing_assisted) {
                        input_state.peer_table_facing_assisted = true;
                        return;
                    }
                    if (!input_state.host_table_retry_sent) {
                        select_joypad_lines(first, true);
                        first.set_button(gameboy::Button::a, true);
                        input_state.host_table_retry_sent = true;
                        return;
                    }
                    select_joypad_lines(second, true);
                    second.set_button(gameboy::Button::a, true);
                    input_state.second_table_confirmed = true;
                    input_state.second_table_a_frame = frame;
                }
                return;
            }
            if (input_state.second_table_confirmed &&
                frame < input_state.second_table_a_frame + 6) {
                select_joypad_lines(second, true);
                second.set_button(gameboy::Button::a, true);
                return;
            }
            if (options.scenario == Scenario::trade) {
                // TradeCenter first presents each player's party list. Select
                // the first monster, switch from STATS to TRADE, then accept
                // the trade confirmation once the game's 100-frame exchange
                // delay has elapsed. Drive each side independently: the game
                // can reach a menu several frames before its peer.
                first.set_button(gameboy::Button::right, false);
                second.set_button(gameboy::Button::right, false);
                first.set_button(gameboy::Button::a, false);
                second.set_button(gameboy::Button::a, false);

                const auto first_selection = at_trade_selection_menu(first);
                const auto second_selection = at_trade_selection_menu(second);
                if (first_selection && !input_state.first_trade_selected) {
                    select_joypad_lines(first, true);
                    first.set_button(gameboy::Button::a, true);
                    input_state.first_trade_selected = true;
                    input_state.first_trade_selection_frame = frame;
                }
                if (second_selection && !input_state.second_trade_selected) {
                    select_joypad_lines(second, true);
                    second.set_button(gameboy::Button::a, true);
                    input_state.second_trade_selected = true;
                    input_state.second_trade_selection_frame = frame;
                }
                if ((input_state.first_trade_selected &&
                     frame < input_state.first_trade_selection_frame + 6) ||
                    (input_state.second_trade_selected &&
                     frame < input_state.second_trade_selection_frame + 6)) {
                    if (input_state.first_trade_selected &&
                        frame < input_state.first_trade_selection_frame + 6) {
                        select_joypad_lines(first, true);
                        first.set_button(gameboy::Button::a, true);
                    }
                    if (input_state.second_trade_selected &&
                        frame < input_state.second_trade_selection_frame + 6) {
                        select_joypad_lines(second, true);
                        second.set_button(gameboy::Button::a, true);
                    }
                    return;
                }

                if (input_state.first_trade_selected &&
                    !input_state.first_trade_right_sent &&
                    at_trade_stats_menu(first)) {
                    select_joypad_lines(first, true);
                    first.set_button(gameboy::Button::right, true);
                    input_state.first_trade_right_sent = true;
                    input_state.first_trade_right_frame = frame;
                }
                if (input_state.second_trade_selected &&
                    !input_state.second_trade_right_sent &&
                    at_trade_stats_menu(second)) {
                    select_joypad_lines(second, true);
                    second.set_button(gameboy::Button::right, true);
                    input_state.second_trade_right_sent = true;
                    input_state.second_trade_right_frame = frame;
                }
                if ((input_state.first_trade_right_sent &&
                     frame < input_state.first_trade_right_frame + 6) ||
                    (input_state.second_trade_right_sent &&
                     frame < input_state.second_trade_right_frame + 6)) {
                    if (input_state.first_trade_right_sent &&
                        frame < input_state.first_trade_right_frame + 6) {
                        select_joypad_lines(first, true);
                        first.set_button(gameboy::Button::right, true);
                    }
                    if (input_state.second_trade_right_sent &&
                        frame < input_state.second_trade_right_frame + 6) {
                        select_joypad_lines(second, true);
                        second.set_button(gameboy::Button::right, true);
                    }
                    return;
                }

                if (input_state.first_trade_right_sent &&
                    !input_state.first_trade_stats_a_frame &&
                    frame >= input_state.first_trade_right_frame + 6) {
                    select_joypad_lines(first, true);
                    first.set_button(gameboy::Button::a, true);
                    input_state.first_trade_stats_a_frame = frame;
                }
                if (input_state.second_trade_right_sent &&
                    !input_state.second_trade_stats_a_frame &&
                    frame >= input_state.second_trade_right_frame + 6) {
                    select_joypad_lines(second, true);
                    second.set_button(gameboy::Button::a, true);
                    input_state.second_trade_stats_a_frame = frame;
                }
                if ((input_state.first_trade_stats_a_frame &&
                     frame < input_state.first_trade_stats_a_frame + 6) ||
                    (input_state.second_trade_stats_a_frame &&
                     frame < input_state.second_trade_stats_a_frame + 6)) {
                    if (input_state.first_trade_stats_a_frame &&
                        frame < input_state.first_trade_stats_a_frame + 6) {
                        select_joypad_lines(first, true);
                        first.set_button(gameboy::Button::a, true);
                    }
                    if (input_state.second_trade_stats_a_frame &&
                        frame < input_state.second_trade_stats_a_frame + 6) {
                        select_joypad_lines(second, true);
                        second.set_button(gameboy::Button::a, true);
                    }
                    return;
                }

                const auto first_cancel = at_trade_cancel_menu(first);
                const auto second_cancel = at_trade_cancel_menu(second);
                if (input_state.first_trade_stats_a_frame &&
                    !input_state.first_trade_confirmed &&
                    (first_cancel ||
                     frame >= input_state.first_trade_stats_a_frame + 130)) {
                    select_joypad_lines(first, true);
                    first.set_button(gameboy::Button::a, true);
                    input_state.first_trade_confirmed = true;
                    input_state.first_trade_confirm_frame = frame;
                }
                if (input_state.second_trade_stats_a_frame &&
                    !input_state.second_trade_confirmed &&
                    (second_cancel ||
                     frame >= input_state.second_trade_stats_a_frame + 130)) {
                    select_joypad_lines(second, true);
                    second.set_button(gameboy::Button::a, true);
                    input_state.second_trade_confirmed = true;
                    input_state.second_trade_confirm_frame = frame;
                }
                if ((input_state.first_trade_confirmed &&
                     frame < input_state.first_trade_confirm_frame + 6) ||
                    (input_state.second_trade_confirmed &&
                     frame < input_state.second_trade_confirm_frame + 6)) {
                    if (input_state.first_trade_confirmed &&
                        frame < input_state.first_trade_confirm_frame + 6) {
                        select_joypad_lines(first, true);
                        first.set_button(gameboy::Button::a, true);
                    }
                    if (input_state.second_trade_confirmed &&
                        frame < input_state.second_trade_confirm_frame + 6) {
                        select_joypad_lines(second, true);
                        second.set_button(gameboy::Button::a, true);
                    }
                    return;
                }
            }
            if (options.scenario == Scenario::battle) {
                const auto menu_ready = frame >= scenario_start_delay;
                const auto first_menu = menu_ready && at_ready_link_choice(first);
                const auto second_menu = menu_ready && at_ready_link_choice(second);
                first.set_button(gameboy::Button::a, false);
                second.set_button(gameboy::Button::a, false);
                input_state.battle_menu_started =
                    input_state.battle_menu_started || first_menu || second_menu;
                if (first_menu && !input_state.first_battle_menu_moved) {
                    select_joypad_lines(first, false);
                    first.set_button(gameboy::Button::down, true);
                    input_state.first_battle_menu_moved = true;
                    input_state.first_battle_menu_frame = frame;
                    input_state.first_battle_down_frame = frame;
                    return;
                }
                if (second_menu && !input_state.second_battle_menu_moved) {
                    select_joypad_lines(second, false);
                    second.set_button(gameboy::Button::down, true);
                    input_state.second_battle_menu_moved = true;
                    input_state.second_battle_menu_frame = frame;
                    input_state.second_battle_down_frame = frame;
                    return;
                }
                if (input_state.first_battle_menu_moved &&
                    !input_state.first_battle_confirmed &&
                    frame < input_state.first_battle_down_frame + 6) {
                    select_joypad_lines(first, false);
                    first.set_button(gameboy::Button::down, true);
                    return;
                }
                if (input_state.second_battle_menu_moved &&
                    !input_state.second_battle_confirmed &&
                    frame < input_state.second_battle_down_frame + 6) {
                    select_joypad_lines(second, false);
                    second.set_button(gameboy::Button::down, true);
                    return;
                }
                first.set_button(gameboy::Button::down, false);
                second.set_button(gameboy::Button::down, false);
                if (input_state.first_battle_confirmed &&
                    frame < input_state.first_battle_a_frame + 6) {
                    select_joypad_lines(first, true);
                    first.set_button(gameboy::Button::a, true);
                }
                if (input_state.second_battle_confirmed &&
                    frame < input_state.second_battle_a_frame + 6) {
                    select_joypad_lines(second, true);
                    second.set_button(gameboy::Button::a, true);
                }
                if (first_menu && second_menu &&
                    input_state.first_battle_menu_moved &&
                    input_state.second_battle_menu_moved &&
                    !input_state.first_battle_confirmed &&
                    !input_state.second_battle_confirmed &&
                    frame >= std::max(input_state.first_battle_down_frame,
                                      input_state.second_battle_down_frame) + 8) {
                    select_joypad_lines(first, true);
                    select_joypad_lines(second, true);
                    first.set_button(gameboy::Button::a, true);
                    second.set_button(gameboy::Button::a, true);
                    input_state.first_battle_confirmed = true;
                    input_state.second_battle_confirmed = true;
                    input_state.first_battle_a_frame = frame;
                    input_state.second_battle_a_frame = frame;
                    return;
                }
            }
        }
        const auto bootstrap_scenario =
            options.scenario != Scenario::none &&
            !input_state.first_table_started &&
            !input_state.second_table_started &&
            !input_state.battle_menu_started &&
            !input_state.first_trade_choice_confirmed &&
            !input_state.second_trade_choice_confirmed &&
            !input_state.first_trade_selected &&
            !input_state.second_trade_selected;
        if ((options.scenario == Scenario::none || bootstrap_scenario) &&
            frame % confirm_interval == 0) {
            if (options.scenario != Scenario::none &&
                !at_cable_club_map(first) && player_map_x(first) == 0x0B &&
                player_map_y(first) == 0x03) {
                set_facing_direction(first, 0x04);
            }
            select_joypad_lines(first, true);
            first.set_button(gameboy::Button::a, true);
            if (frame >= scenario_start_delay) {
                if (options.scenario != Scenario::none &&
                    !at_cable_club_map(second) && player_map_x(second) == 0x0B &&
                    player_map_y(second) == 0x03) {
                    set_facing_direction(second, 0x04);
                }
                select_joypad_lines(second, true);
                second.set_button(gameboy::Button::a, true);
            }
        } else if (frame % confirm_interval == 1) {
            first.set_button(gameboy::Button::a, false);
            if (frame >= scenario_start_delay) second.set_button(gameboy::Button::a, false);
        }
        return;
    }
    // A battery save contains cartridge RAM, not CPU/WRAM state. Without a
    // full state, start from post-boot and enter the title menu first.
    if (frame == 180) {
        first.set_button(gameboy::Button::start, true);
        second.set_button(gameboy::Button::start, true);
    } else if (frame == 181) {
        first.set_button(gameboy::Button::start, false);
        second.set_button(gameboy::Button::start, false);
    } else if (frame == 186) {
        first.set_button(gameboy::Button::down, true);
        second.set_button(gameboy::Button::down, true);
    } else if (frame == 187) {
        first.set_button(gameboy::Button::down, false);
        second.set_button(gameboy::Button::down, false);
    } else if (frame == 192) {
        first.set_button(gameboy::Button::a, true);
    } else if (frame == 193) {
        first.set_button(gameboy::Button::a, false);
    } else if (frame == 216) {
        second.set_button(gameboy::Button::a, true);
    } else if (frame == 217) {
        second.set_button(gameboy::Button::a, false);
    } else if (frame >= 300 &&
               (frame - 300) % confirm_interval == 0) {
        first.set_button(gameboy::Button::a, true);
        if ((frame - 300) >= 24) second.set_button(gameboy::Button::a, true);
    } else if (frame >= 300 &&
               (frame - 300) % confirm_interval == 1) {
        first.set_button(gameboy::Button::a, false);
        if ((frame - 300) >= 24) second.set_button(gameboy::Button::a, false);
    }
}


void append_auto_input_report(std::ostream& report,
                              const AutoInputState& input_state) {
    report << "auto_player1_menu_frame=" << input_state.first_battle_menu_frame << '\n'
           << "auto_player2_menu_frame=" << input_state.second_battle_menu_frame << '\n'
           << "auto_player1_down_frame=" << input_state.first_battle_down_frame << '\n'
           << "auto_player2_down_frame=" << input_state.second_battle_down_frame << '\n'
           << "auto_player1_a_frame=" << input_state.first_battle_a_frame << '\n'
           << "auto_player2_a_frame=" << input_state.second_battle_a_frame << '\n'
           << "auto_player1_table_frame=" << input_state.first_table_frame << '\n'
           << "auto_player2_table_frame=" << input_state.second_table_frame << '\n'
           << "auto_player1_table_positioned="
           << (input_state.first_table_positioned ? "yes" : "no") << '\n'
           << "auto_player2_table_positioned="
           << (input_state.second_table_positioned ? "yes" : "no") << '\n'
           << "auto_player1_table_a_frame=" << input_state.first_table_a_frame << '\n'
           << "auto_player2_table_a_frame=" << input_state.second_table_a_frame << '\n'
           << "auto_table_facing_assisted="
           << (input_state.table_facing_assisted ? "yes" : "no") << '\n'
           << "auto_table_facing_frame=" << input_state.table_facing_frame << '\n'
           << "auto_player1_trade_selection_frame="
           << input_state.first_trade_selection_frame << '\n'
           << "auto_player2_trade_selection_frame="
           << input_state.second_trade_selection_frame << '\n'
           << "auto_player1_trade_choice_frame="
           << input_state.first_trade_choice_frame << '\n'
           << "auto_player2_trade_choice_frame="
           << input_state.second_trade_choice_frame << '\n'
           << "auto_player1_trade_right_frame="
           << input_state.first_trade_right_frame << '\n'
           << "auto_player2_trade_right_frame="
           << input_state.second_trade_right_frame << '\n'
           << "auto_player1_trade_stats_a_frame="
           << input_state.first_trade_stats_a_frame << '\n'
           << "auto_player2_trade_stats_a_frame="
           << input_state.second_trade_stats_a_frame << '\n'
           << "auto_player1_trade_confirm_frame="
           << input_state.first_trade_confirm_frame << '\n'
           << "auto_player2_trade_confirm_frame="
           << input_state.second_trade_confirm_frame << '\n';
}

} // namespace gbb::link_harness


