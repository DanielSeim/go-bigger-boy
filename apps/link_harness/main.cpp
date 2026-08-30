#include "gameboy/emulator.hpp"
#include "gameboy/link_session.hpp"
#include "gameboy/tcp_link_channel.hpp"
#include "gameboy/tcp_serial_endpoint.hpp"

#include <chrono>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr std::uint64_t cycles_per_frame = 70'224;
constexpr std::uint64_t poll_interval_cycles = 1'024;

// Pokémon Red/Blue WRAM locations. These are stable in the English releases
// and let the harness validate game-level outcomes instead of only serial I/O.
constexpr std::uint16_t w_link_state = 0xD12B;
constexpr std::uint16_t w_link_state_localized = w_link_state + 5;
constexpr std::uint16_t w_party_count = 0xD163;
constexpr std::uint16_t w_party_mon1 = 0xD16B;
constexpr std::uint16_t w_is_in_battle = 0xD057;
constexpr std::uint16_t w_is_in_battle_localized = w_is_in_battle + 5;
constexpr std::uint16_t w_top_menu_item_y = 0xCC24;
constexpr std::uint16_t w_top_menu_item_x = 0xCC25;
constexpr std::uint16_t w_cur_map = 0xD35E;
constexpr std::uint16_t w_player_direction = 0xD52A;
constexpr std::uint16_t w_player_sprite_direction = 0xC109;
constexpr std::uint16_t party_mon_size = 0x2C;
constexpr std::uint8_t link_state_battling = 0x04;

enum class Expectation { none, trade, battle };
enum class Scenario { none, trade, battle };

struct Options {
    std::filesystem::path rom;
    std::filesystem::path save1;
    std::filesystem::path save2;
    std::filesystem::path state1;
    std::filesystem::path state2;
    std::filesystem::path report;
    std::filesystem::path capture_dir;
    std::uint64_t frames = 1'200;
    std::uint16_t port = 0;
    bool local{};
    bool auto_confirm{};
    Expectation expectation = Expectation::none;
    Scenario scenario = Scenario::none;
};

void usage() {
    std::cerr
        << "Usage: gbb_link_harness --rom ROM --save1 SAVE --save2 SAVE "
           "[--state1 STATE --state2 STATE] "
           "[--transport tcp|local] [--frames N] [--port N] "
           "[--auto-confirm] [--scenario trade|battle] "
           "[--expect trade|battle] [--report PATH]\n";
}

std::uint64_t parse_positive(const std::string& text, const char* name) {
    std::size_t consumed = 0;
    const auto value = std::stoull(text, &consumed);
    if (consumed != text.size() || value == 0) {
        throw std::invalid_argument(std::string(name) +
                                    " must be a positive integer");
    }
    return value;
}

Options parse_options(const int argc, char** argv) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        const auto require_value = [&](const char* name) -> std::string {
            if (index + 1 >= argc) {
                throw std::invalid_argument(std::string(name) +
                                            " requires a value");
            }
            return argv[++index];
        };
        if (argument == "--rom") {
            options.rom = require_value("--rom");
        } else if (argument == "--save1") {
            options.save1 = require_value("--save1");
        } else if (argument == "--save2") {
            options.save2 = require_value("--save2");
        } else if (argument == "--state1") {
            options.state1 = require_value("--state1");
        } else if (argument == "--state2") {
            options.state2 = require_value("--state2");
        } else if (argument == "--frames") {
            options.frames = parse_positive(require_value("--frames"),
                                            "--frames");
        } else if (argument == "--port") {
            const auto value = parse_positive(require_value("--port"),
                                              "--port");
            if (value > 65'535) {
                throw std::invalid_argument("--port is out of range");
            }
            options.port = static_cast<std::uint16_t>(value);
        } else if (argument == "--transport") {
            const auto value = require_value("--transport");
            if (value == "local") {
                options.local = true;
            } else if (value == "tcp") {
                options.local = false;
            } else {
                throw std::invalid_argument("unknown transport: " + value);
            }
        } else if (argument == "--auto-confirm") {
            options.auto_confirm = true;
        } else if (argument == "--scenario") {
            const auto value = require_value("--scenario");
            if (value == "trade") {
                options.scenario = Scenario::trade;
                options.expectation = Expectation::trade;
            } else if (value == "battle") {
                options.scenario = Scenario::battle;
                options.expectation = Expectation::battle;
            } else {
                throw std::invalid_argument("unknown scenario: " + value);
            }
            options.auto_confirm = true;
        } else if (argument == "--expect") {
            const auto value = require_value("--expect");
            if (value == "trade") {
                options.expectation = Expectation::trade;
            } else if (value == "battle") {
                options.expectation = Expectation::battle;
            } else {
                throw std::invalid_argument("unknown expectation: " + value);
            }
        } else if (argument == "--report") {
            options.report = require_value("--report");
        } else if (argument == "--capture-dir") {
            options.capture_dir = require_value("--capture-dir");
        } else if (argument == "--help" || argument == "-h") {
            usage();
            std::exit(EXIT_SUCCESS);
        } else {
            throw std::invalid_argument("unknown option: " + argument);
        }
    }
    if (options.rom.empty() || options.save1.empty() || options.save2.empty()) {
        throw std::invalid_argument("--rom, --save1, and --save2 are required");
    }
    if (options.state1.empty() != options.state2.empty()) {
        throw std::invalid_argument("--state1 and --state2 must be provided together");
    }
    if ((options.scenario == Scenario::trade &&
         options.expectation != Expectation::trade) ||
        (options.scenario == Scenario::battle &&
         options.expectation != Expectation::battle)) {
        throw std::invalid_argument(
            "--scenario and --expect must refer to the same outcome");
    }
    if (options.scenario != Scenario::none && options.state1.empty()) {
        throw std::invalid_argument(
            "--scenario requires --state1 and --state2 for reproducible input");
    }
    return options;
}

struct PartySnapshot {
    std::uint8_t count{};
    std::array<std::uint8_t, 6> species{};
    std::array<std::uint16_t, 6> ot_ids{};
    std::array<std::uint64_t, 6> signatures{};
    std::uint16_t base_address{};
    bool valid{};
};

std::string hex(std::uint64_t value);

// CGB cartridges expose D000-DFFF through a selectable WRAM bank. The
// original Gen I games keep these variables in bank 1, even when the emulator
// is paused with another bank selected for rendering or sound data.
class WramBank1Guard {
  public:
    explicit WramBank1Guard(gameboy::Emulator& emulator)
        : bus_(emulator.bus()) {
        if (!bus_.cgb_mode()) return;
        previous_ = static_cast<std::uint8_t>(bus_.read8(0xFF70) & 0x07U);
        if (previous_ != 1) {
            bus_.write8(0xFF70, 1);
            switched_ = true;
        }
    }

    ~WramBank1Guard() {
        if (switched_) bus_.write8(0xFF70, previous_);
    }

    WramBank1Guard(const WramBank1Guard&) = delete;
    WramBank1Guard& operator=(const WramBank1Guard&) = delete;

  private:
    gameboy::MemoryBus& bus_;
    std::uint8_t previous_ = 1;
    bool switched_{};
};

std::uint16_t read16be(const gameboy::Emulator& emulator,
                       const std::uint16_t address) {
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(emulator.bus().read8(address)) << 8) |
        emulator.bus().read8(static_cast<std::uint16_t>(address + 1)));
}

PartySnapshot read_party(const gameboy::Emulator& emulator) {
    const auto read_candidate = [&](const std::uint16_t base,
                                    const std::uint16_t mon_start) {
        PartySnapshot snapshot;
        snapshot.base_address = base;
        snapshot.count = emulator.bus().read8(base);
        if (snapshot.count == 0 || snapshot.count > snapshot.species.size()) {
            return snapshot;
        }
        snapshot.valid = emulator.bus().read8(
                             static_cast<std::uint16_t>(base + 1 + snapshot.count)) ==
                         0xFF;
        for (std::size_t index = 0; index < snapshot.species.size(); ++index) {
            snapshot.species[index] = emulator.bus().read8(
                static_cast<std::uint16_t>(base + 1 + index));
            if (index >= snapshot.count) continue;
            if (snapshot.species[index] == 0 || snapshot.species[index] == 0xFF) {
                snapshot.valid = false;
            }
            const auto mon_address = static_cast<std::uint16_t>(
                mon_start + index * party_mon_size);
            if (emulator.bus().read8(mon_address) != snapshot.species[index]) {
                snapshot.valid = false;
            }
            snapshot.ot_ids[index] = read16be(
                emulator, static_cast<std::uint16_t>(mon_address + 0x0C));
            std::uint64_t signature = UINT64_C(1469598103934665603);
            for (std::uint16_t offset = 0; offset < party_mon_size; ++offset) {
                signature ^= emulator.bus().read8(
                    static_cast<std::uint16_t>(mon_address + offset));
                signature *= UINT64_C(1099511628211);
            }
            snapshot.signatures[index] = signature;
        }
        return snapshot;
    };

    auto snapshot = read_candidate(w_party_count, w_party_mon1);
    if (!snapshot.valid) {
        // European translations retain the same data layout with the legacy
        // party block shifted five bytes forward.
        snapshot = read_candidate(static_cast<std::uint16_t>(w_party_count + 5),
                                  static_cast<std::uint16_t>(w_party_mon1 + 5));
    }
    if (snapshot.valid) return snapshot;
    return {};
}

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
        if (contains_mon(destination, source.signatures[index])) {
            return true;
        }
    }
    return false;
}

bool plausible_link_state(const std::uint8_t state) {
    return state <= 0x05 || state == 0x32;
}

std::uint8_t effective_link_state(const std::uint8_t primary,
                                  const std::uint8_t alternate) {
    return plausible_link_state(primary) ? primary : alternate;
}

bool at_battle_trade_menu(const gameboy::Emulator& emulator) {
    const auto matches_layout = [&](const std::uint16_t offset) {
        const auto top_y = emulator.bus().read8(offset);
        const auto top_x = emulator.bus().read8(
            static_cast<std::uint16_t>(offset + 1));
        const auto current = emulator.bus().read8(
            static_cast<std::uint16_t>(offset + 2));
        const auto maximum = emulator.bus().read8(
            static_cast<std::uint16_t>(offset + 4));
        const auto watched_keys = emulator.bus().read8(
            static_cast<std::uint16_t>(offset + 5));
        const auto cursor_low = emulator.bus().read8(
            static_cast<std::uint16_t>(offset + 12));
        const auto cursor_high = emulator.bus().read8(
            static_cast<std::uint16_t>(offset + 13));
        return top_y == 0x07 && (top_x == 0x05 || top_x == 0x06) && current <= 0x02 &&
               maximum == 0x02 && watched_keys == 0x03 &&
               (cursor_high & 0x80U) != 0 && cursor_low != 0;
    };
    return matches_layout(w_top_menu_item_y) ||
           matches_layout(static_cast<std::uint16_t>(w_top_menu_item_y + 5));
}

bool at_cable_club_map(const gameboy::Emulator& emulator) {
    const auto primary = emulator.bus().read8(w_cur_map);
    const auto localized = emulator.bus().read8(static_cast<std::uint16_t>(w_cur_map + 5));
    const auto is_club = [](const std::uint8_t map) {
        return map == 0xEF || map == 0xF0;
    };
    return is_club(primary) || is_club(localized);
}

void select_joypad_lines(gameboy::Emulator& emulator, const bool actions) {
    // Save states taken while a menu is waiting often leave JOYP deselected
    // (0x30).  Select the relevant matrix before synthesising a button edge so
    // the edge raises the normal joypad interrupt and wakes a halted CPU.
    emulator.bus().write8(0xFF00, actions ? 0x10 : 0x20);
}

void set_facing_direction(gameboy::Emulator& emulator, const std::uint8_t direction) {
    // The European Gen I build shifts these WRAM bytes by five. Select the
    // layout from the current-map probe so an English run never overwrites
    // the adjacent variable while applying the table-facing assist.
    const auto primary_map = emulator.bus().read8(w_cur_map);
    const auto localized_map = emulator.bus().read8(
        static_cast<std::uint16_t>(w_cur_map + 5));
    const auto is_club_map_id = [](const std::uint8_t map) {
        return map == 0xEF || map == 0xF0;
    };
    const auto offset = !is_club_map_id(primary_map) &&
                                is_club_map_id(localized_map)
                            ? 5
                            : 0;
    emulator.bus().write8(
        static_cast<std::uint16_t>(w_player_direction + offset), direction);
    emulator.bus().write8(static_cast<std::uint16_t>(
                              w_player_sprite_direction + offset),
                          direction);
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

struct SemanticTracker {
    PartySnapshot initial_first;
    PartySnapshot initial_second;
    PartySnapshot final_first;
    PartySnapshot final_second;
    bool first_battle_seen{};
    bool second_battle_seen{};
    bool first_battle_trade_menu_seen{};
    bool second_battle_trade_menu_seen{};
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

    SemanticTracker(gameboy::Emulator& first, gameboy::Emulator& second) {
        WramBank1Guard first_bank(first);
        WramBank1Guard second_bank(second);
        initial_first = read_party(first);
        initial_second = read_party(second);
        final_first = initial_first;
        final_second = initial_second;
    }

    void sample(gameboy::Emulator& first, gameboy::Emulator& second) {
        WramBank1Guard first_bank(first);
        WramBank1Guard second_bank(second);
        final_first = read_party(first);
        final_second = read_party(second);
        first_link_state = first.bus().read8(w_link_state);
        second_link_state = second.bus().read8(w_link_state);
        first_link_state_localized = first.bus().read8(w_link_state_localized);
        second_link_state_localized = second.bus().read8(w_link_state_localized);
        first_battle_state = first.bus().read8(w_is_in_battle);
        second_battle_state = second.bus().read8(w_is_in_battle);
        first_battle_state_localized = first.bus().read8(w_is_in_battle_localized);
        second_battle_state_localized = second.bus().read8(w_is_in_battle_localized);
        first_map_localized = first.bus().read8(static_cast<std::uint16_t>(w_cur_map + 5));
        second_map_localized = second.bus().read8(static_cast<std::uint16_t>(w_cur_map + 5));
        first_battle_trade_menu_seen =
            first_battle_trade_menu_seen || at_battle_trade_menu(first);
        second_battle_trade_menu_seen =
            second_battle_trade_menu_seen || at_battle_trade_menu(second);
        const auto first_effective_link_state =
            effective_link_state(first_link_state, first_link_state_localized);
        const auto second_effective_link_state =
            effective_link_state(second_link_state, second_link_state_localized);
        first_battle_seen = first_battle_seen ||
                            first_effective_link_state == link_state_battling ||
                            (first_battle_state != 0 && first_battle_state != 0xFF) ||
                            (first_battle_state_localized != 0 &&
                             first_battle_state_localized != 0xFF);
        second_battle_seen = second_battle_seen ||
                             second_effective_link_state == link_state_battling ||
                             (second_battle_state != 0 && second_battle_state != 0xFF) ||
                             (second_battle_state_localized != 0 &&
                              second_battle_state_localized != 0xFF);
    }

    bool trade_observed() const {
        return first_party_changed() && second_party_changed() &&
               contains_party_member_from(final_first, initial_second) &&
               contains_party_member_from(final_second, initial_first);
    }

    bool battle_observed() const { return first_battle_seen && second_battle_seen; }
    bool first_party_changed() const {
        return initial_first.valid && final_first.valid &&
               !same_party(initial_first, final_first);
    }
    bool second_party_changed() const {
        return initial_second.valid && final_second.valid &&
               !same_party(initial_second, final_second);
    }
};

const char* expectation_name(const Expectation expectation) {
    switch (expectation) {
    case Expectation::trade: return "trade";
    case Expectation::battle: return "battle";
    case Expectation::none: return "none";
    }
    return "none";
}

bool expectation_satisfied(const SemanticTracker& tracker,
                           const Expectation expectation) {
    switch (expectation) {
    case Expectation::none: return true;
    case Expectation::trade: return tracker.trade_observed();
    case Expectation::battle: return tracker.battle_observed();
    }
    return false;
}

const char* semantic_failure(const SemanticTracker& tracker,
                             const Expectation expectation) {
    if (expectation_satisfied(tracker, expectation)) return "none";
    if (expectation == Expectation::trade) {
        if (!tracker.initial_first.valid || !tracker.initial_second.valid) {
            return "initial_party_snapshot_unavailable";
        }
        if (!tracker.final_first.valid || !tracker.final_second.valid) {
            return "final_party_snapshot_unavailable";
        }
        if (!tracker.first_party_changed() || !tracker.second_party_changed()) {
            if (tracker.first_battle_trade_menu_seen ||
                tracker.second_battle_trade_menu_seen) {
                return "trade_not_completed_after_menu";
            }
            return "party_snapshots_unchanged";
        }
        return "no_cross_party_record_observed";
    }
    if (!tracker.first_battle_seen && !tracker.second_battle_seen) {
        if (tracker.first_battle_trade_menu_seen &&
            tracker.second_battle_trade_menu_seen) {
            return "battle_not_started_after_menu";
        }
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
           << "player1_link_state_final=" << hex(tracker.first_link_state) << '\n'
           << "player2_link_state_final=" << hex(tracker.second_link_state) << '\n'
           << "player1_link_state_localized_final=" << hex(tracker.first_link_state_localized) << '\n'
           << "player2_link_state_localized_final=" << hex(tracker.second_link_state_localized) << '\n'
           << "player1_battle_state_final=" << hex(tracker.first_battle_state) << '\n'
           << "player2_battle_state_final=" << hex(tracker.second_battle_state) << '\n';
    report << "player1_battle_state_localized_final=" << hex(tracker.first_battle_state_localized) << '\n'
           << "player2_battle_state_localized_final=" << hex(tracker.second_battle_state_localized) << '\n';
    report << "player1_map_localized_final=" << hex(tracker.first_map_localized) << '\n'
           << "player2_map_localized_final=" << hex(tracker.second_map_localized) << '\n';
}

std::vector<std::uint8_t> read_bytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("could not open " + path.string());
    std::vector<std::uint8_t> bytes{
        std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    if (input.bad()) throw std::runtime_error("could not read " + path.string());
    return bytes;
}

std::uint64_t fingerprint(const std::vector<std::uint8_t>& bytes) {
    std::uint64_t hash = UINT64_C(1469598103934665603);
    for (const auto byte : bytes) {
        hash ^= byte;
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

std::string hex(const std::uint64_t value) {
    std::ostringstream output;
    output << "0x" << std::hex << std::setfill('0') << std::setw(16) << value;
    return output.str();
}

void write_report(const std::filesystem::path& path, const std::string& report) {
    if (path.empty()) return;
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path());
    }
    std::ofstream output(path, std::ios::trunc);
    if (!output) throw std::runtime_error("could not open report " + path.string());
    output << report;
    if (!output) throw std::runtime_error("could not write report " + path.string());
}

void write_frame(const std::filesystem::path& path,
                 const gameboy::Ppu::Framebuffer& framebuffer) {
    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) throw std::runtime_error("could not open frame " + path.string());
    output << "P6\n" << gameboy::Ppu::screen_width << ' '
           << gameboy::Ppu::screen_height << "\n255\n";
    for (const auto pixel : framebuffer) {
        const std::array<char, 3> rgb{
            static_cast<char>((pixel >> 16) & 0xFF),
            static_cast<char>((pixel >> 8) & 0xFF),
            static_cast<char>(pixel & 0xFF)};
        output.write(rgb.data(), static_cast<std::streamsize>(rgb.size()));
    }
}

void poll_pair(gameboy::TcpSerialEndpoint& first,
               gameboy::TcpSerialEndpoint& second) {
    first.poll();
    second.poll();
}

void reset_pokemon_link_handshake(gameboy::Emulator& emulator) {
    emulator.bus().write8(0xFFAA, 0xFF);
    emulator.bus().write8(0xFFAB, 0x00);
    emulator.bus().write8(0xFFAC, 0x00);
    emulator.bus().write8(0xFFAD, 0x00);
    emulator.bus().write8(0xFF01, 0x02);
    emulator.bus().write8(0xFF02, 0x80);
    emulator.bus().write8(
        0xFFFF, static_cast<std::uint8_t>(emulator.bus().read8(0xFFFF) | 0x08U));
    emulator.bus().write8(
        0xFF0F, static_cast<std::uint8_t>(emulator.bus().read8(0xFF0F) & ~0x08U));
}

struct AutoInputState {
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
    bool first_table_confirmed{};
    bool second_table_confirmed{};
    std::uint64_t first_table_frame{};
    std::uint64_t second_table_frame{};
    std::uint64_t first_table_a_frame{};
    std::uint64_t second_table_a_frame{};
};

void apply_auto_inputs(const Options& options, const std::uint64_t frame,
                       gameboy::Emulator& first, gameboy::Emulator& second,
                       AutoInputState& input_state) {
    if (!options.auto_confirm && options.scenario == Scenario::none) return;
    constexpr std::uint64_t confirm_interval = 24;
    if (!options.state1.empty()) {
        const auto scenario_start_delay = options.scenario == Scenario::none ? 24 : 120;
        if (options.scenario != Scenario::none) {
            const auto first_club = at_cable_club_map(first);
            const auto second_club = at_cable_club_map(second);
            if (first_club && !input_state.first_table_started) {
                select_joypad_lines(first, false);
                first.set_button(gameboy::Button::right, true);
                input_state.first_table_started = true;
                input_state.first_table_frame = frame;
                return;
            }
            if (second_club && !input_state.second_table_started) {
                select_joypad_lines(second, false);
                second.set_button(gameboy::Button::left, true);
                input_state.second_table_started = true;
                input_state.second_table_frame = frame;
                return;
            }
            if (input_state.first_table_started &&
                !input_state.first_table_confirmed &&
                frame < input_state.first_table_frame + 4) {
                select_joypad_lines(first, false);
                first.set_button(gameboy::Button::right, true);
                return;
            }
            if (input_state.second_table_started &&
                !input_state.second_table_confirmed &&
                frame < input_state.second_table_frame + 4) {
                select_joypad_lines(second, false);
                second.set_button(gameboy::Button::left, true);
                return;
            }
            first.set_button(gameboy::Button::right, false);
            second.set_button(gameboy::Button::left, false);
            if (input_state.first_table_started && input_state.second_table_started &&
                !input_state.first_table_confirmed &&
                !input_state.second_table_confirmed &&
                frame >= std::max(input_state.first_table_frame,
                                  input_state.second_table_frame) + 4) {
                set_facing_direction(first, 0x0C);
                set_facing_direction(second, 0x08);
                select_joypad_lines(first, true);
                select_joypad_lines(second, true);
                first.set_button(gameboy::Button::a, true);
                second.set_button(gameboy::Button::a, true);
                input_state.first_table_confirmed = true;
                input_state.second_table_confirmed = true;
                input_state.first_table_a_frame = frame;
                input_state.second_table_a_frame = frame;
                return;
            }
            if ((input_state.first_table_confirmed &&
                 frame < input_state.first_table_a_frame + 6) ||
                (input_state.second_table_confirmed &&
                 frame < input_state.second_table_a_frame + 6)) {
                if (input_state.first_table_confirmed &&
                    frame < input_state.first_table_a_frame + 6) {
                    select_joypad_lines(first, true);
                    first.set_button(gameboy::Button::a, true);
                }
                if (input_state.second_table_confirmed &&
                    frame < input_state.second_table_a_frame + 6) {
                    select_joypad_lines(second, true);
                    second.set_button(gameboy::Button::a, true);
                }
                return;
            }
            if (options.scenario == Scenario::battle) {
                const auto menu_ready = frame >= scenario_start_delay;
                const auto first_menu = menu_ready && at_cable_club_map(first) &&
                                        at_battle_trade_menu(first);
                const auto second_menu = menu_ready && at_cable_club_map(second) &&
                                         at_battle_trade_menu(second);
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
        if (frame % confirm_interval == 0 &&
            (options.scenario != Scenario::battle ||
             !input_state.battle_menu_started)) {
            select_joypad_lines(first, true);
            first.set_button(gameboy::Button::a, true);
            if (frame >= scenario_start_delay) {
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
           << "auto_player1_table_a_frame=" << input_state.first_table_a_frame << '\n'
           << "auto_player2_table_a_frame=" << input_state.second_table_a_frame << '\n';
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
        if (first.cpu().total_cycles() <= second.cpu().total_cycles() &&
            first.cpu().total_cycles() < first_target) {
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
        }
        SemanticTracker tracker(first, second);
        const auto initial_save1 = first.export_battery_save();
        const auto initial_save2 = second.export_battery_save();
        const auto is_pokemon = first.bus().cartridge().title() == "POKEMON BLUE" ||
                                first.bus().cartridge().title() == "POKEMON RED" ||
                                first.bus().cartridge().title() == "POKEMON YELLOW";

        if (options.local) {
            if (options.scenario != Scenario::none) {
                release_auto_buttons(first);
                release_auto_buttons(second);
            }
            if (is_pokemon) {
                reset_pokemon_link_handshake(first);
                reset_pokemon_link_handshake(second);
            }
            gameboy::LinkSession session;
            session.start(first, second);
            AutoInputState input_state;
            std::uint64_t frames_run = 0;
            for (std::uint64_t frame = 0; frame < options.frames; ++frame) {
                apply_auto_inputs(options, frame, first, second, input_state);
                session.advance(static_cast<unsigned>(cycles_per_frame));
                tracker.sample(first, second);
                frames_run = frame + 1;
                if (options.scenario != Scenario::none &&
                    expectation_satisfied(tracker, options.expectation)) {
                    break;
                }
            }
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
        if (is_pokemon) {
            reset_pokemon_link_handshake(first);
            reset_pokemon_link_handshake(second);
        }
        first_endpoint.attach(first.bus().serial_port(), client);
        second_endpoint.attach(second.bus().serial_port(), server);

        for (unsigned attempt = 0;
             attempt < 500 && !first_endpoint.peer_ready_for_link(); ++attempt) {
            poll_pair(first_endpoint, second_endpoint);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (!first_endpoint.peer_ready_for_link()) {
            throw std::runtime_error("TCP link handshake did not become ready");
        }

        AutoInputState input_state;
        if (options.scenario != Scenario::none) {
            release_auto_buttons(first);
            release_auto_buttons(second);
        }
        std::uint64_t frames_run = 0;
        for (std::uint64_t frame = 0; frame < options.frames; ++frame) {
            apply_auto_inputs(options, frame, first, second, input_state);
            advance_pair(first, second, first_endpoint, second_endpoint,
                         cycles_per_frame);
            tracker.sample(first, second);
            frames_run = frame + 1;
            if (options.scenario != Scenario::none &&
                expectation_satisfied(tracker, options.expectation)) {
                break;
            }
        }
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
