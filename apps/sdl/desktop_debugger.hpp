#pragma once

#ifndef __ANDROID__

#include <SDL3/SDL.h>

#include "gameboy/display_palette.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace gameboy {
class Emulator;
}

namespace gbb::sdl {

class InputMovie;
class DesktopDebuggerImplementation;

// Public debugger boundary. The SDL-heavy implementation lives in
// desktop_debugger.cpp, keeping debugger internals out of the event loop's
// transitive include graph.
class DesktopDebugger final {
public:
    DesktopDebugger();
    ~DesktopDebugger();
    DesktopDebugger(const DesktopDebugger&) = delete;
    DesktopDebugger& operator=(const DesktopDebugger&) = delete;

    [[nodiscard]] bool visible() const noexcept;
    [[nodiscard]] bool execution_paused() const noexcept;
    [[nodiscard]] bool take_instruction_step() noexcept;
    [[nodiscard]] bool take_frame_step() noexcept;
    [[nodiscard]] bool take_record_toggle() noexcept;
    [[nodiscard]] bool take_replay_request() noexcept;
    [[nodiscard]] bool take_tas_request() noexcept;
    [[nodiscard]] bool take_sprite_request() noexcept;
    [[nodiscard]] bool take_video_viewer_request() noexcept;
    [[nodiscard]] bool has_breakpoints() const noexcept;
    [[nodiscard]] bool breakpoint_at(std::uint16_t address) const noexcept;
    [[nodiscard]] std::size_t breakpoint_count() const noexcept;
    [[nodiscard]] const std::vector<std::uint16_t>& breakpoints() const noexcept;
    [[nodiscard]] std::optional<std::uint16_t> breakpoint_hit() const noexcept;
    [[nodiscard]] bool toggle_breakpoint(std::uint16_t address);
    void clear_breakpoints() noexcept;
    [[nodiscard]] bool check_breakpoint(std::uint16_t address) noexcept;
    void request_record_toggle() noexcept;
    void request_replay() noexcept;
    void request_tas_editor() noexcept;
    void request_sprite_editor() noexcept;
    void request_video_viewers() noexcept;
    void run() noexcept;
    void pause() noexcept;
    void toggle(SDL_Window* parent);
    void close() noexcept;
    bool handle_event(const SDL_Event& event, gameboy::Emulator* emulator);
    void present(const gameboy::Emulator& emulator,
                 const gameboy::DisplayPalette& palette,
                 const InputMovie& movie);

private:
    std::unique_ptr<DesktopDebuggerImplementation> implementation_;
};

} // namespace gbb::sdl

#endif
