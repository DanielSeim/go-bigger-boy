#include "presentation.hpp"

#include <stdexcept>
#include <string>

namespace gbb::sdl {

namespace {

[[noreturn]] void presentation_error(const char* action) {
    throw std::runtime_error(std::string{action} + ": " + SDL_GetError());
}

} // namespace

void present_frame(const PresentationContext& context) {
    auto& sdl = context.sdl;
    static_cast<void>(SDL_SetRenderDrawColor(sdl.renderer, 16, 20, 16, 255));
    if (!SDL_RenderClear(sdl.renderer)) presentation_error("Could not clear framebuffer");

    if (context.dashboard_visible) {
        if (context.dashboard_overlay) context.dashboard_overlay();
    } else if (context.core != nullptr && context.emulator != nullptr &&
               has_capability(context.core->descriptor().capabilities,
                              CoreCapability::link_cable) &&
               context.link_emulator != nullptr) {
        FrameRenderContext frame_context{
            sdl.renderer, sdl.texture, sdl.link_texture, sdl.video_mode};
        if (!present_link_frames(*context.emulator, *context.link_emulator,
                                 frame_context, context.palette)) {
            presentation_error("Could not present linked framebuffers");
        }
        if (context.link_session != nullptr &&
            !present_link_status(frame_context, *context.link_session)) {
            presentation_error("Could not present link status");
        }
    } else if (context.core != nullptr) {
        if (context.emulator != nullptr &&
            has_capability(context.core->descriptor().capabilities,
                           CoreCapability::scene_layers) &&
            (sdl.video_mode == gameboy::VideoMode::voxel_diorama ||
             sdl.video_mode == gameboy::VideoMode::voxel_shape ||
             sdl.video_mode == gameboy::VideoMode::voxel_popup)) {
            VoxelRenderContext voxel_context{
                sdl.renderer,
                sdl.texture,
                sdl.video_mode,
                sdl.scene_snapshot,
                sdl.voxel_profile_path,
                sdl.voxel_profile,
                sdl.voxel_profile_fingerprint,
                sdl.voxel_profile_loaded,
                sdl.voxel_vertices,
                sdl.voxel_indices,
                sdl.voxel_camera_pitch_offset,
                sdl.voxel_camera_yaw_offset};
            if (!render_voxel_diorama(
                    *context.emulator, voxel_context, context.palette,
                    sdl.video_mode == gameboy::VideoMode::voxel_shape,
                    sdl.video_mode == gameboy::VideoMode::voxel_popup)) {
                presentation_error("Could not render voxel diorama");
            }
        } else {
            FrameRenderContext frame_context{
                sdl.renderer, sdl.texture, sdl.link_texture, sdl.video_mode};
            const auto colored_pixels =
                colorize_frame(*context.core, frame_context, context.palette);
            const auto frame = context.core->video_frame();
            if (!SDL_UpdateTexture(
                    sdl.texture, nullptr, colored_pixels.data(),
                    static_cast<int>(frame.width * sizeof(std::uint32_t))) ||
                !SDL_RenderTexture(sdl.renderer, sdl.texture, nullptr, nullptr)) {
                presentation_error("Could not present framebuffer");
            }
        }
        // Keep the strip visible while a host is listening so users have
        // confirmation that the lobby is active before a peer connects.
        if (context.remote_link != nullptr && context.remote_link->active()) {
            FrameRenderContext frame_context{
                sdl.renderer, sdl.texture, sdl.link_texture, sdl.video_mode};
            if (!present_remote_link_status(frame_context,
                                             *context.remote_link)) {
                presentation_error("Could not present remote link status");
            }
        }
    }

    if (!context.dashboard_visible && context.core != nullptr) {
        if (context.touch_overlay) context.touch_overlay();
        if (context.menu_overlay) context.menu_overlay();
    }
    if (!SDL_RenderPresent(sdl.renderer)) {
        presentation_error("Could not present framebuffer");
    }
}

} // namespace gbb::sdl
