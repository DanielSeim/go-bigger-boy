#include "gameboy/memory_bus.hpp"

#include <utility>

namespace gameboy {
namespace {
constexpr unsigned serial_transfer_cycles = 4096;
constexpr unsigned oam_dma_byte_cycles = 4;
constexpr unsigned oam_dma_size = 0xA0;
constexpr unsigned oam_dma_start_cycles = 8;
}

MemoryBus::MemoryBus(Cartridge cartridge)
    : cartridge_(std::move(cartridge)),
      cgb_wram_(std::make_unique<std::array<std::uint8_t, 0x6000>>()),
      cgb_mode_(cartridge_.supports_cgb()) {
    ppu_.set_cgb_mode(cgb_mode_);
}

void MemoryBus::initialize_post_boot() noexcept {
    apu_.initialize_post_boot();
    static_cast<void>(joypad_.write(0x00));
    io_[0x0F] = 0xE1;
    static_cast<void>(ppu_.write_register(0xFF42, 0x00));
    static_cast<void>(ppu_.write_register(0xFF43, 0x00));
    static_cast<void>(ppu_.write_register(0xFF45, 0x00));
    static_cast<void>(ppu_.write_register(0xFF47, 0xFC));
    static_cast<void>(ppu_.write_register(0xFF48, 0xFF));
    static_cast<void>(ppu_.write_register(0xFF49, 0xFF));
    static_cast<void>(ppu_.write_register(0xFF4A, 0x00));
    static_cast<void>(ppu_.write_register(0xFF4B, 0x00));
    if (ppu_.write_register(0xFF40, 0x91)) {
        request_interrupt(1);
    }
}

std::uint8_t MemoryBus::read8(const std::uint16_t address) const noexcept {
    if (address <= 0x7FFF || (address >= 0xA000 && address <= 0xBFFF)) {
        return cartridge_.read(address);
    }
    if (address <= 0x9FFF) {
        return ppu_.read_vram(address);
    }
    if (address <= 0xFDFF) {
        return read_wram(address);
    }
    if (address <= 0xFE9F) {
        return ppu_.read_oam(address);
    }
    if (address <= 0xFEFF) {
        return 0xFF;
    }
    if (address == 0xFF00) {
        return joypad_.read();
    }
    switch (address) {
    case 0xFF01: return io_[0x01];
    case 0xFF02:
        return static_cast<std::uint8_t>((cgb_mode_ ? 0x7C : 0x7E) |
                                         (io_[0x02] &
                                          (cgb_mode_ ? 0x83 : 0x81)));
    case 0xFF04: return timer_.divider();
    case 0xFF05: return timer_.counter();
    case 0xFF06: return timer_.modulo();
    case 0xFF07: return timer_.control();
    case 0xFF4D:
        return cgb_mode_
                   ? static_cast<std::uint8_t>(
                         0x7E | (double_speed_ ? 0x80 : 0) |
                         (speed_switch_requested_ ? 0x01 : 0))
                   : 0xFF;
    case 0xFF51: return cgb_mode_ ? static_cast<std::uint8_t>(hdma_source_ >> 8)
                                  : 0xFF;
    case 0xFF52: return cgb_mode_ ? static_cast<std::uint8_t>(hdma_source_ & 0xF0)
                                  : 0xFF;
    case 0xFF53:
        return cgb_mode_ ? static_cast<std::uint8_t>(0xE0 |
                                                     ((hdma_destination_ >> 8) & 0x1F))
                         : 0xFF;
    case 0xFF54:
        return cgb_mode_ ? static_cast<std::uint8_t>(hdma_destination_ & 0xF0)
                         : 0xFF;
    case 0xFF55:
        if (!cgb_mode_) return 0xFF;
        return hdma_active_
                   ? static_cast<std::uint8_t>(hdma_blocks_remaining_ - 1)
                   : static_cast<std::uint8_t>(
                         0x80 | (hdma_blocks_remaining_ == 0
                                     ? 0x7F
                                     : hdma_blocks_remaining_ - 1));
    case 0xFF70:
        return cgb_mode_ ? static_cast<std::uint8_t>(0xF8 | wram_bank_) : 0xFF;
    default: break;
    }
    if (Apu::handles_register(address)) {
        return apu_.read_register(address);
    }
    if (Ppu::handles_register(address)) {
        return ppu_.read_register(address);
    }
    if (address <= 0xFF7F) {
        return io_[address - 0xFF00];
    }
    if (address <= 0xFFFE) {
        return hram_[address - 0xFF80];
    }
    return interrupt_enable_;
}

std::uint16_t MemoryBus::read16(const std::uint16_t address) const noexcept {
    const auto low = read8(address);
    const auto high = read8(static_cast<std::uint16_t>(address + 1));
    return static_cast<std::uint16_t>(low | (static_cast<std::uint16_t>(high) << 8));
}

std::uint8_t MemoryBus::cpu_read8(const std::uint16_t address) const noexcept {
    if (address == 0xFF46) return read8(address);
    if (oam_dma_blocks(address)) return 0xFF;
    return read8(address);
}

void MemoryBus::cpu_write8(const std::uint16_t address,
                           const std::uint8_t value) noexcept {
    if (address != 0xFF46 && oam_dma_blocks(address)) return;
    write8(address, value);
}

bool MemoryBus::oam_dma_blocks(const std::uint16_t address) const noexcept {
    if (!oam_dma_active_) return false;
    if (address >= 0xFE00 && address <= 0xFE9F) return true;

    const auto dma_uses_video_bus =
        oam_dma_source_ >= 0x8000 && oam_dma_source_ <= 0x9FFF;
    if (address >= 0x8000 && address <= 0x9FFF) {
        return dma_uses_video_bus;
    }
    const auto cpu_uses_main_bus = address <= 0x7FFF ||
                                   (address >= 0xA000 && address <= 0xFDFF);
    return cpu_uses_main_bus && !dma_uses_video_bus;
}

void MemoryBus::write8(const std::uint16_t address, const std::uint8_t value) noexcept {
    if (address <= 0x7FFF || (address >= 0xA000 && address <= 0xBFFF)) {
        cartridge_.write(address, value);
    } else if (address <= 0x9FFF) {
        ppu_.write_vram(address, value);
    } else if (address <= 0xFDFF) {
        write_wram(address, value);
    } else if (address <= 0xFE9F) {
        ppu_.write_oam(address, value);
    } else if (address <= 0xFEFF) {
        // This region is unusable on Game Boy hardware.
    } else if (address == 0xFF00) {
        if (joypad_.write(value)) {
            request_interrupt(4);
        }
    } else if (address == 0xFF01) {
        io_[0x01] = value;
    } else if (address == 0xFF02) {
        io_[0x02] = static_cast<std::uint8_t>(
            value & (cgb_mode_ ? 0x83 : 0x81));
        serial_cycles_remaining_ = (io_[0x02] & 0x81) == 0x81
                                       ? (cgb_mode_ && (io_[0x02] & 0x02) != 0
                                              ? 128
                                              : serial_transfer_cycles)
                                       : 0;
    } else if (address == 0xFF04) {
        timer_.write_divider();
        for (auto ticks = timer_.take_apu_ticks(); ticks > 0; --ticks) {
            apu_.clock_frame_sequencer();
        }
    } else if (address == 0xFF05) {
        timer_.write_counter(value);
    } else if (address == 0xFF06) {
        timer_.write_modulo(value);
    } else if (address == 0xFF07) {
        timer_.write_control(value);
    } else if (address == 0xFF46) {
        io_[0x46] = value;
        const auto source_page = value >= 0xE0
                                     ? static_cast<std::uint8_t>(value - 0x20)
                                     : value;
        oam_dma_pending_source_ =
            static_cast<std::uint16_t>(source_page << 8);
        oam_dma_start_delay_ = oam_dma_start_cycles;
    } else if (address == 0xFF4D) {
        if (cgb_mode_) speed_switch_requested_ = (value & 0x01) != 0;
    } else if (address >= 0xFF51 && address <= 0xFF55) {
        write_hdma_register(address, value);
    } else if (address == 0xFF70) {
        if (cgb_mode_) {
            wram_bank_ = static_cast<std::uint8_t>(value & 0x07);
            if (wram_bank_ == 0) wram_bank_ = 1;
        }
    } else if (Apu::handles_register(address)) {
        apu_.write_register(address, value);
    } else if (Ppu::handles_register(address)) {
        if (ppu_.write_register(address, value)) {
            request_interrupt(1);
        }
    } else if (address <= 0xFF7F) {
        io_[address - 0xFF00] = value;
    } else if (address <= 0xFFFE) {
        hram_[address - 0xFF80] = value;
    } else {
        interrupt_enable_ = value;
    }
}

void MemoryBus::tick(const unsigned cycles) noexcept {
    const auto peripheral_cycles = double_speed_ ? cycles / 2 : cycles;
    tick_oam_dma(peripheral_cycles);
    apu_.tick(peripheral_cycles);
    if (serial_cycles_remaining_ != 0) {
        if (cycles >= serial_cycles_remaining_) {
            serial_cycles_remaining_ = 0;
            serial_output_.push_back(static_cast<char>(io_[0x01]));
            io_[0x01] = 0xFF;
            io_[0x02] = static_cast<std::uint8_t>(io_[0x02] & ~0x80U);
            request_interrupt(3);
        } else {
            serial_cycles_remaining_ -= cycles;
        }
    }
    if (timer_.tick(cycles)) {
        request_interrupt(2);
    }
    for (auto ticks = timer_.take_apu_ticks(); ticks > 0; --ticks) {
        apu_.clock_frame_sequencer();
    }
    const auto ppu_requests = ppu_.tick(peripheral_cycles);
    if ((ppu_requests & 0x01) != 0) {
        request_interrupt(0);
    }
    if ((ppu_requests & 0x02) != 0) {
        request_interrupt(1);
    }
    if ((ppu_requests & 0x04) != 0 && hdma_active_) {
        transfer_hdma_block();
    }
}

void MemoryBus::write_hdma_register(const std::uint16_t address,
                                    const std::uint8_t value) noexcept {
    if (!cgb_mode_) return;
    switch (address) {
    case 0xFF51:
        hdma_source_ = static_cast<std::uint16_t>((value << 8) |
                                                  (hdma_source_ & 0x00F0));
        break;
    case 0xFF52:
        hdma_source_ = static_cast<std::uint16_t>((hdma_source_ & 0xFF00) |
                                                  (value & 0xF0));
        break;
    case 0xFF53:
        hdma_destination_ = static_cast<std::uint16_t>(
            0x8000 | ((value & 0x1F) << 8) | (hdma_destination_ & 0x00F0));
        break;
    case 0xFF54:
        hdma_destination_ = static_cast<std::uint16_t>(
            (hdma_destination_ & 0xFF00) | (value & 0xF0));
        break;
    case 0xFF55:
        if (hdma_active_ && (value & 0x80) == 0) {
            hdma_active_ = false;
            return;
        }
        hdma_blocks_remaining_ = static_cast<std::uint8_t>((value & 0x7F) + 1);
        hdma_active_ = (value & 0x80) != 0 &&
                       (ppu_.read_register(0xFF40) & 0x80) != 0;
        if (!hdma_active_) {
            while (hdma_blocks_remaining_ != 0) transfer_hdma_block();
        }
        break;
    default: break;
    }
}

void MemoryBus::transfer_hdma_block() noexcept {
    if (hdma_blocks_remaining_ == 0) {
        hdma_active_ = false;
        return;
    }
    for (unsigned byte = 0; byte < 0x10; ++byte) {
        ppu_.dma_write_vram(hdma_destination_, read8(hdma_source_));
        hdma_source_ = static_cast<std::uint16_t>(hdma_source_ + 1);
        hdma_destination_ = static_cast<std::uint16_t>(
            0x8000 | ((hdma_destination_ + 1) & 0x1FFF));
    }
    --hdma_blocks_remaining_;
    if (hdma_blocks_remaining_ == 0) hdma_active_ = false;
}

void MemoryBus::tick_oam_dma(const unsigned cycles) noexcept {
    const auto copy_byte = [this]() {
        const auto source = static_cast<std::uint16_t>(oam_dma_source_ +
                                                       oam_dma_index_);
        ppu_.dma_write_oam(oam_dma_index_, read8(source));
        ++oam_dma_index_;
        if (oam_dma_index_ == oam_dma_size) {
            oam_dma_active_ = false;
            oam_dma_cycle_ = 0;
        }
    };

    for (unsigned cycle = 0; cycle < cycles; ++cycle) {
        if (oam_dma_start_delay_ != 0) {
            --oam_dma_start_delay_;
            if (oam_dma_start_delay_ == 0) {
                oam_dma_source_ = oam_dma_pending_source_;
                oam_dma_index_ = 0;
                oam_dma_cycle_ = 0;
                oam_dma_active_ = true;
                continue;
            }
        }
        if (!oam_dma_active_) continue;
        if (++oam_dma_cycle_ == oam_dma_byte_cycles) {
            oam_dma_cycle_ = 0;
            copy_byte();
        }
    }
}

void MemoryBus::request_interrupt(const unsigned index) noexcept {
    if (index < 5) {
        io_[0x0F] = static_cast<std::uint8_t>(io_[0x0F] | (1U << index));
    }
}

void MemoryBus::set_button(const Button button, const bool pressed) noexcept {
    if (joypad_.set_button(button, pressed)) {
        request_interrupt(4);
    }
}

void MemoryBus::write16(const std::uint16_t address, const std::uint16_t value) noexcept {
    write8(address, static_cast<std::uint8_t>(value & 0xFF));
    write8(static_cast<std::uint16_t>(address + 1),
           static_cast<std::uint8_t>(value >> 8));
}

const Cartridge& MemoryBus::cartridge() const noexcept {
    return cartridge_;
}

Cartridge& MemoryBus::cartridge() noexcept { return cartridge_; }

bool MemoryBus::cgb_mode() const noexcept { return cgb_mode_; }

bool MemoryBus::double_speed() const noexcept { return double_speed_; }

void MemoryBus::set_dmg_palette(const DmgPalette& palette) noexcept {
    ppu_.set_dmg_palette(palette);
}

bool MemoryBus::try_speed_switch() noexcept {
    if (!cgb_mode_ || !speed_switch_requested_) return false;
    speed_switch_requested_ = false;
    double_speed_ = !double_speed_;
    timer_.set_double_speed(double_speed_);
    return true;
}

std::uint8_t MemoryBus::read_wram(std::uint16_t address) const noexcept {
    if (address >= 0xE000) address = static_cast<std::uint16_t>(address - 0x2000);
    if (address < 0xD000 || !cgb_mode_ || wram_bank_ == 1) {
        return wram_[address - 0xC000];
    }
    const auto offset = static_cast<std::size_t>(wram_bank_ - 2) * 0x1000 +
                        (address - 0xD000);
    return (*cgb_wram_)[offset];
}

void MemoryBus::write_wram(std::uint16_t address,
                           const std::uint8_t value) noexcept {
    if (address >= 0xE000) address = static_cast<std::uint16_t>(address - 0x2000);
    if (address < 0xD000 || !cgb_mode_ || wram_bank_ == 1) {
        wram_[address - 0xC000] = value;
        return;
    }
    const auto offset = static_cast<std::size_t>(wram_bank_ - 2) * 0x1000 +
                        (address - 0xD000);
    (*cgb_wram_)[offset] = value;
}

void MemoryBus::flush_battery() { cartridge_.flush_battery(); }

const Ppu::Framebuffer& MemoryBus::framebuffer() const noexcept {
    return ppu_.framebuffer();
}

bool MemoryBus::frame_ready() const noexcept { return ppu_.frame_ready(); }

void MemoryBus::consume_frame() noexcept { ppu_.consume_frame(); }

std::vector<std::int16_t> MemoryBus::take_audio_samples() {
    return apu_.take_samples();
}

std::string MemoryBus::take_serial_output() {
    auto output = std::move(serial_output_);
    serial_output_.clear();
    return output;
}

} // namespace gameboy
