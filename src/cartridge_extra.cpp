#include "gameboy/cartridge.hpp"
#include "cartridge_extra.hpp"

#include <algorithm>
#include <chrono>

namespace gameboy {
namespace {
constexpr unsigned eeprom_program_cycles = 20972; // 5 ms at the base clock.

unsigned nibbles(const std::array<std::uint8_t, 256>& data, unsigned start) noexcept {
    return data[start] | (unsigned(data[start + 1]) << 4) | (unsigned(data[start + 2]) << 8);
}
void set_nibbles(std::array<std::uint8_t, 256>& data, unsigned start, unsigned value) noexcept {
    for (unsigned i = 0; i < 3; ++i) data[start + i] = (value >> (4 * i)) & 15;
}
unsigned bcd(const std::array<std::uint8_t, 64>& data, unsigned start) noexcept {
    return data[start] + 10U * data[start + 1];
}
void set_bcd(std::array<std::uint8_t, 64>& data, unsigned start, unsigned value) noexcept {
    data[start] = value % 10; data[start + 1] = value / 10;
}
unsigned month_days(unsigned month, unsigned leap) noexcept {
    constexpr unsigned days[]{31,28,31,30,31,30,31,31,30,31,30,31};
    return days[month - 1] + (month == 2 && leap == 0 ? 1U : 0U);
}
unsigned tama_mask(unsigned page, unsigned index) noexcept {
    if (page >= 2) return 15;
    if (page == 1 && (index < 2 || index == 9 || index == 12)) return 0;
    if (page == 1 && index == 10) return 1;
    if (page == 1 && index == 11) return 3;
    if (index == 1 || index == 3 || index == 6) return 7;
    if (index == 5 || index == 8) return 3;
    if (index == 10) return 1;
    return 15;
}
} // namespace

void advance_cartridge_clock(CartridgeExtraState& state, const bool tama,
                             const std::uint64_t seconds) noexcept {
    if (!tama) {
        const auto total = seconds + state.subminute;
        state.subminute = total % 60;
        const auto minutes = total / 60 + nibbles(state.huc_clock, 0x10);
        const auto days = minutes / 1440 + nibbles(state.huc_clock, 0x13);
        set_nibbles(state.huc_clock, 0x10, minutes % 1440);
        set_nibbles(state.huc_clock, 0x13, days & 0xFFF);
        return;
    }
    if (!state.tama_running || !seconds) return;
    auto& data = state.tama_clock;
    // Invalid BCD/date carry behavior is not established. Preserve such data
    // instead of inventing a normalization or entering an unbounded loop.
    for (const unsigned i : {0U,2U,4U,7U,9U,11U})
        if (data[i] > 9 || (i != 4 && data[i + 1] > 9)) return;
    unsigned hour = bcd(data, 4);
    const bool full_day = data[16 + 10] != 0;
    if (!full_day) hour = data[4] + 10U * (data[5] & 1) + 12U * ((data[5] >> 1) & 1);
    unsigned month = bcd(data, 9), day = bcd(data, 7), year = bcd(data, 11);
    unsigned leap = data[16 + 11] & 3;
    if (bcd(data,0) >= 60 || bcd(data,2) >= 60 || hour >= 24 ||
        month < 1 || month > 12 || day < 1 || day > month_days(month, leap)) return;
    const auto total = seconds + bcd(data, 0) + 60ULL * bcd(data, 2) + 3600ULL * hour;
    auto days = total / 86400;
    set_bcd(data, 0, total % 60); set_bcd(data, 2, total / 60 % 60);
    hour = total / 3600 % 24;
    if (full_day) set_bcd(data, 4, hour);
    else { data[4] = hour % 12 % 10; data[5] = hour % 12 / 10 + (hour >= 12 ? 2 : 0); }
    data[6] = (data[6] + days % 7) % 7;
    // The calendar repeats after 100 years; the independent leap register
    // has a four-year cycle. Reduce before iterating (at most 1,200 months).
    days %= 36525;
    while (days) {
        const auto remaining = month_days(month, leap) - day + 1;
        if (days < remaining) { day += static_cast<unsigned>(days); break; }
        days -= remaining; day = 1;
        if (++month == 13) { month = 1; year = (year + 1) % 100; leap = (leap + 1) & 3; }
    }
    set_bcd(data, 7, day); set_bcd(data, 9, month); set_bcd(data, 11, year);
    data[16 + 11] = leap;
}

std::int64_t Cartridge::rtc_now() const noexcept {
    if (rtc_clock_) return rtc_clock_(rtc_clock_context_);
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}
void Cartridge::set_rtc_clock(RtcClock clock, void* context) noexcept {
    update_rtc(); rtc_clock_ = clock; rtc_clock_context_ = context;
    rtc_last_update_ = rtc_now();
}
bool Cartridge::has_accelerometer() const noexcept { return controller_ == Controller::mbc7; }
void Cartridge::set_accelerometer(std::uint16_t x, std::uint16_t y) noexcept {
    if (has_accelerometer()) { extra_->sensor_x = x; extra_->sensor_y = y; }
}
bool Cartridge::has_infrared() const noexcept {
    return controller_ == Controller::huc1 || controller_ == Controller::huc3;
}
void Cartridge::set_infrared_input(bool light) noexcept { if (has_infrared()) extra_->ir_input = light; }
bool Cartridge::infrared_output() const noexcept { return has_infrared() && extra_->ir_output; }

std::size_t Cartridge::extra_rom_bank(bool upper) const noexcept {
    if (controller_ != Controller::mmm01) return upper ? selected_rom_bank_ : 0;
    const auto& r = extra_->mmm;
    if (!(r[0] & 0x40)) return rom_bank_count_ - (upper ? 1 : 2);
    const auto rom_mask = (r[3] >> 1) & 0x1E;
    const auto ram_mask = (r[0] >> 4) & 3;
    const bool mux = (r[3] & 0x40) != 0;
    auto middle = mux ? (r[2] & 3) : (r[1] >> 5);
    if (!upper && mux && !(r[3] & 1)) middle &= ram_mask;
    auto low = r[1] & 0x1F;
    if (!upper) low &= rom_mask;
    else if (!(low & ~rom_mask)) low |= 1;
    return (((r[2] >> 4) & 3) << 7) | (middle << 5) | low;
}
std::size_t Cartridge::extra_ram_bank() const noexcept {
    if (controller_ != Controller::mmm01) return ram_rtc_select_;
    const auto& r = extra_->mmm;
    auto low = r[2] & 3;
    if (r[3] & 0x40) low = r[1] >> 5;
    else if (!(r[3] & 1)) low &= (r[0] >> 4) & 3;
    return (((r[2] >> 2) & 3) << 2) | low;
}

std::uint8_t Cartridge::read_extra(std::uint16_t address) const noexcept {
    const auto& state = *extra_;
    if (address < 0x8000) return read_rom_bank(extra_rom_bank(address >= 0x4000), address & 0x3FFF);
    if (address < 0xA000 || address > 0xBFFF) return 0xFF;
    if (controller_ == Controller::mbc7) {
        if (!ram_enabled_ || !state.mbc7_enable2 || address >= 0xB000) return 0xFF;
        switch (address & 0xF0) {
        case 0x20: return state.latched_x & 0xFF;
        case 0x30: return state.latched_x >> 8;
        case 0x40: return state.latched_y & 0xFF;
        case 0x50: return state.latched_y >> 8;
        case 0x60: return 0;
        case 0x80: return (state.eeprom_pins & 0xC2) | ((state.eeprom_busy && (state.eeprom_pins & 0x80)) ? 0 : state.eeprom_do);
        default: return 0xFF;
        }
    }
    if (controller_ == Controller::huc1 && state.mode == 0x0E)
        return 0xC0 | state.ir_input;
    if (controller_ == Controller::huc3) {
        switch (state.mode) {
        case 0: case 0xA: break;
        case 0xC: return 0x80 | (state.huc_mailbox & 0x70) | state.huc_response;
        case 0xD: return 0xFF; // Synchronous command execution; MCU ready.
        case 0xE: return 0xC0 | state.ir_input;
        default: return 0xFF;
        }
    }
    if (controller_ == Controller::tama5) {
        if (address & 1) return 0xFF;
        if (!state.tama_unlocked) return 0xF0;
        if (state.tama_register == 0xA) return 0xF1;
        if (state.tama_register != 0xC && state.tama_register != 0xD) return 0xFF;
        const auto command = (state.tama[6] << 4) | state.tama[7];
        unsigned value = 0xFF;
        if (command >= 0x20 && command < 0x40) value = ram_[command & 31];
        else if (command == 0x46 || command == 0x47 || (command & 0xF8) == 0x80) {
            auto snapshot = state;
            const auto now = rtc_now();
            if (now > rtc_last_update_)
                advance_cartridge_clock(snapshot, true, static_cast<std::uint64_t>(now - rtc_last_update_));
            if (command == 0x46) value = snapshot.tama_clock[2] | (snapshot.tama_clock[3] << 4);
            else if (command == 0x47) value = snapshot.tama_clock[4] | (snapshot.tama_clock[5] << 4);
            else if ((command & 1) && state.tama_register == 0xC) {
                const auto index = state.tama[4]; const auto page = (command >> 1) & 3;
                value = index < 13 ? snapshot.tama_clock[page * 16 + index] :
                        index == 13 ? (snapshot.tama_running ? 8 : 0) |
                                      (snapshot.tama_clock[13] & 4) | page : 0;
            }
        }
        return 0xF0 | ((state.tama_register == 0xD ? value >> 4 : value) & 15);
    }
    if ((controller_ == Controller::mmm01 && !ram_enabled_) || ram_.empty()) return 0xFF;
    return ram_[(extra_ram_bank() * 0x2000 + (address & 0x1FFF)) % ram_.size()];
}

void Cartridge::write_extra(std::uint16_t address, std::uint8_t value) noexcept {
    auto& state = *extra_;
    if (address < 0x8000) {
        const auto region = address >> 13;
        if (controller_ == Controller::mmm01) {
            auto& r = state.mmm;
            const bool mapped = (r[0] & 0x40) != 0;
            const auto rom_mask = (r[3] >> 1) & 0x1E;
            const auto ram_mask = (r[0] >> 4) & 3;
            switch (region) {
            case 0: ram_enabled_ = (value & 15) == 10;
                if (!mapped) r[0] = value & 0x7F;
                break;
            case 1: r[1] = (r[1] & rom_mask) | (value & (0x1F & ~rom_mask)) |
                           (mapped ? r[1] & 0x60 : value & 0x60); break;
            case 2: r[2] = (r[2] & ram_mask) | (value & (3 & ~ram_mask)) |
                           (mapped ? r[2] & 0x7C : value & 0x7C); break;
            case 3: r[3] = (mapped ? r[3] & 0x7E : value & 0x7C) |
                           ((r[2] & 0x40) ? r[3] & 1 : value & 1); break;
            }
        } else if (controller_ == Controller::mbc7) {
            if (region == 0) ram_enabled_ = value == 0x0A;
            if (region == 1) selected_rom_bank_ = value & 0x7F;
            if (region == 2) state.mbc7_enable2 = value == 0x40;
        } else if (controller_ == Controller::huc1 || controller_ == Controller::huc3) {
            if (region == 0) state.mode = controller_ == Controller::huc3 ? value & 15 : value;
            if (region == 1) selected_rom_bank_ = value & (controller_ == Controller::huc1 ? 0x3F : 0x7F);
            if (region == 2) ram_rtc_select_ = value & (controller_ == Controller::huc1 ? 3 : 15);
        }
        return;
    }
    if (address < 0xA000 || address > 0xBFFF) return;
    if (controller_ == Controller::mbc7) {
        if (!ram_enabled_ || !state.mbc7_enable2 || address >= 0xB000) return;
        switch (address & 0xF0) {
        case 0: if (value == 0x55) {
            state.latched_x = state.latched_y = 0x8000; state.sensor_armed = true;
        } break;
        case 0x10: if (value == 0xAA && state.sensor_armed) {
            state.latched_x = state.sensor_x; state.latched_y = state.sensor_y; state.sensor_armed = false;
        } break;
        case 0x80: write_eeprom(value); break;
        default: break;
        }
        return;
    }
    if (has_infrared() && state.mode == 0xE) { state.ir_output = (value & 1) != 0; return; }
    if (controller_ == Controller::huc3 && state.mode != 0xA) {
        if (state.mode == 0xB) state.huc_mailbox = value & 0x7F;
        else if (state.mode == 0xD && !(value & 1)) execute_huc_command();
        return;
    }
    if (controller_ == Controller::tama5) {
        if (address & 1) {
            state.tama_register = value & 15;
            if (state.tama_register == 0xA) state.tama_unlocked = true;
        } else if (state.tama_unlocked) {
            const auto reg = state.tama_register;
            if (reg == 0 || reg == 1 || reg == 4 || reg == 5 || reg == 6 || reg == 7) {
                state.tama[reg] = value & 15;
                if (reg <= 1) selected_rom_bank_ = state.tama[0] | ((state.tama[1] & 1) << 4);
                if (reg == 7) execute_tama_command();
            }
        }
        return;
    }
    if ((controller_ == Controller::mmm01 && !ram_enabled_) || ram_.empty()) return;
    const auto index = (extra_ram_bank() * 0x2000 + (address & 0x1FFF)) % ram_.size();
    if (ram_[index] != value) { ram_[index] = value; ram_dirty_ = true; }
}

void Cartridge::write_eeprom(std::uint8_t value) noexcept {
    auto& s = *extra_;
    const auto old = s.eeprom_pins; s.eeprom_pins = value & 0xC2;
    if (!(value & 0x80)) {
        if ((old & 0x80) && s.eeprom_operation && !s.eeprom_busy)
            s.eeprom_busy = eeprom_program_cycles;
        s.eeprom_phase = s.eeprom_bits = 0; s.eeprom_shift = 0; s.eeprom_do = true;
        return;
    }
    if (s.eeprom_busy || (old & 0x40) || !(value & 0x40)) return;
    const auto bit = (value >> 1) & 1;
    if (s.eeprom_phase == 0) {
        if (bit) { s.eeprom_phase = 1; s.eeprom_bits = 0; s.eeprom_shift = 0; }
    } else if (s.eeprom_phase == 1) {
        s.eeprom_shift = (s.eeprom_shift << 1) | bit;
        if (++s.eeprom_bits != 10) return;
        const auto decoded = s.eeprom_shift;
        const auto command = decoded >> 8;
        s.eeprom_address = s.eeprom_shift & 0x7F;
        s.eeprom_bits = 0; s.eeprom_shift = 0; s.eeprom_phase = 5;
        if (command == 2) { s.eeprom_phase = 2; s.eeprom_do = false; }
        else if (command == 1) s.eeprom_phase = 3;
        else if (command == 3) { if (s.eeprom_writable) s.eeprom_operation = 2; }
        else {
            switch ((decoded >> 6) & 3) {
            case 0: s.eeprom_writable = false; break;
            case 1: s.eeprom_phase = 4; break;
            case 2: if (s.eeprom_writable) s.eeprom_operation = 4; break;
            case 3: s.eeprom_writable = true; break;
            }
        }
    } else if (s.eeprom_phase == 2) {
        const auto word = (unsigned(ram_[s.eeprom_address * 2]) << 8) | ram_[s.eeprom_address * 2 + 1];
        s.eeprom_do = (word & (0x8000U >> s.eeprom_bits)) != 0;
        if (++s.eeprom_bits == 16) { s.eeprom_bits = 0; s.eeprom_address = (s.eeprom_address + 1) & 0x7F; }
    } else if (s.eeprom_phase == 3 || s.eeprom_phase == 4) {
        s.eeprom_shift = (s.eeprom_shift << 1) | bit;
        if (++s.eeprom_bits == 16) {
            if (s.eeprom_writable) { s.eeprom_data = s.eeprom_shift; s.eeprom_operation = s.eeprom_phase == 3 ? 1 : 3; }
            s.eeprom_phase = 5; s.eeprom_bits = 0;
        }
    }
}

void Cartridge::tick(unsigned cycles) noexcept {
    if (controller_ != Controller::mbc7 || !extra_->eeprom_busy) return;
    auto& s = *extra_;
    if (cycles < s.eeprom_busy) { s.eeprom_busy -= cycles; return; }
    s.eeprom_busy = 0;
    const auto write_word = [&](unsigned address, unsigned word) {
        ram_[2 * address] = word >> 8; ram_[2 * address + 1] = word & 255;
    };
    if (s.eeprom_operation == 1) write_word(s.eeprom_address, s.eeprom_data);
    else if (s.eeprom_operation == 2) write_word(s.eeprom_address, 0xFFFF);
    else for (unsigned i = 0; i < 128; ++i) write_word(i, s.eeprom_operation == 3 ? s.eeprom_data : 0xFFFF);
    s.eeprom_operation = 0; s.eeprom_do = true; ram_dirty_ = true;
}

void Cartridge::execute_huc_command() noexcept {
    update_rtc();
    auto& s = *extra_; auto& data = s.huc_clock;
    const auto argument = s.huc_mailbox & 15;
    switch (s.huc_mailbox >> 4) {
    case 1: s.huc_response = data[s.huc_address]; ++s.huc_address; break;
    case 3: data[s.huc_address++] = argument; break;
    case 4: s.huc_address = (s.huc_address & 0xF0) | argument; break;
    case 5: s.huc_address = (s.huc_address & 15) | (argument << 4); break;
    case 6:
        if (argument == 0) std::copy_n(data.begin() + 0x10, 6, data.begin());
        else if (argument == 1) {
            constexpr unsigned period = 4096U * 1440;
            const auto old = nibbles(data,0x10) + 1440U * nibbles(data,0x13);
            const auto current = nibbles(data,0) + 1440U * nibbles(data,3);
            const auto event = nibbles(data,0x58) + 1440U * nibbles(data,0x5B);
            const auto adjusted = (std::uint64_t(event) + 2ULL * period + current - old) % period;
            set_nibbles(data,0x58,adjusted % 1440); set_nibbles(data,0x5B,adjusted / 1440);
            std::copy_n(data.begin(),6,data.begin() + 0x10); s.subminute = 0;
        }
        // Documented extended status command returns 1; other undocumented
        // commands retain the response, rather than faking successful status.
        if (argument <= 2) s.huc_response = 1;
        break;
    default: break;
    }
    rtc_dirty_ = true;
}

void Cartridge::execute_tama_command() noexcept {
    update_rtc();
    auto& s = *extra_;
    const auto command = (s.tama[6] << 4) | s.tama[7];
    const auto value = s.tama[4] | (s.tama[5] << 4);
    if (command < 0x20) { ram_[command] = value; ram_dirty_ = true; }
    else if ((command & 0xF8) == 0x80 && !(command & 1) && s.tama[4] < 13) {
        const auto page = (command >> 1) & 3;
        const auto index = s.tama[4];
        s.tama_clock[page * 16 + index] = s.tama[5] & tama_mask(page,index);
    } else switch (command) {
    case 0x40: s.tama_running = false; break;
    case 0x41: s.tama_running = true; s.tama_clock[0] = s.tama_clock[1] = 0; break;
    case 0x44: s.tama_clock[2] = s.tama[4]; s.tama_clock[3] = s.tama[5] & 7; break;
    case 0x45: s.tama_clock[4] = s.tama[4]; s.tama_clock[5] = s.tama[5] & 3; break;
    case 0x50: s.tama_clock[13] &= ~4U; break;
    case 0x51: s.tama_clock[13] |= 4; break;
    default: break;
    }
    rtc_dirty_ = true;
}
} // namespace gameboy
