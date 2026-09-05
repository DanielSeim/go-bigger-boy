#include "gameboy/emulator.hpp"
#include "gameboy/gameboy_link_endpoint.hpp"
#include "gameboy/link_session.hpp"
#include "gameboy/tcp_link_channel.hpp"
#include "gameboy/tcp_serial_endpoint.hpp"
#include "harness_io.hpp"
#include "options.hpp"
#include "pokemon_automation.hpp"
#include "pokemon_state.hpp"
#include "semantic_tracker.hpp"
#include "scenario_runner.hpp"
#include "scenario_state.hpp"
#include "scenario_trace.hpp"
#include "gbb/log.hpp"

#include <chrono>
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace {

using gbb::link_harness::Expectation;
using gbb::link_harness::Options;
using gbb::link_harness::Scenario;
using gbb::link_harness::AutoInputState;
using gbb::link_harness::fingerprint;
using gbb::link_harness::hex;
using gbb::link_harness::read_bytes;
using gbb::link_harness::scenario_name;
using gbb::link_harness::SemanticSample;
using gbb::link_harness::SemanticTracker;
using gbb::link_harness::PartySnapshot;
using gbb::link_harness::append_semantic_report;
using gbb::link_harness::expectation_name;
using gbb::link_harness::expectation_satisfied;
using gbb::link_harness::semantic_failure;
using gbb::link_harness::run_scenario;
using gbb::link_harness::write_frame;
using gbb::link_harness::write_report;
using gbb::link_harness::parse_options;
using gbb::link_harness::usage;
using namespace gbb::link_harness;

constexpr std::uint64_t cycles_per_frame = 70'224;
constexpr std::uint64_t poll_interval_cycles = 1'024;


SemanticSample capture_semantic_sample(gameboy::Emulator& first,
                                       gameboy::Emulator& second) {
    WramBank1Guard first_bank(first);
    WramBank1Guard second_bank(second);
    SemanticSample sample;
    sample.first_party = read_party(first);
    sample.second_party = read_party(second);
    sample.first_link_state = first.bus().read8(w_link_state);
    sample.second_link_state = second.bus().read8(w_link_state);
    sample.first_link_state_localized = first.bus().read8(w_link_state_localized);
    sample.second_link_state_localized = second.bus().read8(w_link_state_localized);
    sample.first_battle_state = first.bus().read8(w_is_in_battle);
    sample.second_battle_state = second.bus().read8(w_is_in_battle);
    sample.first_battle_state_localized = first.bus().read8(w_is_in_battle_localized);
    sample.second_battle_state_localized = second.bus().read8(w_is_in_battle_localized);
    sample.first_map_localized = first.bus().read8(
        static_cast<std::uint16_t>(w_cur_map + 5));
    sample.second_map_localized = second.bus().read8(
        static_cast<std::uint16_t>(w_cur_map + 5));
    sample.first_battle_trade_menu = at_battle_trade_menu(first);
    sample.second_battle_trade_menu = at_battle_trade_menu(second);
    sample.first_trade_selection_menu = at_trade_selection_menu(first);
    sample.second_trade_selection_menu = at_trade_selection_menu(second);
    sample.first_trade_stats_menu = at_trade_stats_menu(first);
    sample.second_trade_stats_menu = at_trade_stats_menu(second);
    sample.first_trade_cancel_menu = at_trade_cancel_menu(first);
    sample.second_trade_cancel_menu = at_trade_cancel_menu(second);
    return sample;
}
void poll_pair(gameboy::TcpSerialEndpoint& first,
               gameboy::TcpSerialEndpoint& second) {
    first.poll();
    second.poll();
}

void validate_scenario_states(const Options& options,
                              gameboy::Emulator& first,
                              gameboy::Emulator& second,
                              const bool is_pokemon) {
    // The scripted Gen I driver can navigate within the Cable Club, but it
    // cannot recover from a state captured somewhere else in the game. Fail
    // before attaching a cable so a bad or stale state is not misreported as
    // a transport timeout or a trade deadlock.
    if (options.scenario == Scenario::none || options.state1.empty() ||
        !is_pokemon) {
        return;
    }
    WramBank1Guard first_bank(first);
    WramBank1Guard second_bank(second);
    const auto first_in_club = at_cable_club_map(first);
    const auto second_in_club = at_cable_club_map(second);
    if (!first_in_club || !second_in_club) {
        const auto first_map = first.bus().read8(
            static_cast<std::uint16_t>(w_cur_map + 5));
        const auto second_map = second.bus().read8(
            static_cast<std::uint16_t>(w_cur_map + 5));
        std::ostringstream message;
        message << "--scenario " << scenario_name(options.scenario)
                << " requires both save states to be in the Pokémon Cable Club "
                   "(map 0xEF/0xF0); got player1="
                << (first_in_club ? "yes" : "no") << " player2="
                << (second_in_club ? "yes" : "no")
                << " (localized maps 0x" << std::hex
                << static_cast<unsigned>(first_map) << "/0x"
                << static_cast<unsigned>(second_map) << std::dec << ")"
                << ". Map 0x29 is the pre-link lobby; use states captured after "
                   "both players reach the connected Cable Club choice/table "
                   "(0xEF/0xF0) for scripted scenarios.";
        throw std::invalid_argument(message.str());
    }
}

const char* link_session_state_name(const gameboy::LinkSession::State state) {
    switch (state) {
    case gameboy::LinkSession::State::disconnected: return "disconnected";
    case gameboy::LinkSession::State::starting: return "starting";
    case gameboy::LinkSession::State::connected: return "connected";
    case gameboy::LinkSession::State::transferring: return "transferring";
    case gameboy::LinkSession::State::timed_out: return "timed_out";
    }
    return "unknown";
}


void release_auto_buttons(gameboy::Emulator& emulator) {
    emulator.set_button(gameboy::Button::right, false);
    emulator.set_button(gameboy::Button::left, false);
    emulator.set_button(gameboy::Button::up, false);
    emulator.set_button(gameboy::Button::down, false);
    emulator.set_button(gameboy::Button::a, false);
    emulator.set_button(gameboy::Button::b, false);
    emulator.set_button(gameboy::Button::select, false);
    emulator.set_button(gameboy::Button::start, false);
}

void advance_pair(gameboy::Emulator& first, gameboy::Emulator& second,
                  gameboy::TcpSerialEndpoint& first_endpoint,
                  gameboy::TcpSerialEndpoint& second_endpoint,
                  const std::uint64_t cycles) {
    const auto first_target = first.cpu().total_cycles() + cycles;
    const auto second_target = second.cpu().total_cycles() + cycles;
    std::uint64_t next_poll = std::min(first.cpu().total_cycles(),
                                       second.cpu().total_cycles()) +
                              poll_interval_cycles;
    while (first.cpu().total_cycles() < first_target ||
           second.cpu().total_cycles() < second_target) {
        const auto first_cycles = first.cpu().total_cycles();
        const auto second_cycles = second.cpu().total_cycles();
        if (first_cycles <= second_cycles && first_cycles < first_target) {
            static_cast<void>(first.step());
        } else if (second.cpu().total_cycles() < second_target) {
            static_cast<void>(second.step());
        } else {
            static_cast<void>(first.step());
        }
        const auto now = std::min(first.cpu().total_cycles(),
                                  second.cpu().total_cycles());
        if (now >= next_poll) {
            poll_pair(first_endpoint, second_endpoint);
            next_poll = now + poll_interval_cycles;
        }
    }
    poll_pair(first_endpoint, second_endpoint);
}

} // namespace

int main(int argc, char** argv) {
    try {
        const auto options = parse_options(argc, argv);
        const auto rom_bytes = read_bytes(options.rom);
        const auto save1 = read_bytes(options.save1);
        const auto save2 = read_bytes(options.save2);

        gameboy::Emulator first{gameboy::Cartridge{rom_bytes}};
        gameboy::Emulator second{gameboy::Cartridge{rom_bytes}};
        first.import_battery_save(save1);
        second.import_battery_save(save2);
        if (!options.state1.empty()) {
            first.load_state(read_bytes(options.state1));
            second.load_state(read_bytes(options.state2));
            // A save state captures the renderer's pending-frame flag. The
            // harness owns one complete video frame per iteration, so clear
            // that stale presentation marker before driving the link.
            first.consume_frame();
            second.consume_frame();
        }
        const auto initial_semantic = capture_semantic_sample(first, second);
        SemanticTracker tracker(initial_semantic.first_party,
                                initial_semantic.second_party);
        const auto initial_save1 = first.export_battery_save();
        const auto initial_save2 = second.export_battery_save();
        const auto is_pokemon = first.bus().cartridge().title() == "POKEMON BLUE" ||
                                first.bus().cartridge().title() == "POKEMON RED" ||
                                first.bus().cartridge().title() == "POKEMON YELLOW";
        validate_scenario_states(options, first, second, is_pokemon);
        const auto starts_at_link_choice = [&]() {
            if (options.scenario == Scenario::none || options.state1.empty()) return false;
            WramBank1Guard first_bank(first);
            WramBank1Guard second_bank(second);
            return at_ready_link_choice(first) && at_ready_link_choice(second);
        }();
        if (options.local) {
            if (options.scenario != Scenario::none) {
                release_auto_buttons(first);
                release_auto_buttons(second);
            }
            // Pokémon's Cable Club code initializes the serial handshake from
            // these save states. Preserve that setup and only reset the game
            // handshake registers below; resetting the serial phase here can
            // strand the CPU in its initial transfer wait.
            if (is_pokemon && !starts_at_link_choice) {
                reset_pokemon_link_handshake(first);
                reset_pokemon_link_handshake(second);
            }
            gameboy::LinkSession session;
            gameboy::GameBoyLinkEndpoint first_endpoint{first};
            gameboy::GameBoyLinkEndpoint second_endpoint{second};
            session.start(first_endpoint, second_endpoint);
            AutoInputState input_state;
            ScenarioTrace trace(options.trace, "local", options.scenario);
            SerialProgressWatchdog serial_watchdog;
            const auto run_result = run_scenario(
                options.frames, options.scenario, input_state,
                [&](const std::uint64_t frame, AutoInputState& input) {
                gbb::LogContextScope frame_context{
                    {0, frame + 1, first.cpu().total_cycles(),
                     first.bus().cartridge().rom_fingerprint()}};
                apply_auto_inputs(options, frame, first, second, input);
                if (options.scenario == Scenario::trade) {
                    trace.write_trade_phase_event(
                        frame + 1, first, second, input,
                        link_session_state_name(session.state()),
                        session.transfers_completed());
                }
                session.advance(static_cast<unsigned>(cycles_per_frame));
                tracker.sample(capture_semantic_sample(first, second));
                trace.write_frame(frame + 1, first, second, input,
                                  nullptr, nullptr,
                                  link_session_state_name(session.state()),
                                  session.transfers_completed());
                first.consume_frame();
                second.consume_frame();
                update_serial_progress_watchdog(
                    trace, frame + 1, first, second, serial_watchdog,
                    link_session_state_name(session.state()),
                    session.transfers_completed(),
                    options.scenario == Scenario::trade);
                },
                [&] { return expectation_satisfied(tracker, options.expectation); });
            const auto frames_run = run_result.frames_run;
            if (options.auto_confirm) {
                first.set_button(gameboy::Button::a, false);
                second.set_button(gameboy::Button::a, false);
                first.set_button(gameboy::Button::down, false);
                second.set_button(gameboy::Button::down, false);
            }
            const auto final_save1 = first.export_battery_save();
            const auto final_save2 = second.export_battery_save();
            std::ostringstream report;
            report << "GBB link integration report\n"
                   << "transport=local\n"
                   << "rom=" << options.rom << '\n'
                   << "title=" << first.bus().cartridge().title() << '\n'
               << "rom_fingerprint="
               << hex(first.bus().cartridge().rom_fingerprint()) << '\n'
               << "state_inputs=" << (!options.state1.empty() ? "yes" : "no") << '\n'
                   << "frames=" << frames_run << '\n'
                   << "cycles=" << frames_run * cycles_per_frame << '\n'
                   << "transfers_completed=" << session.transfers_completed() << '\n'
                   << "serial_stall_detected="
                   << (serial_watchdog.stall_reported ? "yes" : "no") << '\n'
                   << "serial_stall_frame=" << serial_watchdog.stall_frame << '\n'
                   << "serial_stall_frames=" << serial_watchdog.stalled_frames << '\n'
                   << "serial_ownership_transitions="
                   << serial_watchdog.ownership_transitions << '\n'
                   << "trace=" << (options.trace.empty() ? "none" : options.trace.string())
                   << '\n'
                   << "save1_before=" << hex(fingerprint(initial_save1)) << '\n'
                   << "save1_after=" << hex(fingerprint(final_save1)) << '\n'
                   << "save2_before=" << hex(fingerprint(initial_save2)) << '\n'
                   << "save2_after=" << hex(fingerprint(final_save2)) << '\n';
            append_semantic_report(report, tracker, options.expectation);
            append_auto_input_report(report, input_state);
            const auto semantic_ok =
                expectation_satisfied(tracker, options.expectation);
            session.stop();
            if (!options.capture_dir.empty()) {
                write_frame(options.capture_dir / "player1.ppm", first.framebuffer());
                write_frame(options.capture_dir / "player2.ppm", second.framebuffer());
            }
            std::cout << report.str();
            write_report(options.report, report.str());
            if (!semantic_ok) {
                std::cerr << "Expected " << expectation_name(options.expectation)
                          << " outcome was not observed ("
                          << semantic_failure(tracker, options.expectation) << ")\n";
                return EXIT_FAILURE;
            }
            return EXIT_SUCCESS;
        }

        gameboy::TcpLinkChannel server;
        gameboy::TcpLinkChannel client;
        if (!server.listen(options.port) || server.local_port() == 0) {
            throw std::runtime_error("could not listen for TCP link harness");
        }
        if (!client.connect("127.0.0.1", server.local_port())) {
            throw std::runtime_error("could not start TCP link harness client");
        }

        gameboy::TcpSerialEndpoint first_endpoint;
        gameboy::TcpSerialEndpoint second_endpoint;
        first_endpoint.set_arbitration_priority(true);
        second_endpoint.set_arbitration_priority(false);
        if (is_pokemon && !starts_at_link_choice) {
            reset_pokemon_link_handshake(first);
            reset_pokemon_link_handshake(second);
        } else {
            first.bus().serial_port().reset_link();
            second.bus().serial_port().reset_link();
        }
        first_endpoint.attach(first.bus().serial_port(), client,
                              first.rom_fingerprint());
        second_endpoint.attach(second.bus().serial_port(), server,
                               second.rom_fingerprint());

        for (unsigned attempt = 0;
             attempt < 500 && !first_endpoint.peer_ready_for_link(); ++attempt) {
            poll_pair(first_endpoint, second_endpoint);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (!first_endpoint.peer_ready_for_link()) {
            throw std::runtime_error("TCP link handshake did not become ready");
        }

        AutoInputState input_state;
        ScenarioTrace trace(options.trace, "tcp", options.scenario);
        SerialProgressWatchdog serial_watchdog;
        if (options.scenario != Scenario::none) {
            release_auto_buttons(first);
            release_auto_buttons(second);
        }
        const auto run_result = run_scenario(
            options.frames, options.scenario, input_state,
            [&](const std::uint64_t frame, AutoInputState& input) {
            gbb::LogContextScope frame_context{
                {0, frame + 1, first.cpu().total_cycles(),
                 first.bus().cartridge().rom_fingerprint()}};
            apply_auto_inputs(options, frame, first, second, input);
            if (options.scenario == Scenario::trade) {
                trace.write_trade_phase_event(
                    frame + 1, first, second, input, nullptr,
                    first_endpoint.transfers_completed() +
                        second_endpoint.transfers_completed());
            }
            advance_pair(first, second, first_endpoint, second_endpoint,
                         cycles_per_frame);
            tracker.sample(capture_semantic_sample(first, second));
            trace.write_frame(frame + 1, first, second, input,
                              &first_endpoint, &second_endpoint);
            update_serial_progress_watchdog(
                trace, frame + 1, first, second, serial_watchdog, nullptr,
                first_endpoint.transfers_completed() +
                    second_endpoint.transfers_completed(),
                options.scenario == Scenario::trade);
            },
            [&] { return expectation_satisfied(tracker, options.expectation); });
        const auto frames_run = run_result.frames_run;
        if (options.auto_confirm) {
            first.set_button(gameboy::Button::a, false);
            second.set_button(gameboy::Button::a, false);
            first.set_button(gameboy::Button::down, false);
            second.set_button(gameboy::Button::down, false);
        }

        const auto final_save1 = first.export_battery_save();
        const auto final_save2 = second.export_battery_save();
        std::ostringstream report;
        report << "GBB link integration report\n"
               << "rom=" << options.rom << '\n'
               << "title=" << first.bus().cartridge().title() << '\n'
               << "rom_fingerprint=" << hex(first.bus().cartridge().rom_fingerprint())
               << '\n'
               << "state_inputs=" << (!options.state1.empty() ? "yes" : "no") << '\n'
               << "frames=" << frames_run << '\n'
               << "cycles=" << frames_run * cycles_per_frame << '\n'
               << "tcp_port=" << server.local_port() << '\n'
               << "trace=" << (options.trace.empty() ? "none" : options.trace.string())
               << '\n'
               << "host_requests_sent=" << first_endpoint.requests_sent() << '\n'
               << "host_requests_received=" << first_endpoint.requests_received() << '\n'
               << "host_responses_sent=" << first_endpoint.responses_sent() << '\n'
               << "host_responses_received=" << first_endpoint.responses_received() << '\n'
               << "join_requests_sent=" << second_endpoint.requests_sent() << '\n'
               << "join_requests_received=" << second_endpoint.requests_received() << '\n'
               << "join_responses_sent=" << second_endpoint.responses_sent() << '\n'
               << "join_responses_received=" << second_endpoint.responses_received() << '\n'
               << "host_unmatched_responses=" << first_endpoint.responses_unmatched() << '\n'
               << "join_unmatched_responses=" << second_endpoint.responses_unmatched() << '\n'
               << "host_transfers_completed=" << first_endpoint.transfers_completed() << '\n'
               << "join_transfers_completed=" << second_endpoint.transfers_completed() << '\n'
               << "serial_stall_detected="
               << (serial_watchdog.stall_reported ? "yes" : "no") << '\n'
               << "serial_stall_frame=" << serial_watchdog.stall_frame << '\n'
               << "serial_stall_frames=" << serial_watchdog.stalled_frames << '\n'
               << "serial_ownership_transitions="
               << serial_watchdog.ownership_transitions << '\n'
               << "save1_before=" << hex(fingerprint(initial_save1)) << '\n'
               << "save1_after=" << hex(fingerprint(final_save1)) << '\n'
               << "save2_before=" << hex(fingerprint(initial_save2)) << '\n'
               << "save2_after=" << hex(fingerprint(final_save2)) << '\n';
        append_semantic_report(report, tracker, options.expectation);
        append_auto_input_report(report, input_state);
        const auto semantic_ok = expectation_satisfied(tracker, options.expectation);
        std::cout << report.str();
        if (!options.capture_dir.empty()) {
            write_frame(options.capture_dir / "player1.ppm", first.framebuffer());
            write_frame(options.capture_dir / "player2.ppm", second.framebuffer());
        }
        write_report(options.report, report.str());

        first_endpoint.detach();
        second_endpoint.detach();
        if (!semantic_ok) {
            std::cerr << "Expected " << expectation_name(options.expectation)
                      << " outcome was not observed ("
                      << semantic_failure(tracker, options.expectation) << ")\n";
            return EXIT_FAILURE;
        }
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "Error: " << error.what() << '\n';
        usage();
        return EXIT_FAILURE;
    }
}
