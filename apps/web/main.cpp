#define SDL_MAIN_USE_CALLBACKS
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include "gameboy/emulator.hpp"
#include "gameboy/display_palette.hpp"
#include "gameboy/video_pipeline.hpp"
#include "gbb/core_registry.hpp"
#include "gbb/gameboy_core.hpp"
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
#include <sstream>
#include <string>
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

SDL_FColor voxel_color(const std::uint32_t pixel, const float shade) {
    const auto component = [pixel, shade](const unsigned shift) {
        const auto value = static_cast<float>((pixel >> shift) & 0xFFU) / 255.0F;
        return std::clamp(value * shade + 0.02F, 0.0F, 1.0F);
    };
    return {component(16), component(8), component(0), 1.0F};
}

void render_web_voxel(WebApp& app, const std::vector<std::uint32_t>& pixels) {
    constexpr unsigned cell_size = 2;
    constexpr unsigned cells_x = 160 / cell_size;
    constexpr unsigned cells_y = 144 / cell_size;
    constexpr float base_depth = -5.0F;
    constexpr float zoom = 0.78F;
    constexpr float perspective = 0.0012F;
    const auto yaw = (-18.0F + app.voxel_camera_yaw_offset) *
                     0.01745329251994329577F;
    const auto pitch = (27.0F + app.voxel_camera_pitch_offset) *
                       0.01745329251994329577F;
    const auto yaw_cos = std::cos(yaw);
    const auto yaw_sin = std::sin(yaw);
    const auto pitch_cos = std::cos(pitch);
    const auto pitch_sin = std::sin(pitch);
    const auto luminance = [](const std::uint32_t pixel) {
        return (0.2126F * static_cast<float>((pixel >> 16) & 0xFFU) +
                0.7152F * static_cast<float>((pixel >> 8) & 0xFFU) +
                0.0722F * static_cast<float>(pixel & 0xFFU)) / 255.0F;
    };
    const auto pixel_at = [&](const int x, const int y) {
        const auto clamped_x = std::clamp(x, 0, 159);
        const auto clamped_y = std::clamp(y, 0, 143);
        return pixels[static_cast<std::size_t>(clamped_y) * 160U +
                      static_cast<std::size_t>(clamped_x)];
    };
    const auto project = [&](const float x, const float y, const float z) {
        const auto centered_x = x - 80.0F;
        const auto centered_y = y - 72.0F;
        // Rotate the framebuffer around its vertical center axis, then pitch
        // it toward the viewer. This is the same camera convention as the
        // desktop diorama, expressed in the browser's logical coordinates.
        const auto yaw_x = centered_x * yaw_cos - z * yaw_sin;
        const auto yaw_depth = centered_x * yaw_sin + z * yaw_cos;
        const auto pitched_y = centered_y * pitch_cos - yaw_depth * pitch_sin;
        const auto depth = centered_y * pitch_sin + yaw_depth * pitch_cos;
        const auto scale = 1.0F / std::max(0.35F, 1.0F + depth * perspective);
        return SDL_FPoint{80.0F + yaw_x * zoom * scale,
                          72.0F + pitched_y * zoom * scale};
    };
    auto& vertices = app.voxel_vertices;
    auto& indices = app.voxel_indices;
    vertices.clear();
    indices.clear();
    vertices.reserve(cells_x * cells_y * 20U);
    indices.reserve(cells_x * cells_y * 30U);
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
    std::array<float, cells_x * cells_y> heights{};
    for (unsigned row = 0; row < cells_y; ++row) {
        for (unsigned column = 0; column < cells_x; ++column) {
            const auto x = static_cast<int>(column * cell_size);
            const auto y = static_cast<int>(row * cell_size);
            const auto pixel = pixel_at(x, y);
            const auto center_luma = luminance(pixel);
            auto minimum = center_luma;
            auto maximum = center_luma;
            for (int oy = -2; oy <= 2; oy += 2) {
                for (int ox = -2; ox <= 2; ox += 2) {
                    const auto neighbor = luminance(pixel_at(x + ox, y + oy));
                    minimum = std::min(minimum, neighbor);
                    maximum = std::max(maximum, neighbor);
                }
            }
            const auto edge = maximum - minimum;
            // Flat fills stay close to the recessed screen; outlines and
            // silhouettes receive a modest extrusion that reads as depth
            // without turning the image into a wall of black voxels.
            heights[row * cells_x + column] =
                base_depth + std::min(7.0F, edge * 8.0F +
                                               (1.0F - center_luma) * 0.55F);
        }
    }
    for (unsigned row = 0; row < cells_y; ++row) {
        for (unsigned column = 0; column < cells_x; ++column) {
            const auto x = static_cast<float>(column * cell_size);
            const auto y = static_cast<float>(row * cell_size);
            const auto depth = heights[row * cells_x + column];
            const auto color = voxel_color(pixel_at(static_cast<int>(x),
                                                    static_cast<int>(y)), 0.94F);
            // Continuous recessed plane: no source pixels disappear between
            // raised columns, even when a browser renderer has no depth test.
            add_quad(project(x, y, base_depth),
                     project(x + cell_size, y, base_depth),
                     project(x + cell_size, y + cell_size, base_depth),
                     project(x, y + cell_size, base_depth), color);
            if (depth <= base_depth + 0.05F) continue;
            const auto top_a = project(x, y, depth);
            const auto top_b = project(x + cell_size, y, depth);
            const auto top_c = project(x + cell_size, y + cell_size, depth);
            const auto top_d = project(x, y + cell_size, depth);
            const auto base_a = project(x, y, base_depth);
            const auto base_b = project(x + cell_size, y, base_depth);
            const auto base_c = project(x + cell_size, y + cell_size, base_depth);
            const auto base_d = project(x, y + cell_size, base_depth);
            const auto neighbor = [&](const int dx, const int dy) {
                const auto nx = static_cast<int>(column) + dx;
                const auto ny = static_cast<int>(row) + dy;
                if (nx < 0 || ny < 0 || nx >= static_cast<int>(cells_x) ||
                    ny >= static_cast<int>(cells_y)) return base_depth;
                return heights[static_cast<std::size_t>(ny) * cells_x +
                               static_cast<std::size_t>(nx)];
            };
            if (depth > neighbor(0, -1) + 0.45F)
                add_quad(base_a, base_b, top_b, top_a,
                         voxel_color(pixel_at(static_cast<int>(x),
                                              static_cast<int>(y)), 0.62F));
            if (depth > neighbor(1, 0) + 0.45F)
                add_quad(base_b, base_c, top_c, top_b,
                         voxel_color(pixel_at(static_cast<int>(x),
                                              static_cast<int>(y)), 0.72F));
            add_quad(top_a, top_b, top_c, top_d, color);
        }
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
        if (app.video_mode == gameboy::VideoMode::voxel_diorama &&
            frame.width == 160 && frame.height == 144) {
            render_web_voxel(app, app.display_pixels);
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
