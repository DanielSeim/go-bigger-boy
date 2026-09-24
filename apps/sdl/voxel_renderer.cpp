#include "voxel_renderer.hpp"

#include "gbb/frontend_logging.hpp"
#include "gbb/gameboy_scene.hpp"
#include "gbb/voxel_scene.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <sstream>
#include <unordered_map>
#include <utility>

namespace gbb::sdl {

SDL_FColor voxel_color(const std::uint32_t pixel, const float shade,
                       const float ambient) {
    const auto component = [pixel, shade, ambient](const unsigned shift) {
        const auto value = static_cast<float>((pixel >> shift) & 0xFFU) / 255.0F;
        // Game Boy artwork uses pure black for outlines.  Multiplication-only
        // lighting therefore leaves every dark edge completely black, which
        // makes shallow voxel shadows look heavier than their geometry.  A
        // restrained ambient lift keeps those faces readable without changing
        // the normal (non-voxel) framebuffer path.
        return std::clamp(value * shade + ambient, 0.0F, 1.0F);
    };
    return {component(16), component(8), component(0), 1.0F};
}

namespace {

#ifdef _WIN32
// The Direct3D renderer currently produces backend-dependent geometry when a
// 160x144 SDL render target is used with SDL_RenderGeometry. Keep Windows on
// the direct presentation path until that backend path has its own visual
// proof; correctness is more important than this optional cache.
constexpr bool voxel_render_target_cache_supported = false;
#else
constexpr bool voxel_render_target_cache_supported = true;
#endif

void mix_render_key(std::uint64_t& key, const std::uint64_t value) noexcept {
    key ^= value + UINT64_C(0x9e3779b97f4a7c15) + (key << 6U) + (key >> 2U);
}

std::uint64_t voxel_profile_signature(const gbb::VoxelProfile& profile) {
    std::uint64_t key = UINT64_C(0xcbf29ce484222325);
    const auto mix_float = [&key](const float value) {
        mix_render_key(key, std::hash<float>{}(value));
    };
    const auto mix_integer = [&key](const std::uint64_t value) {
        mix_render_key(key, value);
    };
    mix_float(profile.depth_scale);
    mix_float(profile.camera_pitch);
    mix_float(profile.camera_yaw);
    mix_float(profile.zoom);
    mix_float(profile.perspective);
    mix_float(profile.sprite_depth);
    mix_float(profile.lighting);
    mix_float(profile.background_depth_far);
    mix_float(profile.background_depth_near);
    mix_float(profile.background_transparent_depth);
    mix_float(profile.window_depth_far);
    mix_float(profile.window_depth_near);
    mix_float(profile.sprite_depth_far);
    mix_float(profile.sprite_depth_near);
    mix_float(profile.popup_parallax);
    mix_float(profile.popup_object_height);
    mix_float(profile.popup_sprite_height);
    mix_float(profile.popup_card_thickness);
    mix_float(profile.popup_sprite_thickness);
    mix_integer(profile.popup_hud_top_rows);
    mix_integer(profile.popup_hud_bottom_rows);
    mix_integer(profile.background_object_detection ? 1U : 0U);
    mix_integer(profile.background_object_min_cells);
    mix_float(profile.background_object_max_fraction);
    mix_float(profile.background_object_confidence);
    mix_integer(profile.background_debug_overlay ? 1U : 0U);
    mix_integer(profile.framebuffer_facade ? 1U : 0U);
    for (const auto& object_template : profile.background_object_templates) {
        mix_integer(std::hash<std::string>{}(object_template.id));
        mix_integer(object_template.width);
        mix_integer(object_template.height);
        for (const auto tile_id : object_template.tile_ids) {
            mix_integer(static_cast<std::uint16_t>(tile_id));
        }
    }
    return key;
}

} // namespace

bool render_voxel_diorama(const gameboy::Emulator& emulator,
                          VoxelRenderContext& context,
                          const gameboy::DisplayPalette& palette,
                          const bool shape_aware,
                          const bool popup_book) {
    const auto frame_started = std::chrono::steady_clock::now();
    // SDL_RenderGeometry is backed by the active SDL GPU renderer (D3D,
    // OpenGL, Metal or Vulkan). We submit a deterministic pixel-relief mesh
    // here and optionally keep the native framebuffer as a textured facade on
    // top of it. This avoids platform-specific shader binaries while keeping
    // the original Game Boy artwork recognizable in the diorama.
    const auto fingerprint = emulator.rom_fingerprint();
    if (!context.voxel_profile_loaded || context.voxel_profile_fingerprint != fingerprint) {
        context.voxel_profile = gbb::load_voxel_profile(context.voxel_profile_path, fingerprint);
        context.voxel_profile_fingerprint = fingerprint;
        context.voxel_profile_loaded = true;
        context.voxel_camera_pitch_offset = 0.0F;
        context.voxel_camera_yaw_offset = 0.0F;
    }
    auto profile = context.voxel_profile;
    ++context.voxel_stats.rendered_frames;
    profile.camera_pitch = std::clamp(
        profile.camera_pitch + context.voxel_camera_pitch_offset, -75.0F, 75.0F);
    profile.camera_yaw = std::clamp(
        profile.camera_yaw + context.voxel_camera_yaw_offset, -180.0F, 180.0F);
    const auto& source_pixels = emulator.framebuffer();
    const auto native_colors = emulator.bus().cgb_mode() || palette.cgb_compatibility;
    std::uint64_t render_key = fingerprint;
    mix_render_key(render_key, static_cast<std::uint64_t>(context.video_mode));
    mix_render_key(render_key, shape_aware ? 1U : 0U);
    mix_render_key(render_key, popup_book ? 1U : 0U);
    mix_render_key(render_key, std::hash<float>{}(context.voxel_camera_pitch_offset));
    mix_render_key(render_key, std::hash<float>{}(context.voxel_camera_yaw_offset));
    mix_render_key(render_key, voxel_profile_signature(profile));
    mix_render_key(render_key, native_colors ? 1U : 0U);
    mix_render_key(render_key, palette.cgb_compatibility ? 1U : 0U);
    for (const auto color : palette.colors) mix_render_key(render_key, color);
    const auto source_state_key = gbb::gameboy_scene_input_signature(
        emulator, popup_book && profile.background_object_detection);
    const auto same_cached_source =
        context.voxel_render_cache_source_pixels.size() == source_pixels.size() &&
        std::equal(source_pixels.begin(), source_pixels.end(),
                   context.voxel_render_cache_source_pixels.begin());
    if (voxel_render_target_cache_supported && context.render_target != nullptr &&
        context.voxel_render_cache_valid &&
        context.voxel_render_cache_key == render_key &&
        context.voxel_render_cache_state_key == source_state_key &&
        same_cached_source) {
        if (SDL_RenderTexture(context.renderer, context.render_target, nullptr,
                              context.output_rect_valid
                                  ? &context.output_rect
                                  : nullptr)) {
            ++context.voxel_stats.render_cache_hits;
            context.voxel_stats.scene_snapshot_us = 0;
            context.voxel_stats.scene_build_us = 0;
            context.voxel_stats.pixel_transform_us = 0;
            context.voxel_stats.geometry_build_us = 0;
            context.voxel_stats.geometry_sort_us = 0;
            context.voxel_stats.geometry_submit_us = 0;
            ++context.voxel_stats.rendered_frames;
            const auto frame_finished = std::chrono::steady_clock::now();
            context.voxel_stats.total_us = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    frame_finished - frame_started)
                    .count());
            return true;
        }
        context.voxel_render_cache_valid = false;
    }

    const auto scene_snapshot_started = std::chrono::steady_clock::now();
    gbb::populate_gameboy_scene_snapshot(emulator, context.scene_snapshot);
    // Scene metadata for SGB describes the complete 256x224 output, while
    // voxel geometry intentionally models only the native 160x144 Game Boy
    // viewport. Normalize the scene before building masks and geometry; the
    // surrounding SGB border is composited by the presentation layer.
    if (context.scene_snapshot.width == gameboy::Ppu::sgb_border_width &&
        context.scene_snapshot.height == gameboy::Ppu::sgb_border_height) {
        context.scene_snapshot.width = gameboy::Ppu::screen_width;
        context.scene_snapshot.height = gameboy::Ppu::screen_height;
    }
    const auto scene_snapshot_finished = std::chrono::steady_clock::now();
    context.voxel_stats.scene_snapshot_us = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            scene_snapshot_finished - scene_snapshot_started)
            .count());
    const auto& scene = context.scene_snapshot;
    // Pop-up book geometry is tuned separately from generic voxel relief so
    // raised scenery reads as a paper card without floating away from its
    // page hinge.
    const auto scene_key = gbb::voxel_scene_signature(context.scene_snapshot);
    bool scene_rebuilt = false;
    const auto scene_build_started = std::chrono::steady_clock::now();
    if (!context.voxel_scene_cached ||
        context.voxel_scene_signature != scene_key) {
        context.voxel_scene = gbb::build_voxel_scene(
            context.scene_snapshot,
            gbb::VoxelSceneBuildOptions{
                true,
                popup_book && profile.background_object_detection &&
                    context.scene_snapshot.width == gameboy::Ppu::screen_width &&
                    context.scene_snapshot.height == gameboy::Ppu::screen_height,
                profile.background_object_min_cells,
                profile.background_object_max_fraction,
                profile.background_object_confidence,
                &profile.background_object_templates});
        context.voxel_scene_signature = scene_key;
        context.voxel_scene_cached = true;
        ++context.voxel_stats.scene_rebuilds;
        scene_rebuilt = true;
    }
    const auto scene_build_finished = std::chrono::steady_clock::now();
    context.voxel_stats.scene_build_us = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            scene_build_finished - scene_build_started)
            .count());
    const auto& voxel_scene = context.voxel_scene;
    context.voxel_stats.accepted_objects = voxel_scene.objects.size();
    context.voxel_stats.candidate_objects = voxel_scene.candidates.size();
    // Reserve a recessed plane for the complete framebuffer. Sprites and
    // window overlays are then elevated relative to this plane, giving the
    // diorama a clear far/middle/foreground separation.
    // In the projection a larger Z value is farther from the viewer. Keep
    // the recessed background at the far end, then subtract each layer's
    // height so windows and sprites move toward the viewer in that order.
    const auto base_depth = 8.0F * profile.depth_scale;
    std::vector<std::uint32_t> colored_pixels;
    const auto pixel_transform_started = std::chrono::steady_clock::now();
    gbb::transform_video_frame(
        source_pixels.data(), source_pixels.size(), gameboy::Ppu::screen_width,
        gameboy::Ppu::screen_height, palette, native_colors, context.video_mode,
        colored_pixels);
    const auto pixel_transform_finished = std::chrono::steady_clock::now();
    context.voxel_stats.pixel_transform_us = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            pixel_transform_finished - pixel_transform_started)
            .count());
    // Keep non-pixel presentation state in a small key, then compare the
    // transformed framebuffer exactly. Hashing all 23,040 pixels with the
    // render-key mixer on every 60 Hz cache hit was expensive enough to erase
    // much of the Android render-target win.
    // Establish a coarse backdrop reference for automatic shape extraction.
    // 3dSen-style profiles can override this interpretation per game, but a
    // dominant-color pass gives unsupported ROMs a useful generic baseline:
    // sky/background pixels remain recessed while differently colored regions
    // become volumetric scene objects.
    const auto backdrop_key = [](const std::uint32_t pixel) {
        const auto quantize = [](const std::uint32_t component) {
            return (component >> 4U) & 0x0FU;
        };
        return (quantize((pixel >> 16) & 0xFFU) << 8U) |
               (quantize((pixel >> 8) & 0xFFU) << 4U) |
               quantize(pixel & 0xFFU);
    };
    std::unordered_map<unsigned, std::size_t> backdrop_histogram;
    std::unordered_map<std::uint32_t, std::size_t> backdrop_colors;
    backdrop_histogram.reserve(4096);
    backdrop_colors.reserve(1024);
    for (unsigned y = 24; y < 124; ++y) {
        for (unsigned x = 0; x < gameboy::Ppu::screen_width; ++x) {
            const auto pixel =
                colored_pixels[y * gameboy::Ppu::screen_width + x];
            ++backdrop_histogram[backdrop_key(pixel)];
            ++backdrop_colors[pixel];
        }
    }
    unsigned backdrop_color_key = 0;
    std::size_t backdrop_pixels = 0;
    for (const auto& [key, count] : backdrop_histogram) {
        if (count > backdrop_pixels) {
            backdrop_color_key = key;
            backdrop_pixels = count;
        }
    }
    std::uint32_t backdrop_color = 0;
    std::size_t backdrop_color_pixels = 0;
    for (const auto& [pixel, count] : backdrop_colors) {
        if (backdrop_key(pixel) == backdrop_color_key &&
            count > backdrop_color_pixels) {
            backdrop_color = pixel;
            backdrop_color_pixels = count;
        }
    }

    auto& vertices = context.voxel_vertices;
    auto& indices = context.voxel_indices;
    vertices.clear();
    indices.clear();
    // All three diorama modes use one source pixel per column. The flat
    // framebuffer is batched as a textured plane below, while only columns
    // with actual relief emit 3D geometry. Shape-aware depth and layer
    // treatment therefore add volume without spatially downsampling the
    // framebuffer, preserving thin outlines, text and small sprites.
    vertices.reserve(120000);
    indices.reserve(180000);
    // Keep outline pixels charcoal rather than absolute black.  This is
    // intentionally subtle (about 14/255 at default lighting) so the
    // original pixel-art contrast remains recognizable.
    constexpr float voxel_ambient = 0.055F;
    const auto radians = [](const float degrees) {
        return degrees * 0.01745329251994329577F;
    };
    const auto yaw = radians(profile.camera_yaw);
    // A pop-up book lays the framebuffer out as a page (X/Z) and raises
    // windows/sprites vertically (Y). The lower edge is the near edge; invert
    // the camera convention for this projection so the source image remains
    // upright instead of appearing upside down.
    const auto pitch = radians(std::clamp(
        popup_book ? -(profile.camera_pitch + 20.0F) : profile.camera_pitch,
        -80.0F, 80.0F));
    const auto yaw_cos = std::cos(yaw);
    const auto yaw_sin = std::sin(yaw);
    const auto pitch_cos = std::cos(pitch);
    const auto pitch_sin = std::sin(pitch);
    const auto project = [&](const float x, const float y, const float z) {
        const auto centered_x = x - 80.0F;
        const auto centered_y = y - 72.0F;
        if (popup_book) {
            // Source Y becomes page depth; the renderer's Z value is the
            // vertical lift above that page.  This is the pop-up-book layout:
            // distant background tiles lie flat, while windows and sprites
            // stand up from the page as independent objects.
            const auto page_depth = popup_book ? -centered_y * profile.popup_parallax
                                               : centered_y * 0.82F;
            const auto world_height = base_depth - z;
            const auto yaw_x = centered_x * yaw_cos - page_depth * yaw_sin;
            const auto yaw_depth = centered_x * yaw_sin + page_depth * yaw_cos;
            const auto pitched_y = world_height * pitch_cos -
                                   yaw_depth * pitch_sin;
            const auto depth = world_height * pitch_sin +
                               yaw_depth * pitch_cos;
            const auto perspective = 1.0F /
                std::max(0.35F, 1.0F + depth * profile.perspective);
            return SDL_FPoint{80.0F + yaw_x * profile.zoom * perspective,
                              72.0F - pitched_y * profile.zoom * perspective};
        }
        // The framebuffer is an X/Y plane with voxel depth on Z. Yaw must
        // rotate X against Z around the vertical center-Y axis; rotating X/Y
        // here would only roll the image in its own plane.
        const auto yaw_x = centered_x * yaw_cos - z * yaw_sin;
        const auto yaw_depth = centered_x * yaw_sin + z * yaw_cos;
        const auto pitched_y = centered_y * pitch_cos - yaw_depth * pitch_sin;
        const auto depth = centered_y * pitch_sin + yaw_depth * pitch_cos;
        const auto perspective = 1.0F /
            std::max(0.35F, 1.0F + depth * profile.perspective);
        return SDL_FPoint{80.0F + yaw_x * profile.zoom * perspective,
                          72.0F + pitched_y * profile.zoom * perspective};
    };
    const auto add_quad = [&](const SDL_FPoint a, const SDL_FPoint b,
                              const SDL_FPoint c, const SDL_FPoint d,
                              const SDL_FColor color) {
        const auto all_left = a.x < 0.0F && b.x < 0.0F && c.x < 0.0F && d.x < 0.0F;
        const auto all_right = a.x > 160.0F && b.x > 160.0F &&
                               c.x > 160.0F && d.x > 160.0F;
        const auto all_above = a.y < 0.0F && b.y < 0.0F && c.y < 0.0F && d.y < 0.0F;
        const auto all_below = a.y > 144.0F && b.y > 144.0F &&
                               c.y > 144.0F && d.y > 144.0F;
        if (all_left || all_right || all_above || all_below) return;
        const auto base = static_cast<int>(vertices.size());
        vertices.push_back({a, color, {0.0F, 0.0F}});
        vertices.push_back({b, color, {0.0F, 0.0F}});
        vertices.push_back({c, color, {0.0F, 0.0F}});
        vertices.push_back({d, color, {0.0F, 0.0F}});
        indices.insert(indices.end(), {base, base + 1, base + 2,
                                       base, base + 2, base + 3});
    };
    // Build a relief from the *visible framebuffer* rather than averaging an
    // entire background tile into one block. Tile averages erase the
    // silhouettes and text that make a Game Boy scene readable. One-pixel
    // Shape-aware cells receive stronger edge relief while layer depth
    // supplies the diorama separation.
    struct VoxelColumn {
        float x{};
        float y{};
        float width{8.0F};
        float extent_y{8.0F};
        float height{};
        float sort_depth{};
        std::size_t stable_order{};
        std::uint32_t color{};
        bool sprite{};
        bool window{};
        bool object{};
        bool relief{};
        float importance{};
    };
    std::vector<VoxelColumn> columns;
    // All diorama modes retain native source-pixel geometry. The flat
    // framebuffer is drawn as one textured plane below, so it does not need
    // one background quad per source pixel.
    constexpr unsigned cell_size = 1U;
    const unsigned cells_x = gameboy::Ppu::screen_width / cell_size;
    const unsigned cells_y = gameboy::Ppu::screen_height / cell_size;
    columns.reserve(cells_x * cells_y);
    std::vector<float> column_heights(cells_x * cells_y, 0.0F);
    std::vector<bool> sprite_mask(gameboy::Ppu::screen_width *
                                      gameboy::Ppu::screen_height);
    std::vector<int> sprite_anchor_y(gameboy::Ppu::screen_width *
                                         gameboy::Ppu::screen_height, -1);
    std::vector<bool> popup_object_mask(gameboy::Ppu::screen_width *
                                         gameboy::Ppu::screen_height);
    std::vector<int> popup_object_anchor_y(gameboy::Ppu::screen_width *
                                               gameboy::Ppu::screen_height, -1);
    const auto popup_hud_pixel = [&](const std::size_t pixel) {
        const auto y = pixel / gameboy::Ppu::screen_width;
        return y < profile.popup_hud_top_rows ||
               y >= gameboy::Ppu::screen_height -
                        std::min<std::size_t>(profile.popup_hud_bottom_rows,
                                              gameboy::Ppu::screen_height);
    };
    std::vector<bool> window_mask(gameboy::Ppu::screen_width *
                                      gameboy::Ppu::screen_height);
    const auto sprite_height = (scene.lcdc & 0x04U) != 0 ? 16 : 8;
    const auto sprite_pixel_opaque = [&](const gbb::SceneSprite& sprite,
                                         const int x, const int y) {
        if (scene.tile_size_bytes == 0 || scene.tile_data.empty()) return false;
        const auto source_x = (sprite.attributes & 0x20U) != 0 ? 7 - x : x;
        const auto source_y = (sprite.attributes & 0x40U) != 0
                                  ? sprite_height - 1 - y
                                  : y;
        const auto tile = sprite_height == 16
                              ? static_cast<unsigned>(sprite.tile & 0xFEU)
                              : static_cast<unsigned>(sprite.tile);
        const auto tile_index = tile + static_cast<unsigned>(source_y / 8);
        const auto bank = scene.cgb_mode && (sprite.attributes & 0x08U) != 0
                              ? 1U
                              : 0U;
        const auto offset = static_cast<std::size_t>(bank) *
                                scene.tile_bank_stride +
                            static_cast<std::size_t>(tile_index) *
                                scene.tile_size_bytes +
                            static_cast<std::size_t>(source_y % 8) * 2U;
        if (offset + 1 >= scene.tile_data.size()) return false;
        const auto bit = static_cast<unsigned>(7 - source_x);
        const auto low = (scene.tile_data[offset] >> bit) & 0x01U;
        const auto high = (scene.tile_data[offset + 1] >> bit) & 0x01U;
        return (low | (high << 1U)) != 0;
    };
    for (const auto& sprite : scene.sprites) {
        if (!sprite.visible) continue;
        for (int local_y = 0; local_y < sprite_height; ++local_y) {
            for (int local_x = 0; local_x < 8; ++local_x) {
                if (!sprite_pixel_opaque(sprite, local_x, local_y)) continue;
                const auto x = static_cast<int>(sprite.screen_x) + local_x;
                const auto y = static_cast<int>(sprite.screen_y) + local_y;
                if (x < 0 || y < 0 ||
                    x >= static_cast<int>(gameboy::Ppu::screen_width) ||
                    y >= static_cast<int>(gameboy::Ppu::screen_height)) {
                    continue;
                }
                sprite_mask[static_cast<std::size_t>(y) *
                                gameboy::Ppu::screen_width +
                             static_cast<std::size_t>(x)] = true;
                sprite_anchor_y[static_cast<std::size_t>(y) *
                                    gameboy::Ppu::screen_width +
                                static_cast<std::size_t>(x)] =
                    std::max(sprite_anchor_y[static_cast<std::size_t>(y) *
                                                  gameboy::Ppu::screen_width +
                                              static_cast<std::size_t>(x)],
                             static_cast<int>(sprite.screen_y) + sprite_height);
            }
        }
    }
    // The hardware window is a separate tile layer and is the closest
    // analogue to an in-game menu/overlay. Mark its visible screen region so
    // the diorama can place it in front of the background without guessing
    // from pixel brightness alone.
    if (scene.window.enabled) {
        const auto window_left = std::clamp(
            static_cast<int>(scene.wx) - 7, 0,
            static_cast<int>(gameboy::Ppu::screen_width));
        const auto window_top = std::clamp(
            static_cast<int>(scene.wy), 0,
            static_cast<int>(gameboy::Ppu::screen_height));
        for (int y = window_top;
             y < static_cast<int>(gameboy::Ppu::screen_height); ++y) {
            for (int x = window_left;
                 x < static_cast<int>(gameboy::Ppu::screen_width); ++x) {
                window_mask[static_cast<std::size_t>(y) *
                                gameboy::Ppu::screen_width +
                            static_cast<std::size_t>(x)] = true;
            }
        }
    }
    const auto luminance = [](const std::uint32_t pixel) {
        const auto red = static_cast<float>((pixel >> 16) & 0xFFU);
        const auto green = static_cast<float>((pixel >> 8) & 0xFFU);
        const auto blue = static_cast<float>(pixel & 0xFFU);
        return (0.2126F * red + 0.7152F * green + 0.0722F * blue) / 255.0F;
    };
    const auto pixel_count = gameboy::Ppu::screen_width *
                             gameboy::Ppu::screen_height;
    std::vector<float> pixel_luminance(pixel_count);
    for (std::size_t index = 0; index < pixel_count; ++index) {
        pixel_luminance[index] = luminance(colored_pixels[index]);
    }
    const auto pixel_luminance_at = [&](const int x, const int y) {
        const auto clamped_x = std::clamp(
            x, 0, static_cast<int>(gameboy::Ppu::screen_width) - 1);
        const auto clamped_y = std::clamp(
            y, 0, static_cast<int>(gameboy::Ppu::screen_height) - 1);
        return pixel_luminance[
            static_cast<std::size_t>(clamped_y) * gameboy::Ppu::screen_width +
            static_cast<std::size_t>(clamped_x)];
    };
    // Reconstruct the flat background under raised pixels. A nearby dominant
    // background sample is used so window/object geometry does not leave a
    // colored copy embedded in the recessed plane beneath it.
    std::vector<std::uint32_t> background_pixels(colored_pixels.begin(),
                                                 colored_pixels.end());
    const auto background_candidate = [&](const std::size_t index) {
        return backdrop_key(colored_pixels[index]) == backdrop_color_key &&
               !sprite_mask[index] && !window_mask[index];
    };
    // The old implementation searched four probes at every radius for every
    // foreground pixel. On mobile that repeated work dominated the voxel
    // frame even though the search result was only used to flatten the same
    // background plane. Cache the nearest eligible pixel in each direction
    // with four linear scans. Selection below retains the old left, right,
    // up, down tie-break order and the same 16-pixel search radius.
    std::vector<int> nearest_left(pixel_count, -1);
    std::vector<int> nearest_right(pixel_count, -1);
    std::vector<int> nearest_up(pixel_count, -1);
    std::vector<int> nearest_down(pixel_count, -1);
    for (std::size_t y = 0; y < gameboy::Ppu::screen_height; ++y) {
        int last = -1;
        for (std::size_t x = 0; x < gameboy::Ppu::screen_width; ++x) {
            const auto index = y * gameboy::Ppu::screen_width + x;
            nearest_left[index] = last;
            if (background_candidate(index)) last = static_cast<int>(index);
        }
        last = -1;
        for (std::size_t x = gameboy::Ppu::screen_width; x-- > 0;) {
            const auto index = y * gameboy::Ppu::screen_width + x;
            nearest_right[index] = last;
            if (background_candidate(index)) last = static_cast<int>(index);
        }
    }
    for (std::size_t x = 0; x < gameboy::Ppu::screen_width; ++x) {
        int last = -1;
        for (std::size_t y = 0; y < gameboy::Ppu::screen_height; ++y) {
            const auto index = y * gameboy::Ppu::screen_width + x;
            nearest_up[index] = last;
            if (background_candidate(index)) last = static_cast<int>(index);
        }
        last = -1;
        for (std::size_t y = gameboy::Ppu::screen_height; y-- > 0;) {
            const auto index = y * gameboy::Ppu::screen_width + x;
            nearest_down[index] = last;
            if (background_candidate(index)) last = static_cast<int>(index);
        }
    }
    const auto nearest_background = [&](const int source_x,
                                        const int source_y,
                                        const std::size_t source_index) {
        int best_distance = 17;
        auto best = backdrop_color;
        const auto consider = [&](const int candidate_index,
                                  const int distance) {
            if (candidate_index >= 0 && distance < best_distance) {
                best_distance = distance;
                best = colored_pixels[static_cast<std::size_t>(candidate_index)];
            }
        };
        const auto left = nearest_left[source_index];
        consider(left, left < 0 ? 17 : source_x -
                                      static_cast<int>(left %
                                                       gameboy::Ppu::screen_width));
        const auto right = nearest_right[source_index];
        consider(right, right < 0 ? 17 :
                                      static_cast<int>(right %
                                                       gameboy::Ppu::screen_width) -
                                          source_x);
        const auto up = nearest_up[source_index];
        consider(up, up < 0 ? 17 : source_y -
                                    static_cast<int>(up /
                                                     gameboy::Ppu::screen_width));
        const auto down = nearest_down[source_index];
        consider(down, down < 0 ? 17 :
                                      static_cast<int>(down /
                                                       gameboy::Ppu::screen_width) -
                                          source_y);
        return best;
    };
    for (int y = 0; y < static_cast<int>(gameboy::Ppu::screen_height); ++y) {
        for (int x = 0; x < static_cast<int>(gameboy::Ppu::screen_width); ++x) {
            const auto index = static_cast<std::size_t>(y) *
                               gameboy::Ppu::screen_width +
                               static_cast<std::size_t>(x);
            if (backdrop_key(colored_pixels[index]) != backdrop_color_key ||
                sprite_mask[index]) {
                background_pixels[index] = nearest_background(x, y, index);
            }
        }
    }
    if (popup_book && profile.background_object_detection &&
        scene.width == gameboy::Ppu::screen_width &&
        scene.height == gameboy::Ppu::screen_height) {
        for (std::size_t pixel = 0; pixel < voxel_scene.object_owner.size();
             ++pixel) {
            const auto owner = voxel_scene.object_owner[pixel];
            if (owner < 0 ||
                sprite_mask[pixel] ||
                popup_hud_pixel(pixel) ||
                static_cast<std::size_t>(owner) >= voxel_scene.objects.size() ||
                voxel_scene.objects[static_cast<std::size_t>(owner)].kind !=
                    gbb::VoxelObjectKind::background_object)
                continue;
            popup_object_mask[pixel] = true;
            popup_object_anchor_y[pixel] =
                voxel_scene.objects[static_cast<std::size_t>(owner)].anchor_y;
        }
    } else if (popup_book && context.scene_snapshot.visible_tile_cells.empty()) {
        // Tile-layer artwork in overhead games (buildings, trees, signs and
        // terrain edges) is not represented by OAM.  Split non-backdrop
        // pixels into connected shapes so substantial shapes can become
        // upright pop-up cut-outs while small dithering remains on the page.
        const auto pixel_count = gameboy::Ppu::screen_width *
                                 gameboy::Ppu::screen_height;
        std::vector<bool> visited(pixel_count);
        std::vector<std::size_t> pending;
        pending.reserve(pixel_count);
        for (int start_y = 0;
             start_y < static_cast<int>(gameboy::Ppu::screen_height);
             ++start_y) {
            for (int start_x = 0;
                 start_x < static_cast<int>(gameboy::Ppu::screen_width);
                 ++start_x) {
                const auto start = static_cast<std::size_t>(start_y) *
                                       gameboy::Ppu::screen_width +
                                   static_cast<std::size_t>(start_x);
                if (visited[start] || sprite_mask[start] ||
                    popup_hud_pixel(start) ||
                    backdrop_key(colored_pixels[start]) == backdrop_color_key) {
                    visited[start] = true;
                    continue;
                }
                pending.clear();
                pending.push_back(start);
                visited[start] = true;
                int max_y = start_y;
                std::size_t cursor = 0;
                while (cursor < pending.size()) {
                    const auto index = pending[cursor++];
                    const auto x = static_cast<int>(index %
                                                    gameboy::Ppu::screen_width);
                    const auto y = static_cast<int>(index /
                                                    gameboy::Ppu::screen_width);
                    max_y = std::max(max_y, y);
                    for (const auto [dx, dy] :
                         std::array<std::pair<int, int>, 4>{{
                             {-1, 0}, {1, 0}, {0, -1}, {0, 1}}}) {
                        const auto nx = x + dx;
                        const auto ny = y + dy;
                        if (nx < 0 || ny < 0 ||
                            nx >= static_cast<int>(gameboy::Ppu::screen_width) ||
                            ny >= static_cast<int>(gameboy::Ppu::screen_height)) {
                            continue;
                        }
                        const auto neighbour = static_cast<std::size_t>(ny) *
                                                   gameboy::Ppu::screen_width +
                                               static_cast<std::size_t>(nx);
                        if (visited[neighbour] || sprite_mask[neighbour] ||
                            popup_hud_pixel(neighbour) ||
                            backdrop_key(colored_pixels[neighbour]) ==
                                backdrop_color_key) {
                            continue;
                        }
                        visited[neighbour] = true;
                        pending.push_back(neighbour);
                    }
                }
                // A minimum area filters isolated anti-aliasing/dither
                // pixels. The shape's bottom is its page hinge.
                if (pending.size() < 6U) continue;
                const auto anchor_y = max_y + 1;
                for (const auto index : pending) {
                    if (sprite_mask[index] || popup_hud_pixel(index)) continue;
                    popup_object_mask[index] = true;
                    popup_object_anchor_y[index] = anchor_y;
                }
            }
        }
    }
    // Layer spans are derived from the configured far/near ranges. The
    // resulting planes are contiguous and ordered even when ROM-specific
    // profiles use different logical depth numbers.
    const auto background_span = std::clamp(
        profile.background_depth_far - profile.background_depth_near,
        1.0F, 200.0F) * 0.10F;
    const auto window_span = std::clamp(
        profile.window_depth_far - profile.window_depth_near, 1.0F, 200.0F) *
                             0.10F;
    const auto sprite_span = std::clamp(
        profile.sprite_depth_far - profile.sprite_depth_near, 1.0F, 200.0F) *
                             0.20F;
    constexpr float window_gap = 0.75F;
    constexpr float sprite_gap = 0.75F;
    // Pull the complete sprite band slightly toward the background. This
    // keeps sprites in front of the window layer while avoiding the detached
    // "floating in air" look caused by an overly large inter-layer offset.
    const auto sprite_pullback = std::min(
        3.0F, std::max(0.0F, sprite_gap + sprite_span * 0.60F - 0.25F));
    const auto geometry_build_started = std::chrono::steady_clock::now();
    for (unsigned cell_y = 0; cell_y < cells_y; ++cell_y) {
        for (unsigned cell_x = 0; cell_x < cells_x; ++cell_x) {
            unsigned red = 0;
            unsigned green = 0;
            unsigned blue = 0;
            float local_luminance = 0.0F;
            bool has_sprite = false;
            bool has_window = false;
            bool has_artwork = false;
            for (unsigned y = 0; y < cell_size; ++y) {
                for (unsigned x = 0; x < cell_size; ++x) {
                    const auto pixel_x = cell_x * cell_size + x;
                    const auto pixel_y = cell_y * cell_size + y;
                    const auto pixel = colored_pixels[pixel_y *
                                                      gameboy::Ppu::screen_width +
                                                      pixel_x];
                    const auto pixel_luminance = luminance(pixel);
                    red += (pixel >> 16) & 0xFFU;
                    green += (pixel >> 8) & 0xFFU;
                    blue += pixel & 0xFFU;
                    local_luminance += pixel_luminance;
                    has_sprite = has_sprite ||
                                 sprite_mask[pixel_y * gameboy::Ppu::screen_width +
                                             pixel_x];
                    has_window = has_window ||
                                 window_mask[pixel_y * gameboy::Ppu::screen_width +
                                             pixel_x];
                    has_artwork = has_artwork ||
                                 backdrop_key(pixel) != backdrop_color_key;
                }
            }
            const auto color = UINT32_C(0xFF000000) |
                               ((red / (cell_size * cell_size)) << 16) |
                               ((green / (cell_size * cell_size)) << 8) |
                               (blue / (cell_size * cell_size));
            local_luminance /= static_cast<float>(cell_size * cell_size);
            // Ownership is explicit: the background is flat, window pixels
            // sit just above it, and sprites/objects form the foreground.
            // Window background-colored pixels remain part of the flat layer
            // so a full hardware window rectangle does not become a slab.
            const auto window_layer = has_window && has_artwork && !has_sprite;
            // OAM ownership identifies the foreground object layer. Other
            // non-background-colored pixels remain part of the static tile
            // layer and use its configurable depth range.
            const auto object_index = static_cast<std::size_t>(cell_y * cell_size) *
                                          gameboy::Ppu::screen_width +
                                      static_cast<std::size_t>(cell_x * cell_size);
            const auto object_layer = has_sprite ||
                                      (popup_book && popup_object_mask[object_index]);
            const auto raised_artwork = object_layer;
            const auto x = static_cast<float>(cell_x * cell_size);
            const auto y = static_cast<float>(cell_y * cell_size);
            // At native 1x1 resolution, contrast within a cell is necessarily
            // zero. Sample the surrounding 3x3 neighborhood instead so only
            // actual silhouettes and tile edges gain relief; flat sky and HUD
            // fills remain on the recessed background plane.
            const auto pixel_x = static_cast<int>(x);
            const auto pixel_y = static_cast<int>(y);
            float neighborhood_min = 1.0F;
            float neighborhood_max = 0.0F;
            for (int offset_y = -1; offset_y <= 1; ++offset_y) {
                for (int offset_x = -1; offset_x <= 1; ++offset_x) {
                    const auto neighbor_luminance = pixel_luminance_at(
                        pixel_x + offset_x, pixel_y + offset_y);
                    neighborhood_min = std::min(neighborhood_min,
                                                neighbor_luminance);
                    neighborhood_max = std::max(neighborhood_max,
                                                neighbor_luminance);
                }
            }
            const auto local_contrast = neighborhood_max - neighborhood_min;
            const auto relief = std::max(
                0.0F, local_contrast *
                           (4.0F + (1.0F - local_luminance) * 4.0F));
            // Separate layer instances keep the static background flat while
            // giving the window and object/sprite layers independent offsets
            // and extrusion budgets.
            // Shape-aware mode keeps every source pixel, but gives genuine
            // edges a little more volume.  This produces cube-like forms
            // without the silhouette loss caused by 2x2 framebuffer cells.
            const auto surface_relief = std::min(
                relief * (shape_aware ? 1.55F : 1.20F),
                shape_aware ? 7.5F : 6.0F);
            const auto normalized_band = [](const float value,
                                            const float far_depth,
                                            const float near_depth) {
                return std::clamp((far_depth - value) /
                                      std::max(0.01F,
                                               far_depth - near_depth),
                                  0.0F, 1.0F);
            };
            // Bands are normalized independently, then placed contiguously in
            // front-to-back order. This keeps user-configured ranges readable
            // while guaranteeing background < window < sprites in depth.
            const auto background_position =
                ((!popup_book && has_artwork) || raised_artwork)
                    ? 1.0F
                    : normalized_band(
                                                       profile.background_transparent_depth,
                                                       profile.background_depth_far,
                                                       profile.background_depth_near);
            const auto window_position = std::clamp(
                0.35F + surface_relief / 10.0F, 0.0F, 1.0F);
            const auto sprite_position = std::clamp(
                0.60F + surface_relief / 10.0F +
                    std::min(profile.sprite_depth * 0.02F, 0.20F),
                0.0F, 1.0F);
            const auto layer_height = window_layer
                                          ? background_span + window_gap +
                                                window_span * window_position
                                          : object_layer
                                                ? background_span + window_gap +
                                                      window_span + sprite_gap -
                                                      sprite_pullback +
                                                      sprite_span * sprite_position
                                                : background_span *
                                                      background_position;
            // Shape-aware mode is intended to read as a relief rather than a
            // nearly flush height map. Pull non-backdrop artwork toward the
            // viewer while leaving the dominant backdrop plane untouched.
            const auto shape_depth_boost = shape_aware &&
                                                   (popup_book ? raised_artwork
                                                               : has_artwork)
                                               ? 0.45F
                                               : 0.0F;
            auto depth = base_depth - profile.depth_scale *
                                             (layer_height + shape_depth_boost);
            if (popup_book && object_layer) {
                const auto source_index = static_cast<std::size_t>(cell_y * cell_size) *
                                              gameboy::Ppu::screen_width +
                                          static_cast<std::size_t>(cell_x * cell_size);
                const auto anchor_y = has_sprite && sprite_anchor_y[source_index] >= 0
                                          ? sprite_anchor_y[source_index]
                                          : popup_object_anchor_y[source_index] >= 0
                                                ? popup_object_anchor_y[source_index]
                                                : static_cast<int>(y) + sprite_height;
                // Sprite pixels are taller than static cut-outs so a player
                // reads as a distinct foreground character.
                const auto pixel_height = has_sprite ? profile.popup_sprite_height
                                                     : profile.popup_object_height;
                const auto pixel_bottom = std::max(
                    0.0F, static_cast<float>(anchor_y) -
                               static_cast<float>(cell_y * cell_size + 1U)) *
                           pixel_height;
                depth = base_depth - pixel_bottom - pixel_height;
            }
            const auto centered_y = y + cell_size * 0.5F - 72.0F;
            const auto centered_x = x + cell_size * 0.5F - 80.0F;
            const auto page_depth = popup_book ? -centered_y * profile.popup_parallax
                                               : centered_y * 0.82F;
            const auto world_height = base_depth - depth;
            // Use the same camera-space depth as project(). The old
            // non-popup expression mixed screen Y into the yaw rotation and
            // voxel height into pitch depth, so farther faces could paint
            // over nearer ones (most visibly on Android/GLES).
            const auto camera_depth = popup_book
                                          ? centered_x * yaw_sin +
                                                page_depth * yaw_cos
                                          : centered_x * yaw_sin +
                                                depth * yaw_cos;
            const auto sort_depth = popup_book
                                        ? world_height * pitch_sin +
                                              camera_depth * pitch_cos
                                        : centered_y * pitch_sin +
                                              camera_depth * pitch_cos;
            column_heights[cell_y * cells_x + cell_x] = depth;
            // Flat backdrop columns never reach the geometry loop below. Do
            // not sort or retain them; their heights remain in the parallel
            // map for exposed-wall comparisons. This is a pure workset
            // reduction and does not alter submitted geometry.
            // The textured plane already preserves every source pixel. Only
            // turn visible contrast into geometry; otherwise every anti-
            // aliased artwork pixel becomes a separate one-pixel wall. This
            // keeps silhouettes and sprites intact while removing the noisy
            // mesh that was saturating mobile GPUs.
            const auto relief_threshold = shape_aware ? 0.20F : 0.35F;
            if (window_layer || object_layer ||
                (has_artwork && surface_relief >= relief_threshold)) {
                columns.push_back({x, y, static_cast<float>(cell_size),
                                   static_cast<float>(cell_size), depth,
                                   sort_depth, columns.size(), color, has_sprite,
                                   window_layer,
                                   object_layer, true, surface_relief});
            }
        }
    }
    const auto geometry_build_finished = std::chrono::steady_clock::now();
    context.voxel_stats.geometry_build_us = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            geometry_build_finished - geometry_build_started)
            .count());
    // The recessed framebuffer is flat geometry. Upload it once as a texture
    // instead of emitting one color quad per source cell; this removes the
    // dominant per-frame cost while preserving the exact source-pixel image.
    if (!SDL_UpdateTexture(context.texture, nullptr, background_pixels.data(),
                           static_cast<int>(gameboy::Ppu::screen_width *
                                            sizeof(std::uint32_t)))) {
        return false;
    }
    bool render_target_active = false;
    bool use_render_target = voxel_render_target_cache_supported &&
                             context.render_target != nullptr;
    const auto restore_render_target = [&]() {
        if (!render_target_active) return true;
        render_target_active = false;
        return SDL_SetRenderTarget(context.renderer, nullptr);
    };
    if (use_render_target) {
        if (!SDL_SetRenderTarget(context.renderer, context.render_target) ||
            !SDL_SetRenderDrawColor(context.renderer, 16, 20, 16, 0) ||
            !SDL_RenderClear(context.renderer)) {
            static_cast<void>(SDL_SetRenderTarget(context.renderer, nullptr));
            use_render_target = false;
        }
        if (use_render_target) render_target_active = true;
    }
    bool output_transform_active = false;
    if (context.output_rect_valid && !use_render_target) {
        const SDL_Rect viewport{
            static_cast<int>(std::lround(context.output_rect.x)),
            static_cast<int>(std::lround(context.output_rect.y)),
            std::max(1, static_cast<int>(std::lround(context.output_rect.w))),
            std::max(1, static_cast<int>(std::lround(context.output_rect.h)))};
        if (!SDL_SetRenderViewport(context.renderer, &viewport) ||
            !SDL_SetRenderScale(context.renderer,
                                context.output_rect.w / 160.0F,
                                context.output_rect.h / 144.0F)) {
            static_cast<void>(SDL_SetRenderViewport(context.renderer, nullptr));
            static_cast<void>(SDL_SetRenderScale(context.renderer, 1.0F, 1.0F));
            return false;
        }
        output_transform_active = true;
    }
    const auto restore_output_transform = [&]() {
        if (!output_transform_active) return true;
        output_transform_active = false;
        return SDL_SetRenderScale(context.renderer, 1.0F, 1.0F) &&
               SDL_SetRenderViewport(context.renderer, nullptr);
    };
    const std::array<SDL_Vertex, 4> background_vertices{{
        {project(0.0F, 0.0F, base_depth), {1.0F, 1.0F, 1.0F, 1.0F}, {0.0F, 0.0F}},
        {project(static_cast<float>(gameboy::Ppu::screen_width), 0.0F,
                 base_depth), {1.0F, 1.0F, 1.0F, 1.0F}, {1.0F, 0.0F}},
        {project(static_cast<float>(gameboy::Ppu::screen_width),
                 static_cast<float>(gameboy::Ppu::screen_height), base_depth),
         {1.0F, 1.0F, 1.0F, 1.0F}, {1.0F, 1.0F}},
        {project(0.0F, static_cast<float>(gameboy::Ppu::screen_height),
                 base_depth), {1.0F, 1.0F, 1.0F, 1.0F}, {0.0F, 1.0F}}}};
    constexpr std::array<int, 6> background_indices{{0, 1, 2, 0, 2, 3}};
    if (!SDL_RenderGeometry(context.renderer, context.texture,
                             background_vertices.data(),
                             static_cast<int>(background_vertices.size()),
                             background_indices.data(),
                             static_cast<int>(background_indices.size()))) {
        static_cast<void>(restore_render_target());
        static_cast<void>(restore_output_transform());
        return false;
    }
    // SDL geometry has no portable depth buffer. Painter ordering gives us
    // deterministic opaque occlusion on software, OpenGL, D3D, Metal and
    // Vulkan renderers alike: farther columns are submitted first.
    const auto geometry_sort_started = std::chrono::steady_clock::now();
    std::sort(columns.begin(), columns.end(),
              [](const VoxelColumn& left, const VoxelColumn& right) {
                         const auto layer_rank = [](const VoxelColumn& column) {
                            return column.sprite ? 3 : column.object ? 2 :
                                                     column.window ? 1 : 0;
                         };
                         const auto left_rank = layer_rank(left);
                         const auto right_rank = layer_rank(right);
                         if (left_rank != right_rank)
                             return left_rank < right_rank;
                         if (left.sort_depth != right.sort_depth)
                             return left.sort_depth > right.sort_depth;
                         return left.stable_order < right.stable_order;
                     });
    const auto geometry_sort_finished = std::chrono::steady_clock::now();
    context.voxel_stats.geometry_sort_us = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            geometry_sort_finished - geometry_sort_started)
            .count());
    const auto height_at = [&](const int cell_x, const int cell_y) {
        if (cell_x < 0 || cell_y < 0 ||
            cell_x >= static_cast<int>(cells_x) ||
            cell_y >= static_cast<int>(cells_y)) {
            return base_depth;
        }
        return column_heights[static_cast<std::size_t>(cell_y) * cells_x +
                              static_cast<std::size_t>(cell_x)];
    };
    for (const auto& column : columns) {
        if (!column.relief || column.height >= base_depth - 0.15F) continue;
        const auto x = column.x;
        const auto y = column.y;
        const auto depth = column.height;
        const auto width = column.width;
        const auto extent_y = column.extent_y;
        if (popup_book && column.object) {
            const auto source_index = static_cast<std::size_t>(y) *
                                          gameboy::Ppu::screen_width +
                                      static_cast<std::size_t>(x);
            const auto anchor_y = column.sprite && sprite_anchor_y[source_index] >= 0
                                      ? sprite_anchor_y[source_index]
                                      : popup_object_anchor_y[source_index] >= 0
                                            ? popup_object_anchor_y[source_index]
                                            : static_cast<int>(y) + sprite_height;
            {
                // Pop-up objects are upright cuboids. The page coordinate is
                // anchored at each shape's feet, while source rows become
                // vertical height. Static cut-outs use a shallower voxel
                // height; OAM sprites retain a taller, readable silhouette.
                const auto sprite_pixel_height = column.sprite
                                                     ? profile.popup_sprite_height
                                                     : profile.popup_object_height;
                const auto pixel_bottom = std::max(
                    0.0F, static_cast<float>(anchor_y) - (y + 1.0F)) *
                           sprite_pixel_height;
                const auto pixel_top = pixel_bottom + sprite_pixel_height;
                const auto extrusion = column.sprite
                                           ? profile.popup_sprite_thickness
                                           : profile.popup_card_thickness;
                const auto front_page = static_cast<float>(anchor_y) -
                                        extrusion * 0.5F;
                const auto back_page = static_cast<float>(anchor_y) +
                                       extrusion * 0.5F;
                const auto front_bottom_a =
                    project(x, front_page, base_depth - pixel_bottom);
                const auto front_bottom_b = project(
                    x + width, front_page, base_depth - pixel_bottom);
                const auto front_top_a =
                    project(x, front_page, base_depth - pixel_top);
                const auto front_top_b = project(
                    x + width, front_page, base_depth - pixel_top);
                const auto back_top_a =
                    project(x, back_page, base_depth - pixel_top);
                const auto back_top_b = project(
                    x + width, back_page, base_depth - pixel_top);
                const auto back_bottom_a =
                    project(x, back_page, base_depth - pixel_bottom);
                const auto back_bottom_b = project(
                    x + width, back_page, base_depth - pixel_bottom);
                const auto sprite_color = voxel_color(
                    column.color, 0.98F * profile.lighting,
                    luminance(column.color) < 0.20F
                        ? 0.0F
                        : voxel_ambient * profile.lighting);
                add_quad(front_bottom_a, front_bottom_b, front_top_b,
                         front_top_a, sprite_color);
                // Keep a real back surface as well as the front surface. This
                // makes tile-layer buildings solid when the camera is moved
                // around the opposite side instead of looking like one-sided
                // paper cut-outs.
                add_quad(back_bottom_a, back_bottom_b, back_top_b,
                         back_top_a,
                         voxel_color(column.color, 0.78F * profile.lighting,
                                     voxel_ambient * profile.lighting));
                const auto same_shape_pixel = [&](const int neighbour_x,
                                                  const int neighbour_y) {
                    if (column.sprite || neighbour_x < 0 || neighbour_y < 0 ||
                        neighbour_x >= static_cast<int>(gameboy::Ppu::screen_width) ||
                        neighbour_y >= static_cast<int>(gameboy::Ppu::screen_height)) {
                        return false;
                    }
                    const auto neighbour = static_cast<std::size_t>(neighbour_y) *
                                               gameboy::Ppu::screen_width +
                                           static_cast<std::size_t>(neighbour_x);
                    return popup_object_mask[neighbour] &&
                           popup_object_anchor_y[neighbour] == anchor_y;
                };
                const auto cap_color = voxel_color(
                    column.color, 0.84F * profile.lighting,
                    voxel_ambient * profile.lighting);
                const auto side_color = voxel_color(
                    column.color, 0.70F * profile.lighting,
                    voxel_ambient * profile.lighting);
                if (column.sprite || !same_shape_pixel(static_cast<int>(x),
                                                        static_cast<int>(y) - 1)) {
                    add_quad(front_top_a, front_top_b, back_top_b, back_top_a,
                             cap_color);
                }
                if (column.sprite || !same_shape_pixel(static_cast<int>(x) + 1,
                                                        static_cast<int>(y))) {
                    add_quad(front_bottom_b, back_bottom_b, back_top_b,
                             front_top_b, side_color);
                }
                if (column.sprite || !same_shape_pixel(static_cast<int>(x) - 1,
                                                        static_cast<int>(y))) {
                    add_quad(back_bottom_a, front_bottom_a, front_top_a,
                             back_top_a,
                             voxel_color(column.color, 0.58F * profile.lighting,
                                         voxel_ambient * profile.lighting));
                }
                if (!column.sprite && !same_shape_pixel(static_cast<int>(x),
                                                        static_cast<int>(y) + 1)) {
                    add_quad(back_bottom_a, back_bottom_b, front_bottom_b,
                             front_bottom_a,
                             voxel_color(column.color, 0.62F * profile.lighting,
                                         voxel_ambient * profile.lighting));
                }
                continue;
            }
        }
        const auto top_a = project(x, y, depth);
        const auto top_b = project(x + width, y, depth);
        const auto top_c = project(x + width, y + extent_y, depth);
        const auto top_d = project(x, y + extent_y, depth);
        // Each raised layer has its own gap and extrusion budget. Window
        // pixels hover just above the background; objects and sprites sit
        // farther forward without connecting to the recessed plane.
        const auto floating = column.sprite || column.window;
        const auto layer_base = column.sprite
                                    ? base_depth - profile.depth_scale *
                                          (background_span + window_gap +
                                           window_span + sprite_gap -
                                           sprite_pullback)
                                    : column.window
                                          ? base_depth - profile.depth_scale *
                                                (background_span + window_gap)
                                          : base_depth;
        const auto column_base = floating ? layer_base : base_depth;
        const auto base_a = project(x, y, column_base);
        const auto base_b = project(x + width, y, column_base);
        const auto base_c = project(x + width, y + extent_y, column_base);
        const auto base_d = project(x, y + extent_y, column_base);
        const auto tile_color = column.color;
        const auto border_color = [&](const int border_x,
                                      const int border_y) {
            if (border_x < 0 || border_y < 0 ||
                border_x >= static_cast<int>(gameboy::Ppu::screen_width) ||
                border_y >= static_cast<int>(gameboy::Ppu::screen_height)) {
                return backdrop_color;
            }
            return colored_pixels[static_cast<std::size_t>(border_y) *
                                      gameboy::Ppu::screen_width +
                                  static_cast<std::size_t>(border_x)];
        };
        // Sprite voxels are self-colored: their side faces continue the
        // sprite pixel's material instead of borrowing the background behind
        // the sprite. Window/object geometry keeps the bordering-pixel rule.
        const auto side_color = [&](const int border_x, const int border_y) {
            return column.sprite ? tile_color
                                  : border_color(border_x, border_y);
        };
        const auto grid_x = static_cast<int>(column.x) /
                            static_cast<int>(cell_size);
        const auto grid_y = static_cast<int>(column.y) /
                            static_cast<int>(cell_size);
        const auto source_x = static_cast<int>(column.x);
        const auto source_y = static_cast<int>(column.y);
        // Draw only exposed sides.  Earlier versions emitted all four walls
        // for every foreground pixel; at native 1x1 resolution that creates
        // a dark cross-hatched shadow around sprites and solid objects.  A
        // small height threshold keeps genuine silhouette edges while
        // suppressing internal relief noise.
        constexpr float wall_threshold = 0.60F;
        if (depth < height_at(grid_x, grid_y - 1) - wall_threshold) {
            add_quad(base_a, base_b, top_b, top_a,
                     voxel_color(side_color(source_x, source_y - 1),
                                 0.58F * profile.lighting,
                                 voxel_ambient * profile.lighting));
        }
        if (depth < height_at(grid_x + 1, grid_y) - wall_threshold) {
            add_quad(base_b, base_c, top_c, top_b,
                     voxel_color(side_color(source_x + static_cast<int>(cell_size), source_y),
                                 0.68F * profile.lighting,
                                 voxel_ambient * profile.lighting));
        }
        if (depth < height_at(grid_x, grid_y + 1) - wall_threshold) {
            add_quad(base_c, base_d, top_d, top_c,
                     voxel_color(side_color(source_x, source_y + static_cast<int>(cell_size)),
                                 0.76F * profile.lighting,
                                 voxel_ambient * profile.lighting));
        }
        if (depth < height_at(grid_x - 1, grid_y) - wall_threshold) {
            add_quad(base_d, base_a, top_a, top_d,
                     voxel_color(side_color(source_x - 1, source_y),
                                 0.62F * profile.lighting,
                                 voxel_ambient * profile.lighting));
        }
        // Preserve dark source pixels on their front faces. Ambient lighting
        // is reserved for voxel shading and must not turn black artwork gray.
        const auto top_ambient = luminance(tile_color) < 0.20F
                                     ? 0.0F
                                     : voxel_ambient * profile.lighting;
        add_quad(top_a, top_b, top_c, top_d,
                 voxel_color(tile_color,
                             (column.sprite ? 0.98F : 0.96F) * profile.lighting,
                             top_ambient));
    }
    const auto geometry_submit_started = std::chrono::steady_clock::now();
    if (!indices.empty() && !SDL_RenderGeometry(
                                 context.renderer, nullptr, vertices.data(),
                                 static_cast<int>(vertices.size()), indices.data(),
                                 static_cast<int>(indices.size()))) {
        static_cast<void>(restore_render_target());
        static_cast<void>(restore_output_transform());
        return false;
    }
    const auto geometry_submit_finished = std::chrono::steady_clock::now();
    context.voxel_stats.geometry_submit_us = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            geometry_submit_finished - geometry_submit_started)
            .count());
    context.voxel_stats.mesh_vertices = vertices.size();
    context.voxel_stats.mesh_indices = indices.size();
    if (scene_rebuilt && profile.background_debug_overlay) {
        std::ostringstream message;
        message << "Voxel popup stats: objects="
                << context.voxel_stats.accepted_objects
                << " candidates=" << context.voxel_stats.candidate_objects
                << " vertices=" << context.voxel_stats.mesh_vertices
                << " indices=" << context.voxel_stats.mesh_indices;
        gbb::log_frontend_info(message.str());
    }
    if (profile.framebuffer_facade) {
        if (!SDL_UpdateTexture(context.texture, nullptr, colored_pixels.data(),
                               static_cast<int>(160 * sizeof(std::uint32_t))) ||
            !SDL_RenderTexture(context.renderer, context.texture, nullptr, nullptr)) {
            static_cast<void>(restore_render_target());
            static_cast<void>(restore_output_transform());
            return false;
        }
    }
    if (popup_book && profile.background_debug_overlay &&
        scene.width == gameboy::Ppu::screen_width &&
        scene.height == gameboy::Ppu::screen_height) {
        // Diagnostics are deliberately opt-in. Rejected candidates are red;
        // accepted profile/generic objects are cyan. Drawing source-space
        // bounds through the same camera makes it possible to tell whether a
        // bad result came from provenance classification or projection.
        SDL_SetRenderDrawBlendMode(context.renderer, SDL_BLENDMODE_BLEND);
        const auto is_accepted = [&](const gbb::VoxelObject& candidate) {
            return std::any_of(
                voxel_scene.objects.begin(), voxel_scene.objects.end(),
                [&](const gbb::VoxelObject& object) {
                    return object.kind == gbb::VoxelObjectKind::background_object &&
                           object.id == candidate.id;
                });
        };
        const auto draw_box = [&](const gbb::VoxelObject& object,
                                  const SDL_Color color) {
            const auto left_top = project(object.min_x, object.min_y,
                                          base_depth - 0.25F);
            const auto right_top = project(object.max_x, object.min_y,
                                           base_depth - 0.25F);
            const auto right_bottom = project(object.max_x, object.max_y,
                                              base_depth - 0.25F);
            const auto left_bottom = project(object.min_x, object.max_y,
                                             base_depth - 0.25F);
            SDL_SetRenderDrawColor(context.renderer, color.r, color.g, color.b,
                                   color.a);
            SDL_RenderLine(context.renderer, left_top.x, left_top.y,
                           right_top.x, right_top.y);
            SDL_RenderLine(context.renderer, right_top.x, right_top.y,
                           right_bottom.x, right_bottom.y);
            SDL_RenderLine(context.renderer, right_bottom.x, right_bottom.y,
                           left_bottom.x, left_bottom.y);
            SDL_RenderLine(context.renderer, left_bottom.x, left_bottom.y,
                           left_top.x, left_top.y);
        };
        for (const auto& candidate : voxel_scene.candidates) {
            if (!is_accepted(candidate))
                draw_box(candidate, SDL_Color{245, 75, 75, 220});
        }
        for (const auto& object : voxel_scene.objects) {
            if (object.kind == gbb::VoxelObjectKind::background_object)
                draw_box(object, SDL_Color{40, 190, 255, 235});
        }
        SDL_SetRenderDrawBlendMode(context.renderer, SDL_BLENDMODE_NONE);
    }
    if (!restore_render_target()) {
        static_cast<void>(restore_output_transform());
        return false;
    }
    if (!restore_output_transform()) return false;
    if (use_render_target) {
        if (!SDL_RenderTexture(context.renderer, context.render_target, nullptr,
                               context.output_rect_valid
                                   ? &context.output_rect
                                   : nullptr)) {
            return false;
        }
        context.voxel_render_cache_key = render_key;
        context.voxel_render_cache_state_key = source_state_key;
        context.voxel_render_cache_valid = true;
        context.voxel_render_cache_source_pixels.assign(source_pixels.begin(),
                                                        source_pixels.end());
    }
    const auto frame_finished = std::chrono::steady_clock::now();
    context.voxel_stats.total_us = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            frame_finished - frame_started)
            .count());
    return true;
}


} // namespace gbb::sdl
