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
    void set_cgb_hardware(bool enabled) noexcept;
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

    struct BackgroundPixel {
        std::uint8_t color{};
        std::uint8_t palette{};
        bool priority{};
    };

    struct ObjectPixel {
        std::uint8_t color{};
        std::uint8_t attributes{};
        std::uint8_t oam_index{0xFF};
        bool valid{};
    };

    [[nodiscard]] bool lcd_enabled() const noexcept;
    [[nodiscard]] bool stat_condition() const noexcept;
    [[nodiscard]] bool update_stat_line() noexcept;
    [[nodiscard]] bool window_active_on_line() const noexcept;
    void begin_visible_line() noexcept;
    void begin_mode3() noexcept;
    void tick_mode3() noexcept;
    void tick_background_fetcher() noexcept;
    void begin_window_fetch() noexcept;
    void resume_background_fetch() noexcept;
    void select_line_sprites() noexcept;
    [[nodiscard]] unsigned trigger_sprites(unsigned x) noexcept;
    void fetch_object(unsigned index) noexcept;
    void emit_pixel() noexcept;
    [[nodiscard]] BackgroundPixel pop_background_pixel() noexcept;
    [[nodiscard]] BackgroundPixel background_pixel_at_screen(
        unsigned x) const noexcept;
    [[nodiscard]] std::uint32_t compose_pixel(
        unsigned x, BackgroundPixel background) const noexcept;
    [[nodiscard]] std::uint32_t palette_color(
        std::uint8_t palette, std::uint8_t color,
        const std::array<std::uint32_t, 4>& colors) const noexcept;
    [[nodiscard]] std::uint32_t cgb_palette_color(
        const std::array<std::uint8_t, 0x40>& palette, std::uint8_t number,
        std::uint8_t color) const noexcept;

    std::array<std::uint8_t, 0x2000> vram_{};
    std::unique_ptr<std::array<std::uint8_t, 0x2000>> cgb_vram_;
    std::array<std::uint8_t, 0xA0> oam_{};
    std::unique_ptr<Framebuffer> framebuffer_;

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
    bool window_rendered_this_line_{};
    bool cgb_mode_{};
    bool cgb_hardware_{};
    bool coincidence_{};
    bool lcd_startup_{};
    bool stat_line_{};
    bool frame_ready_{};

    std::array<BackgroundPixel, 16> background_fifo_{};
    std::array<ObjectPixel, screen_width> object_pixels_{};
    std::array<std::uint8_t, 10> line_sprites_{};
    std::uint8_t background_fifo_size_{};
    std::uint8_t fetcher_phase_{};
    std::uint8_t fetcher_phase_ticks_{};
    std::uint8_t fetcher_tile_index_{};
    std::uint8_t fetched_tile_{};
    std::uint8_t fetched_attributes_{};
    std::uint8_t fetched_low_{};
    std::uint8_t fetched_high_{};
    std::uint8_t fetched_row_{};
    std::uint8_t fetched_source_y_{};
    std::uint8_t line_sprite_count_{};
    std::uint8_t next_line_sprite_{};
    std::uint8_t output_x_{};
    std::uint8_t startup_delay_{};
    std::uint8_t scroll_discard_{};
    std::uint8_t window_delay_{};
    std::uint8_t sprite_delay_{};
    std::uint8_t window_glitch_x_{};
    std::uint8_t window_glitch_applied_x_{};
    std::uint8_t window_activation_count_{};
    std::uint8_t window_fetch_line_{};
    std::uint8_t window_fetch_start_x_{};
    std::uint8_t window_trigger_x_{};
    std::int16_t fetched_source_x_{};
    std::int16_t previous_sprite_tile_{};
    std::uint16_t window_source_x_{};
    std::uint16_t window_disable_source_x_{};
    std::uint32_t window_glitch_restore_color_{};
    bool fetched_window_{};
    bool discard_first_fetch_{};
    bool using_window_{};
    bool previous_sprite_was_window_{};
    bool have_previous_sprite_tile_{};
    bool window_glitch_pending_{};
    bool window_glitch_applied_{};
    bool window_retrigger_armed_{};
    bool window_disable_pending_{};
    bool window_trigger_pending_{};
};

} // namespace gameboy
