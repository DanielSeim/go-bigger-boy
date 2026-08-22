#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace gameboy {

class Ppu {
public:
    static constexpr std::size_t screen_width = 160;
    static constexpr std::size_t screen_height = 144;
    using Framebuffer = std::array<std::uint32_t, screen_width * screen_height>;

    Ppu() noexcept;

    [[nodiscard]] std::uint8_t read_vram(std::uint16_t address) const noexcept;
    void write_vram(std::uint16_t address, std::uint8_t value) noexcept;
    [[nodiscard]] std::uint8_t read_oam(std::uint16_t address) const noexcept;
    void write_oam(std::uint16_t address, std::uint8_t value) noexcept;
    void dma_write_oam(unsigned offset, std::uint8_t value) noexcept;

    [[nodiscard]] static bool handles_register(std::uint16_t address) noexcept;
    [[nodiscard]] std::uint8_t read_register(std::uint16_t address) const noexcept;
    // Returns true when the write raises the STAT interrupt line.
    [[nodiscard]] bool write_register(std::uint16_t address,
                                      std::uint8_t value) noexcept;

    // Returns IF-compatible request bits: bit 0 VBlank, bit 1 STAT.
    [[nodiscard]] std::uint8_t tick(unsigned cycles) noexcept;

    [[nodiscard]] const Framebuffer& framebuffer() const noexcept;
    [[nodiscard]] bool frame_ready() const noexcept;
    void consume_frame() noexcept;

private:
    [[nodiscard]] bool lcd_enabled() const noexcept;
    [[nodiscard]] bool stat_condition() const noexcept;
    [[nodiscard]] bool update_stat_line() noexcept;
    void render_scanline() noexcept;
    [[nodiscard]] std::uint32_t palette_color(std::uint8_t palette,
                                              std::uint8_t color) const noexcept;

    std::array<std::uint8_t, 0x2000> vram_{};
    std::array<std::uint8_t, 0xA0> oam_{};
    Framebuffer framebuffer_{};

    std::uint8_t lcdc_{};
    std::uint8_t stat_select_{};
    std::uint8_t scy_{};
    std::uint8_t scx_{};
    std::uint8_t ly_{};
    std::uint8_t lyc_{};
    std::uint8_t bg_palette_{};
    std::uint8_t object_palette_0_{};
    std::uint8_t object_palette_1_{};
    std::uint8_t window_y_{};
    std::uint8_t window_x_{};
    unsigned dot_{};
    std::uint8_t mode_{};
    bool stat_line_{};
    bool frame_ready_{};
};

} // namespace gameboy
