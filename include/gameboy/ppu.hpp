#pragma once

#include "gameboy/dmg_palette.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace gameboy {

class SaveStateCodec;
enum class HardwareModel;

class Ppu {
public:
    static constexpr std::size_t screen_width = 160;
    static constexpr std::size_t screen_height = 144;
    using Framebuffer = std::array<std::uint32_t, screen_width * screen_height>;

    Ppu();

    void set_cgb_mode(bool enabled) noexcept;
    [[nodiscard]] bool cgb_mode() const noexcept;
    void set_dmg_palette(const DmgPalette& palette) noexcept;
    void initialize_post_boot_phase(HardwareModel model) noexcept;

    [[nodiscard]] std::uint8_t read_vram(std::uint16_t address) const noexcept;
    void write_vram(std::uint16_t address, std::uint8_t value) noexcept;
    void dma_write_vram(std::uint16_t address, std::uint8_t value) noexcept;
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
    friend class SaveStateCodec;

    [[nodiscard]] bool lcd_enabled() const noexcept;
    [[nodiscard]] bool stat_condition() const noexcept;
    [[nodiscard]] bool update_stat_line() noexcept;
    [[nodiscard]] unsigned mode3_duration() const noexcept;
    [[nodiscard]] bool window_active_on_line() const noexcept;
    void begin_visible_line() noexcept;
    void render_scanline() noexcept;
    [[nodiscard]] std::uint32_t palette_color(
        std::uint8_t palette, std::uint8_t color,
        const std::array<std::uint32_t, 4>& colors) const noexcept;
    [[nodiscard]] std::uint32_t cgb_palette_color(
        const std::array<std::uint8_t, 0x40>& palette, std::uint8_t number,
        std::uint8_t color) const noexcept;

    std::array<std::uint8_t, 0x2000> vram_{};
    std::unique_ptr<std::array<std::uint8_t, 0x2000>> cgb_vram_;
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
    DmgPalette dmg_palette_{grayscale_dmg_palette};
    std::uint8_t window_y_{};
    std::uint8_t window_x_{};
    std::array<std::uint8_t, 0x40> cgb_bg_palette_{};
    std::array<std::uint8_t, 0x40> cgb_object_palette_{};
    std::uint8_t vram_bank_{};
    std::uint8_t bg_palette_index_{};
    std::uint8_t object_palette_index_{};
    unsigned dot_{};
    unsigned mode3_end_dot_{252};
    // STAT interrupt sources switch one dot before CPU-visible mode bits.
    std::uint8_t mode_{};
    std::uint8_t stat_mode_{};
    std::uint8_t window_line_{};
    bool window_y_triggered_{};
    bool cgb_mode_{};
    bool coincidence_{};
    bool lcd_startup_{};
    bool stat_line_{};
    bool frame_ready_{};
};

} // namespace gameboy
