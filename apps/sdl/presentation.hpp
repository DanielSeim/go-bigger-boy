#pragma once

#include "frame_presenter.hpp"
#include "remote_link_session.hpp"
#include "sdl_resources.hpp"
#include "voxel_renderer.hpp"

#include "gameboy/display_palette.hpp"
#include "gameboy/emulator.hpp"
#include "gameboy/link_session.hpp"
#include "gbb/core.hpp"

#include <SDL3/SDL.h>

#include <functional>

namespace gbb::sdl {

// Presentation owns the stable ordering of framebuffer, link status, and
// input overlays. Platform-specific dashboard/touch/menu drawing is supplied
// as callbacks so this module does not depend on a particular frontend UI.
struct PresentationContext final {
    const gbb::EmulatorCore* core{};
    const gameboy::Emulator* emulator{};
    const gameboy::Emulator* link_emulator{};
    SdlResources& sdl;
    const gameboy::LinkSession* link_session{};
    const RemoteLinkSession* remote_link{};
    const gameboy::DisplayPalette& palette;
    bool dashboard_visible{};
    std::function<void()> dashboard_overlay;
    std::function<void()> touch_overlay;
    std::function<void()> menu_overlay;
};

void present_frame(const PresentationContext& context);

} // namespace gbb::sdl
