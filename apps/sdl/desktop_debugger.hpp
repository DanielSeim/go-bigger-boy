#pragma once

#ifndef __ANDROID__

#include "gameboy/display_palette.hpp"
#include "gameboy/emulator.hpp"
#include "gameboy/ppu.hpp"
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
    void request_record_toggle() noexcept { toggle_recording_ = true; }
    void request_replay() noexcept { replay_requested_ = true; }
    void request_tas_editor() noexcept { tas_requested_ = true; }
    void request_sprite_editor() noexcept { sprite_requested_ = true; }
    void run() noexcept { execution_paused_ = false; }
    void pause() noexcept { execution_paused_ = true; }

    void toggle(SDL_Window* parent) {
        if (visible()) {
            close();
            return;
        }
        window_ = SDL_CreateWindow("Go Bigger Boy - Debugger", 920, 760,
                                   SDL_WINDOW_RESIZABLE);
        if (window_ == nullptr) throw_sdl_error("Could not create debugger window");
        static_cast<void>(SDL_SetWindowMinimumSize(window_, 920, 720));
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
        if (renderer_ != nullptr) SDL_DestroyRenderer(renderer_);
        if (window_ != nullptr) SDL_DestroyWindow(window_);
        texture_ = nullptr;
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
            } else if (event.key.key == SDLK_F12 || event.key.key == SDLK_ESCAPE) {
                close();
            } else if (event.key.key == SDLK_F5 || event.key.key == SDLK_SPACE) {
                execution_paused_ = !execution_paused_;
            } else if (event.key.key == SDLK_F10) {
                execution_paused_ = true;
                step_instruction_ = true;
            } else if (event.key.key == SDLK_F11) {
                execution_paused_ = true;
                step_frame_ = true;
            } else if (event.key.key == SDLK_F6) {
                toggle_recording_ = true;
            } else if (event.key.key == SDLK_F7) {
                replay_requested_ = true;
            } else if (event.key.key == SDLK_F8) {
                tas_requested_ = true;
            } else if (event.key.key == SDLK_F9) {
                sprite_requested_ = true;
            }
        } else if (event.type == SDL_EVENT_MOUSE_BUTTON_UP &&
                   event.button.button == SDL_BUTTON_LEFT) {
            int width = 0;
            int height = 0;
            static_cast<void>(SDL_GetWindowSize(window_, &width, &height));
            const auto y = static_cast<float>(height - 58);
            const auto movie_y = static_cast<float>(height - 106);
            const auto x = event.button.x;
            const auto register_x =
                std::max(530.0F, static_cast<float>(width) - 350.0F);
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
                    toggle_recording_ = true;
                } else if (x >= 208.0F && x <= 378.0F) {
                    replay_requested_ = true;
                } else if (x >= 734.0F && x <= 884.0F) {
                    tas_requested_ = true;
                }
            } else if (event.button.y >= y && event.button.y <= y + 36.0F) {
                if (x >= 24.0F && x <= 174.0F) {
                    execution_paused_ = !execution_paused_;
                } else if (x >= 188.0F && x <= 358.0F) {
                    execution_paused_ = true;
                    step_instruction_ = true;
                } else if (x >= 372.0F && x <= 522.0F) {
                    execution_paused_ = true;
                    step_frame_ = true;
                } else if (x >= 536.0F && x <= 686.0F) {
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
        static_cast<void>(SDL_RenderDebugText(renderer_, 24, 20,
                                              "GO BIGGER BOY / DEBUGGER"));
        static_cast<void>(SDL_SetRenderDrawColor(renderer_, 177, 192, 208, 255));
        static_cast<void>(SDL_RenderDebugText(
            renderer_, 24, 38, execution_paused_ ? "PAUSED" : "RUNNING"));

        constexpr float scale = 3.0F;
        constexpr float preview_x = 24.0F;
        constexpr float preview_y = 72.0F;
        constexpr float preview_width = gameboy::Ppu::screen_width * scale;
        constexpr float preview_height = gameboy::Ppu::screen_height * scale;
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
        const SDL_FRect preview{preview_x, preview_y, preview_width,
                                preview_height};
        static_cast<void>(SDL_RenderTexture(renderer_, texture_, nullptr, &preview));
        static_cast<void>(SDL_SetRenderDrawColor(renderer_, 69, 207, 238, 255));
        static_cast<void>(SDL_RenderRect(renderer_, &preview));
        const SDL_FRect outer{preview_x - 3, preview_y - 3,
                              preview_width + 6, preview_height + 6};
        static_cast<void>(SDL_RenderRect(renderer_, &outer));
        static_cast<void>(SDL_RenderDebugText(renderer_, preview_x,
                                              preview_y + preview_height + 10,
                                              "VISIBLE VIEWPORT 160 x 144"));

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
        const auto text = [this](const float x, const float y,
                                 const std::string& value) {
            static_cast<void>(SDL_SetRenderDrawColor(renderer_, 230, 249, 255, 255));
            static_cast<void>(SDL_RenderDebugText(renderer_, x, y, value.c_str()));
        };
        const auto register_x = std::max(530.0F, static_cast<float>(width) - 350.0F);
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

        const auto button = [this](const SDL_FRect& rect,
                                   const std::string& label) {
            draw_tool_button_background(renderer_, window_, rect);
            static_cast<void>(SDL_RenderDebugText(renderer_, rect.x + 12,
                                                  rect.y + 14, label.c_str()));
        };
        const auto button_y = static_cast<float>(height - 58);
        const auto movie_y = static_cast<float>(height - 106);
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
        button({188, button_y, 170, 36}, "F10 STEP INSTRUCTION");
        button({372, button_y, 150, 36}, "F11 STEP FRAME");
        button({536, button_y, 150, 36}, "F9 SPRITE EDITOR");
        text(static_cast<float>(width) - 170.0F, button_y + 14, "F12 CLOSE");
        static_cast<void>(SDL_RenderPresent(renderer_));
    }

private:
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
    bool execution_paused_{true};
    bool step_instruction_{};
    bool step_frame_{};
    bool toggle_recording_{};
    bool replay_requested_{};
    bool tas_requested_{};
    bool sprite_requested_{};
    std::optional<RegisterTarget> editing_;
    std::string edit_value_;
    bool replace_on_type_{true};
};

} // namespace gbb::sdl

#endif
