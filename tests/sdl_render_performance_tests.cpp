#include "frame_rate_metrics.hpp"
#include "voxel_renderer.hpp"

#include "gbb/core_runtime.hpp"
#include "gameboy/cartridge.hpp"
#include "gameboy/display_palette.hpp"
#include "gameboy/emulator.hpp"
#include "gameboy/joypad.hpp"
#include "gameboy/video_pipeline.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr unsigned cycles_per_frame = 70224;
constexpr unsigned warmup_frames = 4;
constexpr unsigned measured_frames = 24;

int failures = 0;

void check(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

std::vector<std::uint8_t> read_renderer_pixels(SDL_Renderer* renderer) {
    auto* surface = SDL_RenderReadPixels(renderer, nullptr);
    if (surface == nullptr) {
        check(false, std::string("SDL reads the rendered frame: ") +
                         SDL_GetError());
        return {};
    }
    const auto bytes = static_cast<std::size_t>(surface->pitch) *
                       static_cast<std::size_t>(surface->h);
    std::vector<std::uint8_t> pixels(bytes);
    std::memcpy(pixels.data(), surface->pixels, bytes);
    SDL_DestroySurface(surface);
    return pixels;
}

bool visible_pixels_match(const std::vector<std::uint8_t>& left,
                          const std::vector<std::uint8_t>& right) {
    // SDL's software backend can produce different destination alpha for a
    // direct draw and an opaque same-size texture blit. The window is cleared
    // opaquely, so RGB is the visible contract we must keep identical.
    if (left.size() != right.size() || left.empty() || left.size() % 4U != 0U) {
        return false;
    }
    for (std::size_t index = 0; index < left.size(); index += 4U) {
        if (left[index] != right[index] || left[index + 1U] != right[index + 1U] ||
            left[index + 2U] != right[index + 2U]) {
            return false;
        }
    }
    return true;
}

bool write_renderer_capture(SDL_Renderer* renderer,
                            const std::filesystem::path& output_path) {
    auto* surface = SDL_RenderReadPixels(renderer, nullptr);
    if (surface == nullptr) {
        check(false, std::string("SDL captures the rendered frame: ") +
                         SDL_GetError());
        return false;
    }
    const auto* format = SDL_GetPixelFormatDetails(surface->format);
    if (format == nullptr || format->bytes_per_pixel == 0) {
        check(false, "SDL capture reports an unknown pixel format");
        SDL_DestroySurface(surface);
        return false;
    }
    std::error_code directory_error;
    std::filesystem::create_directories(output_path.parent_path(),
                                        directory_error);
    if (directory_error) {
        check(false, "could not create the voxel capture directory");
        SDL_DestroySurface(surface);
        return false;
    }
    std::ofstream output(output_path, std::ios::binary);
    if (!output) {
        check(false, "could not open the voxel capture output");
        SDL_DestroySurface(surface);
        return false;
    }
    output << "P6\n" << surface->w << ' ' << surface->h << "\n255\n";
    std::vector<std::uint8_t> row(static_cast<std::size_t>(surface->w) * 3U);
    for (int y = 0; y < surface->h; ++y) {
        const auto* source = static_cast<const std::uint8_t*>(surface->pixels) +
                             static_cast<std::size_t>(y) * surface->pitch;
        for (int x = 0; x < surface->w; ++x) {
            std::uint32_t pixel = 0;
            std::memcpy(&pixel, source + static_cast<std::size_t>(x) *
                                      format->bytes_per_pixel,
                        format->bytes_per_pixel);
            SDL_GetRGB(pixel, format, nullptr,
                       &row[static_cast<std::size_t>(x) * 3U],
                       &row[static_cast<std::size_t>(x) * 3U + 1U],
                       &row[static_cast<std::size_t>(x) * 3U + 2U]);
        }
        output.write(reinterpret_cast<const char*>(row.data()),
                     static_cast<std::streamsize>(row.size()));
    }
    const auto success = output.good();
    if (!success) check(false, "could not write the voxel capture output");
    SDL_DestroySurface(surface);
    return success;
}

std::vector<std::uint8_t> render_test_rom() {
    std::vector<std::uint8_t> rom(0x8000, 0);
    const std::string title = "GBB SDL PERF";
    std::copy(title.begin(), title.end(), rom.begin() + 0x134);

    constexpr std::uint8_t program[] = {
        0x3E, 0x01,       // LD A,$01
        0xEA, 0x00, 0xC0, // LD ($C000),A
        0xEE, 0x01,       // XOR $01
        0xC3, 0x02, 0x01, // JP $0102
    };
    std::copy(std::begin(program), std::end(program), rom.begin() + 0x100);
    rom[0x147] = 0x00;
    rom[0x148] = 0x00;
    rom[0x149] = 0x00;
    return rom;
}

void seed_scene(gameboy::Emulator& emulator) {
    // Populate tile graphics and the background map through the existing
    // diagnostic bus hooks. This keeps the benchmark self-contained while
    // producing the varied silhouettes that make voxel geometry expensive.
    for (unsigned tile = 0; tile < 96; ++tile) {
        for (unsigned row = 0; row < 8; ++row) {
            const auto stripe = static_cast<std::uint8_t>(
                ((tile + row) & 1U) != 0U ? 0xAAU : 0x55U);
            const auto high = static_cast<std::uint8_t>(
                ((tile / 3U + row / 2U) & 1U) != 0U ? 0xFFU : stripe);
            const auto offset = static_cast<std::uint16_t>(tile * 16U + row * 2U);
            emulator.bus().debug_write_vram(0, offset, high);
            emulator.bus().debug_write_vram(0, offset + 1U,
                                             static_cast<std::uint8_t>(
                                                 high ^ (tile & 1U ? 0xFFU : 0U)));
        }
    }
    for (unsigned y = 0; y < 32; ++y) {
        for (unsigned x = 0; x < 32; ++x) {
            const auto tile = static_cast<std::uint8_t>(
                ((x / 2U + y / 2U) % 48U) + (y % 3U) * 16U);
            emulator.bus().debug_write_vram(
                0, static_cast<std::uint16_t>(0x1800U + y * 32U + x), tile);
        }
    }
    for (unsigned sprite = 0; sprite < 40; ++sprite) {
        const auto x = static_cast<std::uint8_t>(8U + (sprite * 29U) % 144U);
        const auto y = static_cast<std::uint8_t>(16U + (sprite * 17U) % 112U);
        emulator.bus().debug_write_oam(static_cast<std::uint8_t>(sprite * 4U), y);
        emulator.bus().debug_write_oam(static_cast<std::uint8_t>(sprite * 4U + 1U), x);
        emulator.bus().debug_write_oam(static_cast<std::uint8_t>(sprite * 4U + 2U),
                                       static_cast<std::uint8_t>(sprite % 48U));
        emulator.bus().debug_write_oam(static_cast<std::uint8_t>(sprite * 4U + 3U),
                                       static_cast<std::uint8_t>(sprite & 1U ? 0x20U : 0U));
    }
}

double minimum_render_fps() {
    const auto* value = std::getenv("GBB_RENDER_MIN_FPS");
    if (value == nullptr || *value == '\0') return 60.0;
    char* end = nullptr;
    const auto parsed = std::strtod(value, &end);
    if (end == value || *end != '\0' || parsed < 0.0) return 60.0;
    return parsed;
}

struct RenderResources final {
    SDL_Window* window{};
    SDL_Renderer* renderer{};
    SDL_Texture* texture{};
    SDL_Texture* render_target{};

    ~RenderResources() {
        if (texture != nullptr) SDL_DestroyTexture(texture);
        if (render_target != nullptr) SDL_DestroyTexture(render_target);
        if (renderer != nullptr) SDL_DestroyRenderer(renderer);
        if (window != nullptr) SDL_DestroyWindow(window);
        SDL_Quit();
    }
};

struct ModeResult final {
    std::string mode;
    double fps{};
    double elapsed_ms{};
    unsigned fps_samples{};
    std::size_t mesh_vertices{};
    std::size_t mesh_indices{};
    double scene_snapshot_us{};
    double scene_build_us{};
    double pixel_transform_us{};
    double geometry_build_us{};
    double geometry_sort_us{};
    double geometry_submit_us{};
    double total_us{};
    std::uint64_t render_cache_hits{};
};

ModeResult benchmark_mode(RenderResources& resources,
                          const gameboy::VideoMode mode,
                          gameboy::Emulator& emulator,
                          const gameboy::DisplayPalette& palette,
                          const std::filesystem::path& capture_directory) {
    gbb::SceneSnapshot scene_snapshot;
    std::filesystem::path profile_path;
    gbb::VoxelProfile profile;
    std::uint64_t profile_fingerprint = 0;
    bool profile_loaded = false;
    gbb::VoxelScene voxel_scene;
    std::uint64_t scene_signature = 0;
    bool scene_cached = false;
    std::uint64_t render_cache_key = 0;
    bool render_cache_valid = false;
    gbb::VoxelRenderStats voxel_stats;
    std::vector<SDL_Vertex> vertices;
    std::vector<int> indices;
    float camera_pitch_offset = 0.0F;
    float camera_yaw_offset = 0.0F;
    gbb::sdl::FrameRateMetrics fps_metrics{std::chrono::milliseconds{1}};
    unsigned fps_samples = 0;
    unsigned completed_frames = 0;
    std::chrono::steady_clock::time_point render_started_total{};
    std::chrono::steady_clock::time_point render_finished_total{};
    std::uint64_t scene_snapshot_us = 0;
    std::uint64_t scene_build_us = 0;
    std::uint64_t pixel_transform_us = 0;
    std::uint64_t geometry_build_us = 0;
    std::uint64_t geometry_sort_us = 0;
    std::uint64_t geometry_submit_us = 0;
    std::uint64_t total_us = 0;

    for (unsigned frame = 0;
         frame < warmup_frames + measured_frames; ++frame) {
        const auto advance = gbb::advance_to_frame(emulator, cycles_per_frame * 2U);
        check(advance.frame_ready, "render benchmark reaches every frame");
        if (!advance.frame_ready) break;

        const auto warmup = frame < warmup_frames;
        if (!warmup && completed_frames == 0) {
            render_started_total = std::chrono::steady_clock::now();
        }

        if (!SDL_SetRenderDrawColor(resources.renderer, 16, 20, 16, 255) ||
            !SDL_RenderClear(resources.renderer)) {
            check(false, "SDL clears the benchmark render target");
            break;
        }
        const auto render_started = std::chrono::steady_clock::now();
        bool rendered = false;
        if (mode == gameboy::VideoMode::nearest) {
            std::vector<std::uint32_t> pixels;
            gbb::transform_video_frame(
                emulator.framebuffer().data(), emulator.framebuffer().size(),
                gameboy::Ppu::screen_width, gameboy::Ppu::screen_height,
                palette, false, mode, pixels);
            rendered = SDL_UpdateTexture(
                           resources.texture, nullptr, pixels.data(),
                           static_cast<int>(gameboy::Ppu::screen_width *
                                            sizeof(std::uint32_t))) &&
                       SDL_RenderTexture(resources.renderer, resources.texture,
                                         nullptr, nullptr);
        } else {
            gbb::sdl::VoxelRenderContext context{
                resources.renderer,
                resources.texture,
                resources.render_target,
                mode,
                scene_snapshot,
                profile_path,
                profile,
                profile_fingerprint,
                profile_loaded,
                voxel_scene,
                scene_signature,
                scene_cached,
                voxel_stats,
                vertices,
                indices,
                camera_pitch_offset,
                camera_yaw_offset,
                render_cache_key,
                render_cache_valid};
            rendered = gbb::sdl::render_voxel_diorama(
                emulator, context, palette,
                mode == gameboy::VideoMode::voxel_shape,
                mode == gameboy::VideoMode::voxel_popup);
        }
        check(rendered, "the production renderer accepts every benchmark mode");
        if (!warmup && frame + 1U == warmup_frames + measured_frames &&
            !capture_directory.empty()) {
            static_cast<void>(write_renderer_capture(
                resources.renderer,
                capture_directory /
                    (std::string(gameboy::video_mode_info(mode).id) + ".ppm")));
        }
        check(SDL_RenderPresent(resources.renderer),
              "SDL presents every benchmark frame");
        const auto render_finished = std::chrono::steady_clock::now();
        if (!warmup && fps_metrics.observe(render_finished).has_value()) {
            ++fps_samples;
        }
        if (!warmup) {
            scene_snapshot_us += voxel_stats.scene_snapshot_us;
            scene_build_us += voxel_stats.scene_build_us;
            pixel_transform_us += voxel_stats.pixel_transform_us;
            geometry_build_us += voxel_stats.geometry_build_us;
            geometry_sort_us += voxel_stats.geometry_sort_us;
            geometry_submit_us += voxel_stats.geometry_submit_us;
            total_us += voxel_stats.total_us;
        }
        emulator.consume_frame();
        if (!warmup) ++completed_frames;
        render_finished_total = render_finished;
        if (!rendered) break;
    }

    const auto elapsed = completed_frames == 0
                             ? 0.0
                             : std::chrono::duration<double>(
                                   render_finished_total - render_started_total)
                                   .count();
    const auto fps = elapsed <= 0.0
                         ? std::numeric_limits<double>::infinity()
                         : static_cast<double>(completed_frames) / elapsed;
    check(completed_frames == measured_frames,
          "render benchmark completes its full frame budget");
    check(fps >= minimum_render_fps(),
          "rendering stays above the configured catastrophic-regression floor");
    const auto measured = static_cast<double>(std::max(1U, completed_frames));
    return {{}, fps, elapsed * 1000.0, fps_samples,
            voxel_stats.mesh_vertices, voxel_stats.mesh_indices,
            static_cast<double>(scene_snapshot_us) / measured,
            static_cast<double>(scene_build_us) / measured,
            static_cast<double>(pixel_transform_us) / measured,
            static_cast<double>(geometry_build_us) / measured,
            static_cast<double>(geometry_sort_us) / measured,
            static_cast<double>(geometry_submit_us) / measured,
            static_cast<double>(total_us) / measured,
            voxel_stats.render_cache_hits};
}

void write_json(const std::filesystem::path& path,
                const std::vector<ModeResult>& results) {
    if (path.empty()) return;
    std::ofstream output(path);
    if (!output) {
        std::cerr << "Could not write render performance JSON: "
                  << path.string() << '\n';
        ++failures;
        return;
    }
    output << "{\n  \"schema_version\": 1,\n  \"platform\": \""
           << SDL_GetPlatform() << "\",\n  \"warmup_frames\": "
           << warmup_frames << ",\n  \"measured_frames\": "
           << measured_frames << ",\n  \"modes\": [\n";
    output << std::fixed << std::setprecision(3);
    for (std::size_t index = 0; index < results.size(); ++index) {
        const auto& result = results[index];
        output << "    {\"mode\": \"" << result.mode
               << "\", \"fps\": " << result.fps
               << ", \"elapsed_ms\": " << result.elapsed_ms
               << ", \"fps_samples\": " << result.fps_samples
               << ", \"mesh_vertices\": " << result.mesh_vertices
               << ", \"mesh_indices\": " << result.mesh_indices
               << ", \"timings_us\": {\"scene_snapshot\": "
               << result.scene_snapshot_us
               << ", \"scene_build\": " << result.scene_build_us
               << ", \"pixel_transform\": " << result.pixel_transform_us
               << ", \"geometry_build\": " << result.geometry_build_us
               << ", \"geometry_sort\": " << result.geometry_sort_us
               << ", \"geometry_submit\": " << result.geometry_submit_us
               << ", \"total\": " << result.total_us
               << ", \"render_cache_hits\": " << result.render_cache_hits
               << "}}";
        if (index + 1 != results.size()) output << ',';
        output << '\n';
    }
    output << "  ]\n}\n";
}

} // namespace

int main() {
    // Environment variables intentionally win over hints in SDL. Ignore the
    // hint return value so CI can force the same renderer without turning a
    // successful configuration into a false skip.
    static_cast<void>(SDL_SetHint(SDL_HINT_RENDER_DRIVER, "software"));
    static_cast<void>(SDL_SetHint(SDL_HINT_RENDER_VSYNC, "0"));
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::cerr << "SKIP: software SDL renderer unavailable: "
                  << SDL_GetError() << '\n';
        return 77;
    }

    RenderResources resources;
    resources.window = SDL_CreateWindow(
        "GBB SDL render performance", 160, 144, SDL_WINDOW_HIDDEN);
    resources.renderer = resources.window == nullptr
                             ? nullptr
                             : SDL_CreateRenderer(resources.window, "software");
    resources.texture = resources.renderer == nullptr
                           ? nullptr
                           : SDL_CreateTexture(
                                 resources.renderer, SDL_PIXELFORMAT_ARGB8888,
                                 SDL_TEXTUREACCESS_STREAMING, 160, 144);
    const auto* disable_cache = std::getenv("GBB_RENDER_PERF_DISABLE_CACHE");
    if (disable_cache == nullptr || *disable_cache == '\0' ||
        std::string_view{disable_cache} == "0") {
        resources.render_target = resources.renderer == nullptr
                                      ? nullptr
                                      : SDL_CreateTexture(
                                            resources.renderer,
                                            SDL_PIXELFORMAT_ARGB8888,
                                            SDL_TEXTUREACCESS_TARGET, 160, 144);
        if (resources.render_target != nullptr) {
            check(SDL_SetTextureScaleMode(resources.render_target,
                                          SDL_SCALEMODE_NEAREST),
                  "pixel benchmark configures nearest-neighbor cache scaling");
        }
    }
    if (resources.window == nullptr || resources.renderer == nullptr ||
        resources.texture == nullptr ||
        ((disable_cache == nullptr || *disable_cache == '\0' ||
          std::string_view{disable_cache} == "0") &&
         resources.render_target == nullptr)) {
        std::cerr << "SKIP: could not create software render target: "
                  << SDL_GetError() << '\n';
        return 77;
    }

    const auto palette = gameboy::display_palettes.front();
    const gameboy::VideoMode modes[] = {
        gameboy::VideoMode::nearest,
        gameboy::VideoMode::voxel_diorama,
        gameboy::VideoMode::voxel_shape,
        gameboy::VideoMode::voxel_popup,
    };
    const auto* capture_directory_value =
        std::getenv("GBB_RENDER_PERF_CAPTURE_DIR");
    const auto capture_directory =
        capture_directory_value == nullptr
            ? std::filesystem::path{}
            : std::filesystem::u8path(capture_directory_value);
    // The render-target cache is intended to be a presentation optimization,
    // not a second visual implementation. Compare one direct frame against
    // one cached frame for every voxel mode before running the throughput
    // benchmark. This catches backend filtering, clear-color, and target
    // restore regressions that an FPS-only test cannot see.
    for (const auto mode : {gameboy::VideoMode::voxel_diorama,
                            gameboy::VideoMode::voxel_shape,
                            gameboy::VideoMode::voxel_popup}) {
        gameboy::Emulator emulator{gameboy::Cartridge{render_test_rom()}};
        seed_scene(emulator);
        const auto advance =
            gbb::advance_to_frame(emulator, cycles_per_frame * 2U);
        check(advance.frame_ready, "pixel comparison reaches a frame");
        if (!advance.frame_ready) continue;

        gbb::SceneSnapshot scene_snapshot;
        std::filesystem::path profile_path;
        gbb::VoxelProfile profile;
        std::uint64_t profile_fingerprint = 0;
        bool profile_loaded = false;
        gbb::VoxelScene voxel_scene;
        std::uint64_t scene_signature = 0;
        bool scene_cached = false;
        std::uint64_t render_cache_key = 0;
        bool render_cache_valid = false;
        gbb::VoxelRenderStats voxel_stats;
        std::vector<SDL_Vertex> vertices;
        std::vector<int> indices;
        float camera_pitch_offset = 0.0F;
        float camera_yaw_offset = 0.0F;
        auto* cached_target = resources.render_target;

        const auto render_frame = [&](SDL_Texture* render_target) {
            gbb::sdl::VoxelRenderContext context{
                resources.renderer,
                resources.texture,
                render_target,
                mode,
                scene_snapshot,
                profile_path,
                profile,
                profile_fingerprint,
                profile_loaded,
                voxel_scene,
                scene_signature,
                scene_cached,
                voxel_stats,
                vertices,
                indices,
                camera_pitch_offset,
                camera_yaw_offset,
                render_cache_key,
                render_cache_valid};
            return gbb::sdl::render_voxel_diorama(
                emulator, context, palette,
                mode == gameboy::VideoMode::voxel_shape,
                mode == gameboy::VideoMode::voxel_popup);
        };

        check(SDL_SetRenderDrawColor(resources.renderer, 16, 20, 16, 255) &&
                  SDL_RenderClear(resources.renderer),
              "pixel comparison clears the direct frame");
        resources.render_target = nullptr;
        check(render_frame(nullptr), "direct voxel frame renders");
        const auto direct_pixels = read_renderer_pixels(resources.renderer);

        resources.render_target = cached_target;
        check(cached_target != nullptr,
              "pixel comparison creates a render target");
        render_cache_valid = false;
        check(SDL_SetRenderDrawColor(resources.renderer, 16, 20, 16, 255) &&
                  SDL_RenderClear(resources.renderer),
              "pixel comparison clears the cached frame");
        check(render_frame(resources.render_target), "cached voxel frame renders");
        const auto cached_pixels = read_renderer_pixels(resources.renderer);
        check(visible_pixels_match(direct_pixels, cached_pixels),
              std::string("cached output preserves visible pixels for ") +
                  std::string(gameboy::video_mode_info(mode).id));
        resources.render_target = cached_target;
    }
    std::vector<ModeResult> results;
    for (const auto mode : modes) {
        gameboy::Emulator emulator{gameboy::Cartridge{render_test_rom()}};
        seed_scene(emulator);
        auto result = benchmark_mode(resources, mode, emulator, palette,
                                     capture_directory);
        result.mode = std::string(gameboy::video_mode_info(mode).id);
        results.push_back(result);
        std::cout << std::fixed << std::setprecision(2)
                  << "render_performance_metric mode="
                  << gameboy::video_mode_info(mode).id
                  << " fps=" << result.fps
                  << " elapsed_ms=" << result.elapsed_ms
                  << " fps_samples=" << result.fps_samples
                  << " mesh_vertices=" << result.mesh_vertices
                  << " mesh_indices=" << result.mesh_indices
                  << " total_us=" << result.total_us
                  << " geometry_build_us=" << result.geometry_build_us
                  << " geometry_sort_us=" << result.geometry_sort_us
                  << " geometry_submit_us=" << result.geometry_submit_us
                  << " render_cache_hits=" << result.render_cache_hits
                  << '\n';
    }
    const auto* json_path = std::getenv("GBB_RENDER_PERF_JSON");
    write_json(json_path == nullptr ? std::filesystem::path{}
                                   : std::filesystem::u8path(json_path),
               results);
    return failures == 0 ? 0 : 1;
}
