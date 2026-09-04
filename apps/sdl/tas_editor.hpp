#pragma once

#ifndef __ANDROID__

#include "gameboy/emulator.hpp"
#include "input_movie.hpp"
#include "tool_window_support.hpp"
#include "window_event.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace gbb::sdl {

class TasEditor {
  public:
    ~TasEditor() { close(); }
    TasEditor() = default;
    TasEditor(const TasEditor&) = delete;
    TasEditor& operator=(const TasEditor&) = delete;

    [[nodiscard]] bool visible() const noexcept { return window_ != nullptr; }
    [[nodiscard]] bool take_save_request() noexcept {
        return std::exchange(save_requested_, false);
    }
    [[nodiscard]] bool take_replay_request() noexcept {
        return std::exchange(replay_requested_, false);
    }
    [[nodiscard]] bool take_new_request() noexcept {
        return std::exchange(new_requested_, false);
    }
    [[nodiscard]] const std::vector<std::uint8_t>& frames() const noexcept {
        return frames_;
    }
    [[nodiscard]] const std::vector<std::uint8_t>& start_state() const noexcept {
        return start_state_;
    }
    [[nodiscard]] bool has_unsaved_changes() const noexcept {
        return frames_ != saved_frames_;
    }
    void mark_saved() { saved_frames_ = frames_; }
    [[nodiscard]] std::uint64_t fingerprint() const noexcept {
        return fingerprint_;
    }

    void open(SDL_Window* parent, gameboy::Emulator& emulator) {
        if (!visible()) {
            window_ = SDL_CreateWindow("Go Bigger Boy - TAS Input Editor",
                                       920, 700, SDL_WINDOW_RESIZABLE);
            if (window_ == nullptr) {
                throw_sdl_error("Could not create TAS editor window");
            }
            static_cast<void>(SDL_SetWindowMinimumSize(window_, 900, 520));
            renderer_ = SDL_CreateRenderer(window_, nullptr);
            if (renderer_ == nullptr) {
                close();
                throw_sdl_error("Could not create TAS editor renderer");
            }
            if (parent != nullptr) {
                int x = 0;
                int y = 0;
                static_cast<void>(SDL_GetWindowPosition(parent, &x, &y));
                static_cast<void>(SDL_SetWindowPosition(window_, x + 80, y + 80));
            }
        }
        if (start_state_.empty() || fingerprint_ != emulator.rom_fingerprint()) {
            reset_from(emulator);
        }
        SDL_RaiseWindow(window_);
    }

    void reset_from(gameboy::Emulator& emulator) {
        for (const auto button : InputMovie::movie_buttons) {
            emulator.set_button(button, false);
        }
        start_state_ = emulator.save_state();
        fingerprint_ = emulator.rom_fingerprint();
        frames_.assign(1, 0);
        saved_frames_ = frames_;
        selection_ = 0;
        first_visible_ = 0;
    }

    void close() noexcept {
        if (renderer_ != nullptr) SDL_DestroyRenderer(renderer_);
        if (window_ != nullptr) SDL_DestroyWindow(window_);
        renderer_ = nullptr;
        window_ = nullptr;
        save_requested_ = false;
        replay_requested_ = false;
        new_requested_ = false;
    }

    bool handle_event(const SDL_Event& event) {
        if (!visible()) return false;
        const auto id = SDL_GetWindowID(window_);
        if (event_window_id(event) != id) return false;
        if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
            if (!has_unsaved_changes() ||
                confirm_discard_changes(window_, "Discard unsaved TAS changes?")) {
                close();
            }
            return true;
        }
        if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat) {
            if (event.key.key == SDLK_ESCAPE) {
                if (!has_unsaved_changes() ||
                    confirm_discard_changes(window_, "Discard unsaved TAS changes?")) {
                    close();
                }
            } else if (event.key.key == SDLK_UP && selection_ > 0) {
                --selection_;
                keep_selection_visible();
            } else if (event.key.key == SDLK_DOWN) {
                if (selection_ + 1 < frames_.size()) ++selection_;
                keep_selection_visible();
            } else if (event.key.key == SDLK_INSERT) {
                frames_.insert(frames_.begin() +
                                   static_cast<std::ptrdiff_t>(selection_), 0);
            } else if (event.key.key == SDLK_DELETE) {
                delete_selected();
            } else if (event.key.key == SDLK_END) {
                frames_.push_back(0);
                selection_ = frames_.size() - 1;
                keep_selection_visible();
            } else if (event.key.key == SDLK_N &&
                       (event.key.mod & SDL_KMOD_CTRL) != 0) {
                if (!has_unsaved_changes() ||
                    confirm_discard_changes(window_, "Discard unsaved TAS changes?")) {
                    new_requested_ = true;
                }
            } else if (event.key.key == SDLK_S &&
                       (event.key.mod & SDL_KMOD_CTRL) != 0) {
                save_requested_ = true;
            } else if (event.key.key == SDLK_F7) {
                replay_requested_ = true;
            }
            return true;
        }
        if (event.type == SDL_EVENT_MOUSE_WHEEL) {
            if (event.wheel.y > 0 && first_visible_ > 0) --first_visible_;
            if (event.wheel.y < 0 && first_visible_ + 1 < frames_.size()) {
                ++first_visible_;
            }
            return true;
        }
        if (event.type == SDL_EVENT_MOUSE_BUTTON_UP &&
            event.button.button == SDL_BUTTON_LEFT) {
            int width = 0;
            int height = 0;
            static_cast<void>(SDL_GetWindowSize(window_, &width, &height));
            constexpr float first_row_y = 94.0F;
            constexpr float row_height = 22.0F;
            const auto bottom_y = static_cast<float>(height - 55);
            if (event.button.y >= first_row_y && event.button.y < bottom_y - 18) {
                const auto row = static_cast<std::size_t>(
                    (event.button.y - first_row_y) / row_height);
                const auto frame = first_visible_ + row;
                if (frame < frames_.size()) {
                    selection_ = frame;
                    constexpr float first_button_x = 144.0F;
                    constexpr float column_width = 82.0F;
                    if (event.button.x >= first_button_x) {
                        const auto button = static_cast<std::size_t>(
                            (event.button.x - first_button_x) / column_width);
                        if (button < InputMovie::movie_buttons.size()) {
                            frames_[frame] ^=
                                static_cast<std::uint8_t>(1U << button);
                        }
                    }
                }
            } else if (event.button.y >= bottom_y &&
                       event.button.y <= bottom_y + 36.0F) {
                const auto x = event.button.x;
                if (x >= 24 && x <= 154) {
                    frames_.insert(frames_.begin() +
                                       static_cast<std::ptrdiff_t>(selection_), 0);
                } else if (x >= 166 && x <= 296) {
                    delete_selected();
                } else if (x >= 308 && x <= 438) {
                    frames_.push_back(0);
                    selection_ = frames_.size() - 1;
                    keep_selection_visible();
                } else if (x >= 450 && x <= 580) {
                    save_requested_ = true;
                } else if (x >= 592 && x <= 722) {
                    replay_requested_ = true;
                } else if (x >= 734 && x <= 864) {
                    new_requested_ = true;
                }
            }
            return true;
        }
        return true;
    }

    void present() {
        if (!visible()) return;
        int width = 0;
        int height = 0;
        static_cast<void>(SDL_GetWindowSize(window_, &width, &height));
        static_cast<void>(SDL_SetRenderDrawColor(renderer_, 8, 12, 20, 255));
        static_cast<void>(SDL_RenderClear(renderer_));
        static_cast<void>(SDL_SetRenderDrawColor(renderer_, 69, 207, 238, 255));
        static_cast<void>(SDL_RenderDebugText(renderer_, 24, 18,
                                              "TAS FRAME INPUT EDITOR"));
        static_cast<void>(SDL_SetRenderDrawColor(renderer_, 177, 192, 208, 255));
        static_cast<void>(SDL_RenderDebugText(
            renderer_, 24, 38,
            "CLICK CELLS TO HOLD BUTTONS FOR A FRAME  |  UP/DOWN SELECT"));
        static_cast<void>(SDL_RenderDebugText(
            renderer_, 24, 54,
            "INSERT ADD BEFORE  DELETE REMOVE  END APPEND  CTRL+S SAVE  F7 RUN"));

        constexpr std::array<const char*, 8> names{
            "RIGHT", "LEFT", "UP", "DOWN", "A", "B", "SELECT", "START"};
        static_cast<void>(SDL_SetRenderDrawColor(renderer_, 230, 249, 255, 255));
        static_cast<void>(SDL_RenderDebugText(renderer_, 28, 76, "FRAME"));
        constexpr float first_button_x = 144.0F;
        constexpr float column_width = 82.0F;
        for (std::size_t button = 0; button < names.size(); ++button) {
            static_cast<void>(SDL_RenderDebugText(
                renderer_, first_button_x + button * column_width + 8, 76,
                names[button]));
        }
        constexpr float first_row_y = 94.0F;
        constexpr float row_height = 22.0F;
        const auto visible_rows = std::max(
            1, static_cast<int>((height - 185.0F) / row_height));
        if (selection_ < first_visible_) first_visible_ = selection_;
        if (selection_ >= first_visible_ + static_cast<std::size_t>(visible_rows)) {
            first_visible_ = selection_ - static_cast<std::size_t>(visible_rows) + 1;
        }
        for (int row = 0; row < visible_rows; ++row) {
            const auto frame = first_visible_ + static_cast<std::size_t>(row);
            if (frame >= frames_.size()) break;
            const auto y = first_row_y + row * row_height;
            const SDL_FRect background{24, y - 3, 840, row_height - 2};
            static_cast<void>(SDL_SetRenderDrawColor(
                renderer_, frame == selection_ ? 20 : 16,
                frame == selection_ ? 77 : 27,
                frame == selection_ ? 101 : 39, 255));
            static_cast<void>(SDL_RenderFillRect(renderer_, &background));
            static_cast<void>(SDL_SetRenderDrawColor(renderer_, 230, 249, 255, 255));
            const auto frame_text = std::to_string(frame);
            static_cast<void>(SDL_RenderDebugText(renderer_, 30, y + 3,
                                                  frame_text.c_str()));
            for (std::size_t button = 0; button < names.size(); ++button) {
                const auto active = (frames_[frame] & (1U << button)) != 0;
                const SDL_FRect cell{first_button_x + button * column_width,
                                     y - 2, column_width - 6, row_height - 4};
                static_cast<void>(SDL_SetRenderDrawColor(
                    renderer_, active ? 69 : 34, active ? 207 : 54,
                    active ? 238 : 70, 255));
                if (active) static_cast<void>(SDL_RenderFillRect(renderer_, &cell));
                static_cast<void>(SDL_RenderRect(renderer_, &cell));
                if (active) {
                    static_cast<void>(SDL_SetRenderDrawColor(renderer_, 8, 12, 20, 255));
                    static_cast<void>(SDL_RenderDebugText(renderer_, cell.x + 30,
                                                          y + 3, "X"));
                }
            }
        }
        const auto button = [this](const SDL_FRect& rect, const char* label) {
            draw_tool_button_background(renderer_, window_, rect);
            static_cast<void>(SDL_RenderDebugText(renderer_, rect.x + 10,
                                                  rect.y + 14, label));
        };
        const auto bottom_y = static_cast<float>(height - 55);
        button({24, bottom_y, 130, 36}, "INSERT FRAME");
        button({166, bottom_y, 130, 36}, "DELETE FRAME");
        button({308, bottom_y, 130, 36}, "APPEND FRAME");
        button({450, bottom_y, 130, 36}, "SAVE MOVIE");
        button({592, bottom_y, 130, 36}, "RUN MOVIE");
        button({734, bottom_y, 130, 36}, "NEW FROM NOW");
        static_cast<void>(SDL_RenderPresent(renderer_));
    }

  private:
    [[noreturn]] static void throw_sdl_error(const char* action) {
        throw std::runtime_error(std::string(action) + ": " + SDL_GetError());
    }

    void delete_selected() {
        if (frames_.size() > 1) {
            frames_.erase(frames_.begin() +
                          static_cast<std::ptrdiff_t>(selection_));
            if (selection_ >= frames_.size()) selection_ = frames_.size() - 1;
            keep_selection_visible();
        } else {
            frames_[0] = 0;
        }
    }

    void keep_selection_visible() noexcept {
        if (selection_ < first_visible_) first_visible_ = selection_;
    }

    SDL_Window* window_{};
    SDL_Renderer* renderer_{};
    std::uint64_t fingerprint_{};
    std::vector<std::uint8_t> start_state_;
    std::vector<std::uint8_t> frames_{1, 0};
    std::vector<std::uint8_t> saved_frames_{1, 0};
    std::size_t selection_{};
    std::size_t first_visible_{};
    bool save_requested_{};
    bool replay_requested_{};
    bool new_requested_{};
};

} // namespace gbb::sdl

#endif
