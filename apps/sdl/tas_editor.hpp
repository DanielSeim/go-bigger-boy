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
#include <optional>
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
    void mark_saved() {
        saved_frames_ = frames_;
        status_ = "SAVED";
    }
    [[nodiscard]] const std::string& status() const noexcept { return status_; }
    void set_status(std::string status) { status_ = std::move(status); }
    [[nodiscard]] std::uint64_t fingerprint() const noexcept {
        return fingerprint_;
    }

    [[nodiscard]] bool close_with_confirmation() {
        if (!visible()) return true;
        if (has_unsaved_changes() &&
            !confirm_discard_changes(window_, "Discard unsaved TAS changes?")) {
            return false;
        }
        close();
        return true;
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
        undo_frames_.clear();
        redo_frames_.clear();
        clipboard_frame_.reset();
        status_ = "NEW TIMELINE";
    }

    void close() noexcept {
        clear_tool_text_cache(renderer_);
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
            } else if (event.key.key == SDLK_TAB) {
                focus_index_ = cycle_tool_focus(
                    focus_index_, 7, (event.key.mod & SDL_KMOD_SHIFT) != 0);
            } else if ((event.key.key == SDLK_RETURN ||
                        event.key.key == SDLK_KP_ENTER ||
                        event.key.key == SDLK_SPACE) &&
                       activate_focused()) {
            } else if (event.key.key == SDLK_UP && selection_ > 0) {
                --selection_;
                keep_selection_visible();
            } else if (event.key.key == SDLK_DOWN) {
                if (selection_ + 1 < frames_.size()) ++selection_;
                keep_selection_visible();
            } else if (event.key.key == SDLK_HOME) {
                selection_ = 0;
                keep_selection_visible();
            } else if (event.key.key == SDLK_PAGEUP) {
                int width = 0;
                int height = 0;
                static_cast<void>(SDL_GetWindowSize(window_, &width, &height));
                const auto amount = visible_rows(height);
                selection_ = selection_ > amount ? selection_ - amount : 0;
                keep_selection_visible();
            } else if (event.key.key == SDLK_PAGEDOWN) {
                int width = 0;
                int height = 0;
                static_cast<void>(SDL_GetWindowSize(window_, &width, &height));
                selection_ = std::min(
                    frames_.size() - 1, selection_ + visible_rows(height));
                keep_selection_visible();
            } else if (event.key.key == SDLK_INSERT) {
                remember_edit();
                frames_.insert(frames_.begin() +
                                   static_cast<std::ptrdiff_t>(selection_), 0);
            } else if (event.key.key == SDLK_DELETE) {
                remember_edit();
                delete_selected();
            } else if (event.key.key == SDLK_END) {
                if ((event.key.mod & SDL_KMOD_CTRL) != 0) {
                    selection_ = frames_.size() - 1;
                    keep_selection_visible();
                } else {
                    remember_edit();
                    frames_.push_back(0);
                    selection_ = frames_.size() - 1;
                }
                keep_selection_visible();
            } else if (event.key.key == SDLK_N &&
                       (event.key.mod & SDL_KMOD_CTRL) != 0) {
                request_new();
            } else if (event.key.key == SDLK_S &&
                       (event.key.mod & SDL_KMOD_CTRL) != 0) {
                save_requested_ = true;
                status_ = "SAVING...";
            } else if (event.key.key == SDLK_Z &&
                       (event.key.mod & SDL_KMOD_CTRL) != 0) {
                undo();
            } else if (event.key.key == SDLK_Y &&
                       (event.key.mod & SDL_KMOD_CTRL) != 0) {
                redo();
            } else if (event.key.key == SDLK_C &&
                       (event.key.mod & SDL_KMOD_CTRL) != 0) {
                clipboard_frame_ = frames_[selection_];
                status_ = "FRAME COPIED";
            } else if (event.key.key == SDLK_V &&
                       (event.key.mod & SDL_KMOD_CTRL) != 0) {
                if (clipboard_frame_) {
                    remember_edit();
                    frames_[selection_] = *clipboard_frame_;
                    status_ = "FRAME PASTED";
                }
            } else if (event.key.key == SDLK_D &&
                       (event.key.mod & SDL_KMOD_CTRL) != 0) {
                remember_edit();
                frames_.insert(frames_.begin() +
                                   static_cast<std::ptrdiff_t>(selection_ + 1),
                               frames_[selection_]);
                ++selection_;
                keep_selection_visible();
                status_ = "FRAME DUPLICATED";
            } else if (event.key.key == SDLK_BACKSPACE) {
                if (frames_[selection_] != 0) {
                    remember_edit();
                    frames_[selection_] = 0;
                    status_ = "FRAME CLEARED";
                }
            } else if (event.key.key == SDLK_F7) {
                replay_requested_ = true;
                status_ = "BUILDING MOVIE...";
            }
            return true;
        }
        if (event.type == SDL_EVENT_MOUSE_WHEEL) {
            const auto raw_amount = static_cast<int>(event.wheel.y);
            const auto amount = std::max(1, raw_amount < 0 ? -raw_amount
                                                            : raw_amount);
            if (event.wheel.y > 0) {
                const auto delta = std::min<std::size_t>(
                    first_visible_, static_cast<std::size_t>(amount));
                first_visible_ -= delta;
            } else if (event.wheel.y < 0) {
                first_visible_ = std::min(
                    frames_.size() - 1,
                    first_visible_ + static_cast<std::size_t>(-amount));
            }
            return true;
        }
        if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN &&
            event.button.button == SDL_BUTTON_LEFT) {
            int width = 0;
            int height = 0;
            static_cast<void>(SDL_GetWindowSize(window_, &width, &height));
            if (tool_close_button_hit(width, height, event.button.x,
                                      event.button.y)) {
                focus_index_ = 0;
                if (!has_unsaved_changes() ||
                    confirm_discard_changes(window_, "Discard unsaved TAS changes?")) {
                    close();
                }
                return true;
            }
            if (event.button.x < 144.0F &&
                select_row(event.button.y, height)) {
                return true;
            }
            if (edit_cell(event.button.x, event.button.y, height, true)) {
                dragging_ = true;
                static_cast<void>(SDL_CaptureMouse(true));
            } else if (event.button.y >= static_cast<float>(height - 55) &&
                       event.button.y <= static_cast<float>(height - 19)) {
                const auto x = event.button.x;
                if (x >= 24 && x <= 154) {
                    focus_index_ = 1;
                    remember_edit();
                    frames_.insert(frames_.begin() +
                                       static_cast<std::ptrdiff_t>(selection_), 0);
                } else if (x >= 166 && x <= 296) {
                    focus_index_ = 2;
                    remember_edit();
                    delete_selected();
                } else if (x >= 308 && x <= 438) {
                    focus_index_ = 3;
                    remember_edit();
                    frames_.push_back(0);
                    selection_ = frames_.size() - 1;
                    keep_selection_visible();
                } else if (x >= 450 && x <= 580) {
                    focus_index_ = 4;
                    save_requested_ = true;
                    status_ = "SAVING...";
                } else if (x >= 592 && x <= 722) {
                    focus_index_ = 5;
                    replay_requested_ = true;
                    status_ = "BUILDING MOVIE...";
                } else if (x >= 734 && x <= 864) {
                    focus_index_ = 6;
                    request_new();
                }
            }
            return true;
        }
        if (event.type == SDL_EVENT_MOUSE_MOTION && dragging_ &&
            (event.motion.state & SDL_BUTTON_LMASK) != 0) {
            int width = 0;
            int height = 0;
            static_cast<void>(SDL_GetWindowSize(window_, &width, &height));
            static_cast<void>(edit_cell(event.motion.x, event.motion.y, height,
                                         false));
            return true;
        }
        if (event.type == SDL_EVENT_MOUSE_BUTTON_UP &&
            event.button.button == SDL_BUTTON_LEFT) {
            dragging_ = false;
            static_cast<void>(SDL_CaptureMouse(false));
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
        render_tool_text(renderer_, 24, 18, "TAS FRAME INPUT EDITOR");
        draw_tool_close_button(renderer_, window_, width);
        const auto title = std::string("Go Bigger Boy - TAS Input Editor") +
                           (has_unsaved_changes() ? " *" : "");
        static_cast<void>(SDL_SetWindowTitle(window_, title.c_str()));
        static_cast<void>(SDL_SetRenderDrawColor(renderer_, 177, 192, 208, 255));
        render_tool_text(
            renderer_, 24, 38,
            (std::string(has_unsaved_changes() ? "UNSAVED  " : "SAVED  ") +
             status_).c_str());
        render_tool_text(
            renderer_, 24, 54,
            "CLICK OR DRAG CELLS  |  ARROWS/PAGE SELECT  |  CTRL+Z/Y UNDO/REDO");

        constexpr std::array<const char*, 8> names{
            "RIGHT", "LEFT", "UP", "DOWN", "A", "B", "SELECT", "START"};
        static_cast<void>(SDL_SetRenderDrawColor(renderer_, 230, 249, 255, 255));
        const auto frame_summary = "FRAME " + std::to_string(selection_) +
                                   " / " + std::to_string(frames_.size());
        render_tool_text(renderer_, 28, 76, frame_summary.c_str());
        constexpr float first_button_x = 144.0F;
        constexpr float column_width = 82.0F;
        for (std::size_t button = 0; button < names.size(); ++button) {
            render_tool_text(renderer_, first_button_x + button * column_width + 8,
                             76, names[button]);
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
            const SDL_FRect background{
                24, y - 3, static_cast<float>(std::max(840, width - 48)),
                row_height - 2};
            static_cast<void>(SDL_SetRenderDrawColor(
                renderer_, frame == selection_ ? 20 : 16,
                frame == selection_ ? 77 : 27,
                frame == selection_ ? 101 : 39, 255));
            static_cast<void>(SDL_RenderFillRect(renderer_, &background));
            static_cast<void>(SDL_SetRenderDrawColor(renderer_, 230, 249, 255, 255));
            const auto frame_text = std::to_string(frame);
            render_tool_text(renderer_, 30, y + 3, frame_text.c_str());
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
                    render_tool_text(renderer_, cell.x + 30, y + 3, "X");
                }
            }
        }
        const auto button = [this](const SDL_FRect& rect, const char* label) {
            draw_tool_button_background(renderer_, window_, rect);
            render_tool_text(renderer_, rect.x + 10, rect.y + 14, label,
                             rect.w - 20.0F);
        };
        const auto bottom_y = static_cast<float>(height - 55);
        button({24, bottom_y, 130, 36}, "INSERT FRAME");
        button({166, bottom_y, 130, 36}, "DELETE FRAME");
        button({308, bottom_y, 130, 36}, "APPEND FRAME");
        button({450, bottom_y, 130, 36}, "SAVE MOVIE");
        button({592, bottom_y, 130, 36}, "RUN MOVIE");
        button({734, bottom_y, 130, 36}, "NEW FROM NOW");
        render_tool_text(
            renderer_, 24, bottom_y - 16,
            "Ctrl+C/V COPY/PASTE  Ctrl+D DUPLICATE  Backspace CLEAR  Ctrl+Home/End JUMP");
        draw_tool_focus_outline(renderer_, focused_rect(width, height));
        static_cast<void>(SDL_RenderPresent(renderer_));
    }

  private:
    [[noreturn]] static void throw_sdl_error(const char* action) {
        throw std::runtime_error(std::string(action) + ": " + SDL_GetError());
    }

    [[nodiscard]] static int visible_rows(const int height) noexcept {
        return std::max(1, static_cast<int>((height - 185.0F) / 22.0F));
    }

    void request_new() {
        if (!has_unsaved_changes() ||
            confirm_discard_changes(window_, "Discard unsaved TAS changes?")) {
            new_requested_ = true;
        }
    }

    [[nodiscard]] SDL_FRect focused_rect(const int width,
                                          const int height) const noexcept {
        if (focus_index_ == 0) return tool_close_button_rect(width);
        const auto bottom_y = static_cast<float>(height - 55);
        return {24.0F + static_cast<float>(focus_index_ - 1) * 142.0F,
                bottom_y, 130.0F, 36.0F};
    }

    bool activate_focused() {
        switch (focus_index_) {
        case 0:
            if (!has_unsaved_changes() ||
                confirm_discard_changes(window_, "Discard unsaved TAS changes?")) {
                close();
            }
            return true;
        case 1:
            remember_edit();
            frames_.insert(frames_.begin() +
                               static_cast<std::ptrdiff_t>(selection_), 0);
            return true;
        case 2:
            remember_edit();
            delete_selected();
            return true;
        case 3:
            remember_edit();
            frames_.push_back(0);
            selection_ = frames_.size() - 1;
            keep_selection_visible();
            return true;
        case 4:
            save_requested_ = true;
            status_ = "SAVING...";
            return true;
        case 5:
            replay_requested_ = true;
            status_ = "BUILDING MOVIE...";
            return true;
        case 6:
            request_new();
            return true;
        default: return false;
        }
    }

    void remember_edit() {
        undo_frames_.push_back(frames_);
        constexpr std::size_t maximum_history = 100;
        if (undo_frames_.size() > maximum_history) undo_frames_.erase(undo_frames_.begin());
        redo_frames_.clear();
    }

    void undo() {
        if (undo_frames_.empty()) return;
        redo_frames_.push_back(frames_);
        frames_ = std::move(undo_frames_.back());
        undo_frames_.pop_back();
        if (selection_ >= frames_.size()) selection_ = frames_.size() - 1;
        keep_selection_visible();
        status_ = "UNDO";
    }

    void redo() {
        if (redo_frames_.empty()) return;
        undo_frames_.push_back(frames_);
        frames_ = std::move(redo_frames_.back());
        redo_frames_.pop_back();
        if (selection_ >= frames_.size()) selection_ = frames_.size() - 1;
        keep_selection_visible();
        status_ = "REDO";
    }

    [[nodiscard]] bool edit_cell(const float x, const float y, const int height,
                                 const bool begin_drag) {
        constexpr float first_row_y = 94.0F;
        constexpr float row_height = 22.0F;
        constexpr float first_button_x = 144.0F;
        constexpr float column_width = 82.0F;
        const auto bottom_y = static_cast<float>(height - 55);
        if (y < first_row_y || y >= bottom_y - 18 || x < first_button_x) {
            return false;
        }
        const auto row = static_cast<std::size_t>((y - first_row_y) / row_height);
        const auto frame = first_visible_ + row;
        const auto button = static_cast<std::size_t>((x - first_button_x) /
                                                     column_width);
        if (frame >= frames_.size() || button >= InputMovie::movie_buttons.size()) {
            return false;
        }
        selection_ = frame;
        const auto bit = static_cast<std::uint8_t>(1U << button);
        if (begin_drag) {
            remember_edit();
            drag_value_ = (frames_[frame] & bit) == 0;
        }
        if (((frames_[frame] & bit) != 0) != drag_value_) {
            frames_[frame] = drag_value_
                                 ? static_cast<std::uint8_t>(frames_[frame] | bit)
                                 : static_cast<std::uint8_t>(frames_[frame] & ~bit);
        }
        return true;
    }

    [[nodiscard]] bool select_row(const float y, const int height) noexcept {
        constexpr float first_row_y = 94.0F;
        constexpr float row_height = 22.0F;
        const auto bottom_y = static_cast<float>(height - 55);
        if (y < first_row_y || y >= bottom_y - 18) return false;
        const auto row = static_cast<std::size_t>((y - first_row_y) / row_height);
        const auto frame = first_visible_ + row;
        if (frame >= frames_.size()) return false;
        selection_ = frame;
        return true;
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
    std::vector<std::vector<std::uint8_t>> undo_frames_;
    std::vector<std::vector<std::uint8_t>> redo_frames_;
    std::optional<std::uint8_t> clipboard_frame_;
    std::string status_{"READY"};
    bool dragging_{};
    bool drag_value_{};
    bool save_requested_{};
    bool replay_requested_{};
    bool new_requested_{};
    int focus_index_{1};
};

} // namespace gbb::sdl

#endif
