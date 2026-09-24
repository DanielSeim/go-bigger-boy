#include "presentation.hpp"
#include "tool_window_support.hpp"

#include "gbb/frontend_logging.hpp"

#ifndef __ANDROID__
#include "dialogs.hpp"
#endif

#ifdef __ANDROID__
#include "android_touch_input.hpp"
#include "emulation_session.hpp"
#endif

#include <stdexcept>
#include <iomanip>
#include <sstream>
#include <string>

namespace gbb::sdl {

namespace {

std::uint64_t sgb_border_source_key(
    const std::uint64_t border_revision,
    const std::uint64_t rom_fingerprint,
    const gameboy::DisplayPalette& palette) {
    std::uint64_t key = border_revision ^ rom_fingerprint;
    for (const auto color : palette.colors) {
        key ^= color;
        key *= UINT64_C(1099511628211);
    }
    key ^= palette.cgb_compatibility ? UINT64_C(1) : UINT64_C(0);
    return key;
}

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
#ifdef __ANDROID__
            const auto voxel_mode =
                sdl.video_mode == gameboy::VideoMode::voxel_diorama ||
                sdl.video_mode == gameboy::VideoMode::voxel_shape ||
                sdl.video_mode == gameboy::VideoMode::voxel_popup;
            if (voxel_mode) {
                SDL_Log("GBB fps_sample fps=%.2f mode=%s voxel_total_us=%llu "
                        "scene_us=%llu scene_build_us=%llu pixel_us=%llu "
                        "geometry_build_us=%llu sort_us=%llu submit_us=%llu "
                        "rendered_frames=%llu cache_hits=%llu cache_misses=%llu "
                        "mesh_vertices=%llu mesh_indices=%llu",
                        sdl.fps_value,
                        std::string(gameboy::video_mode_info(sdl.video_mode).id)
                            .c_str(),
                        static_cast<unsigned long long>(
                            sdl.voxel_stats.total_us),
                        static_cast<unsigned long long>(
                            sdl.voxel_stats.scene_snapshot_us),
                        static_cast<unsigned long long>(
                            sdl.voxel_stats.scene_build_us),
                        static_cast<unsigned long long>(
                            sdl.voxel_stats.pixel_transform_us),
                        static_cast<unsigned long long>(
                            sdl.voxel_stats.geometry_build_us),
                        static_cast<unsigned long long>(
                            sdl.voxel_stats.geometry_sort_us),
                        static_cast<unsigned long long>(
                            sdl.voxel_stats.geometry_submit_us),
                        static_cast<unsigned long long>(
                            sdl.voxel_stats.rendered_frames),
                        static_cast<unsigned long long>(
                            sdl.voxel_stats.render_cache_hits),
                        static_cast<unsigned long long>(
                            sdl.voxel_stats.rendered_frames >
                                    sdl.voxel_stats.render_cache_hits
                                ? sdl.voxel_stats.rendered_frames -
                                      sdl.voxel_stats.render_cache_hits
                                : 0),
                        static_cast<unsigned long long>(
                            sdl.voxel_stats.mesh_vertices),
                        static_cast<unsigned long long>(
                            sdl.voxel_stats.mesh_indices));
            } else {
                SDL_Log("GBB fps_sample fps=%.2f mode=%s", sdl.fps_value,
                        std::string(gameboy::video_mode_info(sdl.video_mode).id)
                            .c_str());
            }
#endif
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
        const auto voxel_mode =
            sdl.video_mode == gameboy::VideoMode::voxel_diorama ||
            sdl.video_mode == gameboy::VideoMode::voxel_shape ||
            sdl.video_mode == gameboy::VideoMode::voxel_popup;
        const auto native_gameboy_surface =
            sdl.core_video_width == gameboy::Ppu::screen_width &&
            sdl.core_video_height == gameboy::Ppu::screen_height;
#ifdef __ANDROID__
        const auto sgb_gameboy_surface =
            sdl.core_video_width == gameboy::Ppu::sgb_border_width &&
            sdl.core_video_height == gameboy::Ppu::sgb_border_height;
#else
        constexpr auto sgb_gameboy_surface = false;
#endif
        if (context.emulator != nullptr &&
            has_capability(context.core->descriptor().capabilities,
                           CoreCapability::scene_layers) &&
            voxel_mode && (native_gameboy_surface || sgb_gameboy_surface)) {
#ifdef __ANDROID__
            const auto portrait_voxel = !touch_is_landscape(sdl);
            SDL_FRect full_output{};
            if (portrait_voxel) {
                full_output = android_portrait_game_rect(sdl);
            } else if (!SDL_GetRenderLogicalPresentationRect(
                           sdl.renderer, &full_output)) {
                full_output = SDL_FRect{0.0F, 0.0F,
                                        static_cast<float>(sdl.core_video_width),
                                        static_cast<float>(sdl.core_video_height)};
            }
            const auto voxel_output = sgb_gameboy_surface
                                          ? SDL_FRect{
                                                full_output.x + full_output.w * 48.0F / 256.0F,
                                                full_output.y + full_output.h * 40.0F / 224.0F,
                                                full_output.w * 160.0F / 256.0F,
                                                full_output.h * 144.0F / 224.0F}
                                          : full_output;
            // The SGB texture contains the border and the centered 160x144
            // viewport. Paint that complete surface first; the voxel mesh is
            // then composited only into the native viewport rectangle.
            if (!SDL_SetRenderLogicalPresentation(
                    sdl.renderer, 0, 0,
                    SDL_LOGICAL_PRESENTATION_DISABLED) ||
                !SDL_SetRenderScale(sdl.renderer, 1.0F, 1.0F) ||
                !SDL_SetRenderViewport(sdl.renderer, nullptr)) {
                presentation_error("Could not prepare voxel presentation");
            }
            if (sgb_gameboy_surface) {
                FrameRenderContext frame_context{
                    sdl.renderer, sdl.texture, sdl.link_texture, sdl.video_mode};
                const auto border_revision = context.emulator->bus()
                                                 .debug_sgb_border_revision();
                const auto source_key = sgb_border_source_key(
                    border_revision, context.emulator->rom_fingerprint(),
                    context.palette);
                if (!sdl.sgb_border_texture_valid ||
                    sdl.sgb_border_source_key != source_key) {
                    const auto frame = context.core->video_frame();
                    colorize_frame(*context.core, frame_context, context.palette,
                                   sdl.presentation_pixels);
                    if (!SDL_UpdateTexture(
                            sdl.sgb_border_texture, nullptr,
                            sdl.presentation_pixels.data(),
                            static_cast<int>(frame.width * sizeof(std::uint32_t)))) {
                        presentation_error("Could not present SGB voxel backdrop");
                    }
                    sdl.sgb_border_texture_key = source_key;
                    sdl.sgb_border_source_key = source_key;
                    sdl.sgb_border_texture_valid = true;
                }
                // Leave the 160x144 viewport hole empty. The voxel renderer
                // owns that rectangle; drawing the complete SGB texture here
                // would leave a second copy of the Game Boy image underneath
                // it and becomes visible at the voxel edges.
                const auto draw_sgb_slice = [&](const SDL_FRect& source,
                                                const SDL_FRect& destination) {
                    return SDL_RenderTexture(sdl.renderer, sdl.sgb_border_texture,
                                             &source, &destination);
                };
                const SDL_FRect top_source{0.0F, 0.0F, 256.0F, 40.0F};
                const SDL_FRect bottom_source{0.0F, 184.0F, 256.0F, 40.0F};
                const SDL_FRect left_source{0.0F, 40.0F, 48.0F, 144.0F};
                const SDL_FRect right_source{208.0F, 40.0F, 48.0F, 144.0F};
                const auto inner_x = full_output.x + full_output.w * 48.0F / 256.0F;
                const auto inner_y = full_output.y + full_output.h * 40.0F / 224.0F;
                const auto inner_w = full_output.w * 160.0F / 256.0F;
                const auto inner_h = full_output.h * 144.0F / 224.0F;
                if (!draw_sgb_slice(
                        top_source,
                        SDL_FRect{full_output.x, full_output.y, full_output.w,
                                  inner_y - full_output.y}) ||
                    !draw_sgb_slice(
                        bottom_source,
                        SDL_FRect{full_output.x, inner_y + inner_h,
                                  full_output.w,
                                  full_output.y + full_output.h -
                                      (inner_y + inner_h)}) ||
                    !draw_sgb_slice(
                        left_source,
                        SDL_FRect{full_output.x, inner_y,
                                  inner_x - full_output.x, inner_h}) ||
                    !draw_sgb_slice(
                        right_source,
                        SDL_FRect{inner_x + inner_w, inner_y,
                                  full_output.x + full_output.w -
                                      (inner_x + inner_w),
                                  inner_h})) {
                    presentation_error("Could not present SGB voxel border");
                }
            }
            // Voxel geometry is authored in native 160x144 coordinates.
            // Render it into its native target, then composite that target
            // into the same rectangle used by the framebuffer.
            constexpr auto voxel_output_valid = true;
#else
            constexpr auto voxel_output_valid = false;
            const SDL_FRect voxel_output{};
#endif
            VoxelRenderContext voxel_context{
                sdl.renderer,
                sdl.voxel_texture != nullptr ? sdl.voxel_texture : sdl.texture,
                sdl.voxel_render_target,
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
                sdl.voxel_camera_yaw_offset,
                sdl.voxel_render_cache_key,
                sdl.voxel_render_cache_state_key,
                sdl.voxel_render_cache_valid,
                sdl.voxel_render_cache_source_pixels,
                voxel_output,
                voxel_output_valid};
            if (!render_voxel_diorama(
                    *context.emulator, voxel_context, context.palette,
                    sdl.video_mode == gameboy::VideoMode::voxel_shape,
                    sdl.video_mode == gameboy::VideoMode::voxel_popup)) {
                presentation_error("Could not render voxel diorama");
            }
#ifdef __ANDROID__
            if (!restore_video_presentation(sdl)) {
                presentation_error("Could not restore voxel presentation");
            }
#endif
        } else {
            FrameRenderContext frame_context{
                sdl.renderer, sdl.texture, sdl.link_texture, sdl.video_mode};
            const auto frame = context.core->video_frame();
            const auto native_colors =
                context.core->video_frame_native_colors() ||
                context.core->descriptor().system ==
                    gbb::SystemId::game_boy_color ||
                context.palette.cgb_compatibility;
            gbb::transform_video_frame(
                frame.pixels, frame.pixel_count, frame.width, frame.height,
                context.palette, native_colors, sdl.video_mode,
                sdl.presentation_pixels);
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
