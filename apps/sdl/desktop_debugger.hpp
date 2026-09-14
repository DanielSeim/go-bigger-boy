#pragma once

#ifndef __ANDROID__

#include "gameboy/display_palette.hpp"
#include "gameboy/emulator.hpp"
#include "gameboy/ppu.hpp"
#include "desktop_breakpoints.hpp"
#include "desktop_disassembler.hpp"
#include "desktop_viewport.hpp"
#include "input_movie.hpp"
#include "tool_window_support.hpp"
#include "window_event.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

namespace gbb::sdl {

class DesktopDebugger {
public:
    DesktopDebugger() = default;
    ~DesktopDebugger() { close(); }
    DesktopDebugger(const DesktopDebugger&) = delete;
    DesktopDebugger& operator=(const DesktopDebugger&) = delete;

    [[nodiscard]] bool visible() const noexcept { return window_ != nullptr; }
    [[nodiscard]] bool execution_paused() const noexcept {
        return visible() && execution_paused_;
    }
    [[nodiscard]] bool take_instruction_step() noexcept {
        return std::exchange(step_instruction_, false);
    }
    [[nodiscard]] bool take_frame_step() noexcept {
        return std::exchange(step_frame_, false);
    }
    [[nodiscard]] bool take_record_toggle() noexcept {
        return std::exchange(toggle_recording_, false);
    }
    [[nodiscard]] bool take_replay_request() noexcept {
        return std::exchange(replay_requested_, false);
    }
    [[nodiscard]] bool take_tas_request() noexcept {
        return std::exchange(tas_requested_, false);
    }
    [[nodiscard]] bool take_sprite_request() noexcept {
        return std::exchange(sprite_requested_, false);
    }
    [[nodiscard]] bool has_breakpoints() const noexcept {
        return !breakpoints_.empty();
    }
    [[nodiscard]] bool breakpoint_at(const std::uint16_t address) const noexcept {
        return breakpoints_.contains(address);
    }
    [[nodiscard]] std::size_t breakpoint_count() const noexcept {
        return breakpoints_.size();
    }
    [[nodiscard]] const std::vector<std::uint16_t>& breakpoints() const noexcept {
        return breakpoints_.addresses();
    }
    [[nodiscard]] std::optional<std::uint16_t> breakpoint_hit() const noexcept {
        return breakpoints_.last_hit();
    }
    [[nodiscard]] bool toggle_breakpoint(const std::uint16_t address) {
        return breakpoints_.toggle(address);
    }
    void clear_breakpoints() noexcept { breakpoints_.clear(); }
    [[nodiscard]] bool check_breakpoint(const std::uint16_t address) noexcept {
        if (const auto hit = breakpoints_.check(address)) {
            execution_paused_ = true;
            return true;
        }
        return false;
    }
    void request_record_toggle() noexcept { toggle_recording_ = true; }
    void request_replay() noexcept { replay_requested_ = true; }
    void request_tas_editor() noexcept { tas_requested_ = true; }
    void request_sprite_editor() noexcept { sprite_requested_ = true; }
    void run() noexcept {
        breakpoints_.resume_after_hit();
        execution_paused_ = false;
    }
    void pause() noexcept { execution_paused_ = true; }

    void toggle(SDL_Window* parent) {
        if (visible()) {
            close();
            return;
        }
        window_ = SDL_CreateWindow("Go Bigger Boy - Debugger", 1280, 900,
                                   SDL_WINDOW_RESIZABLE);
        if (window_ == nullptr) throw_sdl_error("Could not create debugger window");
        static_cast<void>(SDL_SetWindowMinimumSize(window_, 1180, 820));
        renderer_ = SDL_CreateRenderer(window_, nullptr);
        if (renderer_ == nullptr) {
            close();
            throw_sdl_error("Could not create debugger renderer");
        }
        texture_ = SDL_CreateTexture(
            renderer_, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
            static_cast<int>(gameboy::Ppu::screen_width),
            static_cast<int>(gameboy::Ppu::screen_height));
        if (texture_ == nullptr) {
            close();
            throw_sdl_error("Could not create debugger framebuffer texture");
        }
        static_cast<void>(SDL_SetTextureScaleMode(texture_, SDL_SCALEMODE_NEAREST));
        background_texture_ = SDL_CreateTexture(
            renderer_, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
            static_cast<int>(DesktopBackgroundMap::width),
            static_cast<int>(DesktopBackgroundMap::height));
        if (background_texture_ == nullptr) {
            close();
            throw_sdl_error("Could not create debugger background map texture");
        }
        static_cast<void>(SDL_SetTextureScaleMode(background_texture_,
                                                   SDL_SCALEMODE_NEAREST));
        execution_paused_ = true;
        if (parent != nullptr) {
            int x = 0;
            int y = 0;
            static_cast<void>(SDL_GetWindowPosition(parent, &x, &y));
            static_cast<void>(SDL_SetWindowPosition(window_, x + 48, y + 48));
        }
    }

    void close() noexcept {
        if (texture_ != nullptr) SDL_DestroyTexture(texture_);
        if (background_texture_ != nullptr) SDL_DestroyTexture(background_texture_);
        clear_tool_text_cache(renderer_);
        if (renderer_ != nullptr) SDL_DestroyRenderer(renderer_);
        if (window_ != nullptr) SDL_DestroyWindow(window_);
        texture_ = nullptr;
        background_texture_ = nullptr;
        renderer_ = nullptr;
        window_ = nullptr;
        execution_paused_ = true;
        step_instruction_ = false;
        step_frame_ = false;
        toggle_recording_ = false;
        replay_requested_ = false;
        tas_requested_ = false;
        sprite_requested_ = false;
        cancel_edit();
    }

    bool handle_event(const SDL_Event& event, gameboy::Emulator* emulator) {
        if (!visible()) return false;
        const auto id = SDL_GetWindowID(window_);
        if (gbb::sdl::event_window_id(event) != id) return false;

        if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
            close();
        } else if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat) {
            if (editing_) {
                if (event.key.key == SDLK_ESCAPE) {
                    cancel_edit();
                } else if (event.key.key == SDLK_RETURN ||
                           event.key.key == SDLK_KP_ENTER) {
                    apply_edit(emulator);
                } else if (event.key.key == SDLK_BACKSPACE) {
                    if (replace_on_type_) {
                        edit_value_.clear();
                        replace_on_type_ = false;
                    } else if (!edit_value_.empty()) {
                        edit_value_.pop_back();
                    }
                } else if (const auto digit = hexadecimal_digit(event.key.key)) {
                    if (replace_on_type_) {
                        edit_value_.clear();
                        replace_on_type_ = false;
                    }
                    const auto maximum = register_width(*editing_);
                    if (edit_value_.size() < maximum) edit_value_ += *digit;
                }
            } else if (event.key.key == SDLK_UP || event.key.key == SDLK_DOWN ||
                       event.key.key == SDLK_PAGEUP ||
                       event.key.key == SDLK_PAGEDOWN) {
                disassembly_follow_pc_ = false;
                const auto count = event.key.key == SDLK_PAGEUP ||
                                           event.key.key == SDLK_PAGEDOWN
                                       ? 10U
                                       : 1U;
                for (unsigned index = 0; index < count; ++index) {
                    if (event.key.key == SDLK_UP ||
                        event.key.key == SDLK_PAGEUP) {
                        disassembly_start_ = previous_instruction_address(
                            emulator, disassembly_start_);
                    } else {
                        disassembly_start_ = next_instruction_address(
                            emulator, disassembly_start_);
                    }
                }
            } else if (event.key.key == SDLK_TAB) {
                focus_index_ = cycle_tool_focus(
                    focus_index_, 10, (event.key.mod & SDL_KMOD_SHIFT) != 0);
            } else if ((event.key.key == SDLK_RETURN ||
                        event.key.key == SDLK_KP_ENTER ||
                        event.key.key == SDLK_SPACE) &&
                       activate_focused(emulator)) {
            } else if (event.key.key == SDLK_F12 || event.key.key == SDLK_ESCAPE) {
                close();
            } else if (event.key.key == SDLK_F5 || event.key.key == SDLK_SPACE) {
                if (execution_paused_) run();
                else pause();
            } else if (event.key.key == SDLK_F10) {
                execution_paused_ = true;
                step_instruction_ = true;
            } else if (event.key.key == SDLK_F11) {
                execution_paused_ = true;
                step_frame_ = true;
            } else if (event.key.key == SDLK_F2 && emulator != nullptr) {
                static_cast<void>(toggle_breakpoint(
                    emulator->cpu().registers().pc));
            } else if (event.key.key == SDLK_F3) {
                clear_breakpoints();
            } else if (event.key.key == SDLK_F6) {
                toggle_recording_ = true;
            } else if (event.key.key == SDLK_F7) {
                replay_requested_ = true;
            } else if (event.key.key == SDLK_F8) {
                tas_requested_ = true;
            } else if (event.key.key == SDLK_F9) {
                sprite_requested_ = true;
            }
        } else if (event.type == SDL_EVENT_MOUSE_WHEEL) {
            float mouse_x = 0.0F;
            float mouse_y = 0.0F;
            static_cast<void>(SDL_GetMouseState(&mouse_x, &mouse_y));
            int width = 0;
            int height = 0;
            static_cast<void>(SDL_GetWindowSize(window_, &width, &height));
            const auto panel = disassembly_panel(width, height);
            if (mouse_x >= panel.x && mouse_x <= panel.x + panel.w &&
                mouse_y >= panel.y && mouse_y <= panel.y + panel.h) {
                disassembly_follow_pc_ = false;
                const auto steps = event.wheel.y > 0 ? 4 : -4;
                if (emulator != nullptr) {
                    for (int index = 0;
                         index < (steps > 0 ? steps : -steps); ++index) {
                        disassembly_start_ =
                            steps > 0
                                ? previous_instruction_address(
                                      emulator, disassembly_start_)
                                : next_instruction_address(
                                      emulator, disassembly_start_);
                    }
                }
                return true;
            }
        } else if (event.type == SDL_EVENT_MOUSE_BUTTON_UP &&
                   event.button.button == SDL_BUTTON_LEFT) {
            int width = 0;
            int height = 0;
            static_cast<void>(SDL_GetWindowSize(window_, &width, &height));
            if (tool_close_button_hit(width, height, event.button.x,
                                      event.button.y)) {
                focus_index_ = 0;
                close();
                return true;
            }
            const auto y = static_cast<float>(height - 58);
            const auto movie_y = static_cast<float>(height - 106);
            const auto breakpoint_y = static_cast<float>(height - 154);
            const auto x = event.button.x;
            const auto register_x = register_panel_x(width);
            const auto panel = disassembly_panel(width, height);
            if (emulator != nullptr &&
                event.button.x >= panel.x &&
                event.button.x <= panel.x + panel.w &&
                event.button.y >= panel.y + 34.0F &&
                event.button.y < panel.y + 34.0F +
                                     disassembly_line_count * disassembly_line_height) {
                const auto disassembly = disassemble(
                    emulator->bus(),
                    disassembly_follow_pc_
                        ? emulator->cpu().registers().pc
                        : disassembly_start_,
                    disassembly_line_count);
                const auto index = static_cast<std::size_t>(
                    (event.button.y - panel.y - 34.0F) /
                    disassembly_line_height);
                if (index < disassembly.size()) {
                    disassembly_follow_pc_ = false;
                    disassembly_start_ = disassembly[index].address;
                    static_cast<void>(toggle_breakpoint(disassembly[index].address));
                    return true;
                }
            }
            if (emulator != nullptr && execution_paused_) {
                if (const auto selected = register_at(
                        event.button.x, event.button.y, register_x)) {
                    begin_edit(*selected, emulator->cpu().registers());
                    return true;
                }
            }
            cancel_edit();
            if (event.button.y >= movie_y &&
                event.button.y <= movie_y + 36.0F) {
                if (x >= 24.0F && x <= 194.0F) {
                    focus_index_ = 3;
                    toggle_recording_ = true;
                } else if (x >= 208.0F && x <= 378.0F) {
                    focus_index_ = 4;
                    replay_requested_ = true;
                } else if (x >= 734.0F && x <= 884.0F) {
                    focus_index_ = 5;
                    tas_requested_ = true;
                }
            } else if (event.button.y >= breakpoint_y &&
                       event.button.y <= breakpoint_y + 36.0F) {
                if (x >= 24.0F && x <= 274.0F && emulator != nullptr) {
                    focus_index_ = 1;
                    static_cast<void>(toggle_breakpoint(
                        emulator->cpu().registers().pc));
                } else if (x >= 288.0F && x <= 488.0F) {
                    focus_index_ = 2;
                    clear_breakpoints();
                }
            } else if (event.button.y >= y && event.button.y <= y + 36.0F) {
                if (x >= 24.0F && x <= 174.0F) {
                    focus_index_ = 6;
                    if (execution_paused_) run();
                    else pause();
                } else if (x >= 188.0F && x <= 358.0F) {
                    focus_index_ = 7;
                    execution_paused_ = true;
                    step_instruction_ = true;
                } else if (x >= 372.0F && x <= 522.0F) {
                    focus_index_ = 8;
                    execution_paused_ = true;
                    step_frame_ = true;
                } else if (x >= 536.0F && x <= 686.0F) {
                    focus_index_ = 9;
                    sprite_requested_ = true;
                }
            }
        }
        return true;
    }

    void present(const gameboy::Emulator& emulator,
                 const gameboy::DisplayPalette& palette,
                 const InputMovie& movie) {
        if (!visible()) return;
        int width = 0;
        int height = 0;
        static_cast<void>(SDL_GetWindowSize(window_, &width, &height));
        static_cast<void>(SDL_SetRenderDrawColor(renderer_, 8, 12, 20, 255));
        static_cast<void>(SDL_RenderClear(renderer_));
        static_cast<void>(SDL_SetRenderDrawColor(renderer_, 69, 207, 238, 255));
        render_tool_text(renderer_, 24, 20, "GO BIGGER BOY / DEBUGGER");
        static_cast<void>(SDL_SetRenderDrawColor(renderer_, 177, 192, 208, 255));

        constexpr float map_scale = 2.0F;
        constexpr float map_x = 24.0F;
        constexpr float map_y = 72.0F;
        constexpr float map_width = DesktopBackgroundMap::width * map_scale;
        constexpr float map_height = DesktopBackgroundMap::height * map_scale;
        gameboy::Ppu::Framebuffer pixels{};
        const auto& source = emulator.framebuffer();
        const auto native = emulator.bus().cgb_mode() || palette.cgb_compatibility;
        for (std::size_t index = 0; index < source.size(); ++index) {
            pixels[index] = native ? source[index]
                                   : gameboy::apply_display_palette(source[index],
                                                                    palette);
        }
        static_cast<void>(SDL_UpdateTexture(
            texture_, nullptr, pixels.data(),
            static_cast<int>(gameboy::Ppu::screen_width * sizeof(std::uint32_t))));
        const auto map = render_desktop_background_map(emulator.bus(), palette);
        static_cast<void>(SDL_UpdateTexture(
            background_texture_, nullptr, map.pixels.data(),
            static_cast<int>(DesktopBackgroundMap::width * sizeof(std::uint32_t))));
        const SDL_FRect background_map{map_x, map_y, map_width, map_height};
        static_cast<void>(SDL_RenderTexture(renderer_, background_texture_, nullptr,
                                            &background_map));
        static_cast<void>(SDL_SetRenderDrawColor(renderer_, 69, 207, 238, 255));
        static_cast<void>(SDL_RenderRect(renderer_, &background_map));
        const SDL_FRect outer{map_x - 3, map_y - 3, map_width + 6,
                              map_height + 6};
        static_cast<void>(SDL_RenderRect(renderer_, &outer));
        render_tool_text(renderer_, map_x, map_y + map_height + 10,
                         "BACKGROUND MAP 256 x 256  /  VISIBLE 160 x 144");
        render_visible_viewport_overlay(renderer_, map_x, map_y, map_scale,
                                        map.scroll_x, map.scroll_y);

        // Keep the actual rasterized output visible as a small diagnostic
        // inset. The expanded map is reconstructed from VRAM; this inset is
        // the emulator's authoritative composition, including window/OBJ.
        constexpr float live_width = gameboy::Ppu::screen_width;
        constexpr float live_height = gameboy::Ppu::screen_height;
        const SDL_FRect live_frame{map_x + map_width - live_width - 12.0F,
                                   map_y + map_height - live_height - 12.0F,
                                   live_width, live_height};
        const SDL_FRect live_outer{live_frame.x - 3.0F, live_frame.y - 3.0F,
                                   live_frame.w + 6.0F, live_frame.h + 6.0F};
        static_cast<void>(SDL_SetRenderDrawColor(renderer_, 8, 12, 20, 255));
        static_cast<void>(SDL_RenderFillRect(renderer_, &live_outer));
        static_cast<void>(SDL_RenderTexture(renderer_, texture_, nullptr,
                                            &live_frame));
        static_cast<void>(SDL_SetRenderDrawColor(renderer_, 230, 249, 255, 255));
        static_cast<void>(SDL_RenderRect(renderer_, &live_outer));
        render_tool_text(renderer_, live_frame.x, live_frame.y - 16.0F,
                         "LIVE OUTPUT");

        const auto& r = emulator.cpu().registers();
        const auto pair = [](const std::uint8_t high, const std::uint8_t low) {
            return static_cast<std::uint16_t>(
                (static_cast<std::uint16_t>(high) << 8U) | low);
        };
        const auto hex8 = [](const std::uint8_t value) {
            std::ostringstream out;
            out << '$' << std::uppercase << std::hex << std::setfill('0')
                << std::setw(2) << static_cast<unsigned>(value);
            return out.str();
        };
        const auto hex16 = [](const std::uint16_t value) {
            std::ostringstream out;
            out << '$' << std::uppercase << std::hex << std::setfill('0')
                << std::setw(4) << value;
            return out.str();
        };
        std::string execution_status = execution_paused_ ? "PAUSED" : "RUNNING";
        if (const auto hit = breakpoint_hit()) {
            execution_status = "BREAKPOINT HIT " + hex16(*hit);
        }
        render_tool_text(renderer_, 24, 38, execution_status.c_str());
        const auto text = [this](const float x, const float y,
                                 const std::string& value) {
            static_cast<void>(SDL_SetRenderDrawColor(renderer_, 230, 249, 255, 255));
            render_tool_text(renderer_, x, y, value.c_str());
        };
        const auto register_x = register_panel_x(width);
        text(register_x, 72, "CPU REGISTERS");
        text(register_x, 86, execution_paused_
                                 ? "CLICK A VALUE TO EDIT"
                                 : "PAUSE TO EDIT REGISTERS");
        text(register_x, 100, "A  " + hex8(r.a) + "    F  " + hex8(r.f));
        text(register_x, 122, "B  " + hex8(r.b) + "    C  " + hex8(r.c));
        text(register_x, 144, "D  " + hex8(r.d) + "    E  " + hex8(r.e));
        text(register_x, 166, "H  " + hex8(r.h) + "    L  " + hex8(r.l));
        text(register_x, 198, "AF " + hex16(pair(r.a, r.f)));
        text(register_x, 220, "BC " + hex16(pair(r.b, r.c)));
        text(register_x, 242, "DE " + hex16(pair(r.d, r.e)));
        text(register_x, 264, "HL " + hex16(pair(r.h, r.l)));
        text(register_x, 296, "SP " + hex16(r.sp));
        text(register_x, 318, "PC " + hex16(r.pc));
        constexpr std::array editable_registers{
            RegisterTarget::a, RegisterTarget::f, RegisterTarget::b,
            RegisterTarget::c, RegisterTarget::d, RegisterTarget::e,
            RegisterTarget::h, RegisterTarget::l, RegisterTarget::sp,
            RegisterTarget::pc};
        static_cast<void>(SDL_SetRenderDrawColor(renderer_, 34, 91, 111, 255));
        for (const auto target : editable_registers) {
            const auto [x, y] = register_position(target, register_x);
            const SDL_FRect box{x - 3.0F, y - 3.0F,
                                register_width(target) == 2 ? 38.0F : 54.0F,
                                14.0F};
            static_cast<void>(SDL_RenderRect(renderer_, &box));
        }
        if (editing_) {
            const auto [x, y] = register_position(*editing_, register_x);
            const SDL_FRect edit_box{x - 3.0F, y - 3.0F,
                                     register_width(*editing_) == 2 ? 38.0F
                                                                    : 54.0F,
                                     14.0F};
            static_cast<void>(SDL_SetRenderDrawColor(renderer_, 20, 77, 101, 255));
            static_cast<void>(SDL_RenderFillRect(renderer_, &edit_box));
            static_cast<void>(SDL_SetRenderDrawColor(renderer_, 69, 207, 238, 255));
            static_cast<void>(SDL_RenderRect(renderer_, &edit_box));
            text(x, y, "$" + edit_value_ + "_");
        }
        text(register_x, 350,
             std::string("FLAGS  Z:") + ((r.f & 0x80U) ? "1" : "0") +
                 " N:" + ((r.f & 0x40U) ? "1" : "0") +
                 " H:" + ((r.f & 0x20U) ? "1" : "0") +
                 " C:" + ((r.f & 0x10U) ? "1" : "0"));
        text(register_x, 372, std::string("IME ") +
                                  (emulator.cpu().interrupts_enabled() ? "ON" : "OFF"));
        text(register_x, 394,
             std::string("CPU ") +
                 (emulator.cpu().stopped()
                      ? "STOPPED"
                      : emulator.cpu().halted() ? "HALTED" : "ACTIVE"));
        text(register_x, 426,
             "CYCLES " + std::to_string(emulator.cpu().total_cycles()));
        const auto& bus = emulator.bus();
        text(register_x, 464, "HARDWARE REGISTERS");
        text(register_x, 486,
             "LCDC " + hex8(bus.read8(0xFF40)) +
                 "  STAT " + hex8(bus.read8(0xFF41)));
        text(register_x, 508,
             "SCX  " + hex8(bus.read8(0xFF43)) +
                 "  SCY  " + hex8(bus.read8(0xFF42)));
        text(register_x, 530,
             "LY   " + hex8(bus.read8(0xFF44)) +
                 "  LYC  " + hex8(bus.read8(0xFF45)));
        text(register_x, 552,
             "WX   " + hex8(bus.read8(0xFF4B)) +
                 "  WY   " + hex8(bus.read8(0xFF4A)));
        text(register_x, 574,
             "IF   " + hex8(bus.read8(0xFF0F)) +
                 "  IE   " + hex8(bus.read8(0xFFFF)));
        const auto disassembly_panel_rect = disassembly_panel(width, height);
        static_cast<void>(SDL_SetRenderDrawColor(renderer_, 12, 20, 30, 255));
        static_cast<void>(SDL_RenderFillRect(renderer_, &disassembly_panel_rect));
        static_cast<void>(SDL_SetRenderDrawColor(renderer_, 34, 91, 111, 255));
        static_cast<void>(SDL_RenderRect(renderer_, &disassembly_panel_rect));
        text(disassembly_panel_rect.x + 12.0F,
             disassembly_panel_rect.y + 10.0F, "DISASSEMBLY");
        text(disassembly_panel_rect.x + 12.0F,
             disassembly_panel_rect.y + 24.0F,
             disassembly_follow_pc_ ? "FOLLOWING PC  (ARROWS TO SCROLL)"
                                    : "SCROLLED  (CLICK ROW FOR BREAKPOINT)");
        const auto start_address = disassembly_follow_pc_
                                       ? r.pc
                                       : disassembly_start_;
        const auto lines = disassemble(bus, start_address,
                                       disassembly_line_count);
        for (std::size_t index = 0; index < lines.size(); ++index) {
            const auto& line = lines[index];
            const auto row_y = disassembly_panel_rect.y + 34.0F +
                               static_cast<float>(index) *
                                   disassembly_line_height;
            const SDL_FRect row{disassembly_panel_rect.x + 4.0F, row_y,
                                disassembly_panel_rect.w - 8.0F,
                                disassembly_line_height - 2.0F};
            if (line.address == r.pc) {
                static_cast<void>(SDL_SetRenderDrawColor(renderer_, 20, 77, 101, 255));
                static_cast<void>(SDL_RenderFillRect(renderer_, &row));
            }
            if (breakpoint_at(line.address)) {
                static_cast<void>(SDL_SetRenderDrawColor(renderer_, 238, 100, 110, 255));
                const SDL_FRect marker{row.x, row.y, 3.0F, row.h};
                static_cast<void>(SDL_RenderFillRect(renderer_, &marker));
            }
            text(disassembly_panel_rect.x + 12.0F, row_y + 4.0F,
                 hex16(line.address));
            text(disassembly_panel_rect.x + 66.0F, row_y + 4.0F,
                 disassembly_bytes(line));
            auto instruction_text = line.text;
            const auto available = static_cast<std::size_t>(
                std::max(12.0F, (disassembly_panel_rect.w - 150.0F) / 8.0F));
            if (instruction_text.size() > available) {
                instruction_text.resize(available - 1);
                instruction_text += '~';
            }
            text(disassembly_panel_rect.x + 150.0F, row_y + 4.0F,
                 instruction_text);
        }
        const auto breakpoint_y = static_cast<float>(height - 154);
        text(24, breakpoint_y - 20,
             "BREAKPOINTS " + std::to_string(breakpoint_count()) +
                 "  F2 TOGGLE  F3 CLEAR");
        if (breakpoints_.empty()) {
            text(24, breakpoint_y - 6, "AT CURRENT PC");
        } else {
            std::string addresses = "PC ";
            for (std::size_t index = 0; index < breakpoints_.addresses().size();
                 ++index) {
                if (index != 0) addresses += ", ";
                addresses += hex16(breakpoints_.addresses()[index]);
            }
            text(24, breakpoint_y - 6, addresses.substr(0, 104));
        }

        const auto button = [this](const SDL_FRect& rect,
                                   const std::string& label) {
            draw_tool_button_background(renderer_, window_, rect);
            render_tool_text(renderer_, rect.x + 12, rect.y + 14,
                             label.c_str());
        };
        const auto button_y = static_cast<float>(height - 58);
        const auto movie_y = static_cast<float>(height - 106);
        button({24, breakpoint_y, 250, 36}, "F2 TOGGLE PC BREAKPOINT");
        button({288, breakpoint_y, 200, 36}, "F3 CLEAR BREAKPOINTS");
        button({24, movie_y, 170, 36},
               movie.recording() ? "F6 STOP + SAVE" : "F6 START RECORDING");
        button({208, movie_y, 170, 36}, "F7 REPLAY LAST");
        text(398, movie_y + 14,
             movie.replaying()
                 ? "REPLAYING"
                 : movie.recording()
                       ? "RECORDING  EVENTS " +
                             std::to_string(movie.event_count())
                       : "INPUT MOVIE IDLE");
        button({734, movie_y, 150, 36}, "F8 TAS EDITOR");
        button({24, button_y, 150, 36}, execution_paused_ ? "F5  RUN" : "F5  PAUSE");
        button({188, button_y, 170, 36}, "F10 STEP CPU");
        button({372, button_y, 150, 36}, "F11 STEP FRAME");
        button({536, button_y, 150, 36}, "F9 SPRITE EDITOR");
        draw_tool_close_button(renderer_, window_, width);
        draw_tool_focus_outline(renderer_, focused_rect(width, height));
        static_cast<void>(SDL_RenderPresent(renderer_));
    }

private:
    static constexpr std::size_t disassembly_line_count = 20;
    static constexpr float disassembly_line_height = 24.0F;

    [[nodiscard]] static SDL_FRect disassembly_panel(const int width,
                                                     const int height) noexcept {
        const auto x = std::max(760.0F, static_cast<float>(width) - 440.0F);
        const auto bottom = static_cast<float>(height - 174);
        return {x, 64.0F, static_cast<float>(width) - x - 24.0F,
                std::max(120.0F, bottom - 64.0F)};
    }

    static void render_visible_viewport_overlay(SDL_Renderer* renderer,
                                                const float map_x,
                                                const float map_y,
                                                const float scale,
                                                const std::uint8_t scroll_x,
                                                const std::uint8_t scroll_y) {
        static_cast<void>(SDL_SetRenderDrawColor(renderer, 69, 207, 238, 255));
        const auto segments = [](const unsigned scroll, const unsigned length) {
            const auto first = std::min(length, 256U - scroll);
            using SegmentList = std::array<std::pair<unsigned, unsigned>, 2>;
            return std::make_pair(
                SegmentList{{{scroll, first}, {0, length - first}}},
                length - first > 0U ? 2U : 1U);
        };
        const auto horizontal = segments(scroll_x, gameboy::Ppu::screen_width);
        const auto vertical = segments(scroll_y, gameboy::Ppu::screen_height);
        for (unsigned x_index = 0; x_index < horizontal.second; ++x_index) {
            for (unsigned y_index = 0; y_index < vertical.second; ++y_index) {
                const auto [x, width] = horizontal.first[x_index];
                const auto [y, height] = vertical.first[y_index];
                const SDL_FRect visible{
                    map_x + static_cast<float>(x) * scale,
                    map_y + static_cast<float>(y) * scale,
                    static_cast<float>(width) * scale,
                    static_cast<float>(height) * scale};
                static_cast<void>(SDL_RenderRect(renderer, &visible));
            }
        }
    }

    [[nodiscard]] static float register_panel_x(const int width) noexcept {
        const auto disassembly_x = disassembly_panel(width, 820).x;
        return std::max(530.0F,
                        std::min(static_cast<float>(width) - 350.0F,
                                 disassembly_x - 290.0F));
    }

    [[nodiscard]] std::uint16_t previous_instruction_address(
        gameboy::Emulator* emulator, const std::uint16_t address) const noexcept {
        if (emulator == nullptr || address == 0) return address;
        const auto& bus = emulator->bus();
        for (std::uint16_t distance = 1; distance <= 3; ++distance) {
            const auto candidate = static_cast<std::uint16_t>(address - distance);
            const auto instruction = disassemble_instruction(bus, candidate);
            if (instruction.length == distance) return candidate;
        }
        return static_cast<std::uint16_t>(address - 1);
    }

    [[nodiscard]] std::uint16_t next_instruction_address(
        gameboy::Emulator* emulator, const std::uint16_t address) const noexcept {
        if (emulator == nullptr) return address;
        const auto instruction = disassemble_instruction(emulator->bus(), address);
        return static_cast<std::uint16_t>(address + instruction.length);
    }

    [[nodiscard]] SDL_FRect focused_rect(const int width,
                                          const int height) const noexcept {
        if (focus_index_ == 0) return tool_close_button_rect(width);
        const auto breakpoint_y = static_cast<float>(height - 154);
        const auto movie_y = static_cast<float>(height - 106);
        const auto button_y = static_cast<float>(height - 58);
        switch (focus_index_) {
        case 1: return {24, breakpoint_y, 250, 36};
        case 2: return {288, breakpoint_y, 200, 36};
        case 3: return {24, movie_y, 170, 36};
        case 4: return {208, movie_y, 170, 36};
        case 5: return {734, movie_y, 150, 36};
        case 6: return {24, button_y, 150, 36};
        case 7: return {188, button_y, 170, 36};
        case 8: return {372, button_y, 150, 36};
        case 9: return {536, button_y, 150, 36};
        default: return tool_close_button_rect(width);
        }
    }

    bool activate_focused(gameboy::Emulator* emulator) {
        switch (focus_index_) {
        case 0: close(); return true;
        case 1:
            if (emulator != nullptr) {
                static_cast<void>(toggle_breakpoint(
                    emulator->cpu().registers().pc));
            }
            return true;
        case 2: clear_breakpoints(); return true;
        case 3: toggle_recording_ = true; return true;
        case 4: replay_requested_ = true; return true;
        case 5: tas_requested_ = true; return true;
        case 6:
            if (execution_paused_) run();
            else pause();
            return true;
        case 7: execution_paused_ = true; step_instruction_ = true; return true;
        case 8: execution_paused_ = true; step_frame_ = true; return true;
        case 9: sprite_requested_ = true; return true;
        default: return false;
        }
    }

    [[noreturn]] static void throw_sdl_error(const char* action) {
        throw std::runtime_error(std::string(action) + ": " + SDL_GetError());
    }

    enum class RegisterTarget { a, f, b, c, d, e, h, l, sp, pc };

    [[nodiscard]] static std::optional<char> hexadecimal_digit(
        const SDL_Keycode key) noexcept {
        if (key >= SDLK_0 && key <= SDLK_9) {
            return static_cast<char>('0' + (key - SDLK_0));
        }
        if (key >= SDLK_A && key <= SDLK_F) {
            return static_cast<char>('A' + (key - SDLK_A));
        }
        if (key >= SDLK_KP_0 && key <= SDLK_KP_9) {
            return static_cast<char>('0' + (key - SDLK_KP_0));
        }
        return std::nullopt;
    }

    [[nodiscard]] static std::size_t register_width(
        const RegisterTarget target) noexcept {
        return target == RegisterTarget::sp || target == RegisterTarget::pc
                   ? 4U
                   : 2U;
    }

    [[nodiscard]] static std::pair<float, float> register_position(
        const RegisterTarget target, const float x) noexcept {
        switch (target) {
        case RegisterTarget::a: return {x + 24, 100};
        case RegisterTarget::f: return {x + 104, 100};
        case RegisterTarget::b: return {x + 24, 122};
        case RegisterTarget::c: return {x + 104, 122};
        case RegisterTarget::d: return {x + 24, 144};
        case RegisterTarget::e: return {x + 104, 144};
        case RegisterTarget::h: return {x + 24, 166};
        case RegisterTarget::l: return {x + 104, 166};
        case RegisterTarget::sp: return {x + 24, 296};
        case RegisterTarget::pc: return {x + 24, 318};
        }
        return {x, 0};
    }

    [[nodiscard]] static std::optional<RegisterTarget> register_at(
        const float mouse_x, const float mouse_y, const float register_x) noexcept {
        constexpr std::array targets{
            RegisterTarget::a, RegisterTarget::f, RegisterTarget::b,
            RegisterTarget::c, RegisterTarget::d, RegisterTarget::e,
            RegisterTarget::h, RegisterTarget::l, RegisterTarget::sp,
            RegisterTarget::pc};
        for (const auto target : targets) {
            const auto [x, y] = register_position(target, register_x);
            const auto width = register_width(target) == 2 ? 38.0F : 54.0F;
            if (mouse_x >= x - 4.0F && mouse_x <= x + width &&
                mouse_y >= y - 5.0F && mouse_y <= y + 13.0F) {
                return target;
            }
        }
        return std::nullopt;
    }

    void begin_edit(const RegisterTarget target,
                    const gameboy::CpuRegisters& registers) {
        std::uint16_t value = 0;
        switch (target) {
        case RegisterTarget::a: value = registers.a; break;
        case RegisterTarget::f: value = registers.f; break;
        case RegisterTarget::b: value = registers.b; break;
        case RegisterTarget::c: value = registers.c; break;
        case RegisterTarget::d: value = registers.d; break;
        case RegisterTarget::e: value = registers.e; break;
        case RegisterTarget::h: value = registers.h; break;
        case RegisterTarget::l: value = registers.l; break;
        case RegisterTarget::sp: value = registers.sp; break;
        case RegisterTarget::pc: value = registers.pc; break;
        }
        std::ostringstream out;
        out << std::uppercase << std::hex << std::setfill('0')
            << std::setw(static_cast<int>(register_width(target))) << value;
        editing_ = target;
        edit_value_ = out.str();
        replace_on_type_ = true;
    }

    void apply_edit(gameboy::Emulator* emulator) {
        if (!editing_ || emulator == nullptr || edit_value_.empty()) return;
        const auto value = static_cast<std::uint16_t>(
            std::stoul(edit_value_, nullptr, 16));
        auto registers = emulator->cpu().registers();
        switch (*editing_) {
        case RegisterTarget::a: registers.a = static_cast<std::uint8_t>(value); break;
        case RegisterTarget::f: registers.f = static_cast<std::uint8_t>(value); break;
        case RegisterTarget::b: registers.b = static_cast<std::uint8_t>(value); break;
        case RegisterTarget::c: registers.c = static_cast<std::uint8_t>(value); break;
        case RegisterTarget::d: registers.d = static_cast<std::uint8_t>(value); break;
        case RegisterTarget::e: registers.e = static_cast<std::uint8_t>(value); break;
        case RegisterTarget::h: registers.h = static_cast<std::uint8_t>(value); break;
        case RegisterTarget::l: registers.l = static_cast<std::uint8_t>(value); break;
        case RegisterTarget::sp: registers.sp = value; break;
        case RegisterTarget::pc: registers.pc = value; break;
        }
        emulator->set_cpu_registers(registers);
        cancel_edit();
    }

    void cancel_edit() noexcept {
        editing_.reset();
        edit_value_.clear();
        replace_on_type_ = true;
    }

    SDL_Window* window_{};
    SDL_Renderer* renderer_{};
    SDL_Texture* texture_{};
    SDL_Texture* background_texture_{};
    bool execution_paused_{true};
    bool step_instruction_{};
    bool step_frame_{};
    bool toggle_recording_{};
    bool replay_requested_{};
    bool tas_requested_{};
    bool sprite_requested_{};
    int focus_index_{6};
    DesktopBreakpoints breakpoints_;
    std::uint16_t disassembly_start_{0x0100};
    bool disassembly_follow_pc_{true};
    std::optional<RegisterTarget> editing_;
    std::string edit_value_;
    bool replace_on_type_{true};
};

} // namespace gbb::sdl

#endif
