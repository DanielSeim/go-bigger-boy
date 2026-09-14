#pragma once

#ifndef __ANDROID__

#include "gameboy/display_palette.hpp"
#include "gameboy/emulator.hpp"
#include "desktop_viewport.hpp"
#include "tool_window_support.hpp"
#include "window_event.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>

namespace gbb::sdl {

class VideoViewer {
public:
    VideoViewer() = default;
    ~VideoViewer() { close(); }
    VideoViewer(const VideoViewer&) = delete;
    VideoViewer& operator=(const VideoViewer&) = delete;

    [[nodiscard]] bool visible() const noexcept { return window_ != nullptr; }

    void open(SDL_Window* parent) {
        if (visible()) {
            SDL_RaiseWindow(window_);
            return;
        }
        window_ = SDL_CreateWindow("Go Bigger Boy - Video Viewers", 1100, 920,
                                   SDL_WINDOW_RESIZABLE);
        if (window_ == nullptr) throw_sdl_error("Could not create video viewer window");
        static_cast<void>(SDL_SetWindowMinimumSize(window_, 1000, 820));
        renderer_ = SDL_CreateRenderer(window_, nullptr);
        if (renderer_ == nullptr) {
            close();
            throw_sdl_error("Could not create video viewer renderer");
        }
        texture_ = SDL_CreateTexture(
            renderer_, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
            static_cast<int>(DesktopBackgroundMap::width),
            static_cast<int>(DesktopBackgroundMap::height));
        if (texture_ == nullptr) {
            close();
            throw_sdl_error("Could not create video viewer texture");
        }
        static_cast<void>(SDL_SetTextureScaleMode(texture_, SDL_SCALEMODE_NEAREST));
        if (parent != nullptr) {
            int x = 0;
            int y = 0;
            static_cast<void>(SDL_GetWindowPosition(parent, &x, &y));
            static_cast<void>(SDL_SetWindowPosition(window_, x + 96, y + 96));
        }
    }

    void close() noexcept {
        clear_tool_text_cache(renderer_);
        if (texture_ != nullptr) SDL_DestroyTexture(texture_);
        if (renderer_ != nullptr) SDL_DestroyRenderer(renderer_);
        if (window_ != nullptr) SDL_DestroyWindow(window_);
        texture_ = nullptr;
        renderer_ = nullptr;
        window_ = nullptr;
    }

    bool handle_event(const SDL_Event& event,
                      const gameboy::Emulator* emulator) {
        if (!visible()) return false;
        if (gbb::sdl::event_window_id(event) != SDL_GetWindowID(window_)) {
            return false;
        }
        if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
            close();
            return true;
        }
        if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat) {
            switch (event.key.key) {
            case SDLK_ESCAPE:
            case SDLK_F12:
                close();
                return true;
            case SDLK_1: mode_ = Mode::background; return true;
            case SDLK_2: mode_ = Mode::oam; return true;
            case SDLK_3: mode_ = Mode::tiles; return true;
            case SDLK_B:
                if (emulator != nullptr && emulator->bus().cgb_mode()) {
                    bank_ ^= 1U;
                }
                return true;
            case SDLK_LEFT: move_selection(-1, 0); return true;
            case SDLK_RIGHT: move_selection(1, 0); return true;
            case SDLK_UP: move_selection(0, -1); return true;
            case SDLK_DOWN: move_selection(0, 1); return true;
            default: break;
            }
        }
        if (event.type == SDL_EVENT_MOUSE_BUTTON_UP &&
            event.button.button == SDL_BUTTON_LEFT) {
            int width = 0;
            int height = 0;
            static_cast<void>(SDL_GetWindowSize(window_, &width, &height));
            if (tool_close_button_hit(width, height, event.button.x,
                                      event.button.y)) {
                close();
                return true;
            }
            if (event.button.y >= 48.0F && event.button.y <= 84.0F) {
                if (event.button.x >= 24.0F && event.button.x < 190.0F) {
                    mode_ = Mode::background;
                    return true;
                }
                if (event.button.x >= 198.0F && event.button.x < 330.0F) {
                    mode_ = Mode::oam;
                    return true;
                }
                if (event.button.x >= 338.0F && event.button.x < 470.0F) {
                    mode_ = Mode::tiles;
                    return true;
                }
            }
            if (emulator != nullptr) {
                select_at(event.button.x, event.button.y, *emulator);
            }
            return true;
        }
        return true;
    }

    void present(const gameboy::Emulator* emulator,
                 const gameboy::DisplayPalette& palette) {
        if (!visible() || emulator == nullptr) return;
        int width = 1100;
        int height = 920;
        static_cast<void>(SDL_GetWindowSize(window_, &width, &height));
        static_cast<void>(SDL_SetRenderDrawColor(renderer_, 8, 12, 20, 255));
        static_cast<void>(SDL_RenderClear(renderer_));
        static_cast<void>(SDL_SetRenderDrawColor(renderer_, 69, 207, 238, 255));
        render_tool_text(renderer_, 24, 18, "VIDEO DEBUG VIEWERS");
        draw_tool_close_button(renderer_, window_, width);
        draw_mode_button({24, 48, 166, 36}, "1  BG VIEWER", mode_ == Mode::background);
        draw_mode_button({198, 48, 132, 36}, "2  OAM VIEWER", mode_ == Mode::oam);
        draw_mode_button({338, 48, 132, 36}, "3  TILES VIEWER", mode_ == Mode::tiles);
        static_cast<void>(SDL_SetRenderDrawColor(renderer_, 177, 192, 208, 255));
        render_tool_text(renderer_, 490, 60,
                         "CLICK TO INSPECT  /  ARROWS MOVE  /  B CHANGES CGB BANK");

        switch (mode_) {
        case Mode::background: present_background(*emulator, palette); break;
        case Mode::oam: present_oam(*emulator, palette); break;
        case Mode::tiles: present_tiles(*emulator, palette); break;
        }
        static_cast<void>(SDL_RenderPresent(renderer_));
    }

private:
    enum class Mode { background, oam, tiles };

    [[noreturn]] static void throw_sdl_error(const char* action) {
        throw std::runtime_error(std::string(action) + ": " + SDL_GetError());
    }

    static void set_color(SDL_Renderer* renderer, const std::uint32_t color) {
        static_cast<void>(SDL_SetRenderDrawColor(
            renderer, static_cast<std::uint8_t>(color >> 16U),
            static_cast<std::uint8_t>(color >> 8U),
            static_cast<std::uint8_t>(color), 255));
    }

    static std::uint32_t rgb555_color(const std::uint8_t low,
                                      const std::uint8_t high) noexcept {
        const auto value = static_cast<std::uint16_t>(
            low | (static_cast<std::uint16_t>(high) << 8U));
        const auto expand = [](const unsigned component) {
            return (component << 3U) | (component >> 2U);
        };
        return UINT32_C(0xFF000000) | (expand(value & 0x1FU) << 16U) |
               (expand((value >> 5U) & 0x1FU) << 8U) |
               expand((value >> 10U) & 0x1FU);
    }

    [[nodiscard]] static std::uint32_t tile_color(
        const gameboy::MemoryBus& bus, const gameboy::DisplayPalette& palette,
        const unsigned bank, const unsigned tile, const unsigned row,
        const unsigned x, const bool object, const std::uint8_t attributes = 0) {
        const auto offset = static_cast<std::uint16_t>(tile * 16U + row * 2U);
        const auto low = bus.debug_read_vram(static_cast<std::uint8_t>(bank), offset);
        const auto high = bus.debug_read_vram(static_cast<std::uint8_t>(bank),
                                              static_cast<std::uint16_t>(offset + 1));
        const auto color = static_cast<std::uint8_t>(
            ((low >> (7U - x)) & 1U) | (((high >> (7U - x)) & 1U) << 1U));
        if (bus.cgb_mode()) {
            const auto cgb_palette = static_cast<std::uint8_t>(attributes & 7U);
            const auto palette_offset = static_cast<std::uint8_t>(
                cgb_palette * 8U + color * 2U);
            return rgb555_color(
                object ? bus.debug_read_cgb_object_palette(palette_offset)
                       : bus.debug_read_cgb_bg_palette(palette_offset),
                object ? bus.debug_read_cgb_object_palette(
                             static_cast<std::uint8_t>(palette_offset + 1))
                       : bus.debug_read_cgb_bg_palette(
                             static_cast<std::uint8_t>(palette_offset + 1)));
        }
        const auto register_address = object && (attributes & 0x10U)
                                           ? 0xFF49U
                                           : object ? 0xFF48U : 0xFF47U;
        return palette.colors[(bus.read8(register_address) >> (color * 2U)) & 3U];
    }

    static std::string hex8(const std::uint8_t value) {
        std::ostringstream output;
        output << '$' << std::uppercase << std::hex << std::setfill('0')
               << std::setw(2) << static_cast<unsigned>(value);
        return output.str();
    }

    static std::string hex16(const std::uint16_t value) {
        std::ostringstream output;
        output << '$' << std::uppercase << std::hex << std::setfill('0')
               << std::setw(4) << static_cast<unsigned>(value);
        return output.str();
    }

    static void panel(SDL_Renderer* renderer, const SDL_FRect& rect) {
        static_cast<void>(SDL_SetRenderDrawColor(renderer, 12, 20, 30, 255));
        static_cast<void>(SDL_RenderFillRect(renderer, &rect));
        static_cast<void>(SDL_SetRenderDrawColor(renderer, 34, 91, 111, 255));
        static_cast<void>(SDL_RenderRect(renderer, &rect));
    }

    void draw_mode_button(const SDL_FRect& rect, const char* label,
                          const bool selected) {
        draw_tool_button_background(renderer_, window_, rect);
        if (selected) {
            static_cast<void>(SDL_SetRenderDrawColor(renderer_, 69, 207, 238,
                                                      255));
            static_cast<void>(SDL_RenderRect(renderer_, &rect));
        }
        static_cast<void>(SDL_SetRenderDrawColor(renderer_, 230, 249, 255, 255));
        render_tool_text(renderer_, rect.x + 10, rect.y + 13, label);
    }

    void info_text(const float x, const float y, const std::string& value,
                   const bool heading = false) {
        static_cast<void>(SDL_SetRenderDrawColor(
            renderer_, heading ? 238 : 230, heading ? 196 : 249,
            heading ? 100 : 255, 255));
        render_tool_text(renderer_, x, y, value.c_str());
    }

    void present_background(const gameboy::Emulator& emulator,
                            const gameboy::DisplayPalette& palette) {
        constexpr float image_x = 24.0F;
        constexpr float image_y = 94.0F;
        const auto map = render_desktop_background_map(emulator.bus(), palette);
        static_cast<void>(SDL_UpdateTexture(
            texture_, nullptr, map.pixels.data(),
            static_cast<int>(DesktopBackgroundMap::width * sizeof(std::uint32_t))));
        const SDL_FRect image{image_x, image_y, 512, 512};
        static_cast<void>(SDL_RenderTexture(renderer_, texture_, nullptr, &image));
        static_cast<void>(SDL_SetRenderDrawColor(renderer_, 34, 47, 58, 255));
        for (unsigned tile = 0; tile <= 32; ++tile) {
            const auto coordinate = image_x + tile * 16.0F;
            static_cast<void>(SDL_RenderLine(renderer_, coordinate, image_y,
                                             coordinate, image_y + 512));
            static_cast<void>(SDL_RenderLine(renderer_, image_x,
                                             image_y + tile * 16.0F,
                                             image_x + 512,
                                             image_y + tile * 16.0F));
        }
        const SDL_FRect selected{image_x + selected_bg_x_ * 16.0F,
                                 image_y + selected_bg_y_ * 16.0F, 16, 16};
        static_cast<void>(SDL_SetRenderDrawColor(renderer_, 69, 207, 238, 255));
        static_cast<void>(SDL_RenderRect(renderer_, &selected));
        const auto lcdc = emulator.bus().read8(0xFF40);
        const auto map_base = (lcdc & 0x08U) != 0 ? 0x1C00U : 0x1800U;
        const auto map_offset = static_cast<std::uint16_t>(
            map_base + selected_bg_y_ * 32U + selected_bg_x_);
        const auto tile = emulator.bus().debug_read_vram(0, map_offset);
        const auto attributes = emulator.bus().cgb_mode()
                                    ? emulator.bus().debug_read_vram(1, map_offset)
                                    : 0;
        panel(renderer_, {560, 94, 300, 230});
        info_text(576, 108, "BG VIEWER", true);
        info_text(576, 136, "X        " + std::to_string(selected_bg_x_));
        info_text(576, 156, "Y        " + std::to_string(selected_bg_y_));
        info_text(576, 176, "TILE NO  " + hex8(tile));
        info_text(576, 196, "ATTRIBUTE " + hex8(attributes));
        info_text(576, 220, "MAP      " + hex8(static_cast<std::uint8_t>(map_base >> 8U)) +
                              "00");
        info_text(576, 240, "BANK     " + std::to_string((attributes & 8U) ? 1 : 0));
        info_text(576, 260, "PALETTE  " + std::to_string(attributes & 7U));
        info_text(576, 280, std::string("FLIP     ") +
                              ((attributes & 0x20U) ? "H" : "-") +
                              ((attributes & 0x40U) ? "V" : "-"));
        info_text(576, 308, "SCROLL   " + hex8(map.scroll_x) + " / " +
                              hex8(map.scroll_y));
    }

    void present_oam(const gameboy::Emulator& emulator,
                     const gameboy::DisplayPalette& palette) {
        constexpr float origin_x = 24.0F;
        constexpr float origin_y = 94.0F;
        constexpr float cell = 100.0F;
        const auto sprite_height = (emulator.bus().read8(0xFF40) & 4U) ? 16U : 8U;
        for (unsigned index = 0; index < 40; ++index) {
            const auto column = index % 5U;
            const auto row = index / 5U;
            const auto cell_x = origin_x + column * cell;
            const auto cell_y = origin_y + row * cell;
            panel(renderer_, {cell_x, cell_y, 92, 92});
            const auto attributes = emulator.bus().debug_read_oam(
                static_cast<std::uint8_t>(index * 4U + 3U));
            const auto bank = emulator.bus().cgb_mode() && (attributes & 8U) ? 1U : 0U;
            auto tile = static_cast<unsigned>(emulator.bus().debug_read_oam(
                static_cast<std::uint8_t>(index * 4U + 2U)));
            const auto x_scale = sprite_height == 16 ? 6.0F : 10.0F;
            const auto y_scale = sprite_height == 16 ? 6.0F : 10.0F;
            const auto draw_x = cell_x + (92.0F - 8.0F * x_scale) / 2.0F;
            const auto draw_y = cell_y + (92.0F - sprite_height * y_scale) / 2.0F;
            if (sprite_height == 16) tile &= 0xFEU;
            for (unsigned y = 0; y < sprite_height; ++y) {
                const auto source_y = (attributes & 0x40U) ? sprite_height - 1 - y : y;
                const auto tile_index = tile + source_y / 8U;
                for (unsigned x = 0; x < 8; ++x) {
                    const auto source_x = (attributes & 0x20U) ? 7U - x : x;
                    set_color(renderer_, tile_color(emulator.bus(), palette, bank,
                                                     tile_index, source_y & 7U,
                                                     source_x, true, attributes));
                    const SDL_FRect pixel{draw_x + x * x_scale,
                                          draw_y + y * y_scale, x_scale, y_scale};
                    static_cast<void>(SDL_RenderFillRect(renderer_, &pixel));
                }
            }
            if (index == selected_oam_) {
                static_cast<void>(SDL_SetRenderDrawColor(renderer_, 69, 207, 238, 255));
                const SDL_FRect selection{cell_x, cell_y, 92, 92};
                static_cast<void>(SDL_RenderRect(renderer_, &selection));
            }
            static_cast<void>(SDL_SetRenderDrawColor(renderer_, 177, 192, 208, 255));
            render_tool_text(renderer_, cell_x + 4, cell_y + 78,
                             ("#" + std::to_string(index)).c_str());
        }
        const auto base = static_cast<std::uint8_t>(selected_oam_ * 4U);
        const auto y = emulator.bus().debug_read_oam(base);
        const auto x = emulator.bus().debug_read_oam(static_cast<std::uint8_t>(base + 1));
        const auto tile = emulator.bus().debug_read_oam(static_cast<std::uint8_t>(base + 2));
        const auto attributes = emulator.bus().debug_read_oam(static_cast<std::uint8_t>(base + 3));
        panel(renderer_, {560, 94, 300, 220});
        info_text(576, 108, "OAM VIEWER", true);
        info_text(576, 136, "INDEX    " + std::to_string(selected_oam_));
        info_text(576, 156, "X        " + hex8(x) + " (" + std::to_string(static_cast<int>(x) - 8) + ")");
        info_text(576, 176, "Y        " + hex8(y) + " (" + std::to_string(static_cast<int>(y) - 16) + ")");
        info_text(576, 196, "TILE NO  " + hex8(tile));
        info_text(576, 216, "ATTRIBUTE " + hex8(attributes));
        info_text(576, 240, "BANK     " + std::to_string((attributes & 8U) ? 1 : 0));
        info_text(576, 260, "PALETTE  " + std::to_string(attributes & 7U));
        info_text(576, 280, std::string("FLIP     ") +
                              ((attributes & 0x20U) ? "H" : "-") +
                              ((attributes & 0x40U) ? "V" : "-"));
        info_text(576, 300, std::string("PRIORITY ") +
                              ((attributes & 0x80U) ? "BEHIND BG" : "FRONT"));
    }

    void present_tiles(const gameboy::Emulator& emulator,
                       const gameboy::DisplayPalette& palette) {
        constexpr float origin_x = 24.0F;
        constexpr float origin_y = 94.0F;
        constexpr float cell = 26.0F;
        constexpr unsigned columns = 16;
        constexpr auto count = 384U;
        for (unsigned tile = 0; tile < count; ++tile) {
            const auto x = origin_x + (tile % columns) * cell;
            const auto y = origin_y + (tile / columns) * cell;
            for (unsigned row = 0; row < 8; ++row) {
                for (unsigned pixel = 0; pixel < 8; ++pixel) {
                    set_color(renderer_, tile_color(emulator.bus(), palette, bank_,
                                                     tile, row, pixel, false));
                    const SDL_FRect rect{x + pixel * 3.0F, y + row * 3.0F, 3, 3};
                    static_cast<void>(SDL_RenderFillRect(renderer_, &rect));
                }
            }
            static_cast<void>(SDL_SetRenderDrawColor(renderer_, 28, 47, 68, 255));
            const SDL_FRect tile_rect{x, y, 24, 24};
            static_cast<void>(SDL_RenderRect(renderer_, &tile_rect));
        }
        const auto selected_x = origin_x + (selected_tile_ % columns) * cell;
        const auto selected_y = origin_y + (selected_tile_ / columns) * cell;
        static_cast<void>(SDL_SetRenderDrawColor(renderer_, 69, 207, 238, 255));
        const SDL_FRect selection{selected_x - 1, selected_y - 1, 26, 26};
        static_cast<void>(SDL_RenderRect(renderer_, &selection));
        panel(renderer_, {470, 94, 300, 170});
        info_text(486, 108, "TILES VIEWER", true);
        info_text(486, 136, "TILE     " + std::to_string(selected_tile_));
        info_text(486, 156, "ADDRESS  " +
                              hex16(static_cast<std::uint16_t>(selected_tile_ * 16U)));
        info_text(486, 176, "BANK     " + std::to_string(bank_));
        info_text(486, 196, "BYTES    16  (2BPP)");
        info_text(486, 226, "READ-ONLY VIEW");
    }

    void move_selection(const int dx, const int dy) noexcept {
        if (mode_ == Mode::background) {
            selected_bg_x_ = static_cast<unsigned>(
                std::clamp(static_cast<int>(selected_bg_x_) + dx, 0, 31));
            selected_bg_y_ = static_cast<unsigned>(
                std::clamp(static_cast<int>(selected_bg_y_) + dy, 0, 31));
        } else if (mode_ == Mode::oam) {
            const auto column = static_cast<int>(selected_oam_ % 5U) + dx;
            const auto row = static_cast<int>(selected_oam_ / 5U) + dy;
            selected_oam_ = static_cast<unsigned>(
                std::clamp(row, 0, 7) * 5 + std::clamp(column, 0, 4));
        } else {
            const auto column = static_cast<int>(selected_tile_ % 16U) + dx;
            const auto row = static_cast<int>(selected_tile_ / 16U) + dy;
            selected_tile_ = static_cast<unsigned>(
                std::clamp(row, 0, 23) * 16 + std::clamp(column, 0, 15));
        }
    }

    void select_at(const float x, const float y,
                   const gameboy::Emulator& emulator) noexcept {
        if (mode_ == Mode::background && x >= 24 && x < 536 && y >= 94 && y < 606) {
            selected_bg_x_ = static_cast<unsigned>((x - 24) / 16);
            selected_bg_y_ = static_cast<unsigned>((y - 94) / 16);
        } else if (mode_ == Mode::oam && x >= 24 && x < 524 && y >= 94 && y < 894) {
            const auto column = static_cast<unsigned>((x - 24) / 100);
            const auto row = static_cast<unsigned>((y - 94) / 100);
            if (column < 5 && row < 8) selected_oam_ = row * 5 + column;
        } else if (mode_ == Mode::tiles && x >= 24 && x < 440 && y >= 94 && y < 718) {
            const auto column = static_cast<unsigned>((x - 24) / 26);
            const auto row = static_cast<unsigned>((y - 94) / 26);
            if (column < 16 && row < 24) selected_tile_ = row * 16 + column;
        }
        if (!emulator.bus().cgb_mode()) bank_ = 0;
    }

    SDL_Window* window_{};
    SDL_Renderer* renderer_{};
    SDL_Texture* texture_{};
    Mode mode_{Mode::background};
    unsigned selected_bg_x_{};
    unsigned selected_bg_y_{};
    unsigned selected_oam_{};
    unsigned selected_tile_{};
    unsigned bank_{};
};

} // namespace gbb::sdl

#endif // __ANDROID__
