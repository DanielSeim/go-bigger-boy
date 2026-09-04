#pragma once

#include "gameboy/emulator.hpp"
#include "semantic_tracker.hpp"

#include <cstdint>

namespace gbb::link_harness {

// Pokémon Red/Blue WRAM locations. European builds retain the same layout
// with a five-byte offset for the translated variables.
inline constexpr std::uint16_t w_link_state = 0xD12B;
inline constexpr std::uint16_t w_link_state_localized = w_link_state + 5;
inline constexpr std::uint16_t w_party_count = 0xD163;
inline constexpr std::uint16_t w_party_mon1 = 0xD16B;
inline constexpr std::uint16_t w_is_in_battle = 0xD057;
inline constexpr std::uint16_t w_is_in_battle_localized = w_is_in_battle + 5;
inline constexpr std::uint16_t w_top_menu_item_y = 0xCC24;
inline constexpr std::uint16_t w_top_menu_item_x = 0xCC25;
inline constexpr std::uint16_t w_current_menu_item = 0xCC26;
inline constexpr std::uint16_t w_cur_map = 0xD35E;
inline constexpr std::uint16_t w_player_direction = 0xD52A;
inline constexpr std::uint16_t w_player_sprite_direction = 0xC109;
inline constexpr std::uint16_t w_trade_pointer_index = 0xCC38;
inline constexpr std::uint16_t w_trade_menu = 0xCC49;
inline constexpr std::uint16_t w_serial_sync_receive = 0xCC3D;
inline constexpr std::uint16_t w_serial_nybble_receive = 0xCC3E;
inline constexpr std::uint16_t w_serial_nybble_temp = 0xCC3F;
inline constexpr std::uint16_t w_serial_nybble_send = 0xCC42;
inline constexpr std::uint16_t w_unknown_serial_counter = 0xCC47;
inline constexpr std::uint16_t w_text_box_id = 0xD125;
inline constexpr std::uint16_t w_two_option_menu_id = 0xD12C;
inline constexpr std::uint16_t party_mon_size = 0x2C;

inline constexpr std::uint8_t link_state_battling = 0x04;
inline constexpr std::uint8_t link_state_trading = 0x32;

class WramBank1Guard {
public:
    explicit WramBank1Guard(gameboy::Emulator& emulator);
    ~WramBank1Guard();

    WramBank1Guard(const WramBank1Guard&) = delete;
    WramBank1Guard& operator=(const WramBank1Guard&) = delete;

private:
    gameboy::MemoryBus& bus_;
    std::uint8_t previous_ = 1;
    bool switched_{};
};

[[nodiscard]] std::uint16_t read16be(const gameboy::Emulator& emulator,
                                     std::uint16_t address);
[[nodiscard]] PartySnapshot read_party(const gameboy::Emulator& emulator);
[[nodiscard]] bool same_party(const PartySnapshot& first,
                              const PartySnapshot& second);
[[nodiscard]] bool contains_mon(const PartySnapshot& party,
                                std::uint64_t signature);
[[nodiscard]] bool contains_party_member_from(const PartySnapshot& destination,
                                              const PartySnapshot& source);
[[nodiscard]] bool plausible_link_state(std::uint8_t state) noexcept;
[[nodiscard]] std::uint8_t effective_link_state(std::uint8_t primary,
                                                std::uint8_t alternate) noexcept;

[[nodiscard]] bool at_battle_trade_menu(const gameboy::Emulator& emulator);
[[nodiscard]] bool at_ready_link_choice(const gameboy::Emulator& emulator);
[[nodiscard]] bool at_trade_selection_menu(const gameboy::Emulator& emulator);
[[nodiscard]] bool at_trade_stats_menu(const gameboy::Emulator& emulator);
[[nodiscard]] bool at_trade_cancel_menu(const gameboy::Emulator& emulator);
[[nodiscard]] bool at_cable_club_map(const gameboy::Emulator& emulator);

void select_joypad_lines(gameboy::Emulator& emulator, bool actions);
void set_facing_direction(gameboy::Emulator& emulator, std::uint8_t direction);
void reset_pokemon_link_handshake(gameboy::Emulator& emulator);
[[nodiscard]] std::uint16_t pokemon_wram_offset(
    const gameboy::Emulator& emulator);
[[nodiscard]] std::uint8_t player_map_x(const gameboy::Emulator& emulator);
[[nodiscard]] std::uint8_t player_map_y(const gameboy::Emulator& emulator);

} // namespace gbb::link_harness
