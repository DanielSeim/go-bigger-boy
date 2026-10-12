#pragma once

#include <array>
#include <cstdint>

namespace gameboy {

// Only uncommon controllers allocate this state. No host pointers or native
// struct layout are persisted; the save codec writes every field explicitly.
struct CartridgeExtraState {
    std::array<std::uint8_t, 4> mmm{};
    std::uint8_t mode{};
    bool ir_input{}, ir_output{};
    bool mbc7_enable2{}, sensor_armed{};
    std::uint16_t sensor_x{0x81D0}, sensor_y{0x81D0};
    std::uint16_t latched_x{0x8000}, latched_y{0x8000};
    bool eeprom_writable{}, eeprom_do{true};
    std::uint8_t eeprom_pins{}, eeprom_phase{}, eeprom_bits{}, eeprom_address{};
    std::uint16_t eeprom_shift{}, eeprom_data{};
    std::uint8_t eeprom_operation{};
    std::uint32_t eeprom_busy{};
    std::array<std::uint8_t, 256> huc_clock{};
    std::uint8_t huc_address{}, huc_mailbox{}, huc_response{}, subminute{};
    bool tama_unlocked{};
    std::uint8_t tama_register{};
    std::array<std::uint8_t, 16> tama{};
    std::array<std::uint8_t, 64> tama_clock{};
    bool tama_running{};
};

// Pure arithmetic, also used on a temporary copy for side-effect-free reads.
void advance_cartridge_clock(CartridgeExtraState& state, bool tama,
                             std::uint64_t seconds) noexcept;

} // namespace gameboy
