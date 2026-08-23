#pragma once

#include "gameboy/apu.hpp"
#include "gameboy/cartridge.hpp"
#include "gameboy/joypad.hpp"
#include "gameboy/ppu.hpp"
#include "gameboy/timer.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace gameboy {

class SaveStateCodec;
class Cpu;

class MemoryBus {
public:
    explicit MemoryBus(Cartridge cartridge);
    void initialize_post_boot() noexcept;

    [[nodiscard]] std::uint8_t read8(std::uint16_t address) const noexcept;
    [[nodiscard]] std::uint16_t read16(std::uint16_t address) const noexcept;
    void write8(std::uint16_t address, std::uint8_t value) noexcept;
    void write16(std::uint16_t address, std::uint16_t value) noexcept;
    void tick(unsigned cycles) noexcept;
    void request_interrupt(unsigned index) noexcept;
    void set_button(Button button, bool pressed) noexcept;

    [[nodiscard]] const Cartridge& cartridge() const noexcept;
    void flush_battery();
    [[nodiscard]] const Ppu::Framebuffer& framebuffer() const noexcept;
    [[nodiscard]] bool frame_ready() const noexcept;
    void consume_frame() noexcept;
    [[nodiscard]] std::vector<std::int16_t> take_audio_samples();
    [[nodiscard]] std::string take_serial_output();

private:
    friend class Cpu;
    friend class SaveStateCodec;

    [[nodiscard]] std::uint8_t cpu_read8(std::uint16_t address) const noexcept;
    void cpu_write8(std::uint16_t address, std::uint8_t value) noexcept;
    [[nodiscard]] bool oam_dma_blocks(std::uint16_t address) const noexcept;
    void tick_oam_dma(unsigned cycles) noexcept;

    Cartridge cartridge_;
    std::array<std::uint8_t, 0x2000> wram_{};
    std::array<std::uint8_t, 0x80> io_{};
    std::array<std::uint8_t, 0x7F> hram_{};
    std::uint8_t interrupt_enable_{};
    Joypad joypad_{};
    Apu apu_{};
    Ppu ppu_{};
    Timer timer_{};
    std::string serial_output_{};
    unsigned serial_cycles_remaining_{};
    std::uint16_t oam_dma_source_{};
    std::uint16_t oam_dma_index_{};
    unsigned oam_dma_cycle_{};
    bool oam_dma_active_{};
    std::uint16_t oam_dma_pending_source_{};
    unsigned oam_dma_start_delay_{};
};

} // namespace gameboy
