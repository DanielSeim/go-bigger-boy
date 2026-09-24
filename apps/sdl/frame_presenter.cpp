#include "frame_presenter.hpp"
#include "tool_window_support.hpp"

#include <algorithm>
#include <cstddef>
#include <sstream>

namespace gbb::sdl {

void colorize_frame(
    const gbb::EmulatorCore& core, FrameRenderContext& context,
    const gameboy::DisplayPalette& palette,
    std::vector<std::uint32_t>& destination) {
    const auto frame = core.video_frame();
    const auto native_colors =
        core.video_frame_native_colors() ||
        core.descriptor().system == gbb::SystemId::game_boy_color ||
        palette.cgb_compatibility;
    gbb::transform_video_frame(
        frame.pixels, frame.pixel_count, frame.width, frame.height, palette,
        native_colors, context.video_mode, destination);
}

void colorize_frame(
    const gameboy::Emulator& emulator, FrameRenderContext& context,
    const gameboy::DisplayPalette& palette,
    std::vector<std::uint32_t>& destination) {
    const auto& pixels = emulator.framebuffer();
    const auto native_colors =
        emulator.bus().cgb_mode() || palette.cgb_compatibility;
    gbb::transform_video_frame(
        pixels.data(), pixels.size(), gameboy::Ppu::screen_width,
        gameboy::Ppu::screen_height, palette, native_colors, context.video_mode,
        destination);
}

bool present_link_frames(const gameboy::Emulator& first,
                         const gameboy::Emulator& second,
                         FrameRenderContext& context,
                         const gameboy::DisplayPalette& palette,
                         std::vector<std::uint32_t>& color_buffer) {
    // A local cable session deliberately uses two native 160x144 views. Voxel
    // geometry is a single-camera presentation and is therefore bypassed for
    // the split view; users can switch back to the diorama after disconnecting.
    constexpr auto pitch = static_cast<int>(gameboy::Ppu::screen_width *
                                             sizeof(std::uint32_t));
    const SDL_FRect left{0, 0, static_cast<float>(gameboy::Ppu::screen_width),
                         static_cast<float>(gameboy::Ppu::screen_height)};
    const SDL_FRect right{static_cast<float>(gameboy::Ppu::screen_width), 0,
                          static_cast<float>(gameboy::Ppu::screen_width),
                          static_cast<float>(gameboy::Ppu::screen_height)};
    colorize_frame(first, context, palette, color_buffer);
    if (!SDL_UpdateTexture(context.texture, nullptr, color_buffer.data(), pitch) ||
        !SDL_RenderTexture(context.renderer, context.texture, nullptr, &left)) {
        return false;
    }
    colorize_frame(second, context, palette, color_buffer);
    if (!SDL_UpdateTexture(context.link_texture, nullptr, color_buffer.data(),
                           pitch) ||
        !SDL_RenderTexture(context.renderer, context.link_texture, nullptr,
                           &right)) {
        return false;
    }
    return true;
}

const char* link_state_label(const gameboy::LinkSession::State state) noexcept {
    switch (state) {
    case gameboy::LinkSession::State::disconnected: return "DISCONNECTED";
    case gameboy::LinkSession::State::starting: return "STARTING";
    case gameboy::LinkSession::State::connected: return "CONNECTED";
    case gameboy::LinkSession::State::transferring: return "TRANSFERRING";
    case gameboy::LinkSession::State::timed_out: return "TIMED OUT";
    }
    return "UNKNOWN";
}

bool present_link_status(FrameRenderContext& context,
                         const gameboy::LinkSession& session) {
    // The split presentation has a 320x144 logical canvas. Keep the
    // indicator deliberately small and translucent so it confirms that both
    // consoles are attached without obscuring the Cable Club UI.
    static_cast<void>(SDL_SetRenderDrawBlendMode(context.renderer,
                                                 SDL_BLENDMODE_BLEND));
    static_cast<void>(SDL_SetRenderDrawColor(context.renderer, 0, 0, 0, 170));
    const SDL_FRect bar{0, 0, 320, 11};
    static_cast<void>(SDL_RenderFillRect(context.renderer, &bar));
    static_cast<void>(SDL_SetRenderDrawColor(context.renderer, 235, 245, 235, 255));
    std::ostringstream text;
    text << "LINK " << link_state_label(session.state())
         << "  XFER " << session.transfers_completed();
#ifndef __ANDROID__
    render_tool_text(context.renderer, 3, 2, text.str().c_str(), 314.0F,
                     0.57F);
#else
    static_cast<void>(SDL_RenderDebugText(context.renderer, 3, 2,
                                          text.str().c_str()));
#endif
    static_cast<void>(SDL_SetRenderDrawBlendMode(context.renderer,
                                                 SDL_BLENDMODE_NONE));
    return true;
}

const char* remote_link_state_label(
    const gameboy::LinkPacketChannel::State state) noexcept {
    switch (state) {
    case gameboy::LinkPacketChannel::State::disconnected: return "DISCONNECTED";
    case gameboy::LinkPacketChannel::State::listening: return "LISTENING";
    case gameboy::LinkPacketChannel::State::connecting: return "CONNECTING";
    case gameboy::LinkPacketChannel::State::connected: return "CONNECTED";
    case gameboy::LinkPacketChannel::State::failed: return "FAILED";
    }
    return "UNKNOWN";
}

bool present_remote_link_status(FrameRenderContext& context,
                                const RemoteLinkSession& remote) {
    static_cast<void>(SDL_SetRenderDrawBlendMode(context.renderer,
                                                 SDL_BLENDMODE_BLEND));
#ifndef __ANDROID__
    static_cast<void>(SDL_SetRenderDrawColor(context.renderer, 0, 0, 0, 170));
    const SDL_FRect bar{0, 0, 160, 11};
    static_cast<void>(SDL_RenderFillRect(context.renderer, &bar));
    static_cast<void>(SDL_SetRenderDrawColor(context.renderer, 235, 245, 235, 255));
#endif
    const auto role = remote.hosting ? "H" : "J";
    const auto state = [&]() {
        switch (remote.active_channel().state()) {
        case gameboy::LinkPacketChannel::State::disconnected: return "D";
        case gameboy::LinkPacketChannel::State::listening: return "L";
        case gameboy::LinkPacketChannel::State::connecting: return "N";
        case gameboy::LinkPacketChannel::State::connected: return "C";
        case gameboy::LinkPacketChannel::State::failed: return "F";
        }
        return "?";
    }();
    const auto link_state = state[0] == 'C' &&
                                    remote.endpoint.peer_hello_seen() &&
                                    !remote.endpoint.peer_compatible()
                                ? "M"
                                : (state[0] == 'C' &&
                                           !remote.endpoint.peer_ready_for_link()
                                       ? "W"
                                       : state);
    const auto recovery_marker = remote.endpoint.suspended()
                                     ? "P"
                                     : (remote.endpoint.failure_during_transfer()
                                            ? "!"
                                            : "");
    // Q/R are request/response counts generated by this endpoint. Keep the
    // labels abbreviated so both counters remain visible on the 160-pixel
    // Game Boy canvas. A nonzero Q with zero R means the peer is not servicing
    // its serial endpoint; zero Q means the game has not started its clock.
    const auto compact_count = [](const std::uint64_t value) {
        // Keep the diagnostic strip bounded even during battle payloads that
        // exchange thousands of bytes. The counters are deliberately shown
        // modulo 1000; Q/R equality and X progress are the useful signals.
        return static_cast<unsigned>(value % 1000U);
    };
    std::ostringstream status;
    status << (remote.bluetooth ? "BT " : "TCP ") << role << ' ' << link_state
           << recovery_marker << (remote.diagnostics ? " D" : "") << " Q"
           << compact_count(remote.endpoint.requests_sent()) << " R"
           << compact_count(remote.endpoint.responses_received()) << " X"
           << compact_count(remote.endpoint.transfers_completed());
    const auto text = status.str();
#ifndef __ANDROID__
    render_tool_text(context.renderer, 3, 2, text.c_str(), 154.0F, 0.57F);
#else
    // Portrait gameplay is rendered with logical presentation disabled so the
    // framebuffer can occupy its calculated phone viewport. Scale the status
    // strip independently; otherwise SDL's 8 px debug font remains at raw
    // screen pixels and is effectively unreadable on a high-density phone.
    int window_width = 1;
    int window_height = 1;
    auto* window = SDL_GetRenderWindow(context.renderer);
    if (window != nullptr) {
        static_cast<void>(SDL_GetWindowSize(window, &window_width,
                                            &window_height));
    }
    float old_scale_x = 1.0F;
    float old_scale_y = 1.0F;
    static_cast<void>(SDL_GetRenderScale(context.renderer, &old_scale_x,
                                         &old_scale_y));
    const auto portrait = window_height > window_width;
    const auto logical_status_width = std::max(
        160.0F, static_cast<float>(text.size() * 8U + 6U));
    const auto status_scale = portrait
                                  ? std::min(
                                        2.5F,
                                        std::max(1.0F,
                                                 (static_cast<float>(window_width) -
                                                  6.0F) /
                                                     logical_status_width))
                                  : 1.0F;
    if (portrait) {
        static_cast<void>(SDL_SetRenderScale(
            context.renderer, old_scale_x * status_scale,
            old_scale_y * status_scale));
    }
    const auto effective_scale_x = old_scale_x * status_scale;
    const auto effective_scale_y = old_scale_y * status_scale;
    const SDL_FRect bar{0.0F, 0.0F, logical_status_width,
                        11.0F};
    static_cast<void>(SDL_SetRenderDrawColor(context.renderer, 0, 0, 0, 170));
    static_cast<void>(SDL_RenderFillRect(context.renderer, &bar));
    static_cast<void>(SDL_SetRenderDrawColor(context.renderer, 235, 245, 235,
                                              255));
    static_cast<void>(SDL_RenderDebugText(
        context.renderer, 3.0F / effective_scale_x,
        2.0F / effective_scale_y, text.c_str()));
    if (portrait) {
        static_cast<void>(SDL_SetRenderScale(context.renderer, old_scale_x,
                                             old_scale_y));
    }
#endif
    static_cast<void>(SDL_SetRenderDrawBlendMode(context.renderer,
                                                 SDL_BLENDMODE_NONE));
    return true;
}


} // namespace gbb::sdl
