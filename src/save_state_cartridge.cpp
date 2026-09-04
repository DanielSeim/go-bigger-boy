#include "save_state_cartridge.hpp"

#include "gameboy/cartridge.hpp"

#include <chrono>

namespace gameboy {
namespace {

std::int64_t current_unix_seconds() noexcept {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

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
    cartridge.rtc_last_update_ = current_unix_seconds();
    cartridge.ram_dirty_ = cartridge.battery_ && !cartridge.ram_.empty();
    cartridge.rtc_dirty_ = cartridge.rtc_present_;
}

} // namespace gameboy
