#pragma once

#include "gameboy/apu.hpp"
#include "gameboy/cartridge.hpp"
#include "gameboy/hardware_model.hpp"
#include "gameboy/joypad.hpp"
#include "gameboy/ppu.hpp"
#include "gameboy/printer.hpp"
#include "gameboy/serial.hpp"
#include "gameboy/timer.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace gameboy {

class SaveStateCodec;
class SaveStateBusCodec;
class Cpu;

class MemoryBus {
public:
    explicit MemoryBus(Cartridge cartridge);
    void initialize_post_boot(HardwareModel model = HardwareModel::dmg) noexcept;

    [[nodiscard]] std::uint8_t read8(std::uint16_t address) const noexcept;
    [[nodiscard]] std::uint16_t read16(std::uint16_t address) const noexcept;
    void write8(std::uint16_t address, std::uint8_t value) noexcept;
    void write16(std::uint16_t address, std::uint16_t value) noexcept;
    void tick(unsigned cycles) noexcept;
    void request_interrupt(unsigned index) noexcept;
    void set_button(Button button, bool pressed) noexcept;

    [[nodiscard]] const Cartridge& cartridge() const noexcept;
    [[nodiscard]] Cartridge& cartridge() noexcept;
    [[nodiscard]] bool cgb_mode() const noexcept;
    [[nodiscard]] bool double_speed() const noexcept;
    void set_dmg_palette(const DmgPalette& palette) noexcept;
    [[nodiscard]] std::uint8_t debug_read_vram(std::uint8_t bank,
                                               std::uint16_t offset) const noexcept;
    [[nodiscard]] std::uint8_t debug_read_oam(std::uint8_t offset) const noexcept;
    void debug_write_oam(std::uint8_t offset, std::uint8_t value) noexcept;
    [[nodiscard]] std::uint8_t debug_read_cgb_bg_palette(
        std::uint8_t index) const noexcept;
    [[nodiscard]] std::uint8_t debug_read_cgb_object_palette(
        std::uint8_t index) const noexcept;
    void debug_write_vram(std::uint8_t bank, std::uint16_t offset,
                          std::uint8_t value) noexcept;
    void flush_battery();
    [[nodiscard]] const Ppu::Framebuffer& framebuffer() const noexcept;
    [[nodiscard]] bool frame_ready() const noexcept;
    void consume_frame() noexcept;
    [[nodiscard]] std::vector<std::int16_t> take_audio_samples();
    [[nodiscard]] std::string take_serial_output();
    [[nodiscard]] SerialPort& serial_port() noexcept;
    [[nodiscard]] const SerialPort& serial_port() const noexcept;
    void connect_printer(bool connected = true) noexcept;
    [[nodiscard]] std::vector<PrinterImage> take_printer_images();

private:
    friend class Cpu;
    friend class SaveStateCodec;
    friend class SaveStateBusCodec;

    [[nodiscard]] std::uint8_t cpu_read8(std::uint16_t address) const noexcept;
    void cpu_write8(std::uint16_t address, std::uint8_t value) noexcept;
    [[nodiscard]] bool oam_dma_blocks(std::uint16_t address) const noexcept;
    [[nodiscard]] std::uint8_t read_wram(std::uint16_t address) const noexcept;
    void write_wram(std::uint16_t address, std::uint8_t value) noexcept;
    void tick_oam_dma(unsigned cycles) noexcept;
    void write_hdma_register(std::uint16_t address, std::uint8_t value) noexcept;
    void transfer_hdma_block() noexcept;
    [[nodiscard]] bool try_speed_switch() noexcept;
    static void serial_transfer_complete(void*, std::uint8_t,
                                         std::uint8_t) noexcept;
    void handle_serial_transfer(std::uint8_t transmitted,
                                std::uint8_t received) noexcept;

    Cartridge cartridge_;
    std::array<std::uint8_t, 0x2000> wram_{};
    std::unique_ptr<std::array<std::uint8_t, 0x6000>> cgb_wram_;
    std::array<std::uint8_t, 0x80> io_{};
    std::array<std::uint8_t, 0x7F> hram_{};
    std::uint8_t interrupt_enable_{};
    Joypad joypad_{};
    Apu apu_{};
    Ppu ppu_{};
    Timer timer_{};
    SerialPort serial_{};
    std::string serial_output_{};
    GameBoyPrinter printer_{};
    bool printer_connected_{};
    unsigned serial_cycles_remaining_{};
    std::uint16_t serial_clock_{};
    std::uint16_t oam_dma_source_{};
    std::uint16_t oam_dma_index_{};
    unsigned oam_dma_cycle_{};
    bool oam_dma_active_{};
    std::uint16_t oam_dma_pending_source_{};
    unsigned oam_dma_start_delay_{};
    std::uint8_t wram_bank_{1};
    bool cgb_mode_{};
    bool cgb_hardware_{};
    bool apu_cycle_phase_{};
    std::uint16_t hdma_source_{};
    std::uint16_t hdma_destination_{0x8000};
    std::uint8_t hdma_blocks_remaining_{};
    bool hdma_active_{};
    bool double_speed_{};
    bool speed_switch_requested_{};
    std::array<std::uint8_t, Joypad::sgb_packet_size * Joypad::sgb_max_packets>
        sgb_packet_{};
};

} // namespace gameboy
