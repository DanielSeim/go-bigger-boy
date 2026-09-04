#include "gameboy/ppu.hpp"

#include "gbb/log.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <string_view>

namespace gameboy {
namespace {

constexpr std::array<std::uint16_t, 16> default_sgb_palettes{
    0x7FFF, 0x5294, 0x294A, 0x0000,
    0x7FFF, 0x5294, 0x294A, 0x0000,
    0x7FFF, 0x5294, 0x294A, 0x0000,
    0x7FFF, 0x5294, 0x294A, 0x0000,
};
constexpr unsigned object_cancellation_tail_dots = 10;

bool sgb_palette_is_default(
    const std::array<std::uint16_t, 16>& palettes) noexcept {
    return palettes == default_sgb_palettes;
}

} // namespace

std::uint8_t Ppu::tick(const unsigned cycles) noexcept {
    if (!lcd_enabled()) {
        return 0;
    }

    std::uint8_t requests = 0;
    for (unsigned cycle = 0; cycle < cycles; ++cycle) {
        ++dot_;
        if (ly_ < screen_height) {
            // LCD startup exposes its transitions immediately. On subsequent
            // lines, STAT sources and memory arbitration change internally
            // one dot before the mode bits visible to the CPU.
            if (lcd_startup_ && dot_ == 80) {
                stat_mode_ = 3;
                mode_ = 3;
                begin_mode3();
                if (update_stat_line()) requests |= 0x02;
            } else if (!lcd_startup_ && dot_ == 1) {
                mode_ = 2;
                coincidence_ = ly_ == lyc_;
                if (update_stat_line()) requests |= 0x02;
            } else if (!lcd_startup_ && dot_ == 80) {
                stat_mode_ = 3;
                begin_mode3();
                if (update_stat_line()) requests |= 0x02;
            } else if (!lcd_startup_ && dot_ == 81) {
                mode_ = 3;
            }

            if (stat_mode_ == 3 && dot_ < mode3_end_dot_) tick_mode3();

            if (stat_mode_ == 3) trace_window_state("dot");

            if (dot_ == mode3_end_dot_) {
                stat_mode_ = 0;
                window_line_ = static_cast<std::uint8_t>(
                    window_line_ + window_activation_count_);
                if (lcd_startup_) {
                    mode_ = 0;
                    requests |= 0x04;
                }
                if (update_stat_line()) requests |= 0x02;
            } else if (!lcd_startup_ && dot_ == mode3_end_dot_ + 1) {
                mode_ = 0;
                requests |= 0x04;
            }

            // On CGB hardware the mode-2 STAT source associated with line 144
            // rises one M-cycle before the VBlank request and visible mode 1.
            if (cgb_hardware_ && ly_ == screen_height - 1 && dot_ == 452 &&
                (stat_select_ & 0x20) != 0 && !stat_line_) {
                stat_line_ = true;
                requests |= 0x02;
            }
        }

        // The shortened first line is a DMG LCD-enable startup quirk.
        const auto line_length = lcd_startup_ ? 452U : 456U;
        if (dot_ == line_length) {
            dot_ = 0;
            ++ly_;
            lcd_startup_ = false;
            if (ly_ == screen_height) {
                coincidence_ = ly_ == lyc_;
                mode_ = 1;
                stat_mode_ = 1;
                frame_ready_ = true;
                requests |= 0x01;
            } else if (ly_ > 153) {
                ly_ = 0;
                coincidence_ = ly_ == lyc_;
                mode_ = 0;
                stat_mode_ = 2;
                window_line_ = 0;
                window_y_triggered_ = false;
                begin_visible_line();
            } else if (ly_ < screen_height) {
                coincidence_ = false;
                mode_ = 0;
                stat_mode_ = 2;
                begin_visible_line();
            }
            if (update_stat_line()) requests |= 0x02;
        }
    }
    return requests;
}

const Ppu::Framebuffer& Ppu::framebuffer() const noexcept {
    return *framebuffer_;
}

bool Ppu::frame_ready() const noexcept { return frame_ready_; }

void Ppu::consume_frame() noexcept { frame_ready_ = false; }

bool Ppu::lcd_enabled() const noexcept { return (lcdc_ & 0x80) != 0; }

bool Ppu::stat_condition() const noexcept {
    if (!lcd_enabled()) {
        return false;
    }
    return ((stat_select_ & 0x40) != 0 && coincidence_) ||
           ((stat_select_ & 0x20) != 0 &&
            (stat_mode_ == 2 ||
             (stat_mode_ == 1 && ly_ == screen_height))) ||
           ((stat_select_ & 0x10) != 0 && stat_mode_ == 1) ||
           ((stat_select_ & 0x08) != 0 && stat_mode_ == 0);
}

bool Ppu::update_stat_line() noexcept {
    const auto new_line = stat_condition();
    const auto rising_edge = new_line && !stat_line_;
    stat_line_ = new_line;
    return rising_edge;
}

bool Ppu::window_active_on_line() const noexcept {
    const auto background_allows_window = cgb_mode_ || (lcdc_ & 0x01) != 0;
    return background_allows_window && (lcdc_ & 0x20) != 0 &&
           window_y_triggered_ && window_x_ <= 166;
}

void Ppu::begin_visible_line() noexcept {
    window_rendered_this_line_ = false;
    if (ly_ == window_y_) {
        window_y_triggered_ = true;
    }
}

void Ppu::begin_mode3() noexcept {
    mode3_end_dot_ = 369;
    background_fifo_size_ = 0;
    fetcher_phase_ = 0;
    fetcher_phase_ticks_ = 2;
    fetcher_tile_index_ = 0;
    output_x_ = 0;
    startup_delay_ = 12;
    scroll_discard_ = static_cast<std::uint8_t>(scx_ & 7);
    window_delay_ = 0;
    sprite_delay_ = 0;
    pending_sprite_mask_ = 0;
    rendered_sprite_mask_ = 0;
    pending_sprite_deadlines_.fill(0);
    render_sprite_deadlines_.fill(0);
    emitted_background_.fill(BackgroundPixel{});
    fetched_window_ = false;
    discard_first_fetch_ = true;
    using_window_ = false;
    window_glitch_x_ = 0;
    window_glitch_applied_x_ = 0;
    window_glitch_pending_ = false;
    window_glitch_applied_ = false;
    window_glitch_restore_color_ = 0;
    window_source_x_ = 0;
    window_activation_count_ = 0;
    window_fetch_line_ = window_line_;
    window_fetch_start_x_ = 0;
    window_retrigger_armed_ = false;
    window_disable_pending_ = false;
    window_disable_source_x_ = 0;
    window_trigger_pending_ = false;
    window_trigger_x_ = 0;
    previous_sprite_tile_ = 0;
    previous_sprite_was_window_ = false;
    have_previous_sprite_tile_ = false;
    object_pixels_.fill(ObjectPixel{});
    object_pixel_deadlines_.fill(0);
    select_line_sprites();
    trace_window_state("mode3_start");
}

void Ppu::tick_mode3() noexcept {
    tick_background_fetcher();

    if (startup_delay_ != 0) {
        --startup_delay_;
        return;
    }
    if (window_delay_ != 0) {
        --window_delay_;
        return;
    }
    for (unsigned index = 0; index < line_sprite_count_; ++index) {
        const auto sprite = line_sprites_[index];
        if ((pending_sprite_mask_ & (std::uint64_t{1} << sprite)) == 0 ||
            pending_sprite_deadlines_[sprite] == 0) {
            continue;
        }
        auto& cancellation_deadline = pending_sprite_deadlines_[sprite];
        if (cancellation_deadline != 0) {
            --cancellation_deadline;
            if (cancellation_deadline == 0) {
                pending_sprite_mask_ &= ~(std::uint64_t{1} << sprite);
            }
        }
        auto& render_deadline = render_sprite_deadlines_[sprite];
        if (render_deadline != 0) {
            --render_deadline;
            if (render_deadline == 0 &&
                (rendered_sprite_mask_ & (std::uint64_t{1} << sprite)) == 0) {
                if ((lcdc_ & 0x02) != 0) fetch_object(sprite);
                rendered_sprite_mask_ |= std::uint64_t{1} << sprite;
            }
        }
    }
    for (auto& deadline : object_pixel_deadlines_) {
        if (deadline != 0) --deadline;
    }
    if (sprite_delay_ != 0) {
        --sprite_delay_;
        return;
    }

    if (scroll_discard_ != 0) {
        if (background_fifo_size_ == 0) return;
        for (unsigned index = 1; index < background_fifo_size_; ++index) {
            background_fifo_[index - 1] = background_fifo_[index];
        }
        --background_fifo_size_;
        --scroll_discard_;
        return;
    }

    if (using_window_ && window_disable_pending_ &&
        window_source_x_ >= window_disable_source_x_) {
        resume_background_fetch();
        return;
    }

    const auto window_start = static_cast<int>(window_x_) - 7;
    const auto window_triggered = window_active_on_line() ||
                                  window_trigger_pending_;
    if (!using_window_ && window_triggered &&
        (window_activation_count_ == 0 || window_retrigger_armed_) &&
        static_cast<int>(output_x_) >=
            (window_trigger_pending_ ? window_trigger_x_
                                     : std::max(0, window_start))) {
        begin_window_fetch();
        return;
    }

    const auto sprite_penalty = trigger_sprites(output_x_);
    if (sprite_penalty != 0) {
        sprite_delay_ = static_cast<std::uint8_t>(sprite_penalty - 1);
        return;
    }
    if (background_fifo_size_ == 0) return;

    emit_pixel();
    if (output_x_ == screen_width) {
        mode3_end_dot_ = dot_ + 1;
    }
}

void Ppu::tick_background_fetcher() noexcept {
    if (fetcher_phase_ == 4 && background_fifo_size_ > 8) return;
    if (fetcher_phase_ticks_ != 0 && --fetcher_phase_ticks_ != 0) return;

    const auto push_pixels = [this]() {
        if (discard_first_fetch_) {
            discard_first_fetch_ = false;
            fetcher_phase_ = 0;
            fetcher_phase_ticks_ = 2;
            return;
        }
        for (unsigned column = 0; column < 8; ++column) {
            const auto source_x =
                static_cast<unsigned>(fetched_source_x_) + column;
            auto attributes = fetched_attributes_;
            auto low = fetched_low_;
            auto high = fetched_high_;
            if ((source_x / 8) !=
                (static_cast<unsigned>(fetched_source_x_) / 8)) {
                const auto source_y = fetched_window_
                                          ? static_cast<unsigned>(window_fetch_line_)
                                          : static_cast<unsigned>(
                                                static_cast<std::uint8_t>(ly_ + scy_));
                const auto map_base = fetched_window_
                                          ? ((lcdc_ & 0x40) != 0 ? 0x1C00U
                                                                  : 0x1800U)
                                          : ((lcdc_ & 0x08) != 0 ? 0x1C00U
                                                                  : 0x1800U);
                const auto map_offset = map_base + (source_y / 8) * 32 +
                                        ((source_x / 8) & 31U);
                const auto tile_number = vram_[map_offset & 0x1FFF];
                attributes = cgb_mode_
                                 ? (*cgb_vram_)[map_offset & 0x1FFF]
                                 : std::uint8_t{0};
                unsigned tile_offset = 0;
                if ((lcdc_ & 0x10) != 0) {
                    tile_offset = static_cast<unsigned>(tile_number) * 16;
                } else {
                    const auto tile = static_cast<std::int8_t>(tile_number);
                    tile_offset = static_cast<unsigned>(0x1000 + tile * 16);
                }
                auto row = static_cast<unsigned>(source_y & 7U);
                if ((attributes & 0x40) != 0) row = 7 - row;
                const auto& bank = (attributes & 0x08) != 0
                                       ? *cgb_vram_
                                       : vram_;
                low = bank[tile_offset + row * 2];
                high = bank[tile_offset + row * 2 + 1];
            }
            const auto source_column = source_x & 7U;
            const auto bit = (attributes & 0x20) != 0
                                 ? source_column
                                 : 7U - source_column;
            background_fifo_[background_fifo_size_++] = BackgroundPixel{
                static_cast<std::uint8_t>(
                    ((low >> bit) & 1U) | (((high >> bit) & 1U) << 1)),
                static_cast<std::uint8_t>(attributes & 0x07),
                (attributes & 0x80) != 0,
            };
        }
        ++fetcher_tile_index_;
        fetcher_phase_ = 0;
        fetcher_phase_ticks_ = 2;
    };

    const auto read_bitplane = [this](const unsigned byte) {
        auto row = fetched_row_;
        unsigned tile_offset = 0;
        if ((lcdc_ & 0x10) != 0) {
            tile_offset = static_cast<unsigned>(fetched_tile_) * 16;
        } else {
            const auto tile = static_cast<std::int8_t>(fetched_tile_);
            tile_offset = static_cast<unsigned>(0x1000 + tile * 16);
        }
        const auto& bank = (fetched_attributes_ & 0x08) != 0
                               ? *cgb_vram_
                               : vram_;
        return bank[tile_offset + row * 2 + byte];
    };

    switch (fetcher_phase_) {
    case 0: {
        fetched_window_ = using_window_;
        if (fetched_window_) {
            fetched_source_x_ = static_cast<std::int16_t>(
                window_fetch_start_x_ +
                static_cast<int>(fetcher_tile_index_) * 8);
        } else {
            fetched_source_x_ = static_cast<std::int16_t>(
                static_cast<std::uint8_t>((scx_ & 0xF8) +
                                          fetcher_tile_index_ * 8));
        }
        fetched_source_y_ = fetched_window_
                                ? window_fetch_line_
                                : static_cast<std::uint8_t>(ly_ + scy_);
        fetched_row_ = static_cast<std::uint8_t>(fetched_source_y_ & 7U);
        fetcher_phase_ = 1;
        fetcher_phase_ticks_ = 2;
        return;
    }
    case 1: {
        const auto source_y = static_cast<unsigned>(fetched_source_y_);
        const auto source_x = static_cast<unsigned>(fetched_source_x_) & 0xFFU;
        const auto map_base = fetched_window_
                                  ? ((lcdc_ & 0x40) != 0 ? 0x1C00U : 0x1800U)
                                  : ((lcdc_ & 0x08) != 0 ? 0x1C00U : 0x1800U);
        const auto map_offset = map_base + (source_y / 8) * 32 + source_x / 8;
        fetched_tile_ = vram_[map_offset & 0x1FFF];
        fetched_attributes_ = cgb_mode_
                                  ? (*cgb_vram_)[map_offset & 0x1FFF]
                                  : std::uint8_t{0};
        auto row = static_cast<std::uint8_t>(source_y & 7U);
        if ((fetched_attributes_ & 0x40) != 0) row = 7 - row;
        fetched_row_ = row;
        if (!cgb_hardware_) {
            const auto live_y = fetched_window_
                                    ? window_fetch_line_
                                    : static_cast<std::uint8_t>(ly_ + scy_);
            fetched_row_ = static_cast<std::uint8_t>(live_y & 7U);
        }
        fetcher_phase_ = 2;
        fetcher_phase_ticks_ = 2;
        return;
    }
    case 2: {
        fetched_low_ = read_bitplane(0);
        if (!cgb_hardware_) {
            const auto live_y = fetched_window_
                                    ? window_fetch_line_
                                    : static_cast<std::uint8_t>(ly_ + scy_);
            fetched_row_ = static_cast<std::uint8_t>(live_y & 7U);
        }
        if (background_fifo_size_ == 0) {
            fetched_high_ = read_bitplane(1);
            push_pixels();
            return;
        }
        fetcher_phase_ = 3;
        fetcher_phase_ticks_ = 2;
        return;
    }
    case 3:
        fetched_high_ = read_bitplane(1);
        fetcher_phase_ = 4;
        [[fallthrough]];
    case 4:
        if (background_fifo_size_ > 8) {
            fetcher_phase_ticks_ = 1;
            return;
        }
        push_pixels();
        return;
    default: return;
    }
}

void Ppu::begin_window_fetch() noexcept {
    window_trigger_pending_ = false;
    using_window_ = true;
    // A cancelled handoff has already consumed the first window tile. Keep a
    // one-shot marker in the otherwise-idle disable-source register so the
    // reactivation starts at that tile boundary instead of restarting at the
    // comparator-derived source offset.
    const auto resumed_window_handoff = window_disable_source_x_ == 0xFFFF &&
                                        output_x_ < 8;
    if (resumed_window_handoff) window_disable_source_x_ = 0;
    window_fetch_line_ = static_cast<std::uint8_t>(
        window_line_ + window_activation_count_);
    ++window_activation_count_;
    window_retrigger_armed_ = false;
    background_fifo_size_ = 0;
    fetcher_phase_ = 0;
    fetcher_phase_ticks_ = 2;
    fetcher_tile_index_ = 0;
    window_delay_ = static_cast<std::uint8_t>(
        5 + (!cgb_hardware_ && window_x_ == 0 && (scx_ & 7U) != 0));
    window_source_x_ = resumed_window_handoff
                           ? std::uint16_t{7}
                           : static_cast<std::uint16_t>(
                                 std::max(0, 7 - static_cast<int>(window_x_)));
    window_fetch_start_x_ = static_cast<std::uint8_t>(window_source_x_);
    trace_window_state("window_fetch_start");
}

void Ppu::resume_background_fetch() noexcept {
    // Preserve the special early-rewrite handoff across the background-fetch
    // reset. The marker is consumed by the next window reactivation.
    const auto resumed_window_handoff = window_disable_pending_ &&
                                        window_disable_source_x_ == 8 &&
                                        output_x_ < 8;
    using_window_ = false;
    window_disable_pending_ = false;
    if (resumed_window_handoff) window_disable_source_x_ = 0xFFFF;
    window_glitch_pending_ = false;
    window_glitch_applied_ = false;
    background_fifo_size_ = 0;
    fetcher_phase_ = 0;
    fetcher_phase_ticks_ = 2;
    fetcher_tile_index_ = static_cast<std::uint8_t>(output_x_ / 8);
    trace_window_state("window_fetch_resume");
}

void Ppu::trace_window_state(const char* event) const noexcept {
    // The logger level is intentionally consulted for every event. This keeps
    // tracing reconfigurable at runtime for debuggers and tests; GBB_TRACE_WX
    // still bootstraps the logger to trace level and optionally selects a file.
    if (!gbb::Logger::instance().enabled(gbb::LogLevel::trace)) {
        return;
    }
    char message[2048]{};
    const auto written = std::snprintf(
        message, sizeof(message),
        "window event=%s ly=%u dot=%u mode=%u stat=%u wx=%u wy=%u "
        "scx=%u out=%u using=%u triggered=%u pending=%u retrigger=%u "
        "activation=%u source=%u fetch_line=%u fetch_start=%u "
        "fetched_window=%u fetched_x=%d fetched_y=%u fetched_row=%u "
        "tile=%u low=%u high=%u fifo=%u phase=%u phase_ticks=%u delay=%u "
        "disable=%u disable_source=%u glitch=%u glitch_x=%u",
        event == nullptr ? "unknown" : event, static_cast<unsigned>(ly_), dot_,
        static_cast<unsigned>(mode_), static_cast<unsigned>(stat_mode_),
        static_cast<unsigned>(window_x_), static_cast<unsigned>(window_y_),
        static_cast<unsigned>(scx_), static_cast<unsigned>(output_x_),
        using_window_ ? 1U : 0U, window_active_on_line() ? 1U : 0U,
        window_trigger_pending_ ? 1U : 0U,
        window_retrigger_armed_ ? 1U : 0U,
        static_cast<unsigned>(window_activation_count_),
        static_cast<unsigned>(window_source_x_),
        static_cast<unsigned>(window_fetch_line_),
        static_cast<unsigned>(window_fetch_start_x_), fetched_window_ ? 1U : 0U,
        static_cast<int>(fetched_source_x_), static_cast<unsigned>(fetched_source_y_),
        static_cast<unsigned>(fetched_row_), static_cast<unsigned>(fetched_tile_),
        static_cast<unsigned>(fetched_low_), static_cast<unsigned>(fetched_high_),
        static_cast<unsigned>(background_fifo_size_),
        static_cast<unsigned>(fetcher_phase_),
        static_cast<unsigned>(fetcher_phase_ticks_),
        static_cast<unsigned>(window_delay_), window_disable_pending_ ? 1U : 0U,
        static_cast<unsigned>(window_disable_source_x_),
        window_glitch_pending_ || window_glitch_applied_ ? 1U : 0U,
        static_cast<unsigned>(window_glitch_x_));
    if (written <= 0) return;
    gbb::Logger::instance().write(gbb::LogLevel::trace, gbb::LogCategory::ppu,
                                   std::string_view{message,
                                                    static_cast<std::size_t>(
                                                        std::min<int>(written,
                                                                      sizeof(message) - 1))});
}

void Ppu::select_line_sprites() noexcept {
    line_sprite_height_ = (lcdc_ & 0x04) != 0 ? 16 : 8;
    line_sprite_count_ = 0;
    next_line_sprite_ = 0;
    for (unsigned index = 0; index < 40 && line_sprite_count_ < 10; ++index) {
        const auto sprite_y = static_cast<int>(oam_[index * 4]) - 16;
        if (static_cast<int>(ly_) >= sprite_y &&
            static_cast<int>(ly_) < sprite_y + line_sprite_height_) {
            line_sprites_[line_sprite_count_++] = static_cast<std::uint8_t>(index);
        }
    }
    std::sort(line_sprites_.begin(),
              line_sprites_.begin() + line_sprite_count_,
              [this](const std::uint8_t left, const std::uint8_t right) {
                  const auto left_x = oam_[left * 4 + 1];
                  const auto right_x = oam_[right * 4 + 1];
                  return left_x != right_x ? left_x < right_x : left < right;
              });
}

unsigned Ppu::trigger_sprites(const unsigned x) noexcept {
    unsigned penalty = 0;
    while (next_line_sprite_ < line_sprite_count_) {
        const auto index = line_sprites_[next_line_sprite_];
        const auto raw_x = static_cast<unsigned>(oam_[index * 4 + 1]);
        if (raw_x >= 168) {
            ++next_line_sprite_;
            continue;
        }
        const auto trigger_x = raw_x <= 8 ? 0U : raw_x - 8;
        if (trigger_x > x) break;
        ++next_line_sprite_;
        // OBJ fetches are only scheduled while OBJ rendering is enabled. A
        // sprite that reaches its trigger point during an OBJ-off interval is
        // skipped; if OBJ is enabled again, later sprites can still trigger.
        if ((lcdc_ & 0x02) == 0) continue;
        pending_sprite_mask_ |= std::uint64_t{1} << index;
        const auto window_start = static_cast<int>(window_x_) - 7;
        const auto screen_x = static_cast<int>(raw_x) - 8;
        const auto in_window = using_window_ && screen_x >= window_start;
        const auto fetch_x = in_window
                                 ? screen_x - window_start
                                 : screen_x + static_cast<int>(scx_);
        const auto tile = fetch_x >= 0 ? fetch_x / 8 : (fetch_x - 7) / 8;
        const auto same_tile = have_previous_sprite_tile_ &&
                               tile == previous_sprite_tile_ &&
                               in_window == previous_sprite_was_window_;
        if (!same_tile) {
            const auto phase = raw_x == 0
                                   ? 0U
                                   : static_cast<unsigned>((fetch_x % 8 + 8) % 8);
            if (phase < 5) penalty += 5 - phase;
        }
        penalty += 6;
        // Pixel data becomes available at the normal fetch completion, but
        // the DMG keeps the object bus transaction cancellable for a short
        // tail. This later boundary is observable when OBJ is toggled.
        // For the two partially off-screen X positions (3 and 4), the DMG
        // hands the object bus back four dots earlier than the general tail.
        pending_sprite_deadlines_[index] =
            static_cast<std::uint8_t>(
                static_cast<int>(penalty) +
                static_cast<int>(object_cancellation_tail_dots) +
                ((raw_x == 3U || raw_x == 4U) ? -4 : 0));
        render_sprite_deadlines_[index] =
            static_cast<std::uint8_t>(penalty - 1);
        previous_sprite_tile_ = static_cast<std::int16_t>(tile);
        previous_sprite_was_window_ = in_window;
        have_previous_sprite_tile_ = true;
    }
    return penalty;
}

void Ppu::fetch_object(const unsigned index) noexcept {
    const auto sprite_height = line_sprite_height_;
    const auto sprite_y = static_cast<int>(oam_[index * 4]) - 16;
    const auto sprite_x = static_cast<int>(oam_[index * 4 + 1]) - 8;
    auto tile = oam_[index * 4 + 2];
    const auto attributes = oam_[index * 4 + 3];
    auto row = static_cast<unsigned>(static_cast<int>(ly_) - sprite_y);
    if ((attributes & 0x40) != 0) row = sprite_height - 1U - row;
    if (sprite_height == 16) {
        tile &= 0xFE;
        if (row >= 8) {
            ++tile;
            row -= 8;
        }
    }
    const auto tile_offset = static_cast<unsigned>(tile) * 16 + row * 2;
    const auto& bank = cgb_mode_ && (attributes & 0x08) != 0
                           ? *cgb_vram_
                           : vram_;
    const auto low = bank[tile_offset];
    const auto high = bank[tile_offset + 1];
    for (unsigned pixel = 0; pixel < 8; ++pixel) {
        const auto screen_x = sprite_x + static_cast<int>(pixel);
        if (screen_x < 0 || screen_x >= static_cast<int>(screen_width)) continue;
        const auto bit = (attributes & 0x20) != 0 ? pixel : 7U - pixel;
        const auto color = static_cast<std::uint8_t>(
            ((low >> bit) & 1U) | (((high >> bit) & 1U) << 1));
        if (color == 0) continue;
        auto& destination = object_pixels_[static_cast<unsigned>(screen_x)];
        const auto wins_priority = !destination.valid ||
            (cgb_mode_ && index < destination.oam_index);
        if (wins_priority) {
            destination = ObjectPixel{color, attributes,
                                      static_cast<std::uint8_t>(index), true};
            const auto deadline =
                static_cast<unsigned>(pending_sprite_deadlines_[index]) + pixel;
            object_pixel_deadlines_[static_cast<unsigned>(screen_x)] =
                static_cast<std::uint8_t>(std::min(deadline, 0xFFU));
        }
    }
}

void Ppu::emit_pixel() noexcept {
    const auto insert_window_glitch =
        window_glitch_pending_ && output_x_ == window_glitch_x_;
    auto background = insert_window_glitch
                          ? BackgroundPixel{}
                          : pop_background_pixel();
    if (!cgb_mode_ && (lcdc_ & 0x01) == 0) background = BackgroundPixel{};
    if (using_window_ && !insert_window_glitch) {
        window_rendered_this_line_ = true;
        ++window_source_x_;
    }

    auto result = compose_pixel(output_x_, background);
    if (sgb_mode_) {
        if (sgb_mask_mode_ == 1) {
            // Freeze leaves the existing SNES framebuffer untouched while
            // the Game Boy continues to advance its link/PPU state.
            ++output_x_;
            return;
        }
        if (sgb_mask_mode_ == 2) result = UINT32_C(0xFF000000);
        if (sgb_mask_mode_ == 3) result = sgb_palette_color(0, 0);
    }
    emitted_background_[output_x_] = background;
    if (insert_window_glitch) {
        window_glitch_restore_color_ =
            compose_pixel(output_x_, background_fifo_[0]);
        window_glitch_applied_x_ = output_x_;
        window_glitch_applied_ = true;
        window_glitch_pending_ = false;
    }
    const auto framebuffer_offset =
        static_cast<std::size_t>(ly_) * screen_width + output_x_;
    (*framebuffer_)[framebuffer_offset] = result;
    ++output_x_;
}

Ppu::BackgroundPixel Ppu::pop_background_pixel() noexcept {
    const auto pixel = background_fifo_[0];
    for (unsigned index = 1; index < background_fifo_size_; ++index) {
        background_fifo_[index - 1] = background_fifo_[index];
    }
    --background_fifo_size_;
    return pixel;
}

Ppu::BackgroundPixel Ppu::background_pixel_at_screen(
    const unsigned x) const noexcept {
    const auto source_x = static_cast<unsigned>(
        static_cast<std::uint8_t>(scx_ + x));
    const auto source_y = static_cast<unsigned>(
        static_cast<std::uint8_t>(scy_ + ly_));
    const auto map_base = (lcdc_ & 0x08) != 0 ? 0x1C00U : 0x1800U;
    const auto map_offset = map_base + (source_y / 8U) * 32U +
                            source_x / 8U;
    const auto tile_number = vram_[map_offset & 0x1FFFU];
    const auto attributes = cgb_mode_
                                ? (*cgb_vram_)[map_offset & 0x1FFFU]
                                : std::uint8_t{0};
    unsigned tile_offset = 0;
    if ((lcdc_ & 0x10) != 0) {
        tile_offset = static_cast<unsigned>(tile_number) * 16U;
    } else {
        tile_offset = static_cast<unsigned>(
            0x1000 + static_cast<std::int8_t>(tile_number) * 16);
    }
    auto row = source_y & 7U;
    if ((attributes & 0x40) != 0) row = 7U - row;
    const auto& bank = (attributes & 0x08) != 0 ? *cgb_vram_ : vram_;
    const auto low = bank[tile_offset + row * 2U];
    const auto high = bank[tile_offset + row * 2U + 1U];
    const auto column = source_x & 7U;
    const auto bit = (attributes & 0x20) != 0 ? column : 7U - column;
    return BackgroundPixel{
        static_cast<std::uint8_t>(
            ((low >> bit) & 1U) | (((high >> bit) & 1U) << 1U)),
        static_cast<std::uint8_t>(attributes & 0x07U),
        (attributes & 0x80) != 0,
    };
}

std::uint32_t Ppu::compose_pixel(
    const unsigned x, const BackgroundPixel background) const noexcept {
    // SGB-capable DMG cartridges start with a neutral SGB palette and may
    // never issue a PAL command (for example while a state is captured in a
    // transition). In that case use the configured DMG palette so the
    // automatic cartridge compatibility colors still work. A real SGB PAL
    // command changes the latched values and immediately restores native SGB
    // rendering.
    const auto use_sgb_palette = sgb_mode_ && !sgb_palette_is_default(sgb_palettes_);
    std::uint32_t result{};
    if (cgb_mode_) {
        result = cgb_palette_color(cgb_bg_palette_, background.palette,
                                   background.color);
    } else if (use_sgb_palette) {
        result = sgb_palette_color(sgb_attribute_for_pixel(x),
                                   background.color);
    } else {
        result = palette_color(bg_palette_, background.color,
                               dmg_palette_.background);
    }
    const auto& object = object_pixels_[x];
    if ((lcdc_ & 0x02) == 0 || !object.valid) return result;
    const auto background_blocks_object =
        background.color != 0 &&
        (cgb_mode_
             ? (lcdc_ & 0x01) != 0 &&
                   (background.priority || (object.attributes & 0x80) != 0)
             : (object.attributes & 0x80) != 0);
    if (background_blocks_object) return result;
    if (cgb_mode_) {
        return cgb_palette_color(
            cgb_object_palette_,
            static_cast<std::uint8_t>(object.attributes & 0x07),
            object.color);
    }
    if (use_sgb_palette) {
        return sgb_palette_color(sgb_attribute_for_pixel(x), object.color);
    }
    return palette_color(
        (object.attributes & 0x10) != 0 ? object_palette_1_ : object_palette_0_,
        object.color,
        (object.attributes & 0x10) != 0 ? dmg_palette_.object_1
                                        : dmg_palette_.object_0);
}

std::uint32_t Ppu::cgb_palette_color(
    const std::array<std::uint8_t, 0x40>& palette, const std::uint8_t number,
    const std::uint8_t color) const noexcept {
    const auto offset = static_cast<unsigned>(number) * 8 + color * 2;
    const auto rgb555 = static_cast<std::uint16_t>(
        palette[offset] | (static_cast<std::uint16_t>(palette[offset + 1]) << 8));
    const auto expand = [](const unsigned component) {
        return (component << 3) | (component >> 2);
    };
    const auto red = expand(rgb555 & 0x1F);
    const auto green = expand((rgb555 >> 5) & 0x1F);
    const auto blue = expand((rgb555 >> 10) & 0x1F);
    return UINT32_C(0xFF000000) | (red << 16) | (green << 8) | blue;
}

std::uint32_t Ppu::palette_color(
    const std::uint8_t palette, const std::uint8_t color,
    const std::array<std::uint32_t, 4>& colors) const noexcept {
    return colors[(palette >> (color * 2)) & 0x03];
}

std::uint8_t Ppu::sgb_attribute_for_pixel(const unsigned x) const noexcept {
    if (!sgb_mode_) return 0;
    const auto tile_x = std::min<unsigned>(x / 8, 19);
    const auto tile_y = std::min<unsigned>(ly_ / 8, 17);
    return sgb_attributes_[tile_x + tile_y * 20] & 3U;
}

std::uint32_t Ppu::sgb_palette_color(const std::uint8_t palette,
                                     const std::uint8_t color) const noexcept {
    const auto rgb555 = sgb_palettes_[static_cast<std::size_t>(palette & 3U) * 4 +
                                      (color & 3U)];
    const auto expand = [](const unsigned component) {
        return (component << 3) | (component >> 2);
    };
    return UINT32_C(0xFF000000) | (expand(rgb555 & 0x1F) << 16) |
           (expand((rgb555 >> 5) & 0x1F) << 8) | expand((rgb555 >> 10) & 0x1F);
}


} // namespace gameboy

