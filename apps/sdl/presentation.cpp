#include "presentation.hpp"
#include "tool_window_support.hpp"

#include "gbb/frontend_logging.hpp"

#ifndef __ANDROID__
#include "dialogs.hpp"
#endif

#ifdef __ANDROID__
#include "android_touch_input.hpp"
#endif

#include <stdexcept>
#include <iomanip>
#include <sstream>
#include <string>

namespace gbb::sdl {

namespace {

[[noreturn]] void presentation_error(const char* action) {
    throw std::runtime_error(std::string{action} + ": " + SDL_GetError());
}

void present_fps_overlay(SdlResources& sdl, const bool enabled,
                         const bool dashboard_visible) {
    if (!enabled || dashboard_visible) {
        sdl.fps_metrics.reset();
        sdl.fps_log_windows = 0;
        sdl.fps_value = 0.0F;
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (const auto sample = sdl.fps_metrics.observe(now); sample.has_value()) {
        sdl.fps_value = sample->fps;
        ++sdl.fps_log_windows;
        if (sdl.fps_log_windows >= 4U) {
            if (gbb::Logger::instance().enabled(gbb::LogLevel::debug)) {
                gbb::log_frontend(
                    gbb::LogLevel::debug,
                    std::string("fps_sample fps=") +
                        std::to_string(sdl.fps_value) +
                        " window_ms=" + std::to_string(sample->window_ms) +
                        " video_mode=" +
                        std::string(gameboy::video_mode_info(sdl.video_mode).id));
            }
            sdl.fps_log_windows = 0;
        }
    }
    if (sdl.fps_value <= 0.0F) return;

    const auto logical_width = static_cast<float>(
        sdl.core_video_width * (sdl.split_screen ? 2U : 1U));
    constexpr float bar_width = 49.0F;
    static_cast<void>(SDL_SetRenderDrawBlendMode(sdl.renderer,
                                                  SDL_BLENDMODE_BLEND));
    static_cast<void>(SDL_SetRenderDrawColor(sdl.renderer, 0, 0, 0, 175));
    const SDL_FRect bar{logical_width - bar_width, 0.0F, bar_width, 11.0F};
    static_cast<void>(SDL_RenderFillRect(sdl.renderer, &bar));
    static_cast<void>(SDL_SetRenderDrawColor(sdl.renderer, 235, 245, 235, 255));
    std::ostringstream text;
    text << "FPS " << std::fixed << std::setprecision(1) << sdl.fps_value;
    render_tool_text(sdl.renderer, logical_width - bar_width + 2.0F, 2.0F,
                     text.str().c_str(), bar_width - 3.0F, 0.57F);
    static_cast<void>(SDL_SetRenderDrawBlendMode(sdl.renderer,
                                                  SDL_BLENDMODE_NONE));
}

} // namespace

void present_frame(const PresentationContext& context) {
    auto& sdl = context.sdl;
#ifdef __ANDROID__
    static_cast<void>(SDL_SetRenderDrawColor(sdl.renderer, 8, 12, 20, 255));
#else
    static_cast<void>(SDL_SetRenderDrawColor(sdl.renderer, 16, 20, 16, 255));
#endif
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
                                 frame_context, context.palette,
                                 sdl.presentation_pixels)) {
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
            sdl.core_video_width == gameboy::Ppu::screen_width &&
            sdl.core_video_height == gameboy::Ppu::screen_height &&
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
                sdl.voxel_scene,
                sdl.voxel_scene_signature,
                sdl.voxel_scene_cached,
                sdl.voxel_stats,
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
            colorize_frame(*context.core, frame_context, context.palette,
                           sdl.presentation_pixels);
            const auto frame = context.core->video_frame();
            if (!SDL_UpdateTexture(
                    sdl.texture, nullptr, sdl.presentation_pixels.data(),
                    static_cast<int>(frame.width * sizeof(std::uint32_t)))) {
                presentation_error("Could not present framebuffer");
            }
#ifdef __ANDROID__
            if (!touch_is_landscape(sdl) && !voxel_mode_enabled(sdl)) {
                const auto game_rect = android_portrait_game_rect(sdl);
                if (!SDL_SetRenderLogicalPresentation(
                        sdl.renderer, 0, 0,
                        SDL_LOGICAL_PRESENTATION_DISABLED) ||
                    !SDL_RenderTexture(sdl.renderer, sdl.texture, nullptr,
                                       &game_rect)) {
                    presentation_error("Could not present portrait framebuffer");
                }
            } else
#endif
            if (!SDL_RenderTexture(sdl.renderer, sdl.texture, nullptr, nullptr)) {
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
    present_fps_overlay(sdl, context.show_fps && context.core != nullptr,
                        context.dashboard_visible);
#ifndef __ANDROID__
    if (desktop_dialog_visible(sdl.window)) {
        present_desktop_dialog(sdl.renderer, sdl.window);
        const auto presentation = sdl.video_mode == gameboy::VideoMode::integer
                                       ? SDL_LOGICAL_PRESENTATION_INTEGER_SCALE
                                       : SDL_LOGICAL_PRESENTATION_LETTERBOX;
        const auto logical_width = static_cast<int>(
            sdl.core_video_width * (sdl.split_screen ? 2U : 1U));
        static_cast<void>(SDL_SetRenderLogicalPresentation(
            sdl.renderer, logical_width,
            static_cast<int>(sdl.core_video_height), presentation));
    } else if (desktop_notification_visible(sdl.window)) {
        present_desktop_notification(sdl.renderer, sdl.window);
        const auto presentation = sdl.video_mode == gameboy::VideoMode::integer
                                       ? SDL_LOGICAL_PRESENTATION_INTEGER_SCALE
                                       : SDL_LOGICAL_PRESENTATION_LETTERBOX;
        const auto logical_width = static_cast<int>(
            sdl.core_video_width * (sdl.split_screen ? 2U : 1U));
        static_cast<void>(SDL_SetRenderLogicalPresentation(
            sdl.renderer, logical_width,
            static_cast<int>(sdl.core_video_height), presentation));
    }
#endif
    if (!SDL_RenderPresent(sdl.renderer)) {
        presentation_error("Could not present framebuffer");
    }
}

} // namespace gbb::sdl
