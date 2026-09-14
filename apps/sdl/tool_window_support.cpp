#include "tool_window_support.hpp"

#ifdef GBB_HAS_SDL_TTF
#include <SDL3_ttf/SDL_ttf.h>
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>

namespace gbb::sdl {
namespace {

#ifdef GBB_HAS_SDL_TTF
struct CachedToolText {
    SDL_Texture* texture{};
    int width{};
    int height{};
};

struct ToolRendererCache {
    std::unordered_map<std::string, CachedToolText> entries;
};

std::unordered_map<int, TTF_Font*>& tool_fonts() {
    static std::unordered_map<int, TTF_Font*> fonts;
    return fonts;
}

std::unordered_map<SDL_Renderer*, ToolRendererCache>& tool_text_cache() {
    static std::unordered_map<SDL_Renderer*, ToolRendererCache> cache;
    return cache;
}

TTF_Font* font_for_renderer(SDL_Renderer* renderer, int& size) {
    size = 14;
    if (renderer != nullptr) {
        auto* window = SDL_GetRenderWindow(renderer);
        int window_width = 0;
        int window_height = 0;
        int output_width = 0;
        int output_height = 0;
        if (window != nullptr && SDL_GetWindowSize(window, &window_width,
                                                   &window_height) &&
            SDL_GetRenderOutputSize(renderer, &output_width, &output_height) &&
            window_width > 0 && window_height > 0) {
            const auto scale = std::clamp(
                std::max(static_cast<float>(output_width) / window_width,
                         static_cast<float>(output_height) / window_height),
                1.0F, 1.15F);
            size = std::clamp(static_cast<int>(std::lround(14.0F * scale)),
                              14, 16);
        }
    }
    auto& fonts = tool_fonts();
    if (const auto found = fonts.find(size); found != fonts.end()) {
        return found->second;
    }
    constexpr std::array<const char*, 5> candidates{
        "fonts/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "C:/Windows/Fonts/segoeui.ttf",
        "/System/Library/Fonts/Supplemental/Arial.ttf",
        "/System/Library/Fonts/SFNS.ttf"};
    for (const auto* candidate : candidates) {
        auto* font = TTF_OpenFont(candidate, static_cast<float>(size));
        if (font != nullptr) return fonts.emplace(size, font).first->second;
    }
    return nullptr;
}
#endif

bool tool_button_hovered(SDL_Window* window, const SDL_FRect& rect) {
    if (window == nullptr) return false;
    float x = 0.0F;
    float y = 0.0F;
    if (!SDL_GetMouseState(&x, &y)) return false;
    return x >= rect.x && x <= rect.x + rect.w && y >= rect.y &&
           y <= rect.y + rect.h;
}

} // namespace

void render_tool_text(SDL_Renderer* renderer, const float x, const float y,
                      const char* value) {
#ifdef GBB_HAS_SDL_TTF
    static std::once_flag initialized;
    std::call_once(initialized, [] {
        if (!TTF_Init()) return;
    });
    int font_size = 14;
    auto* const font = font_for_renderer(renderer, font_size);
    if (font != nullptr && renderer != nullptr && value != nullptr &&
        *value != '\0') {
        SDL_Color color{255, 255, 255, 255};
        static_cast<void>(SDL_GetRenderDrawColor(renderer, &color.r, &color.g,
                                                  &color.b, &color.a));
        std::string key{value};
        key.push_back('\0');
        key.push_back(static_cast<char>(color.r));
        key.push_back(static_cast<char>(color.g));
        key.push_back(static_cast<char>(color.b));
        key.push_back(static_cast<char>(color.a));
        key.push_back(static_cast<char>(font_size));
        auto& entries = tool_text_cache()[renderer].entries;
        auto cached = entries.find(key);
        if (cached == entries.end()) {
            auto* surface = TTF_RenderText_Blended(font, value, 0, color);
            if (surface != nullptr) {
                auto* texture = SDL_CreateTextureFromSurface(renderer, surface);
                if (texture != nullptr) {
                    static_cast<void>(SDL_SetTextureBlendMode(
                        texture, SDL_BLENDMODE_BLEND));
                    if (entries.size() >= 256) {
                        SDL_DestroyTexture(entries.begin()->second.texture);
                        entries.erase(entries.begin());
                    }
                    cached = entries.emplace(
                        std::move(key),
                        CachedToolText{texture, surface->w, surface->h})
                                  .first;
                }
                SDL_DestroySurface(surface);
            }
        }
        if (cached != entries.end()) {
            const SDL_FRect destination{
                x, y, static_cast<float>(cached->second.width),
                static_cast<float>(cached->second.height)};
            static_cast<void>(SDL_RenderTexture(renderer,
                                                 cached->second.texture,
                                                 nullptr, &destination));
            return;
        }
    }
#endif
    if (renderer != nullptr && value != nullptr) {
        // SDL's built-in debug font is only 8px high. Keep it as a dependency-
        // free fallback, but enlarge it slightly so tool windows remain
        // legible when SDL_ttf is unavailable.
        constexpr float fallback_scale = 1.15F;
        float old_scale_x = 1.0F;
        float old_scale_y = 1.0F;
        static_cast<void>(SDL_GetRenderScale(renderer, &old_scale_x,
                                             &old_scale_y));
        static_cast<void>(SDL_SetRenderScale(
            renderer, old_scale_x * fallback_scale,
            old_scale_y * fallback_scale));
        static_cast<void>(SDL_RenderDebugText(renderer, x / fallback_scale,
                                              y / fallback_scale, value));
        static_cast<void>(SDL_SetRenderScale(renderer, old_scale_x,
                                             old_scale_y));
    }
}

void clear_tool_text_cache(SDL_Renderer* renderer) noexcept {
#ifdef GBB_HAS_SDL_TTF
    if (renderer == nullptr) return;
    auto& cache = tool_text_cache();
    const auto found = cache.find(renderer);
    if (found == cache.end()) return;
    for (const auto& [unused, text] : found->second.entries) {
        SDL_DestroyTexture(text.texture);
    }
    cache.erase(found);
#else
    static_cast<void>(renderer);
#endif
}

void draw_tool_button_background(SDL_Renderer* renderer, SDL_Window* window,
                                 const SDL_FRect& rect) {
    const auto hovered = tool_button_hovered(window, rect);
    float mouse_x = 0.0F;
    float mouse_y = 0.0F;
    const auto mouse_buttons = SDL_GetMouseState(&mouse_x, &mouse_y);
    const auto pressed = hovered && (mouse_buttons & SDL_BUTTON_LMASK) != 0;
    static_cast<void>(SDL_SetRenderDrawColor(
        renderer, pressed ? 24 : (hovered ? 40 : 28),
        pressed ? 58 : (hovered ? 74 : 47),
        pressed ? 80 : (hovered ? 98 : 68), 255));
    static_cast<void>(SDL_RenderFillRect(renderer, &rect));
    static_cast<void>(SDL_SetRenderDrawColor(
        renderer, pressed ? 93 : (hovered ? 120 : 69),
        pressed ? 207 : (hovered ? 232 : 207),
        pressed ? 230 : (hovered ? 250 : 238), 255));
    static_cast<void>(SDL_RenderRect(renderer, &rect));
}

SDL_FRect tool_close_button_rect(const int width) noexcept {
    return {static_cast<float>(std::max(24, width - 150)), 12.0F, 126.0F,
            36.0F};
}

bool tool_close_button_hit(const int width, const int height, const float x,
                           const float y) noexcept {
    const auto rect = tool_close_button_rect(width);
    return x >= rect.x && x <= rect.x + rect.w && y >= rect.y &&
           y <= rect.y + rect.h && height >= 64;
}

void draw_tool_close_button(SDL_Renderer* renderer, SDL_Window* window,
                            const int width) {
    const auto rect = tool_close_button_rect(width);
    draw_tool_button_background(renderer, window, rect);
    static_cast<void>(SDL_SetRenderDrawColor(renderer, 238, 249, 255, 255));
    render_tool_text(renderer, rect.x + 28.0F, rect.y + 13.0F, "CLOSE");
}

bool confirm_discard_changes(SDL_Window* window, const char* message) {
    constexpr SDL_MessageBoxButtonData buttons[] = {
        {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, "Keep editing"},
        {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, "Discard"},
    };
    const SDL_MessageBoxData box{
        SDL_MESSAGEBOX_WARNING, window, "Unsaved changes", message, 2,
        buttons, nullptr,
    };
    int selection = 0;
    return SDL_ShowMessageBox(&box, &selection) && selection == 1;
}

} // namespace gbb::sdl
