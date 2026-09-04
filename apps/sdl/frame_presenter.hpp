#pragma once

#include "gameboy/display_palette.hpp"
#include "gameboy/emulator.hpp"
#include "gameboy/link_session.hpp"
#include "gameboy/tcp_link_channel.hpp"
#include "gameboy/video_pipeline.hpp"
#include "gbb/video.hpp"
#include "gbb/core.hpp"
#include "remote_link_session.hpp"

#include <SDL3/SDL.h>

#include <cstdint>
#include <vector>

namespace gbb::sdl {

// Presentation-only state for linked framebuffers and status overlays. This
// keeps transport counters and video conversion out of the SDL lifecycle type.
struct FrameRenderContext {
    SDL_Renderer* renderer{};
    SDL_Texture* texture{};
    SDL_Texture* link_texture{};
    gameboy::VideoMode video_mode{};
};

[[nodiscard]] std::vector<std::uint32_t> colorize_frame(
    const gbb::EmulatorCore& core, FrameRenderContext& context,
    const gameboy::DisplayPalette& palette);

[[nodiscard]] std::vector<std::uint32_t> colorize_frame(
    const gameboy::Emulator& emulator, FrameRenderContext& context,
    const gameboy::DisplayPalette& palette);

[[nodiscard]] bool present_link_frames(const gameboy::Emulator& first,
                                        const gameboy::Emulator& second,
                                        FrameRenderContext& context,
                                        const gameboy::DisplayPalette& palette);

[[nodiscard]] bool present_link_status(FrameRenderContext& context,
                                        const gameboy::LinkSession& session);

[[nodiscard]] bool present_remote_link_status(
    FrameRenderContext& context, const RemoteLinkSession& remote);

} // namespace gbb::sdl
