#include "scenario_trace.hpp"

#include "pokemon_state.hpp"
#include "gbb/trace_format.hpp"

#include <iomanip>
#include <sstream>
#include <string>

namespace gbb::link_harness {

void append_trace_player(std::ostream& output,
                         gameboy::Emulator& emulator,
                         const unsigned player,
                         const gameboy::TcpSerialEndpoint* endpoint) {
    WramBank1Guard bank(emulator);
    const auto& bus = emulator.bus();
    const auto& serial = bus.serial_port();
    const auto& registers = emulator.cpu().registers();
    const auto prefix = std::string{"p"} + std::to_string(player) + '_';
    const auto field = [&](const char* name, const std::uint64_t value,
                           const bool hexadecimal = false) {
        output << ' ' << prefix << name << '=';
        if (hexadecimal) output << "0x" << std::hex;
        output << value;
        if (hexadecimal) output << std::dec;
    };
    field("cycles", emulator.cpu().total_cycles());
    field("pc", registers.pc, true);
    field("sp", registers.sp, true);
    field("a", registers.a, true);
    field("f", registers.f, true);
    field("b", registers.b, true);
    field("c", registers.c, true);
    field("d", registers.d, true);
    field("e", registers.e, true);
    field("h", registers.h, true);
    field("l", registers.l, true);
    field("halted", emulator.cpu().halted());
    field("stopped", emulator.cpu().stopped());
    field("ime", emulator.cpu().interrupts_enabled());
    field("serial_new_data", bus.read8(0xFFA9), true);
    field("hr", bus.read8(0xFFAA), true);
    field("rom_bank", bus.read8(0xFFB8), true);
    field("ab", bus.read8(0xFFAB), true);
    field("ac", bus.read8(0xFFAC), true);
    field("ad", bus.read8(0xFFAD), true);
    field("joyp", bus.read8(0xFF00), true);
    field("joy_pressed", bus.read8(0xFFB3), true);
    field("joy_held", bus.read8(0xFFB4), true);
    field("joy5", bus.read8(0xFFB5), true);
    field("lcdc", bus.read8(0xFF40), true);
    field("ly", bus.read8(0xFF44), true);
    field("vblank", bus.read8(0xFFD6), true);
    field("sb", serial.read_data(), true);
    field("sc", serial.read_control(), true);
    field("active", serial.transfer_active());
    field("internal", serial.internal_clock());
    field("fast", serial.fast_clock());
    field("bits", serial.bits_shifted());
    field("phase", serial.phase());
    field("done", serial.transfers_completed());
    field("tx", serial.last_transmitted(), true);
    field("rx", serial.last_received(), true);
    field("if", bus.read8(0xFF0F), true);
    field("ie", bus.read8(0xFFFF), true);
    field("link", bus.read8(w_link_state), true);
    field("link_alt", bus.read8(w_link_state_localized), true);
    field("battle", bus.read8(w_is_in_battle), true);
    field("battle_alt", bus.read8(w_is_in_battle_localized), true);
    field("map", bus.read8(w_cur_map), true);
    field("map_alt", bus.read8(static_cast<std::uint16_t>(w_cur_map + 5)), true);
    field("y", bus.read8(static_cast<std::uint16_t>(0xD361)), true);
    field("x", bus.read8(static_cast<std::uint16_t>(0xD362)), true);
    field("sprite_y", bus.read8(static_cast<std::uint16_t>(0xC10A)), true);
    field("sprite_x", bus.read8(static_cast<std::uint16_t>(0xC10B)), true);
    field("facing", bus.read8(static_cast<std::uint16_t>(w_player_direction)), true);
    field("facing_sprite", bus.read8(static_cast<std::uint16_t>(w_player_sprite_direction)), true);
    field("facing_sprite_alt", bus.read8(static_cast<std::uint16_t>(w_player_sprite_direction + 5)), true);
    field("y_alt", bus.read8(static_cast<std::uint16_t>(0xD361 + 5)), true);
    field("x_alt", bus.read8(static_cast<std::uint16_t>(0xD362 + 5)), true);
    field("facing_alt", bus.read8(static_cast<std::uint16_t>(w_player_direction + 5)), true);
    field("party", bus.read8(w_party_count));
    field("party_alt", bus.read8(static_cast<std::uint16_t>(w_party_count + 5)));
    field("g2_link_mode", bus.read8(g2_w_link_mode), true);
    field("g2_party", bus.read8(g2_w_party_count));
    field("g2_party_species_1", bus.read8(g2_w_party_species), true);
    field("menu", at_battle_trade_menu(emulator));
    field("trade_selection", at_trade_selection_menu(emulator));
    field("trade_stats", at_trade_stats_menu(emulator));
    field("trade_cancel", at_trade_cancel_menu(emulator));
    const auto battle = probe_battle(emulator);
    field("battle_active", battle.active);
    field("battle_link_mode", battle.link_mode, true);
    field("battle_just_started", battle.battle_just_started, true);
    field("battle_ended", battle.battle_ended, true);
    field("battle_mode", battle.battle_mode, true);
    field("battle_type", battle.battle_type, true);
    field("battle_mon", battle.current_mon, true);
    field("battle_action", battle.player_action, true);
    field("battle_mon_hp", battle.battle_mon_hp, true);
    field("enemy_mon_hp", battle.enemy_mon_hp, true);
    field("serial_nybble_send", bus.read8(w_serial_nybble_send), true);
    field("serial_nybble_receive", bus.read8(w_serial_nybble_receive), true);
    field("serial_nybble_temp", bus.read8(w_serial_nybble_temp), true);
    field("serial_sync_receive", bus.read8(w_serial_sync_receive), true);
    field("serial_unknown_counter", read16be(emulator, w_unknown_serial_counter), true);
    field("menu_y", bus.read8(w_top_menu_item_y), true);
    field("menu_x", bus.read8(w_top_menu_item_x), true);
    field("menu_current", bus.read8(static_cast<std::uint16_t>(w_top_menu_item_y + 2)));
    field("menu_max", bus.read8(static_cast<std::uint16_t>(w_top_menu_item_y + 4)));
    field("menu_keys", bus.read8(static_cast<std::uint16_t>(w_top_menu_item_y + 5)), true);
    field("menu_ptr", read16be(emulator,
                                static_cast<std::uint16_t>(w_top_menu_item_y + 12)),
          true);
    if (endpoint != nullptr) {
        field("q_sent", endpoint->requests_sent());
        field("q_recv", endpoint->requests_received());
        field("r_sent", endpoint->responses_sent());
        field("r_recv", endpoint->responses_received());
        field("deny_sent", endpoint->denials_sent());
        field("deny_recv", endpoint->denials_received());
        field("unmatched", endpoint->responses_unmatched());
        field("byte_sent", endpoint->byte_packets_sent());
        field("byte_recv", endpoint->byte_packets_received());
        field("byte_cap", endpoint->peer_byte_transfer());
        field("waiting", endpoint->waiting_for_peer());
        field("connected", endpoint->connected());
    }
}


namespace {
SerialOwnershipSnapshot serial_ownership(const gameboy::Emulator& emulator) {
    const auto& serial = emulator.bus().serial_port();
    return {serial.transfer_active(), serial.internal_clock(),
            serial.bits_shifted(), serial.read_control()};
}
} // namespace

ScenarioTrace::ScenarioTrace(const std::filesystem::path& path,
                             const std::string& transport,
                             const Scenario scenario)
    : writer_(path, transport, scenario_name(scenario)) {}

void ScenarioTrace::write_frame(
    const std::uint64_t frame, gameboy::Emulator& first,
    gameboy::Emulator& second, const AutoInputState& input_state,
    const gameboy::TcpSerialEndpoint* first_endpoint,
    const gameboy::TcpSerialEndpoint* second_endpoint,
    const char* session_state, const std::uint64_t session_transfers) {
    if (!writer_.enabled()) return;
    auto& output = writer_.stream();
    gbb::write_trace_event_prefix(output, "frame", writer_.session(), frame,
                                  writer_.elapsed_ms(), writer_.transport(),
                                  writer_.role());
    append_trace_player(output, first, 1, first_endpoint);
    append_trace_player(output, second, 2, second_endpoint);
    output << " auto_p1_table_started=" << input_state.first_table_started
             << " auto_p2_table_started=" << input_state.second_table_started
             << " auto_p1_table_positioned=" << input_state.first_table_positioned
             << " auto_p2_table_positioned=" << input_state.second_table_positioned
             << " auto_p1_table_confirmed=" << input_state.first_table_confirmed
             << " auto_p2_table_confirmed=" << input_state.second_table_confirmed
             << " auto_host_table_retry_sent=" << input_state.host_table_retry_sent
             << " auto_table_facing_assisted=" << input_state.table_facing_assisted
             << " auto_peer_table_facing_assisted="
             << input_state.peer_table_facing_assisted
             << " auto_p1_trade_selected=" << input_state.first_trade_selected
             << " auto_p2_trade_selected=" << input_state.second_trade_selected
             << " auto_p1_trade_choice=" << input_state.first_trade_choice_confirmed
             << " auto_p2_trade_choice=" << input_state.second_trade_choice_confirmed
             << " auto_p1_trade_right=" << input_state.first_trade_right_sent
             << " auto_p2_trade_right=" << input_state.second_trade_right_sent
             << " auto_p1_trade_confirmed=" << input_state.first_trade_confirmed
             << " auto_p2_trade_confirmed=" << input_state.second_trade_confirmed
             << " auto_p1_battle_moved=" << input_state.first_battle_menu_moved
             << " auto_p2_battle_moved=" << input_state.second_battle_menu_moved
             << " auto_p1_battle_confirmed=" << input_state.first_battle_confirmed
             << " auto_p2_battle_confirmed=" << input_state.second_battle_confirmed
             << " link_session=" << (session_state == nullptr ? "tcp" : session_state)
             << " session_transfers=" << session_transfers << '\n';
    write_battle_checkpoint(frame, first, second);
    writer_.checkpoint_frame(frame);
}

void ScenarioTrace::write_battle_checkpoint(const std::uint64_t frame,
                                            gameboy::Emulator& first,
                                            gameboy::Emulator& second) {
    if (!writer_.enabled()) return;
    WramBank1Guard first_bank(first);
    WramBank1Guard second_bank(second);
    const auto first_battle = probe_battle(first);
    const auto second_battle = probe_battle(second);
    const auto first_entered = !first_battle_seen_ && first_battle.active;
    const auto second_entered = !second_battle_seen_ && second_battle.active;
    const auto has_entry = first_battle_seen_ || second_battle_seen_ ||
                           first_battle.active || second_battle.active;
    if (!has_entry) return;

    const char* phase = "checkpoint";
    if (first_entered || second_entered) {
        phase = "entry";
    } else if (!battle_divergence_reported_ &&
               first_battle.active != second_battle.active &&
               (first_battle_seen_ || second_battle_seen_)) {
        phase = "first_divergence";
        battle_divergence_reported_ = true;
    } else if (frame < last_battle_checkpoint_frame_ + 30) {
        return;
    }

    auto& output = writer_.stream();
    gbb::write_trace_event_prefix(output, "battle_checkpoint", writer_.session(),
                                  frame, writer_.elapsed_ms(), writer_.transport(),
                                  writer_.role());
    output << " phase=" << phase;
    const auto append = [&](const char* prefix,
                            gameboy::Emulator& emulator,
                            const PokemonBattleSnapshot& battle) {
        const auto& registers = emulator.cpu().registers();
        const auto& serial = emulator.bus().serial_port();
        output << ' ' << prefix << "_active=" << battle.active
               << ' ' << prefix << "_generation="
               << static_cast<unsigned>(battle.generation)
               << ' ' << prefix << "_link_mode=0x" << std::hex
               << static_cast<unsigned>(battle.link_mode)
               << ' ' << prefix << "_just_started=0x"
               << static_cast<unsigned>(battle.battle_just_started)
               << ' ' << prefix << "_ended=0x"
               << static_cast<unsigned>(battle.battle_ended)
               << ' ' << prefix << "_mode=0x"
               << static_cast<unsigned>(battle.battle_mode)
               << ' ' << prefix << "_type=0x"
               << static_cast<unsigned>(battle.battle_type)
               << ' ' << prefix << "_mon=0x"
               << static_cast<unsigned>(battle.current_mon)
               << ' ' << prefix << "_action=0x"
               << static_cast<unsigned>(battle.player_action)
               << ' ' << prefix << "_player_hp=0x" << battle.battle_mon_hp
               << ' ' << prefix << "_enemy_hp=0x" << battle.enemy_mon_hp
               << ' ' << prefix << "_pc=0x" << registers.pc
               << ' ' << prefix << "_sp=0x" << registers.sp
               << ' ' << prefix << "_cycles=" << std::dec
               << emulator.cpu().total_cycles()
               << ' ' << prefix << "_serial_active=" << serial.transfer_active()
               << ' ' << prefix << "_serial_internal=" << serial.internal_clock()
               << ' ' << prefix << "_serial_bits="
               << static_cast<unsigned>(serial.bits_shifted())
               << ' ' << prefix << "_sb=0x" << std::hex
               << static_cast<unsigned>(serial.read_data())
               << ' ' << prefix << "_sc=0x"
               << static_cast<unsigned>(serial.read_control())
               << std::dec;
    };
    append("p1", first, first_battle);
    append("p2", second, second_battle);
    output << " first_seen=" << first_battle_seen_
           << " second_seen=" << second_battle_seen_ << '\n';
    writer_.flush();
    last_battle_checkpoint_frame_ = frame;
    first_battle_seen_ = first_battle_seen_ || first_battle.active;
    second_battle_seen_ = second_battle_seen_ || second_battle.active;
}

void ScenarioTrace::write_serial_event(
    const std::uint64_t frame, const char* event,
    gameboy::Emulator& first, gameboy::Emulator& second,
    const char* session_state, const std::uint64_t session_transfers,
    const std::uint64_t stalled_frames) {
    if (!writer_.enabled()) return;
    auto& output = writer_.stream();
    WramBank1Guard first_bank(first);
    WramBank1Guard second_bank(second);
    const auto first_serial = serial_ownership(first);
    const auto second_serial = serial_ownership(second);
    gbb::write_trace_event_prefix(output, event, writer_.session(), frame,
                                  writer_.elapsed_ms(), writer_.transport(),
                                  writer_.role());
    output << " link_session=" << (session_state == nullptr ? "tcp" : session_state)
            << " session_transfers=" << session_transfers
            << " stalled_frames=" << stalled_frames
            << " p1_active=" << first_serial.active
            << " p1_internal=" << first_serial.internal
            << " p1_bits=" << static_cast<unsigned>(first_serial.bits)
            << " p1_sc=0x" << std::hex
            << static_cast<unsigned>(first_serial.control)
            << " p2_active=" << std::dec << second_serial.active
            << " p2_internal=" << second_serial.internal
            << " p2_bits=" << static_cast<unsigned>(second_serial.bits)
            << " p2_sc=0x" << std::hex
            << static_cast<unsigned>(second_serial.control) << std::dec
            << " p1_link=0x" << std::hex
            << static_cast<unsigned>(first.bus().read8(w_link_state))
            << " p1_link_alt=0x"
            << static_cast<unsigned>(first.bus().read8(w_link_state_localized))
            << " p2_link=0x"
            << static_cast<unsigned>(second.bus().read8(w_link_state))
            << " p2_link_alt=0x"
            << static_cast<unsigned>(second.bus().read8(w_link_state_localized))
            << std::dec
            << " p1_pc=0x" << std::hex << first.cpu().registers().pc
            << " p1_sp=0x" << first.cpu().registers().sp
            << " p2_pc=0x" << second.cpu().registers().pc
            << " p2_sp=0x" << second.cpu().registers().sp
            << " p1_sb=0x" << static_cast<unsigned>(first.bus().read8(0xFF01))
            << " p2_sb=0x" << static_cast<unsigned>(second.bus().read8(0xFF01))
            << " p1_if=0x" << static_cast<unsigned>(first.bus().read8(0xFF0F))
            << " p2_if=0x" << static_cast<unsigned>(second.bus().read8(0xFF0F))
            << std::dec << '\n';
    writer_.flush();
}

void ScenarioTrace::write_trade_phase_event(
    const std::uint64_t frame, gameboy::Emulator& first,
    gameboy::Emulator& second, const AutoInputState& input_state,
    const char* session_state, const std::uint64_t session_transfers) {
    if (!writer_.enabled()) return;
    const auto phase_mask = trade_input_phase_mask(input_state);
    if (trade_phase_initialized_ && phase_mask == last_trade_phase_mask_) return;
    trade_phase_initialized_ = true;
    last_trade_phase_mask_ = phase_mask;
    WramBank1Guard first_bank(first);
    WramBank1Guard second_bank(second);
    const auto first_serial = serial_ownership(first);
    const auto second_serial = serial_ownership(second);
    auto& output = writer_.stream();
    gbb::write_trace_event_prefix(output, "trade_input_phase", writer_.session(),
                                  frame, writer_.elapsed_ms(), writer_.transport(),
                                  writer_.role());
    output << " phase_mask=0x" << std::hex << phase_mask
            << " link_session=" << (session_state == nullptr ? "tcp" : session_state)
            << std::dec << " session_transfers=" << session_transfers
            << " p1_pc=0x" << std::hex << first.cpu().registers().pc
            << " p1_sp=0x" << first.cpu().registers().sp
            << " p2_pc=0x" << second.cpu().registers().pc
            << " p2_sp=0x" << second.cpu().registers().sp
            << " p1_joy_pressed=0x"
            << static_cast<unsigned>(first.bus().read8(0xFFB3))
            << " p1_joy_held=0x"
            << static_cast<unsigned>(first.bus().read8(0xFFB4))
            << " p2_joy_pressed=0x"
            << static_cast<unsigned>(second.bus().read8(0xFFB3))
            << " p2_joy_held=0x"
            << static_cast<unsigned>(second.bus().read8(0xFFB4))
            << " p1_sc=0x" << static_cast<unsigned>(first_serial.control)
            << " p2_sc=0x" << static_cast<unsigned>(second_serial.control)
            << " p1_link=0x"
            << static_cast<unsigned>(first.bus().read8(w_link_state))
            << " p1_link_alt=0x"
            << static_cast<unsigned>(first.bus().read8(w_link_state_localized))
            << " p2_link=0x"
            << static_cast<unsigned>(second.bus().read8(w_link_state))
            << " p2_link_alt=0x"
            << static_cast<unsigned>(second.bus().read8(w_link_state_localized))
            << std::dec << '\n';
    writer_.flush();
}

void update_serial_progress_watchdog(
    ScenarioTrace& trace, const std::uint64_t frame,
    gameboy::Emulator& first, gameboy::Emulator& second,
    SerialProgressWatchdog& watchdog, const char* session_state,
    const std::uint64_t session_transfers, const bool watch_trade_stall) {
    WramBank1Guard first_bank(first);
    WramBank1Guard second_bank(second);
    const auto first_serial = serial_ownership(first);
    const auto second_serial = serial_ownership(second);
    const auto marker = session_transfers + first_serial.bits + second_serial.bits;
    const auto first_link = effective_link_state(
        first.bus().read8(w_link_state), first.bus().read8(w_link_state_localized));
    const auto second_link = effective_link_state(
        second.bus().read8(w_link_state), second.bus().read8(w_link_state_localized));
    const auto post_menu_transfer =
        watch_trade_stall && first_link == link_state_trading &&
        second_link == link_state_trading &&
        (first_serial.active || second_serial.active);
    const auto ownership_changed =
        watchdog.initialized &&
        (first_serial != watchdog.first_previous ||
         second_serial != watchdog.second_previous);
    if (ownership_changed) {
        ++watchdog.ownership_transitions;
        trace.write_serial_event(frame, "serial_ownership_transition", first,
                                 second, session_state, session_transfers);
    }
    if (!post_menu_transfer || !watchdog.initialized ||
        marker != watchdog.last_marker) {
        watchdog.stalled_frames = 0;
    } else {
        ++watchdog.stalled_frames;
    }
    watchdog.last_marker = marker;
    watchdog.first_previous = first_serial;
    watchdog.second_previous = second_serial;
    watchdog.initialized = true;

    constexpr std::uint64_t stall_threshold_frames = 120;
    if (!watchdog.stall_reported && watchdog.stalled_frames >= stall_threshold_frames &&
        post_menu_transfer) {
        watchdog.stall_reported = true;
        watchdog.stall_frame = frame;
        trace.write_serial_event(frame, "post_menu_serial_stall", first, second,
                                 session_state, session_transfers,
                                 watchdog.stalled_frames);
    }
}

} // namespace gbb::link_harness
