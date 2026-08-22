#include "gameboy/memory_bus.hpp"

#include <utility>

namespace gameboy {
namespace {
constexpr unsigned serial_transfer_cycles = 4096;
}

MemoryBus::MemoryBus(Cartridge cartridge) : cartridge_(std::move(cartridge)) {}

void MemoryBus::initialize_post_boot() noexcept {
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
    if (address <= 0xDFFF) {
        return wram_[address - 0xC000];
    }
    if (address <= 0xFDFF) {
        return wram_[address - 0xE000];
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
        return static_cast<std::uint8_t>(0x7E | (io_[0x02] & 0x81));
    case 0xFF04: return timer_.divider();
    case 0xFF05: return timer_.counter();
    case 0xFF06: return timer_.modulo();
    case 0xFF07: return timer_.control();
    default: break;
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

void MemoryBus::write8(const std::uint16_t address, const std::uint8_t value) noexcept {
    if (address <= 0x7FFF || (address >= 0xA000 && address <= 0xBFFF)) {
        cartridge_.write(address, value);
    } else if (address <= 0x9FFF) {
        ppu_.write_vram(address, value);
    } else if (address <= 0xDFFF) {
        wram_[address - 0xC000] = value;
    } else if (address <= 0xFDFF) {
        wram_[address - 0xE000] = value;
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
        io_[0x02] = static_cast<std::uint8_t>(value & 0x81);
        serial_cycles_remaining_ = (io_[0x02] & 0x81) == 0x81
                                       ? serial_transfer_cycles
                                       : 0;
    } else if (address == 0xFF04) {
        timer_.write_divider();
    } else if (address == 0xFF05) {
        timer_.write_counter(value);
    } else if (address == 0xFF06) {
        timer_.write_modulo(value);
    } else if (address == 0xFF07) {
        timer_.write_control(value);
    } else if (address == 0xFF46) {
        io_[0x46] = value;
        const auto source = static_cast<std::uint16_t>(value << 8);
        for (unsigned offset = 0; offset < 0xA0; ++offset) {
            ppu_.dma_write_oam(
                offset, read8(static_cast<std::uint16_t>(source + offset)));
        }
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
    const auto ppu_requests = ppu_.tick(cycles);
    if ((ppu_requests & 0x01) != 0) {
        request_interrupt(0);
    }
    if ((ppu_requests & 0x02) != 0) {
        request_interrupt(1);
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

void MemoryBus::flush_battery() { cartridge_.flush_battery(); }

const Ppu::Framebuffer& MemoryBus::framebuffer() const noexcept {
    return ppu_.framebuffer();
}

bool MemoryBus::frame_ready() const noexcept { return ppu_.frame_ready(); }

void MemoryBus::consume_frame() noexcept { ppu_.consume_frame(); }

std::string MemoryBus::take_serial_output() {
    auto output = std::move(serial_output_);
    serial_output_.clear();
    return output;
}

} // namespace gameboy
