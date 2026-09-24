#include "sdl_resources.hpp"
#include "tool_window_support.hpp"

#include <stdexcept>
#include <string>

namespace gbb::sdl {
namespace {

[[noreturn]] void sdl_error(const std::string& action) {
    throw std::runtime_error(action + ": " + SDL_GetError());
}

} // namespace

SdlResources::SdlResources(const std::string_view version,
                           const bool initially_hidden) {
    try {
        // SDL_SetAppMetadata may retain the pointer, so keep the version in a
        // null-terminated owned string for the duration of the call.
        const std::string app_version(version);
        if (!SDL_SetAppMetadata("Go Bigger Boy (GBB)", app_version.c_str(),
                                "go-bigger-boy")) {
            sdl_error("Could not set application metadata");
        }
        if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
            sdl_error("Could not initialize SDL");
        }
        sdl_initialized_ = true;
        auto window_flags = static_cast<SDL_WindowFlags>(SDL_WINDOW_RESIZABLE);
        if (initially_hidden) window_flags |= SDL_WINDOW_HIDDEN;
        window = SDL_CreateWindow(
            "Go Bigger Boy (GBB) - Drop a ROM here or press Ctrl+O", 640, 576,
            window_flags);
        if (window == nullptr) sdl_error("Could not create window");
        // SDL normally selects the first available renderer, but the order
        // can vary with the DLL/runtime location on Windows. Prefer the
        // hardware Direct3D11 backend explicitly so a local copy cannot
        // silently fall back to the much slower software renderer. Keep the
        // generic path as a compatibility fallback for systems without D3D11.
#ifdef _WIN32
        renderer = SDL_CreateRenderer(window, "direct3d11");
        if (renderer == nullptr) renderer = SDL_CreateRenderer(window, nullptr);
#else
        renderer = SDL_CreateRenderer(window, nullptr);
#endif
        if (renderer == nullptr) sdl_error("Could not create renderer");
#ifdef __ANDROID__
        const auto* renderer_name = SDL_GetRendererName(renderer);
        SDL_Log("GBB renderer backend=%s",
                renderer_name == nullptr ? "unknown" : renderer_name);
#endif
        if (!SDL_SetRenderLogicalPresentation(
                renderer, static_cast<int>(gameboy::Ppu::screen_width),
                static_cast<int>(gameboy::Ppu::screen_height),
                SDL_LOGICAL_PRESENTATION_LETTERBOX)) {
            sdl_error("Could not configure logical presentation");
        }
        texture = SDL_CreateTexture(
            renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
            static_cast<int>(gameboy::Ppu::screen_width),
            static_cast<int>(gameboy::Ppu::screen_height));
        if (texture == nullptr) sdl_error("Could not create framebuffer texture");
        link_texture = SDL_CreateTexture(
            renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
            static_cast<int>(gameboy::Ppu::screen_width),
            static_cast<int>(gameboy::Ppu::screen_height));
        if (link_texture == nullptr) {
            sdl_error("Could not create link framebuffer texture");
        }
        sgb_border_texture = SDL_CreateTexture(
            renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
            static_cast<int>(gameboy::Ppu::sgb_border_width),
            static_cast<int>(gameboy::Ppu::sgb_border_height));
        if (sgb_border_texture == nullptr) {
            sdl_error("Could not create SGB border texture");
        }
#ifdef __ANDROID__
        sgb_viewport_overlay_texture = SDL_CreateTexture(
            renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
            static_cast<int>(gameboy::Ppu::screen_width),
            static_cast<int>(gameboy::Ppu::screen_height));
        if (sgb_viewport_overlay_texture == nullptr ||
            !SDL_SetTextureBlendMode(sgb_viewport_overlay_texture,
                                     SDL_BLENDMODE_BLEND) ||
            !SDL_SetTextureScaleMode(sgb_viewport_overlay_texture,
                                     SDL_SCALEMODE_NEAREST)) {
            sdl_error("Could not create SGB viewport overlay texture");
        }
#endif
        voxel_texture = SDL_CreateTexture(
            renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
            static_cast<int>(gameboy::Ppu::screen_width),
            static_cast<int>(gameboy::Ppu::screen_height));
        if (voxel_texture == nullptr) {
            sdl_error("Could not create native voxel framebuffer texture");
        }
        voxel_render_target = SDL_CreateTexture(
            renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET,
            static_cast<int>(gameboy::Ppu::screen_width),
            static_cast<int>(gameboy::Ppu::screen_height));
        if (voxel_render_target == nullptr) {
            sdl_error("Could not create voxel render target");
        }
        if (!SDL_SetTextureScaleMode(voxel_render_target,
                                     SDL_SCALEMODE_NEAREST)) {
            sdl_error("Could not configure nearest-neighbor voxel render target scaling");
        }
        if (!SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_NEAREST)) {
            sdl_error("Could not configure nearest-neighbor scaling");
        }
        if (!SDL_SetTextureScaleMode(sgb_border_texture,
                                     SDL_SCALEMODE_NEAREST)) {
            sdl_error("Could not configure nearest-neighbor SGB border scaling");
        }
        if (!SDL_SetTextureScaleMode(voxel_texture, SDL_SCALEMODE_NEAREST)) {
            sdl_error("Could not configure nearest-neighbor voxel scaling");
        }
        static_cast<void>(SDL_SetRenderDrawColor(renderer, 16, 20, 16, 255));
    } catch (...) {
        release();
        throw;
    }
}

SdlResources::~SdlResources() {
    release();
}

void SdlResources::release() noexcept {
    camera.close();
    if (gamepad != nullptr) {
        static_cast<void>(SDL_RumbleGamepad(gamepad, 0, 0, 0));
        SDL_CloseGamepad(gamepad);
        gamepad = nullptr;
    }
    audio.close();
    if (texture != nullptr) {
        SDL_DestroyTexture(texture);
        texture = nullptr;
    }
    if (link_texture != nullptr) {
        SDL_DestroyTexture(link_texture);
        link_texture = nullptr;
    }
    if (sgb_border_texture != nullptr) {
        SDL_DestroyTexture(sgb_border_texture);
        sgb_border_texture = nullptr;
    }
    if (sgb_viewport_overlay_texture != nullptr) {
        SDL_DestroyTexture(sgb_viewport_overlay_texture);
        sgb_viewport_overlay_texture = nullptr;
    }
#ifdef __ANDROID__
    if (touch_overlay_texture != nullptr) {
        SDL_DestroyTexture(touch_overlay_texture);
        touch_overlay_texture = nullptr;
    }
#endif
    if (voxel_texture != nullptr) {
        SDL_DestroyTexture(voxel_texture);
        voxel_texture = nullptr;
    }
    if (voxel_render_target != nullptr) {
        SDL_DestroyTexture(voxel_render_target);
        voxel_render_target = nullptr;
    }
    if (renderer != nullptr) {
        clear_tool_text_cache(renderer);
        SDL_DestroyRenderer(renderer);
        renderer = nullptr;
    }
    if (window != nullptr) {
        SDL_DestroyWindow(window);
        window = nullptr;
    }
    if (sdl_initialized_) {
        SDL_Quit();
        sdl_initialized_ = false;
    }
}

} // namespace gbb::sdl
