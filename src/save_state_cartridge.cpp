#include "save_state_cartridge.hpp"

#include "gameboy/cartridge.hpp"
#include "cartridge_extra.hpp"

#include <algorithm>

namespace gameboy {
namespace {

template <std::size_t Size>
void write_bytes(save_state_format::Writer& writer,
                 const std::array<std::uint8_t, Size>& values) {
    writer.bytes(values.data(), values.size());
}

template <std::size_t Size>
void read_bytes(save_state_format::Reader& reader,
                std::array<std::uint8_t, Size>& values) {
    reader.bytes(values.data(), values.size());
}

} // namespace

void SaveStateCartridgeCodec::write(save_state_format::Writer& writer,
                                     const Cartridge& cartridge) {
    writer.u32(static_cast<std::uint32_t>(cartridge.ram_.size()));
    if (!cartridge.ram_.empty()) {
        writer.bytes(cartridge.ram_.data(), cartridge.ram_.size());
    }
    writer.boolean(cartridge.rumble_active_);
    writer.boolean(cartridge.ram_enabled_);
    writer.u8(cartridge.rom_bank_low_);
    writer.u8(cartridge.bank_upper_);
    writer.u8(cartridge.banking_mode_);
    writer.u16(cartridge.selected_rom_bank_);
    writer.u8(cartridge.ram_rtc_select_);
    writer.u8(cartridge.rtc_latch_value_);
    write_bytes(writer, cartridge.rtc_);
    write_bytes(writer, cartridge.latched_rtc_);
}

void SaveStateCartridgeCodec::read(save_state_format::Reader& reader,
                                    Cartridge& cartridge) {
    const auto ram_size = static_cast<std::size_t>(reader.u32());
    if (ram_size != cartridge.ram_.size()) {
        throw SaveStateError("Save-state cartridge RAM size does not match");
    }
    if (!cartridge.ram_.empty()) {
        reader.bytes(cartridge.ram_.data(), cartridge.ram_.size());
    }
    cartridge.rumble_active_ = reader.boolean() && cartridge.rumble_present_;
    cartridge.ram_enabled_ = reader.boolean();
    cartridge.rom_bank_low_ = reader.u8();
    cartridge.bank_upper_ = reader.u8();
    cartridge.banking_mode_ = reader.u8();
    cartridge.selected_rom_bank_ = reader.u16();
    cartridge.ram_rtc_select_ = reader.u8();
    cartridge.rtc_latch_value_ = reader.u8();
    read_bytes(reader, cartridge.rtc_);
    read_bytes(reader, cartridge.latched_rtc_);
    cartridge.rtc_last_update_ = cartridge.rtc_now();
    cartridge.ram_dirty_ = cartridge.battery_ && !cartridge.ram_.empty();
    cartridge.rtc_dirty_ = cartridge.rtc_present_;
}

void SaveStateCartridgeCodec::write_extra(save_state_format::Writer& w, const Cartridge& cartridge) {
    w.boolean(static_cast<bool>(cartridge.extra_));
    if (!cartridge.extra_) return;
    w.u8(cartridge.type());
    const auto& s = *cartridge.extra_;
    write_bytes(w,s.mmm); w.u8(s.mode); w.boolean(s.ir_input); w.boolean(s.ir_output);
    w.boolean(s.mbc7_enable2); w.boolean(s.sensor_armed);
    w.u16(s.sensor_x); w.u16(s.sensor_y); w.u16(s.latched_x); w.u16(s.latched_y);
    w.boolean(s.eeprom_writable); w.boolean(s.eeprom_do);
    w.u8(s.eeprom_pins); w.u8(s.eeprom_phase); w.u8(s.eeprom_bits); w.u8(s.eeprom_address);
    w.u16(s.eeprom_shift); w.u16(s.eeprom_data); w.u8(s.eeprom_operation); w.u32(s.eeprom_busy);
    write_bytes(w,s.huc_clock); w.u8(s.huc_address); w.u8(s.huc_mailbox); w.u8(s.huc_response); w.u8(s.subminute);
    w.boolean(s.tama_unlocked); w.u8(s.tama_register); write_bytes(w,s.tama); write_bytes(w,s.tama_clock);
    w.boolean(s.tama_running);
}

void SaveStateCartridgeCodec::read_extra(save_state_format::Reader& r, Cartridge& cartridge) {
    if (r.boolean() != static_cast<bool>(cartridge.extra_))
        throw SaveStateError("Save-state extended controller does not match cartridge");
    if (!cartridge.extra_) return;
    if (r.u8() != cartridge.type()) throw SaveStateError("Save-state controller type does not match");
    auto s = *cartridge.extra_;
    read_bytes(r,s.mmm); s.mode = r.u8(); s.ir_input = r.boolean(); s.ir_output = r.boolean();
    s.mbc7_enable2 = r.boolean(); s.sensor_armed = r.boolean();
    s.sensor_x = r.u16(); s.sensor_y = r.u16(); s.latched_x = r.u16(); s.latched_y = r.u16();
    s.eeprom_writable = r.boolean(); s.eeprom_do = r.boolean();
    s.eeprom_pins = r.u8(); s.eeprom_phase = r.u8(); s.eeprom_bits = r.u8(); s.eeprom_address = r.u8();
    s.eeprom_shift = r.u16(); s.eeprom_data = r.u16(); s.eeprom_operation = r.u8(); s.eeprom_busy = r.u32();
    read_bytes(r,s.huc_clock); s.huc_address = r.u8(); s.huc_mailbox = r.u8(); s.huc_response = r.u8(); s.subminute = r.u8();
    s.tama_unlocked = r.boolean(); s.tama_register = r.u8(); read_bytes(r,s.tama); read_bytes(r,s.tama_clock);
    s.tama_running = r.boolean();
    const auto nibbles_valid = [](const auto& bytes) {
        return std::all_of(bytes.begin(),bytes.end(),[](auto v){return v < 16;});
    };
    if (s.eeprom_phase > 5 || s.eeprom_bits > 15 || s.eeprom_address > 127 ||
        s.eeprom_operation > 4 || s.eeprom_busy > 20972 || (s.eeprom_busy && !s.eeprom_operation) ||
        (s.eeprom_pins & ~0xC2U) || s.subminute >= 60 || s.huc_response > 15 || s.huc_mailbox > 127 ||
        s.tama_register > 15 || !nibbles_valid(s.tama) || !nibbles_valid(s.tama_clock) ||
        !nibbles_valid(s.huc_clock) ||
        std::any_of(s.mmm.begin(),s.mmm.end(),[](auto v){return v > 127;}))
        throw SaveStateError("Invalid extended controller state");
    *cartridge.extra_ = s;
    cartridge.rtc_last_update_ = cartridge.rtc_now();
}

} // namespace gameboy
