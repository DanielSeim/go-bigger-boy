#include "tool_window_support.hpp"

#ifdef GBB_HAS_SDL_TTF
#include <SDL3_ttf/SDL_ttf.h>
#endif

#include <array>
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

std::unordered_map<SDL_Renderer*, ToolRendererCache>& tool_text_cache() {
    static std::unordered_map<SDL_Renderer*, ToolRendererCache> cache;
    return cache;
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
    static TTF_Font* font = nullptr;
    std::call_once(initialized, [] {
        if (!TTF_Init()) return;
        constexpr std::array<const char*, 5> candidates{
            "fonts/DejaVuSans.ttf",
            "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
            "C:/Windows/Fonts/segoeui.ttf",
            "/System/Library/Fonts/Supplemental/Arial.ttf",
            "/System/Library/Fonts/SFNS.ttf"};
        for (const auto* candidate : candidates) {
            font = TTF_OpenFont(candidate, 14.0F);
            if (font != nullptr) break;
        }
    });
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
        static_cast<void>(SDL_RenderDebugText(renderer, x, y, value));
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
    static_cast<void>(SDL_SetRenderDrawColor(
        renderer, hovered ? 40 : 28, hovered ? 74 : 47,
        hovered ? 98 : 68, 255));
    static_cast<void>(SDL_RenderFillRect(renderer, &rect));
    static_cast<void>(SDL_SetRenderDrawColor(
        renderer, hovered ? 120 : 69, hovered ? 232 : 207,
        hovered ? 250 : 238, 255));
    static_cast<void>(SDL_RenderRect(renderer, &rect));
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
