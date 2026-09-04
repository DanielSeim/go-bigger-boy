#include "pokemon_state.hpp"

#include <cstddef>

namespace gbb::link_harness {

// CGB cartridges expose D000-DFFF through a selectable WRAM bank. The
// original Gen I games keep these variables in bank 1, even when the emulator
// is paused with another bank selected for rendering or sound data.
WramBank1Guard::WramBank1Guard(gameboy::Emulator& emulator)
    : bus_(emulator.bus()) {
    if (!bus_.cgb_mode()) return;
    previous_ = static_cast<std::uint8_t>(bus_.read8(0xFF70) & 0x07U);
    if (previous_ != 1) {
        bus_.write8(0xFF70, 1);
        switched_ = true;
    }
}

WramBank1Guard::~WramBank1Guard() {
    if (switched_) bus_.write8(0xFF70, previous_);
}

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

bool plausible_link_state(const std::uint8_t state) noexcept {
    return state <= 0x05 || state == link_state_trading;
}

std::uint8_t effective_link_state(const std::uint8_t primary,
                                  const std::uint8_t alternate) noexcept {
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

bool at_ready_link_choice(const gameboy::Emulator& emulator) {
    // The menu itself is the authoritative guest-level readiness signal. The
    // localized build keeps a second copy of the link state, and a state
    // saved on the menu can legitimately retain the preceding guest value.
    return at_battle_trade_menu(emulator);
}

bool at_trade_selection_menu(const gameboy::Emulator& emulator) {
    const auto matches_layout = [&](const std::uint16_t offset) {
        const auto top_y = emulator.bus().read8(offset);
        const auto top_x = emulator.bus().read8(
            static_cast<std::uint16_t>(offset + 1));
        const auto maximum = emulator.bus().read8(
            static_cast<std::uint16_t>(offset + 4));
        const auto watched_keys = emulator.bus().read8(
            static_cast<std::uint16_t>(offset + 5));
        const auto cursor_low = emulator.bus().read8(
            static_cast<std::uint16_t>(offset + 12));
        const auto cursor_high = emulator.bus().read8(
            static_cast<std::uint16_t>(offset + 13));
        const auto pointer_index = emulator.bus().read8(
            static_cast<std::uint16_t>(w_trade_pointer_index + offset -
                                       w_top_menu_item_y));
        const auto menu = emulator.bus().read8(
            static_cast<std::uint16_t>(w_trade_menu + offset - w_top_menu_item_y));
        return top_y == 0x01 && top_x == 0x01 && maximum >= 1 && maximum <= 6 &&
               watched_keys == 0x91 && pointer_index == 0 && menu == 0 &&
               (cursor_high & 0x80U) != 0 && cursor_low != 0;
    };
    return matches_layout(w_top_menu_item_y) ||
           matches_layout(static_cast<std::uint16_t>(w_top_menu_item_y + 5));
}

bool at_trade_stats_menu(const gameboy::Emulator& emulator) {
    const auto matches_layout = [&](const std::uint16_t offset) {
        return emulator.bus().read8(offset) == 0x10 &&
               emulator.bus().read8(static_cast<std::uint16_t>(offset + 1)) == 0x01 &&
               emulator.bus().read8(static_cast<std::uint16_t>(offset + 4)) == 0 &&
               emulator.bus().read8(static_cast<std::uint16_t>(offset + 5)) == 0x13;
    };
    return matches_layout(w_top_menu_item_y) ||
           matches_layout(static_cast<std::uint16_t>(w_top_menu_item_y + 5));
}

bool at_trade_cancel_menu(const gameboy::Emulator& emulator) {
    const auto matches_layout = [&](const std::uint16_t offset) {
        return emulator.bus().read8(static_cast<std::uint16_t>(w_text_box_id + offset)) ==
                   0x14 &&
               emulator.bus().read8(static_cast<std::uint16_t>(w_two_option_menu_id + offset)) ==
                   0x05 &&
               emulator.bus().read8(static_cast<std::uint16_t>(w_top_menu_item_y + offset)) ==
                   0x07 &&
               emulator.bus().read8(static_cast<std::uint16_t>(w_top_menu_item_x + offset)) ==
                   0x0A &&
               emulator.bus().read8(static_cast<std::uint16_t>(w_current_menu_item + offset)) <=
                   0x01 &&
               emulator.bus().read8(static_cast<std::uint16_t>(w_top_menu_item_y + 4 + offset)) ==
                   0x01 &&
               emulator.bus().read8(static_cast<std::uint16_t>(w_top_menu_item_y + 5 + offset)) ==
                   0x03;
    };
    return matches_layout(0) || matches_layout(5);
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
    // The European Gen I build shifts these WRAM bytes by five. Use the
    // link-state probe rather than the map ID: before entering the Cable Club
    // the primary map byte is also displaced, so a map-only check misses the
    // receptionist interaction entirely.
    const auto offset = pokemon_wram_offset(emulator);
    emulator.bus().write8(static_cast<std::uint16_t>(w_player_direction + offset),
                          direction);
    emulator.bus().write8(w_player_direction, direction);
    emulator.bus().write8(static_cast<std::uint16_t>(w_player_sprite_direction + offset),
                          direction);
    // Sprite state lives in WRAM0 in the English and European builds, while
    // the gameplay variables above move with the translated WRAM1 layout.
    emulator.bus().write8(w_player_sprite_direction, direction);
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

std::uint16_t pokemon_wram_offset(const gameboy::Emulator& emulator) {
    const auto primary = emulator.bus().read8(w_link_state);
    const auto localized = emulator.bus().read8(w_link_state_localized);
    return !plausible_link_state(primary) && plausible_link_state(localized) ? 5 : 0;
}

std::uint8_t player_map_x(const gameboy::Emulator& emulator) {
    const auto offset = pokemon_wram_offset(emulator);
    return emulator.bus().read8(static_cast<std::uint16_t>(0xD362 + offset));
}

std::uint8_t player_map_y(const gameboy::Emulator& emulator) {
    const auto offset = pokemon_wram_offset(emulator);
    return emulator.bus().read8(static_cast<std::uint16_t>(0xD361 + offset));
}

} // namespace gbb::link_harness
