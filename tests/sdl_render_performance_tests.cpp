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
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace {

constexpr unsigned cycles_per_frame = 70224;
constexpr unsigned measured_frames = 24;

int failures = 0;

void check(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
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
    if (value == nullptr || *value == '\0') return 1.0;
    char* end = nullptr;
    const auto parsed = std::strtod(value, &end);
    if (end == value || *end != '\0' || parsed < 0.0) return 1.0;
    return parsed;
}

struct RenderResources final {
    SDL_Window* window{};
    SDL_Renderer* renderer{};
    SDL_Texture* texture{};

    ~RenderResources() {
        if (texture != nullptr) SDL_DestroyTexture(texture);
        if (renderer != nullptr) SDL_DestroyRenderer(renderer);
        if (window != nullptr) SDL_DestroyWindow(window);
        SDL_Quit();
    }
};

struct ModeResult final {
    double fps{};
    double elapsed_ms{};
    unsigned fps_samples{};
    std::size_t mesh_vertices{};
    std::size_t mesh_indices{};
};

ModeResult benchmark_mode(RenderResources& resources,
                          const gameboy::VideoMode mode,
                          gameboy::Emulator& emulator,
                          const gameboy::DisplayPalette& palette) {
    gbb::SceneSnapshot scene_snapshot;
    std::filesystem::path profile_path;
    gbb::VoxelProfile profile;
    std::uint64_t profile_fingerprint = 0;
    bool profile_loaded = false;
    gbb::VoxelScene voxel_scene;
    std::uint64_t scene_signature = 0;
    bool scene_cached = false;
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

    for (unsigned frame = 0; frame < measured_frames; ++frame) {
        const auto advance = gbb::advance_to_frame(emulator, cycles_per_frame * 2U);
        check(advance.frame_ready, "render benchmark reaches every frame");
        if (!advance.frame_ready) break;

        if (completed_frames == 0) {
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
                camera_yaw_offset};
            rendered = gbb::sdl::render_voxel_diorama(
                emulator, context, palette,
                mode == gameboy::VideoMode::voxel_shape,
                mode == gameboy::VideoMode::voxel_popup);
        }
        check(rendered, "the production renderer accepts every benchmark mode");
        check(SDL_RenderPresent(resources.renderer),
              "SDL presents every benchmark frame");
        const auto render_finished = std::chrono::steady_clock::now();
        if (fps_metrics.observe(render_finished).has_value()) ++fps_samples;
        emulator.consume_frame();
        ++completed_frames;
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
    return {fps, elapsed * 1000.0, fps_samples, voxel_stats.mesh_vertices,
            voxel_stats.mesh_indices};
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
    if (resources.window == nullptr || resources.renderer == nullptr ||
        resources.texture == nullptr) {
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
    for (const auto mode : modes) {
        gameboy::Emulator emulator{gameboy::Cartridge{render_test_rom()}};
        seed_scene(emulator);
        const auto result = benchmark_mode(resources, mode, emulator, palette);
        std::cout << std::fixed << std::setprecision(2)
                  << "render_performance_metric mode="
                  << gameboy::video_mode_info(mode).id
                  << " fps=" << result.fps
                  << " elapsed_ms=" << result.elapsed_ms
                  << " fps_samples=" << result.fps_samples
                  << " mesh_vertices=" << result.mesh_vertices
                  << " mesh_indices=" << result.mesh_indices << '\n';
    }
    return failures == 0 ? 0 : 1;
}
