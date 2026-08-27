#define SDL_MAIN_USE_CALLBACKS
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include "gameboy/emulator.hpp"
#include "gameboy/display_palette.hpp"
#include "gameboy/video_pipeline.hpp"
#include "gbb/core_registry.hpp"
#include "gbb/gameboy_core.hpp"
#include "gbb/scene_json.hpp"
#include "gbb/voxel_profile.hpp"
#include "gbb/audio.hpp"

#include <emscripten.h>
#include <emscripten/bind.h>
#include <emscripten/val.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cmath>
#include <exception>
#include <iomanip>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

constexpr std::array<gbb::InputId, 8> button_order{
    gbb::InputId::right, gbb::InputId::left, gbb::InputId::up,
    gbb::InputId::down, gbb::InputId::a, gbb::InputId::b,
    gbb::InputId::select, gbb::InputId::start,
};

struct WebApp {
    ~WebApp() {
        if (gamepad) SDL_CloseGamepad(gamepad);
        if (audio_stream) SDL_DestroyAudioStream(audio_stream);
        if (texture) SDL_DestroyTexture(texture);
        if (renderer) SDL_DestroyRenderer(renderer);
        if (window) SDL_DestroyWindow(window);
    }

    SDL_Window* window{};
    SDL_Renderer* renderer{};
    SDL_Texture* texture{};
    SDL_Gamepad* gamepad{};
    SDL_AudioStream* audio_stream{};
    std::unique_ptr<gbb::EmulatorCore> emulator;
    std::vector<std::uint32_t> display_pixels;
    std::vector<SDL_Vertex> voxel_vertices;
    std::vector<int> voxel_indices;
    float voxel_camera_pitch_offset{};
    float voxel_camera_yaw_offset{};
    std::chrono::steady_clock::time_point previous_time{
        std::chrono::steady_clock::now()};
    double cycle_credit{};
    std::size_t display_palette{};
    gameboy::VideoMode video_mode{gameboy::default_video_mode};
    bool paused{};
};

WebApp* active_app{};
unsigned requested_video_mode{};
std::string scene_snapshot_export;

void set_status(const std::string& message, bool error);

void apply_video_mode(WebApp& app, const unsigned mode) noexcept {
    if (mode >= gameboy::video_modes.size()) return;
    app.video_mode = gameboy::video_modes[mode].mode;
    if (!app.emulator || !app.texture) return;
    const auto& core = app.emulator->descriptor();
    const auto presentation = app.video_mode == gameboy::VideoMode::integer
                                  ? SDL_LOGICAL_PRESENTATION_INTEGER_SCALE
                                  : SDL_LOGICAL_PRESENTATION_LETTERBOX;
    const auto filtering = app.video_mode == gameboy::VideoMode::bilinear
                               ? SDL_SCALEMODE_LINEAR
                               : SDL_SCALEMODE_NEAREST;
    static_cast<void>(SDL_SetRenderLogicalPresentation(
        app.renderer, static_cast<int>(core.video_width),
        static_cast<int>(core.video_height), presentation));
    static_cast<void>(SDL_SetTextureScaleMode(app.texture, filtering));
}

SDL_FColor voxel_color(const std::uint32_t pixel, const float shade,
                       const float ambient = 0.0F) {
    const auto component = [pixel, shade, ambient](const unsigned shift) {
        const auto value = static_cast<float>((pixel >> shift) & 0xFFU) / 255.0F;
        return std::clamp(value * shade + ambient, 0.0F, 1.0F);
    };
    return {component(16), component(8), component(0), 1.0F};
}

void render_web_voxel(WebApp& app, const std::vector<std::uint32_t>& pixels,
                     const bool shape_aware = false,
                     const bool popup_book = false) {
    // Keep the browser presentation in lockstep with the desktop renderer.
    // Both use the same native-resolution relief, layer heights and painter
    // ordering; only the input framebuffer and SDL renderer differ.
    const auto profile =
        gbb::built_in_voxel_profile(app.emulator->rom_fingerprint());
    const auto depth_scale = profile.depth_scale;
    // In the projection a larger Z value is farther from the viewer. Keep
    // the recessed background at the far end, then subtract each layer's
    // height so windows and sprites move toward the viewer in that order.
    const auto base_depth = 8.0F * depth_scale;
    const auto zoom = profile.zoom;
    const auto perspective = profile.perspective;
    const auto camera_pitch = profile.camera_pitch;
    const auto camera_yaw = profile.camera_yaw;
    const auto sprite_depth = profile.sprite_depth;
    const auto lighting = profile.lighting;
    constexpr float voxel_ambient = 0.055F;
    const auto& scene = app.emulator->scene_snapshot();
    const auto yaw = (camera_yaw + app.voxel_camera_yaw_offset) *
                     0.01745329251994329577F;
    const auto pitch = (popup_book
                            ? -(camera_pitch + app.voxel_camera_pitch_offset +
                                20.0F)
                            : camera_pitch + app.voxel_camera_pitch_offset) *
                       0.01745329251994329577F;
    const auto yaw_cos = std::cos(yaw);
    const auto yaw_sin = std::sin(yaw);
    const auto pitch_cos = std::cos(pitch);
    const auto pitch_sin = std::sin(pitch);
    const auto project = [&](const float x, const float y, const float z) {
        const auto centered_x = x - 80.0F;
        const auto centered_y = y - 72.0F;
        if (popup_book) {
            // Source Y becomes page depth; the renderer's Z value is the
            // vertical lift above that page. This keeps the background flat
            // like a book page while windows and sprites stand above it.
            const auto page_depth = popup_book ? -centered_y * 0.82F
                                               : centered_y * 0.82F;
            const auto world_height = base_depth - z;
            const auto yaw_x = centered_x * yaw_cos - page_depth * yaw_sin;
            const auto yaw_depth = centered_x * yaw_sin + page_depth * yaw_cos;
            const auto pitched_y = world_height * pitch_cos -
                                   yaw_depth * pitch_sin;
            const auto depth = world_height * pitch_sin +
                               yaw_depth * pitch_cos;
            const auto scale = 1.0F /
                std::max(0.35F, 1.0F + depth * perspective);
            return SDL_FPoint{80.0F + yaw_x * zoom * scale,
                              72.0F - pitched_y * zoom * scale};
        }
        const auto yaw_x = centered_x * yaw_cos - z * yaw_sin;
        const auto yaw_depth = centered_x * yaw_sin + z * yaw_cos;
        const auto pitched_y = centered_y * pitch_cos - yaw_depth * pitch_sin;
        const auto depth = centered_y * pitch_sin + yaw_depth * pitch_cos;
        const auto scale = 1.0F /
            std::max(0.35F, 1.0F + depth * perspective);
        return SDL_FPoint{80.0F + yaw_x * zoom * scale,
                          72.0F + pitched_y * zoom * scale};
    };
    auto& vertices = app.voxel_vertices;
    auto& indices = app.voxel_indices;
    vertices.clear();
    indices.clear();
    vertices.reserve(120000);
    indices.reserve(180000);
    const auto add_quad = [&](const SDL_FPoint a, const SDL_FPoint b,
                              const SDL_FPoint c, const SDL_FPoint d,
                              const SDL_FColor color) {
        const auto base = static_cast<int>(vertices.size());
        vertices.push_back({a, color, {0.0F, 0.0F}});
        vertices.push_back({b, color, {0.0F, 0.0F}});
        vertices.push_back({c, color, {0.0F, 0.0F}});
        vertices.push_back({d, color, {0.0F, 0.0F}});
        indices.insert(indices.end(), {base, base + 1, base + 2,
                                       base, base + 2, base + 3});
    };
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
    for (unsigned y = 24; y < 124; ++y) {
        for (unsigned x = 0; x < 160; ++x) {
            const auto pixel = pixels[y * 160U + x];
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

    // Both modes keep native pixel silhouettes. The first shape-aware
    // prototype grouped pixels into 2x2 cells, which made thin
    // outlines, text and small sprites merge into chunky blobs.  The refined
    // mode uses one source pixel per column and expresses its shape through
    // layer-aware depth instead of framebuffer downsampling.
    const unsigned cell_size = 1U;
    const unsigned cells_x = 160U / cell_size;
    const unsigned cells_y = 144U / cell_size;
    struct VoxelColumn {
        float x{};
        float y{};
        float width{1.0F};
        float extent_y{1.0F};
        float height{};
        float sort_depth{};
        std::uint32_t color{};
        bool sprite{};
        bool window{};
        bool object{};
    };
    std::vector<VoxelColumn> columns;
    columns.reserve(cells_x * cells_y);
    std::vector<float> column_heights(cells_x * cells_y, 0.0F);
    std::vector<bool> sprite_mask(160U * 144U);
    std::vector<int> sprite_anchor_y(160U * 144U, -1);
    std::vector<bool> popup_object_mask(160U * 144U);
    std::vector<int> popup_object_anchor_y(160U * 144U, -1);
    std::vector<bool> window_mask(160U * 144U);
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
                if (x < 0 || y < 0 || x >= 160 || y >= 144) continue;
                sprite_mask[static_cast<std::size_t>(y) * 160U +
                            static_cast<std::size_t>(x)] = true;
                sprite_anchor_y[static_cast<std::size_t>(y) * 160U +
                                static_cast<std::size_t>(x)] =
                    std::max(sprite_anchor_y[static_cast<std::size_t>(y) * 160U +
                                                 static_cast<std::size_t>(x)],
                             static_cast<int>(sprite.screen_y) + sprite_height);
            }
        }
    }
    if (popup_book) {
        // Tile-layer artwork in overhead games (buildings, trees, signs and
        // terrain edges) is not represented by OAM. Split non-backdrop
        // pixels into connected shapes so substantial shapes become upright
        // pop-up cut-outs while small dithering remains on the page.
        const auto pixel_count = 160U * 144U;
        std::vector<bool> visited(pixel_count);
        std::vector<std::size_t> pending;
        pending.reserve(pixel_count);
        for (int start_y = 0; start_y < 144; ++start_y) {
            for (int start_x = 0; start_x < 160; ++start_x) {
                const auto start = static_cast<std::size_t>(start_y) * 160U +
                                   static_cast<std::size_t>(start_x);
                if (visited[start] || sprite_mask[start] ||
                    backdrop_key(pixels[start]) == backdrop_color_key) {
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
                    const auto x = static_cast<int>(index % 160U);
                    const auto y = static_cast<int>(index / 160U);
                    max_y = std::max(max_y, y);
                    for (const auto [dx, dy] :
                         std::array<std::pair<int, int>, 4>{{
                             {-1, 0}, {1, 0}, {0, -1}, {0, 1}}}) {
                        const auto nx = x + dx;
                        const auto ny = y + dy;
                        if (nx < 0 || ny < 0 || nx >= 160 || ny >= 144) continue;
                        const auto neighbour = static_cast<std::size_t>(ny) * 160U +
                                               static_cast<std::size_t>(nx);
                        if (visited[neighbour] || sprite_mask[neighbour] ||
                            backdrop_key(pixels[neighbour]) == backdrop_color_key) {
                            continue;
                        }
                        visited[neighbour] = true;
                        pending.push_back(neighbour);
                    }
                }
                if (pending.size() < 6U) continue;
                const auto anchor_y = max_y + 1;
                for (const auto index : pending) {
                    popup_object_mask[index] = true;
                    popup_object_anchor_y[index] = anchor_y;
                }
            }
        }
    }
    if (scene.window.enabled) {
        const auto window_left = std::clamp(static_cast<int>(scene.wx) - 7,
                                            0, 160);
        const auto window_top = std::clamp(static_cast<int>(scene.wy), 0, 144);
        for (int y = window_top; y < 144; ++y) {
            for (int x = window_left; x < 160; ++x) {
                window_mask[static_cast<std::size_t>(y) * 160U +
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
    const auto pixel_luminance_at = [&](const int x, const int y) {
        const auto clamped_x = std::clamp(x, 0, 159);
        const auto clamped_y = std::clamp(y, 0, 143);
        return luminance(pixels[static_cast<std::size_t>(clamped_y) * 160U +
                               static_cast<std::size_t>(clamped_x)]);
    };
    // Reconstruct the flat background under raised pixels. A nearby dominant
    // background sample is used so window/object geometry does not leave a
    // colored copy embedded in the recessed plane beneath it.
    std::vector<std::uint32_t> background_pixels = pixels;
    const auto nearest_background = [&](const int source_x,
                                        const int source_y) {
        const auto candidate = [&](const int x, const int y)
            -> std::optional<std::uint32_t> {
            if (x < 0 || y < 0 || x >= 160 || y >= 144) {
                return std::nullopt;
            }
            const auto index = static_cast<std::size_t>(y) * 160U +
                               static_cast<std::size_t>(x);
            if (backdrop_key(pixels[index]) != backdrop_color_key ||
                sprite_mask[index] || window_mask[index]) {
                return std::nullopt;
            }
            return pixels[index];
        };
        for (int radius = 1; radius <= 16; ++radius) {
            const std::array<std::pair<int, int>, 4> probes{{
                {source_x - radius, source_y},
                {source_x + radius, source_y},
                {source_x, source_y - radius},
                {source_x, source_y + radius}}};
            for (const auto [x, y] : probes) {
                if (const auto value = candidate(x, y)) return *value;
            }
        }
        return backdrop_color;
    };
    for (int y = 0; y < 144; ++y) {
        for (int x = 0; x < 160; ++x) {
            const auto index = static_cast<std::size_t>(y) * 160U +
                               static_cast<std::size_t>(x);
            if (backdrop_key(pixels[index]) != backdrop_color_key ||
                sprite_mask[index]) {
                background_pixels[index] = nearest_background(x, y);
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
    for (unsigned cell_y = 0; cell_y < cells_y; ++cell_y) {
        for (unsigned cell_x = 0; cell_x < cells_x; ++cell_x) {
            const auto source_x = cell_x * cell_size;
            const auto source_y = cell_y * cell_size;
            const auto pixel_index = source_y * 160U + source_x;
            unsigned red = 0;
            unsigned green = 0;
            unsigned blue = 0;
            float local_luminance = 0.0F;
            bool has_sprite = false;
            bool has_window = false;
            bool has_object = false;
            for (unsigned y = 0; y < cell_size; ++y) {
                for (unsigned x = 0; x < cell_size; ++x) {
                    const auto sample_x = source_x + x;
                    const auto sample_y = source_y + y;
                    const auto sample_index = sample_y * 160U + sample_x;
                    const auto sample = pixels[sample_index];
                    red += (sample >> 16) & 0xFFU;
                    green += (sample >> 8) & 0xFFU;
                    blue += sample & 0xFFU;
                    local_luminance += luminance(sample);
                    has_sprite = has_sprite || sprite_mask[sample_index];
                    has_window = has_window || window_mask[sample_index];
                    has_object = has_object ||
                                 backdrop_key(sample) != backdrop_color_key;
                }
            }
            const auto sample_count = cell_size * cell_size;
            const auto color = UINT32_C(0xFF000000) |
                               ((red / sample_count) << 16) |
                               ((green / sample_count) << 8) |
                               (blue / sample_count);
            local_luminance /= static_cast<float>(sample_count);
            const auto window_layer = has_window && has_object && !has_sprite;
            const auto object_index = static_cast<std::size_t>(source_y) * 160U +
                                      static_cast<std::size_t>(source_x);
            const auto object_layer = has_sprite ||
                                      (popup_book && popup_object_mask[object_index]);
            float neighborhood_min = 1.0F;
            float neighborhood_max = 0.0F;
            for (int offset_y = -1; offset_y <= 1; ++offset_y) {
                for (int offset_x = -1; offset_x <= 1; ++offset_x) {
                    const auto neighbor_luminance = pixel_luminance_at(
                        static_cast<int>(source_x) + offset_x,
                        static_cast<int>(source_y) + offset_y);
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
            // Shape-aware mode keeps every source pixel, but gives genuine
            // edges a little more volume. This produces cube-like forms
            // without the silhouette loss caused by 2x2 framebuffer cells.
            const auto surface_relief = std::min(
                relief * (shape_aware ? 1.55F : 1.20F),
                shape_aware ? 7.5F : 6.0F);
            // Separate layer instances keep the static background flat while
            // giving the window and object/sprite layers independent offsets
            // and extrusion budgets.
            const auto normalized_band = [](const float value,
                                            const float far_depth,
                                            const float near_depth) {
                return std::clamp((far_depth - value) /
                                      std::max(0.01F,
                                               far_depth - near_depth),
                                  0.0F, 1.0F);
            };
            const auto background_position = has_object
                                                 ? 1.0F
                                                 : normalized_band(
                                                       profile.background_transparent_depth,
                                                       profile.background_depth_far,
                                                       profile.background_depth_near);
            const auto window_position = std::clamp(
                0.35F + surface_relief / 10.0F, 0.0F, 1.0F);
            const auto sprite_position = std::clamp(
                0.60F + surface_relief / 10.0F +
                    std::min(sprite_depth * 0.02F, 0.20F),
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
            // Pull non-backdrop artwork toward the viewer in shape-aware mode
            // while keeping the dominant backdrop plane recessed.
            const auto shape_depth_boost = shape_aware && has_object
                                               ? 0.45F
                                               : 0.0F;
            auto depth = base_depth - depth_scale *
                                             (layer_height + shape_depth_boost);
            if (popup_book && object_layer) {
                const auto source_index = static_cast<std::size_t>(cell_y) *
                                              160U +
                                          static_cast<std::size_t>(cell_x);
                const auto anchor_y = has_sprite && sprite_anchor_y[source_index] >= 0
                                          ? sprite_anchor_y[source_index]
                                          : popup_object_anchor_y[source_index] >= 0
                                                ? popup_object_anchor_y[source_index]
                                                : static_cast<int>(cell_y) + sprite_height;
                const auto pixel_height = has_sprite ? 0.72F : 0.20F;
                const auto pixel_bottom = std::max(
                    0.0F, static_cast<float>(anchor_y) -
                               static_cast<float>(cell_y + 1U)) *
                           pixel_height;
                depth = base_depth - pixel_bottom - pixel_height;
            }
            const auto x = static_cast<float>(source_x);
            const auto y = static_cast<float>(source_y);
            const auto centered_x = x + cell_size * 0.5F - 80.0F;
            const auto centered_y = y + cell_size * 0.5F - 72.0F;
            const auto page_depth = popup_book ? -centered_y * 0.82F
                                               : centered_y * 0.82F;
            const auto world_height = base_depth - depth;
            const auto rotated_y = popup_book
                                       ? page_depth * yaw_cos + centered_x * yaw_sin
                                       : centered_x * yaw_sin +
                                             centered_y * yaw_cos;
            const auto sort_depth = popup_book
                                        ? world_height * pitch_sin +
                                              (centered_x * yaw_sin +
                                               page_depth * yaw_cos) * pitch_cos
                                        : rotated_y * pitch_sin +
                                              depth * pitch_cos;
            column_heights[cell_y * cells_x + cell_x] = depth;
            columns.push_back({x, y, static_cast<float>(cell_size),
                               static_cast<float>(cell_size), depth,
                               sort_depth,
                               color, has_sprite, window_layer, object_layer});
        }
    }
    for (const auto& column : columns) {
        const auto pixel_index = static_cast<std::size_t>(column.y) * 160U +
                                 static_cast<std::size_t>(column.x);
        add_quad(project(column.x, column.y, base_depth),
                 project(column.x + column.width, column.y, base_depth),
                 project(column.x + column.width,
                         column.y + column.extent_y, base_depth),
                 project(column.x, column.y + column.extent_y, base_depth),
                 voxel_color(background_pixels[pixel_index],
                             0.90F * lighting,
                             voxel_ambient * lighting));
    }
    std::stable_sort(columns.begin(), columns.end(),
                     [](const VoxelColumn& left, const VoxelColumn& right) {
                         const auto layer_rank = [](const VoxelColumn& column) {
                            return column.sprite ? 3 : column.object ? 2 :
                                                     column.window ? 1 : 0;
                         };
                         const auto left_rank = layer_rank(left);
                         const auto right_rank = layer_rank(right);
                         if (left_rank != right_rank)
                             return left_rank < right_rank;
                         return left.sort_depth > right.sort_depth;
                     });
    const auto height_at = [&](const int cell_x, const int cell_y) {
        if (cell_x < 0 || cell_y < 0 ||
            cell_x >= static_cast<int>(cells_x) ||
            cell_y >= static_cast<int>(cells_y))
            return base_depth;
        return column_heights[static_cast<std::size_t>(cell_y) * cells_x +
                              static_cast<std::size_t>(cell_x)];
    };
    for (const auto& column : columns) {
        if (column.height >= base_depth - 0.15F) continue;
        if (popup_book && column.object) {
            const auto source_index = static_cast<std::size_t>(column.y) *
                                          160U +
                                      static_cast<std::size_t>(column.x);
                const auto anchor_y = column.sprite && sprite_anchor_y[source_index] >= 0
                                          ? sprite_anchor_y[source_index]
                                          : popup_object_anchor_y[source_index] >= 0
                                                ? popup_object_anchor_y[source_index]
                                                : static_cast<int>(column.y) +
                                                      sprite_height;
                {
                    const auto sprite_pixel_height = column.sprite ? 0.72F : 0.20F;
                const auto pixel_bottom = std::max(
                    0.0F, static_cast<float>(anchor_y) -
                               (column.y + 1.0F)) * sprite_pixel_height;
                const auto pixel_top = pixel_bottom + sprite_pixel_height;
                const auto extrusion = column.sprite ? 1.35F : 4.0F;
                const auto front_page = static_cast<float>(anchor_y) -
                                        extrusion * 0.5F;
                const auto back_page = static_cast<float>(anchor_y) +
                                       extrusion * 0.5F;
                const auto front_bottom_a = project(
                    column.x, front_page, base_depth - pixel_bottom);
                const auto front_bottom_b = project(
                    column.x + column.width, front_page,
                    base_depth - pixel_bottom);
                const auto front_top_a = project(
                    column.x, front_page, base_depth - pixel_top);
                const auto front_top_b = project(
                    column.x + column.width, front_page,
                    base_depth - pixel_top);
                const auto back_top_a = project(
                    column.x, back_page, base_depth - pixel_top);
                const auto back_top_b = project(
                    column.x + column.width, back_page,
                    base_depth - pixel_top);
                const auto back_bottom_a = project(
                    column.x, back_page, base_depth - pixel_bottom);
                const auto back_bottom_b = project(
                    column.x + column.width, back_page,
                    base_depth - pixel_bottom);
                const auto sprite_color = voxel_color(
                    column.color, 0.98F * lighting,
                    luminance(column.color) < 0.20F
                        ? 0.0F
                        : voxel_ambient * lighting);
                add_quad(front_bottom_a, front_bottom_b, front_top_b,
                         front_top_a, sprite_color);
                add_quad(back_bottom_a, back_bottom_b, back_top_b,
                         back_top_a,
                         voxel_color(column.color, 0.78F * lighting,
                                     voxel_ambient * lighting));
                const auto same_shape_pixel = [&](const int neighbour_x,
                                                  const int neighbour_y) {
                    if (column.sprite || neighbour_x < 0 || neighbour_y < 0 ||
                        neighbour_x >= 160 || neighbour_y >= 144) {
                        return false;
                    }
                    const auto neighbour = static_cast<std::size_t>(neighbour_y) *
                                               160U +
                                           static_cast<std::size_t>(neighbour_x);
                    return popup_object_mask[neighbour] &&
                           popup_object_anchor_y[neighbour] == anchor_y;
                };
                const auto cap_color = voxel_color(
                    column.color, 0.84F * lighting,
                    voxel_ambient * lighting);
                const auto side_color = voxel_color(
                    column.color, 0.70F * lighting,
                    voxel_ambient * lighting);
                if (column.sprite || !same_shape_pixel(static_cast<int>(column.x),
                                                        static_cast<int>(column.y) - 1)) {
                    add_quad(front_top_a, front_top_b, back_top_b, back_top_a,
                             cap_color);
                }
                if (column.sprite || !same_shape_pixel(static_cast<int>(column.x) + 1,
                                                        static_cast<int>(column.y))) {
                    add_quad(front_bottom_b, back_bottom_b, back_top_b,
                             front_top_b, side_color);
                }
                if (column.sprite || !same_shape_pixel(static_cast<int>(column.x) - 1,
                                                        static_cast<int>(column.y))) {
                    add_quad(back_bottom_a, front_bottom_a, front_top_a,
                             back_top_a,
                             voxel_color(column.color, 0.58F * lighting,
                                         voxel_ambient * lighting));
                }
                if (!column.sprite && !same_shape_pixel(static_cast<int>(column.x),
                                                        static_cast<int>(column.y) + 1)) {
                    add_quad(back_bottom_a, back_bottom_b, front_bottom_b,
                             front_bottom_a,
                             voxel_color(column.color, 0.62F * lighting,
                                         voxel_ambient * lighting));
                }
                    continue;
                }
        }
        const auto top_a = project(column.x, column.y, column.height);
        const auto top_b = project(column.x + column.width, column.y,
                                   column.height);
        const auto top_c = project(column.x + column.width,
                                   column.y + column.extent_y, column.height);
        const auto top_d = project(column.x, column.y + column.extent_y,
                                   column.height);
        // Each raised layer has its own gap and extrusion budget. Window
        // pixels hover just above the background; objects and sprites sit
        // farther forward without connecting to the recessed plane.
        const auto floating = column.sprite || column.window;
        const auto layer_base = column.sprite
                                    ? base_depth - depth_scale *
                                          (background_span + window_gap +
                                           window_span + sprite_gap -
                                           sprite_pullback)
                                    : column.window
                                          ? base_depth - depth_scale *
                                                (background_span + window_gap)
                                          : base_depth;
        const auto column_base = floating ? layer_base : base_depth;
        const auto base_a = project(column.x, column.y, column_base);
        const auto base_b = project(column.x + column.width, column.y,
                                    column_base);
        const auto base_c = project(column.x + column.width,
                                    column.y + column.extent_y, column_base);
        const auto base_d = project(column.x, column.y + column.extent_y,
                                    column_base);
        const auto border_color = [&](const int border_x,
                                      const int border_y) {
            if (border_x < 0 || border_y < 0 || border_x >= 160 ||
                border_y >= 144) {
                return backdrop_color;
            }
            return pixels[static_cast<std::size_t>(border_y) * 160U +
                          static_cast<std::size_t>(border_x)];
        };
        // Sprite voxels are self-colored: their side faces continue the
        // sprite pixel's material instead of borrowing the background behind
        // the sprite. Window/object geometry keeps the bordering-pixel rule.
        const auto side_color = [&](const int border_x, const int border_y) {
            return column.sprite ? column.color
                                  : border_color(border_x, border_y);
        };
        const auto grid_x = static_cast<int>(column.x) /
                            static_cast<int>(cell_size);
        const auto grid_y = static_cast<int>(column.y) /
                            static_cast<int>(cell_size);
        const auto source_x = static_cast<int>(column.x);
        const auto source_y = static_cast<int>(column.y);
        constexpr float wall_threshold = 0.60F;
        if (column.height < height_at(grid_x, grid_y - 1) - wall_threshold)
            add_quad(base_a, base_b, top_b, top_a,
                     voxel_color(side_color(source_x, source_y - 1),
                                 0.58F * lighting,
                                 voxel_ambient * lighting));
        if (column.height < height_at(grid_x + 1, grid_y) - wall_threshold)
            add_quad(base_b, base_c, top_c, top_b,
                     voxel_color(side_color(source_x + static_cast<int>(cell_size), source_y),
                                 0.68F * lighting,
                                 voxel_ambient * lighting));
        if (column.height < height_at(grid_x, grid_y + 1) - wall_threshold)
            add_quad(base_c, base_d, top_d, top_c,
                     voxel_color(side_color(source_x, source_y + static_cast<int>(cell_size)),
                                 0.76F * lighting,
                                 voxel_ambient * lighting));
        if (column.height < height_at(grid_x - 1, grid_y) - wall_threshold)
            add_quad(base_d, base_a, top_a, top_d,
                     voxel_color(side_color(source_x - 1, source_y),
                                 0.62F * lighting,
                                 voxel_ambient * lighting));
        // Preserve dark source pixels on their front faces. Ambient lighting
        // is reserved for voxel shading and must not turn black artwork gray.
        const auto top_ambient = luminance(column.color) < 0.20F
                                     ? 0.0F : voxel_ambient * lighting;
        add_quad(top_a, top_b, top_c, top_d,
                 voxel_color(column.color,
                             (column.sprite ? 0.98F : 0.96F) * lighting,
                             top_ambient));
    }
    if (!indices.empty() && !SDL_RenderGeometry(
                                app.renderer, nullptr, vertices.data(),
                                static_cast<int>(vertices.size()), indices.data(),
                                static_cast<int>(indices.size()))) {
        SDL_Log("Could not render browser voxel diorama: %s", SDL_GetError());
    }
}

bool configure_core_io(WebApp& app) {
    if (!app.emulator) return false;
    const auto& core = app.emulator->descriptor();
    if (core.video_width == 0 || core.video_height == 0 ||
        core.audio_sample_rate == 0 || core.audio_channels == 0) {
        set_status("The selected core reported an invalid media format.", true);
        return false;
    }
    if (app.texture) SDL_DestroyTexture(app.texture);
    app.texture = SDL_CreateTexture(
        app.renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
        static_cast<int>(core.video_width), static_cast<int>(core.video_height));
    if (!app.texture) return false;
    if (app.audio_stream) SDL_DestroyAudioStream(app.audio_stream);
    const SDL_AudioSpec audio_spec{
        SDL_AUDIO_S16, static_cast<int>(core.audio_channels),
        static_cast<int>(core.audio_sample_rate)};
    app.audio_stream = SDL_OpenAudioDeviceStream(
        SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &audio_spec, nullptr, nullptr);
    if (!app.audio_stream) {
        SDL_Log("Audio output is unavailable: %s", SDL_GetError());
    }
    app.display_pixels.assign(core.video_width * core.video_height, 0);
    apply_video_mode(app, requested_video_mode);
    return true;
}

std::vector<std::uint8_t> copy_browser_bytes(const emscripten::val& bytes) {
    if (bytes.isUndefined() || bytes.isNull()) return {};
    const auto size = bytes["length"].as<std::size_t>();
    std::vector<std::uint8_t> copied(size);
    if (!copied.empty()) {
        auto destination = emscripten::val(
            emscripten::typed_memory_view(copied.size(), copied.data()));
        destination.call<void>("set", bytes);
    }
    return copied;
}

emscripten::val browser_bytes(const std::vector<std::uint8_t>& bytes) {
    auto result = emscripten::val::global("Uint8Array").new_(bytes.size());
    if (!bytes.empty()) {
        result.call<void>(
            "set", emscripten::val(
                       emscripten::typed_memory_view(bytes.size(), bytes.data())));
    }
    return result;
}

void set_status(const std::string& message, const bool error = false) {
    EM_ASM({
        if (Module.gbbSetStatus) {
            Module.gbbSetStatus(UTF8ToString($0), Boolean($1));
        }
    }, message.c_str(), error);
}

void release_all_buttons(WebApp& app) {
    if (!app.emulator) return;
    for (const auto button : button_order) app.emulator->set_input(button, false);
}

gbb::InputId keyboard_button(const SDL_Keycode key, bool& matched) {
    matched = true;
    switch (key) {
    case SDLK_RIGHT: return gbb::InputId::right;
    case SDLK_LEFT: return gbb::InputId::left;
    case SDLK_UP: return gbb::InputId::up;
    case SDLK_DOWN: return gbb::InputId::down;
    case SDLK_X: return gbb::InputId::a;
    case SDLK_Z: return gbb::InputId::b;
    case SDLK_BACKSPACE: return gbb::InputId::select;
    case SDLK_RETURN: return gbb::InputId::start;
    default:
        matched = false;
        return gbb::InputId::a;
    }
}

bool gamepad_button(const Uint8 raw, gbb::InputId& button) {
    switch (static_cast<SDL_GamepadButton>(raw)) {
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: button = gbb::InputId::right; break;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: button = gbb::InputId::left; break;
    case SDL_GAMEPAD_BUTTON_DPAD_UP: button = gbb::InputId::up; break;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: button = gbb::InputId::down; break;
    case SDL_GAMEPAD_BUTTON_SOUTH: button = gbb::InputId::a; break;
    case SDL_GAMEPAD_BUTTON_EAST: button = gbb::InputId::b; break;
    case SDL_GAMEPAD_BUTTON_BACK: button = gbb::InputId::select; break;
    case SDL_GAMEPAD_BUTTON_START: button = gbb::InputId::start; break;
    default: return false;
    }
    return true;
}

void submit_audio(WebApp& app) {
    if (!app.emulator) return;
    const auto samples = app.emulator->take_audio_samples();
    if (!app.audio_stream || samples.empty()) return;

    const auto maximum_queued_bytes = static_cast<int>(gbb::audio_queue_bytes(
        app.emulator->descriptor().audio_sample_rate,
        app.emulator->descriptor().audio_channels, 200));
    if (SDL_GetAudioStreamQueued(app.audio_stream) > maximum_queued_bytes) {
        static_cast<void>(SDL_ClearAudioStream(app.audio_stream));
    }
    if (!SDL_PutAudioStreamData(
            app.audio_stream, samples.data(),
            static_cast<int>(samples.size() * sizeof(samples.front())))) {
        set_status(std::string{"Audio error: "} + SDL_GetError(), true);
    }
}

void present(WebApp& app) {
    static_cast<void>(SDL_SetRenderDrawColor(app.renderer, 16, 20, 16, 255));
    static_cast<void>(SDL_RenderClear(app.renderer));
    if (app.emulator) {
        const auto frame = app.emulator->video_frame();
        const auto* pixels = frame.pixels;
        const auto& palette = gameboy::display_palettes[app.display_palette];
        const auto* game_boy = gbb::gameboy_emulator(app.emulator.get());
        const auto native_colors = game_boy == nullptr ||
                                   game_boy->bus().cgb_mode() ||
                                   palette.cgb_compatibility;
        const auto color_at = [&](const std::size_t source_index) {
            return native_colors
                       ? pixels[source_index]
                       : gameboy::apply_display_palette(pixels[source_index],
                                                        palette);
        };
        app.display_pixels.resize(frame.pixel_count);
        for (std::size_t index = 0; index < frame.pixel_count; ++index) {
            auto pixel = color_at(index);
            const auto x = index % frame.width;
            const auto y = index / frame.width;
            if (app.video_mode == gameboy::VideoMode::sharp_smoothing) {
                const auto left = x == 0 ? index : index - 1;
                const auto right = x + 1 == frame.width
                                       ? index : index + 1;
                const auto up = y == 0 ? index : index - frame.width;
                const auto down = y + 1 == frame.height
                                      ? index : index + frame.width;
                pixel = gameboy::apply_sharp_smoothing(
                    pixel, color_at(left), color_at(right), color_at(up),
                    color_at(down));
            } else if (app.video_mode == gameboy::VideoMode::lcd_shader) {
                pixel = gameboy::apply_lcd_shader(pixel, x, y);
            }
            app.display_pixels[index] = pixel;
        }
        if ((app.video_mode == gameboy::VideoMode::voxel_diorama ||
             app.video_mode == gameboy::VideoMode::voxel_shape ||
             app.video_mode == gameboy::VideoMode::voxel_popup) &&
            frame.width == 160 && frame.height == 144) {
            render_web_voxel(
                app, app.display_pixels,
                app.video_mode == gameboy::VideoMode::voxel_shape,
                app.video_mode == gameboy::VideoMode::voxel_popup);
        } else {
            static_cast<void>(SDL_UpdateTexture(
                app.texture, nullptr, app.display_pixels.data(),
                static_cast<int>(frame.width * sizeof(std::uint32_t))));
            static_cast<void>(SDL_RenderTexture(
                app.renderer, app.texture, nullptr, nullptr));
        }
    }
    static_cast<void>(SDL_RenderPresent(app.renderer));
}

void destroy(WebApp* app) {
    if (!app) return;
    delete app;
    active_app = nullptr;
    SDL_Quit();
}

} // namespace

int load_rom_from_browser(emscripten::val bytes) noexcept {
    if (!active_app || bytes.isUndefined() || bytes.isNull()) return 0;
    try {
        auto rom = copy_browser_bytes(bytes);
        if (rom.empty()) return 0;
        active_app->emulator = gbb::create_core(std::move(rom));
        active_app->voxel_camera_pitch_offset = 0.0F;
        active_app->voxel_camera_yaw_offset = 0.0F;
        if (!configure_core_io(*active_app)) {
            active_app->emulator.reset();
            throw std::runtime_error("Could not configure the selected core");
        }
        auto* game_boy = gbb::gameboy_emulator(active_app->emulator.get());
        // The browser has no physical printer, but it still needs to expose
        // the Game Boy Printer protocol so camera and other printer-enabled
        // games can complete their print jobs. Completed pages are drained
        // through the JavaScript binding below.
        if (game_boy != nullptr &&
            gbb::has_capability(active_app->emulator->descriptor().capabilities,
                                gbb::CoreCapability::printer)) {
            game_boy->bus().connect_printer();
        }
        active_app->emulator->set_compatibility_colors(
            gameboy::display_palettes[active_app->display_palette]
                .cgb_compatibility);
        // Browser storage is asynchronous. Remain paused until JavaScript has
        // restored battery RAM and RTC data for this ROM.
        active_app->paused = true;
        active_app->cycle_credit = 0.0;
        active_app->previous_time = std::chrono::steady_clock::now();
        if (active_app->audio_stream) {
            static_cast<void>(SDL_ClearAudioStream(active_app->audio_stream));
        }
        return 1;
    } catch (const std::exception& error) {
        set_status(error.what(), true);
        return 0;
    }
}

int load_rom_from_browser_with_palette(emscripten::val bytes,
                                       const unsigned palette) noexcept {
    if (!active_app || palette >= gameboy::display_palettes.size()) return 0;
    active_app->display_palette = palette;
    return load_rom_from_browser(std::move(bytes));
}

std::string browser_rom_fingerprint() {
    if (!active_app || !active_app->emulator) return {};
    std::ostringstream fingerprint;
    fingerprint << std::hex << std::setw(16) << std::setfill('0')
                << active_app->emulator->rom_fingerprint();
    return fingerprint.str();
}

bool browser_has_battery() noexcept {
    return active_app && active_app->emulator &&
           active_app->emulator->has_persistent_data(
               gbb::PersistentDataKind::battery_save);
}

bool browser_has_rtc() noexcept {
    return active_app && active_app->emulator &&
           active_app->emulator->has_persistent_data(
               gbb::PersistentDataKind::rtc);
}

bool browser_has_camera() noexcept {
    return active_app && active_app->emulator &&
           gbb::has_capability(active_app->emulator->descriptor().capabilities,
                               gbb::CoreCapability::camera);
}

void set_browser_camera_frame(const emscripten::val bytes) {
    if (!active_app || !active_app->emulator ||
        !gbb::has_capability(active_app->emulator->descriptor().capabilities,
                             gbb::CoreCapability::camera)) {
        throw std::runtime_error("No Game Boy Camera ROM is loaded");
    }
    auto frame = copy_browser_bytes(bytes);
    constexpr auto expected_size = gameboy::Cartridge::camera_width *
                                   gameboy::Cartridge::camera_height;
    if (frame.size() != expected_size) {
        throw std::invalid_argument("Invalid Game Boy Camera frame size");
    }
    active_app->emulator->set_camera_frame(frame.data(), frame.size());
}

emscripten::val export_browser_save_ram() {
    if (!active_app || !active_app->emulator) {
        return emscripten::val::global("Uint8Array").new_(0);
    }
    return browser_bytes(active_app->emulator->export_persistent_data(
        gbb::PersistentDataKind::battery_ram));
}

emscripten::val export_browser_save_data() {
    if (!active_app || !active_app->emulator) {
        return emscripten::val::global("Uint8Array").new_(0);
    }
    return browser_bytes(active_app->emulator->export_persistent_data(
        gbb::PersistentDataKind::battery_save));
}

emscripten::val export_browser_state() {
    if (!active_app || !active_app->emulator) {
        return emscripten::val::global("Uint8Array").new_(0);
    }
    return browser_bytes(active_app->emulator->save_state());
}

void import_browser_save_ram(const emscripten::val bytes) {
    if (!active_app || !active_app->emulator) {
        throw std::runtime_error("No ROM is loaded");
    }
    active_app->emulator->import_persistent_data(
        gbb::PersistentDataKind::battery_ram, copy_browser_bytes(bytes));
}

void import_browser_save_data(const emscripten::val bytes) {
    if (!active_app || !active_app->emulator) {
        throw std::runtime_error("No ROM is loaded");
    }
    active_app->emulator->import_persistent_data(
        gbb::PersistentDataKind::battery_save, copy_browser_bytes(bytes));
}

void import_browser_state(const emscripten::val bytes) {
    if (!active_app || !active_app->emulator) {
        throw std::runtime_error("No ROM is loaded");
    }
    active_app->emulator->load_state(copy_browser_bytes(bytes));
}

emscripten::val export_browser_rtc_data() {
    if (!active_app || !active_app->emulator) {
        return emscripten::val::global("Uint8Array").new_(0);
    }
    return browser_bytes(active_app->emulator->export_persistent_data(
        gbb::PersistentDataKind::rtc));
}

emscripten::val take_browser_printer_images() {
    auto result = emscripten::val::array();
    if (!active_app || !active_app->emulator) return result;

    auto* game_boy = gbb::gameboy_emulator(active_app->emulator.get());
    if (game_boy == nullptr) return result;
    for (const auto& image : game_boy->bus().take_printer_images()) {
        result.call<void>("push", browser_bytes(gameboy::encode_printer_bmp(image)));
    }
    return result;
}

void import_browser_rtc_data(const emscripten::val bytes) {
    if (!active_app || !active_app->emulator) {
        throw std::runtime_error("No ROM is loaded");
    }
    active_app->emulator->import_persistent_data(
        gbb::PersistentDataKind::rtc, copy_browser_bytes(bytes));
}

EMSCRIPTEN_BINDINGS(gbb_web_bindings) {
    emscripten::function("loadRom", &load_rom_from_browser);
    emscripten::function("loadRomWithPalette",
                         &load_rom_from_browser_with_palette);
    emscripten::function("romFingerprint", &browser_rom_fingerprint);
    emscripten::function("hasBattery", &browser_has_battery);
    emscripten::function("hasRtc", &browser_has_rtc);
    emscripten::function("hasCamera", &browser_has_camera);
    emscripten::function("setCameraFrame", &set_browser_camera_frame);
    emscripten::function("exportSaveRam", &export_browser_save_ram);
    emscripten::function("importSaveRam", &import_browser_save_ram);
    emscripten::function("exportSaveData", &export_browser_save_data);
    emscripten::function("importSaveData", &import_browser_save_data);
    emscripten::function("exportState", &export_browser_state);
    emscripten::function("importState", &import_browser_state);
    emscripten::function("exportRtcData", &export_browser_rtc_data);
    emscripten::function("importRtcData", &import_browser_rtc_data);
    emscripten::function("takePrinterImages", &take_browser_printer_images);
}

extern "C" EMSCRIPTEN_KEEPALIVE void gbb_resume_audio() noexcept {
    if (active_app && active_app->audio_stream) {
        static_cast<void>(
            SDL_ResumeAudioStreamDevice(active_app->audio_stream));
    }
}

extern "C" EMSCRIPTEN_KEEPALIVE void gbb_start_rom() noexcept {
    if (!active_app || !active_app->emulator) return;
    active_app->paused = false;
    active_app->cycle_credit = 0.0;
    active_app->previous_time = std::chrono::steady_clock::now();
    if (active_app->audio_stream) {
        static_cast<void>(
            SDL_ResumeAudioStreamDevice(active_app->audio_stream));
    }
}

extern "C" EMSCRIPTEN_KEEPALIVE void gbb_pause_rom() noexcept {
    if (!active_app || !active_app->emulator) return;
    active_app->paused = true;
    release_all_buttons(*active_app);
}

extern "C" EMSCRIPTEN_KEEPALIVE void gbb_set_palette(
    const unsigned palette) noexcept {
    if (active_app && palette < gameboy::display_palettes.size()) {
        active_app->display_palette = palette;
        if (active_app->emulator) {
            active_app->emulator->set_compatibility_colors(
                gameboy::display_palettes[palette].cgb_compatibility);
        }
    }
}

extern "C" EMSCRIPTEN_KEEPALIVE void gbb_set_video_mode(
    const unsigned mode) noexcept {
    if (mode >= gameboy::video_modes.size()) return;
    requested_video_mode = mode;
    if (active_app) apply_video_mode(*active_app, requested_video_mode);
}

extern "C" EMSCRIPTEN_KEEPALIVE void gbb_set_voxel_camera(
    const float yaw_delta, const float pitch_delta) noexcept {
    if (!active_app) return;
    active_app->voxel_camera_yaw_offset = std::clamp(
        active_app->voxel_camera_yaw_offset + yaw_delta, -45.0F, 45.0F);
    active_app->voxel_camera_pitch_offset = std::clamp(
        active_app->voxel_camera_pitch_offset + pitch_delta, -55.0F, 48.0F);
}

extern "C" EMSCRIPTEN_KEEPALIVE void gbb_reset_voxel_camera() noexcept {
    if (!active_app) return;
    active_app->voxel_camera_pitch_offset = 0.0F;
    active_app->voxel_camera_yaw_offset = 0.0F;
}

extern "C" EMSCRIPTEN_KEEPALIVE const char* gbb_export_scene_snapshot() noexcept {
    scene_snapshot_export.clear();
    if (active_app == nullptr || active_app->emulator == nullptr) {
        return scene_snapshot_export.c_str();
    }
    scene_snapshot_export =
        gbb::scene_snapshot_to_json(active_app->emulator->scene_snapshot());
    return scene_snapshot_export.c_str();
}

SDL_AppResult SDL_AppInit(void** appstate, int, char**) {
    auto app = std::make_unique<WebApp>();
    if (!SDL_SetAppMetadata("Go Bigger Boy (GBB)", "0.2.0",
                            "go-bigger-boy") ||
        !SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD)) {
        SDL_Log("Could not initialize SDL: %s", SDL_GetError());
        return SDL_APP_FAILURE;
    }

    app->window = SDL_CreateWindow("Go Bigger Boy (GBB)", 640, 576,
                                   SDL_WINDOW_RESIZABLE);
    if (!app->window) return SDL_APP_FAILURE;
    app->renderer = SDL_CreateRenderer(app->window, nullptr);
    if (!app->renderer) return SDL_APP_FAILURE;
    static_cast<void>(SDL_SetRenderVSync(app->renderer, 1));
    active_app = app.get();
    *appstate = app.release();
    set_status("Ready. Choose a Game Boy ROM to begin.");
    return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppEvent(void* appstate, SDL_Event* event) {
    auto& app = *static_cast<WebApp*>(appstate);
    switch (event->type) {
    case SDL_EVENT_QUIT:
        return SDL_APP_SUCCESS;
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP: {
        if (event->key.repeat) break;
        if (event->type == SDL_EVENT_KEY_DOWN && event->key.key == SDLK_SPACE &&
            app.emulator) {
            app.paused = !app.paused;
            release_all_buttons(app);
            set_status(app.paused ? "Paused." : "Running.");
            break;
        }
        bool matched{};
        const auto button = keyboard_button(event->key.key, matched);
        if (matched && app.emulator) {
            app.emulator->set_input(button,
                                    event->type == SDL_EVENT_KEY_DOWN);
        }
        break;
    }
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        release_all_buttons(app);
        break;
    case SDL_EVENT_GAMEPAD_ADDED:
        if (!app.gamepad) app.gamepad = SDL_OpenGamepad(event->gdevice.which);
        break;
    case SDL_EVENT_GAMEPAD_REMOVED:
        if (app.gamepad && SDL_GetGamepadID(app.gamepad) == event->gdevice.which) {
            SDL_CloseGamepad(app.gamepad);
            app.gamepad = nullptr;
        }
        break;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP: {
        gbb::InputId button{};
        if (app.emulator && gamepad_button(event->gbutton.button, button)) {
            app.emulator->set_input(
                button, event->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN);
        }
        break;
    }
    default:
        break;
    }
    return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppIterate(void* appstate) {
    auto& app = *static_cast<WebApp*>(appstate);
    const auto now = std::chrono::steady_clock::now();
    const auto elapsed = std::min(
        std::chrono::duration<double>(now - app.previous_time).count(), 0.1);
    app.previous_time = now;

    if (app.emulator && !app.paused) {
        const auto& core = app.emulator->descriptor();
        app.cycle_credit = std::min(
            app.cycle_credit + elapsed * core.clock_rate,
            static_cast<double>(core.nominal_cycles_per_frame) * 2.0);
        while (app.cycle_credit >= 4.0) {
            const auto cycles = app.emulator->step_instruction();
            app.cycle_credit -= static_cast<double>(cycles);
            if (app.emulator->frame_ready()) app.emulator->consume_frame();
        }
        submit_audio(app);
    }
    present(app);
    return SDL_APP_CONTINUE;
}

void SDL_AppQuit(void* appstate, SDL_AppResult) {
    destroy(static_cast<WebApp*>(appstate));
}
