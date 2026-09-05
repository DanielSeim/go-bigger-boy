#pragma once

#ifdef _WIN32

#include "gameboy/video_pipeline.hpp"
#include "gbb/core.hpp"

#include <SDL3/SDL.h>

#include <cstddef>
#include <memory>

enum class DesktopMenuCommand : int {
    none = 0,
    open_rom = 0x8100,
    library,
    save_state,
    load_state,
    exit_app,
    pause,
    reset,
    link_session,
    link_retry,
    remote_host,
    remote_join,
    remote_discover,
    remote_stop,
    fullscreen,
    controls,
    gameshark,
    debugger,
    record_input,
    replay_input,
    tas_editor,
    sprite_editor,
    shortcuts,
    about,
    palette_first = 0x8200,
    video_first = 0x8300,
};

class DesktopMenuBar {
public:
    DesktopMenuBar();
    ~DesktopMenuBar();

    DesktopMenuBar(const DesktopMenuBar&) = delete;
    DesktopMenuBar& operator=(const DesktopMenuBar&) = delete;

    void attach(SDL_Window* window);
    void detach() noexcept;
    [[nodiscard]] DesktopMenuCommand take_command() noexcept;
    void update(bool has_rom, gbb::CoreCapability capabilities, bool paused,
                bool fullscreen, bool recording,
                std::size_t palette, gameboy::VideoMode video,
                bool link_active, bool remote_link_active);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

#endif // _WIN32
