#include "frame_presenter.hpp"

#include <sstream>

namespace gbb::sdl {

std::vector<std::uint32_t> colorize_frame(
    const gbb::EmulatorCore& core, FrameRenderContext& context,
    const gameboy::DisplayPalette& palette) {
    const auto frame = core.video_frame();
    const auto native_colors =
        core.descriptor().system == gbb::SystemId::game_boy_color ||
        palette.cgb_compatibility;
    std::vector<std::uint32_t> colored_pixels;
    gbb::transform_video_frame(
        frame.pixels, frame.pixel_count, frame.width, frame.height, palette,
        native_colors, context.video_mode, colored_pixels);
    return colored_pixels;
}

std::vector<std::uint32_t> colorize_frame(
    const gameboy::Emulator& emulator, FrameRenderContext& context,
    const gameboy::DisplayPalette& palette) {
    const auto& pixels = emulator.framebuffer();
    const auto native_colors =
        emulator.bus().cgb_mode() || palette.cgb_compatibility;
    std::vector<std::uint32_t> colored_pixels;
    gbb::transform_video_frame(
        pixels.data(), pixels.size(), gameboy::Ppu::screen_width,
        gameboy::Ppu::screen_height, palette, native_colors, context.video_mode,
        colored_pixels);
    return colored_pixels;
}

bool present_link_frames(const gameboy::Emulator& first,
                         const gameboy::Emulator& second,
                         FrameRenderContext& context,
                         const gameboy::DisplayPalette& palette) {
    // A local cable session deliberately uses two native 160x144 views. Voxel
    // geometry is a single-camera presentation and is therefore bypassed for
    // the split view; users can switch back to the diorama after disconnecting.
    const auto first_pixels = colorize_frame(first, context, palette);
    const auto second_pixels = colorize_frame(second, context, palette);
    constexpr auto pitch = static_cast<int>(gameboy::Ppu::screen_width *
                                             sizeof(std::uint32_t));
    const SDL_FRect left{0, 0, static_cast<float>(gameboy::Ppu::screen_width),
                         static_cast<float>(gameboy::Ppu::screen_height)};
    const SDL_FRect right{static_cast<float>(gameboy::Ppu::screen_width), 0,
                          static_cast<float>(gameboy::Ppu::screen_width),
                          static_cast<float>(gameboy::Ppu::screen_height)};
    if (!SDL_UpdateTexture(context.texture, nullptr, first_pixels.data(), pitch) ||
        !SDL_RenderTexture(context.renderer, context.texture, nullptr, &left) ||
        !SDL_UpdateTexture(context.link_texture, nullptr, second_pixels.data(),
                           pitch) ||
        !SDL_RenderTexture(context.renderer, context.link_texture, nullptr, &right)) {
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
    static_cast<void>(SDL_RenderDebugText(context.renderer, 3, 2,
                                          text.str().c_str()));
    static_cast<void>(SDL_SetRenderDrawBlendMode(context.renderer,
                                                 SDL_BLENDMODE_NONE));
    return true;
}

const char* remote_link_state_label(
    const gameboy::TcpLinkChannel::State state) noexcept {
    switch (state) {
    case gameboy::TcpLinkChannel::State::disconnected: return "DISCONNECTED";
    case gameboy::TcpLinkChannel::State::listening: return "LISTENING";
    case gameboy::TcpLinkChannel::State::connecting: return "CONNECTING";
    case gameboy::TcpLinkChannel::State::connected: return "CONNECTED";
    case gameboy::TcpLinkChannel::State::failed: return "FAILED";
    }
    return "UNKNOWN";
}

bool present_remote_link_status(FrameRenderContext& context,
                                const RemoteLinkSession& remote) {
    static_cast<void>(SDL_SetRenderDrawBlendMode(context.renderer,
                                                 SDL_BLENDMODE_BLEND));
    static_cast<void>(SDL_SetRenderDrawColor(context.renderer, 0, 0, 0, 170));
    const SDL_FRect bar{0, 0, 160, 11};
    static_cast<void>(SDL_RenderFillRect(context.renderer, &bar));
    static_cast<void>(SDL_SetRenderDrawColor(context.renderer, 235, 245, 235, 255));
    const auto role = remote.hosting ? "H" : "J";
    const auto state = [&]() {
        switch (remote.channel.state()) {
        case gameboy::TcpLinkChannel::State::disconnected: return "D";
        case gameboy::TcpLinkChannel::State::listening: return "L";
        case gameboy::TcpLinkChannel::State::connecting: return "N";
        case gameboy::TcpLinkChannel::State::connected: return "C";
        case gameboy::TcpLinkChannel::State::failed: return "F";
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
    status << "TCP " << role << ' ' << link_state
           << (remote.diagnostics ? " D" : "") << " Q"
           << compact_count(remote.endpoint.requests_sent()) << " R"
           << compact_count(remote.endpoint.responses_received()) << " X"
           << compact_count(remote.endpoint.transfers_completed());
    const auto text = status.str();
    static_cast<void>(SDL_RenderDebugText(context.renderer, 3, 2, text.c_str()));
    static_cast<void>(SDL_SetRenderDrawBlendMode(context.renderer,
                                                 SDL_BLENDMODE_NONE));
    return true;
}


} // namespace gbb::sdl
