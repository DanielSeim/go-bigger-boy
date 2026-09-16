#include "android_touch_renderer.hpp"

#ifdef __ANDROID__

#include "android_touch_geometry.hpp"
#include "android_touch_input.hpp"
#include "emulation_session.hpp"
#include "presentation.hpp"
#include "sdl_resources.hpp"
#include "settings_model.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <string>

namespace gbb::sdl {

[[noreturn]] void sdl_error(const char* action) {
    throw std::runtime_error(std::string{action} + ": " + SDL_GetError());
}

void draw_touch_circle(SDL_Renderer* renderer, const float center_x,
                       const float center_y, const float radius,
                       const SDL_Color color) {
    static_cast<void>(SDL_SetRenderDrawColor(renderer, color.r, color.g,
                                             color.b, color.a));
    const auto top = static_cast<int>(std::ceil(center_y - radius));
    const auto bottom = static_cast<int>(std::floor(center_y + radius));
    for (auto y = top; y <= bottom; ++y) {
        const auto dy = static_cast<float>(y) - center_y;
        const auto span = std::sqrt(std::max(0.0F, radius * radius - dy * dy));
        const SDL_FRect row{center_x - span, static_cast<float>(y), span * 2.0F,
                            1.0F};
        static_cast<void>(SDL_RenderFillRect(renderer, &row));
    }
}

void draw_touch_pill(SDL_Renderer* renderer, const SDL_FRect rect,
                     const SDL_Color color) {
    // A capsule's radius is half its height. Using the full height here
    // places the cap centers below the rectangle and leaves only the top
    // half visible on Android.
    const auto radius = std::min(rect.h * 0.5F, rect.w * 0.5F);
    static_cast<void>(SDL_SetRenderDrawColor(renderer, color.r, color.g,
                                             color.b, color.a));
    const auto center_x = rect.x + rect.w * 0.5F;
    const auto center_y = rect.y + radius;
    const auto half_body = rect.w * 0.5F - radius;
    const auto top = static_cast<int>(std::ceil(rect.y));
    const auto bottom = static_cast<int>(std::floor(rect.y + rect.h));
    for (auto y = top; y <= bottom; ++y) {
        const auto dy = static_cast<float>(y) - center_y;
        const auto span = std::sqrt(
            std::max(0.0F, radius * radius - dy * dy));
        const SDL_FRect row{center_x - half_body - span,
                            static_cast<float>(y),
                            (half_body + span) * 2.0F, 1.0F};
        static_cast<void>(SDL_RenderFillRect(renderer, &row));
    }
}

void draw_touch_pill_accent(SDL_Renderer* renderer, const SDL_FRect rect,
                            const bool right, const float accent_width,
                            const SDL_Color color) {
    const auto radius = std::min(rect.h * 0.5F, rect.w * 0.5F);
    static_cast<void>(SDL_SetRenderDrawColor(renderer, color.r, color.g,
                                             color.b, color.a));
    const auto center_x = rect.x + rect.w * 0.5F;
    const auto center_y = rect.y + rect.h * 0.5F;
    const auto half_body = rect.w * 0.5F - radius;
    const auto top = static_cast<int>(std::ceil(rect.y));
    const auto bottom = static_cast<int>(std::floor(rect.y + rect.h));
    const auto accent_left = right ? rect.x + rect.w - accent_width : rect.x;
    const auto accent_right = right ? rect.x + rect.w : rect.x + accent_width;
    for (auto y = top; y <= bottom; ++y) {
        const auto dy = static_cast<float>(y) - center_y;
        const auto span = std::sqrt(
            std::max(0.0F, radius * radius - dy * dy));
        const auto left = std::max(center_x - half_body - span, accent_left);
        const auto right_edge = std::min(center_x + half_body + span,
                                         accent_right);
        if (right_edge <= left) continue;
        const SDL_FRect row{left, static_cast<float>(y),
                            right_edge - left, 1.0F};
        static_cast<void>(SDL_RenderFillRect(renderer, &row));
    }
}

void draw_touch_cross(SDL_Renderer* renderer, const SDL_FPoint center,
                      const float half_extent, const float thickness,
                      const SDL_Color color) {
    // The draft uses rounded rectangle corners, not pill-shaped arm ends.
    const auto radius = thickness * 0.30F;
    static_cast<void>(SDL_SetRenderDrawColor(renderer, color.r, color.g,
                                             color.b, color.a));
    const auto top = static_cast<int>(std::ceil(center.y - half_extent));
    const auto bottom = static_cast<int>(std::floor(center.y + half_extent));
    for (auto y = top; y <= bottom; ++y) {
        const auto distance = std::abs(static_cast<float>(y) - center.y);
        float half_width = 0.0F;
        // Union two rounded rectangles: one horizontal and one vertical.
        // This preserves the rounded ends of the draft instead of making the
        // horizontal arm terminate in square corners.
        if (distance <= radius) {
            const auto cap = std::sqrt(
                std::max(0.0F, radius * radius - distance * distance));
            half_width = half_extent - radius + cap;
        }
        if (distance <= half_extent) {
            const auto cap_distance = std::max(0.0F,
                                                distance - (half_extent - radius));
            const auto vertical_half_width =
                cap_distance == 0.0F
                    ? radius
                    : std::sqrt(std::max(0.0F, radius * radius -
                                                   cap_distance * cap_distance));
            half_width = std::max(half_width, vertical_half_width);
        }
        const SDL_FRect row{center.x - half_width, static_cast<float>(y),
                            half_width * 2.0F, 1.0F};
        static_cast<void>(SDL_RenderFillRect(renderer, &row));
    }
}

void draw_touch_cross_accent(SDL_Renderer* renderer, const SDL_FPoint center,
                             const float half_extent, const float thickness,
                             const float accent_width,
                             const SDL_Color color) {
    const auto radius = thickness * 0.30F;
    static_cast<void>(SDL_SetRenderDrawColor(renderer, color.r, color.g,
                                             color.b, color.a));
    const auto top = static_cast<int>(std::ceil(center.y - half_extent));
    const auto bottom = static_cast<int>(std::floor(center.y + half_extent));
    const auto accent_left = center.x - half_extent;
    const auto accent_right = accent_left + accent_width;
    for (auto y = top; y <= bottom; ++y) {
        const auto distance = std::abs(static_cast<float>(y) - center.y);
        float half_width = 0.0F;
        if (distance <= radius) {
            const auto cap = std::sqrt(
                std::max(0.0F, radius * radius - distance * distance));
            half_width = half_extent - radius + cap;
        }
        if (distance <= half_extent) {
            const auto cap_distance = std::max(
                0.0F, distance - (half_extent - radius));
            const auto vertical_half_width =
                cap_distance == 0.0F
                    ? radius
                    : std::sqrt(std::max(0.0F, radius * radius -
                                                   cap_distance * cap_distance));
            half_width = std::max(half_width, vertical_half_width);
        }
        const auto left = std::max(center.x - half_width, accent_left);
        const auto right = std::min(center.x + half_width, accent_right);
        if (right <= left) continue;
        const SDL_FRect row{left, static_cast<float>(y), right - left, 1.0F};
        static_cast<void>(SDL_RenderFillRect(renderer, &row));
    }
}

void draw_touch_cross_arm(SDL_Renderer* renderer, const SDL_FPoint center,
                          const float half_extent, const float thickness,
                          const std::size_t direction,
                          const SDL_Color color) {
    const auto radius = thickness * 0.30F;
    static_cast<void>(SDL_SetRenderDrawColor(renderer, color.r, color.g,
                                             color.b, color.a));
    const auto top = static_cast<int>(std::ceil(center.y - half_extent));
    const auto bottom = static_cast<int>(std::floor(center.y + half_extent));
    for (auto y = top; y <= bottom; ++y) {
        const auto distance = std::abs(static_cast<float>(y) - center.y);
        if (direction < 2) {
            if (distance > radius) continue;
            const auto cap = std::sqrt(
                std::max(0.0F, radius * radius - distance * distance));
            const auto extent = half_extent - radius + cap;
            const SDL_FRect row{
                direction == 0 ? center.x : center.x - extent,
                static_cast<float>(y), extent, 1.0F};
            static_cast<void>(SDL_RenderFillRect(renderer, &row));
        } else {
            const auto start = direction == 2 ? center.y - half_extent
                                              : center.y;
            const auto end = direction == 2 ? center.y : center.y + half_extent;
            const auto current_y = static_cast<float>(y);
            if (current_y < start || current_y > end) continue;
            float half_width = radius;
            if (direction == 2 && current_y < start + radius * 2.0F) {
                const auto cap_distance = current_y - (start + radius);
                half_width = std::sqrt(std::max(
                    0.0F, radius * radius - cap_distance * cap_distance));
            } else if (direction == 3 && current_y > end - radius * 2.0F) {
                const auto cap_distance = current_y - (end - radius);
                half_width = std::sqrt(std::max(
                    0.0F, radius * radius - cap_distance * cap_distance));
            }
            const SDL_FRect row{center.x - half_width, current_y,
                                half_width * 2.0F, 1.0F};
            static_cast<void>(SDL_RenderFillRect(renderer, &row));
        }
    }
}

void draw_touch_triangle(SDL_Renderer* renderer, const SDL_FPoint first,
                         const SDL_FPoint second, const SDL_FPoint third,
                         const SDL_Color color) {
    const SDL_FColor vertex_color{
        static_cast<float>(color.r) / 255.0F,
        static_cast<float>(color.g) / 255.0F,
        static_cast<float>(color.b) / 255.0F,
        static_cast<float>(color.a) / 255.0F};
    const SDL_Vertex vertices[3]{
        {first, vertex_color, {0.0F, 0.0F}},
        {second, vertex_color, {0.0F, 0.0F}},
        {third, vertex_color, {0.0F, 0.0F}},
    };
    static_cast<void>(SDL_RenderGeometry(renderer, nullptr, vertices, 3,
                                          nullptr, 0));
}

void draw_branded_touch_label(SDL_Renderer* renderer, const float center_x,
                              const float top, const float scale,
                              const std::uint8_t alpha, const char* label) {
    if (label == nullptr) return;
    const auto length = static_cast<float>(std::char_traits<char>::length(label));
    const auto text_scale = std::clamp(scale * 0.62F, 4.0F, 5.8F);
    static_cast<void>(SDL_SetRenderScale(renderer, text_scale, text_scale));
    static_cast<void>(SDL_SetRenderDrawColor(renderer, 8, 175, 244, alpha));
    static_cast<void>(SDL_RenderDebugText(
        renderer, center_x / text_scale - length * 4.0F,
        top / text_scale, label));
    static_cast<void>(SDL_SetRenderScale(renderer, 1.0F, 1.0F));
}

void draw_touch_frame(SDL_Renderer* renderer, const SDL_FRect rect,
                      const float thickness, const SDL_Color color) {
    if (rect.w <= 0.0F || rect.h <= 0.0F || thickness <= 0.0F) return;
    static_cast<void>(SDL_SetRenderDrawColor(renderer, color.r, color.g,
                                             color.b, color.a));
    const auto horizontal = std::min(thickness, rect.h * 0.5F);
    const auto vertical = std::min(thickness, rect.w * 0.5F);
    const SDL_FRect top{rect.x, rect.y, rect.w, horizontal};
    const SDL_FRect bottom{rect.x, rect.y + rect.h - horizontal, rect.w,
                           horizontal};
    const SDL_FRect left{rect.x, rect.y + horizontal, vertical,
                         rect.h - horizontal * 2.0F};
    const SDL_FRect right{rect.x + rect.w - vertical, rect.y + horizontal,
                          vertical, rect.h - horizontal * 2.0F};
    static_cast<void>(SDL_RenderFillRect(renderer, &top));
    static_cast<void>(SDL_RenderFillRect(renderer, &bottom));
    static_cast<void>(SDL_RenderFillRect(renderer, &left));
    static_cast<void>(SDL_RenderFillRect(renderer, &right));
}

void draw_branded_touch_dpad(SDL_Renderer* renderer, const SDL_FPoint center,
                             const float scale, const std::uint8_t alpha,
                             const std::array<bool, 4>& pressed,
                             const bool landscape) {
    const auto half_extent = android_touch_dpad_dimension * scale * 0.5F;
    const auto thickness = (landscape ? 30.0F : 26.0F) * scale;
    const auto shadow_offset = 3.0F * scale;
    draw_touch_cross(renderer, {center.x + shadow_offset,
                                center.y + shadow_offset},
                     half_extent, thickness, SDL_Color{0, 0, 0, 120});
    draw_touch_cross(renderer, center, half_extent, thickness,
                     SDL_Color{247, 249, 250, alpha});
    draw_touch_cross_accent(renderer, center, half_extent, thickness,
                            4.0F * scale, SDL_Color{8, 175, 244, alpha});
    const auto inset = 3.0F * scale;
    draw_touch_cross(renderer, center, half_extent - inset,
                     thickness - inset * 2.0F, SDL_Color{12, 18, 24, alpha});
    for (std::size_t direction = 0; direction < pressed.size(); ++direction) {
        if (pressed[direction]) {
            draw_touch_cross_arm(renderer, center, half_extent - inset,
                                 thickness - inset * 2.0F, direction,
                                 SDL_Color{8, 175, 244, alpha});
        }
    }
    draw_touch_circle(renderer, center.x, center.y, thickness * 0.35F,
                      SDL_Color{18, 25, 31, alpha});

    static_cast<void>(SDL_SetRenderDrawColor(renderer, 8, 175, 244, alpha));
    const auto arrow = thickness * 0.12F;
    const auto arrow_offset = half_extent * 0.56F;
    const auto cyan = SDL_Color{8, 175, 244, alpha};
    draw_touch_triangle(
        renderer, {center.x, center.y - arrow_offset - arrow},
        {center.x - arrow, center.y - arrow_offset + arrow},
        {center.x + arrow, center.y - arrow_offset + arrow}, cyan);
    draw_touch_triangle(
        renderer, {center.x, center.y + arrow_offset + arrow},
        {center.x - arrow, center.y + arrow_offset - arrow},
        {center.x + arrow, center.y + arrow_offset - arrow}, cyan);
    draw_touch_triangle(
        renderer, {center.x - arrow_offset - arrow, center.y},
        {center.x - arrow_offset + arrow, center.y - arrow},
        {center.x - arrow_offset + arrow, center.y + arrow}, cyan);
    draw_touch_triangle(
        renderer, {center.x + arrow_offset + arrow, center.y},
        {center.x + arrow_offset - arrow, center.y - arrow},
        {center.x + arrow_offset - arrow, center.y + arrow}, cyan);
}

void draw_branded_touch_circle(SDL_Renderer* renderer, const SDL_FPoint center,
                               const float scale, const std::uint8_t alpha,
                               const bool pressed, const char* label) {
    const auto radius = android_touch_action_diameter * scale * 0.5F;
    const auto outer_radius = radius + 3.0F * scale;
    const auto shadow = 3.0F * scale;
    draw_touch_circle(renderer, center.x + shadow, center.y + shadow,
                      outer_radius, SDL_Color{0, 0, 0, 120});
    draw_touch_circle(renderer, center.x, center.y, outer_radius,
                      SDL_Color{247, 249, 250, alpha});
    draw_touch_circle(renderer, center.x, center.y, radius,
                      pressed ? SDL_Color{8, 175, 244, alpha}
                              : SDL_Color{12, 18, 24, alpha});
    draw_touch_circle(renderer, center.x, center.y, radius * 0.34F,
                      pressed ? SDL_Color{118, 216, 255, alpha}
                              : SDL_Color{8, 175, 244, alpha});
    draw_branded_touch_label(renderer, center.x,
                             center.y + outer_radius + 8.0F * scale, scale,
                             alpha, label);
}

void draw_branded_touch_system(SDL_Renderer* renderer, const SDL_FPoint center,
                               const float scale, const std::uint8_t alpha,
                               const bool pressed, const char* label) {
    const auto width = android_touch_system_width * scale;
    const auto height = android_touch_system_height * scale;
    const auto outer_width = width + 4.0F * scale;
    const auto outer_height = height + 4.0F * scale;
    const SDL_FRect outer{center.x - outer_width * 0.5F,
                          center.y - outer_height * 0.5F, outer_width,
                          outer_height};
    draw_touch_pill(renderer,
                    {outer.x + 3.0F * scale, outer.y + 3.0F * scale,
                     outer.w, outer.h},
                    SDL_Color{0, 0, 0, 120});
    draw_touch_pill(renderer, outer, SDL_Color{247, 249, 250, alpha});
    draw_touch_pill_accent(renderer, outer,
                           label != nullptr &&
                               std::strcmp(label, "START") == 0,
                           4.0F * scale, SDL_Color{8, 175, 244, alpha});
    const SDL_FRect inner{center.x - width * 0.5F, center.y - height * 0.5F,
                          width, height};
    draw_touch_pill(renderer, inner,
                    pressed ? SDL_Color{8, 175, 244, alpha}
                            : SDL_Color{12, 18, 24, alpha});
    draw_branded_touch_label(renderer, center.x,
                             outer.y + outer.h + 8.0F * scale, scale, alpha,
                             label);
}

void present_touch_controls(SdlResources& sdl) {
    const auto opacity = std::clamp(sdl.touch_settings.opacity,
                                    minimum_touch_opacity,
                                    maximum_touch_opacity);
    static_cast<void>(SDL_SetRenderDrawBlendMode(sdl.renderer,
                                                 SDL_BLENDMODE_BLEND));
    const auto size = touch_control_scale(sdl);
    SDL_FRect game_viewport{};
    const auto have_game_viewport =
        touch_is_landscape(sdl) &&
        SDL_GetRenderLogicalPresentationRect(sdl.renderer, &game_viewport);
    if (!SDL_SetRenderLogicalPresentation(
            sdl.renderer, 0, 0, SDL_LOGICAL_PRESENTATION_DISABLED)) {
        sdl_error("Could not prepare touch controls");
    }
    const auto alpha = static_cast<std::uint8_t>(std::clamp(
        opacity * 255.0F, 0.0F, 255.0F));
    const auto point_for = [&sdl, have_game_viewport,
                            game_viewport](const std::size_t control) {
        return have_game_viewport
                   ? touch_control_pixel_position_for_viewport(
                         sdl, control, game_viewport)
                   : touch_control_pixel_position(sdl, control);
    };

    if (!touch_is_landscape(sdl)) {
        const auto game_rect = android_portrait_game_rect(sdl);
        const auto frame = std::clamp(size * 0.70F, 5.0F, 10.0F);
        const SDL_FRect shadow_frame{game_rect.x - frame - 3.0F,
                                     game_rect.y - frame - 3.0F,
                                     game_rect.w + (frame + 3.0F) * 2.0F,
                                     game_rect.h + (frame + 3.0F) * 2.0F};
        draw_touch_frame(sdl.renderer, shadow_frame, frame + 3.0F,
                         SDL_Color{0, 0, 0, 150});
        const SDL_FRect cyan_frame{game_rect.x - frame, game_rect.y - frame,
                                   game_rect.w + frame * 2.0F,
                                   game_rect.h + frame * 2.0F};
        draw_touch_frame(sdl.renderer, cyan_frame, frame,
                         SDL_Color{8, 175, 244, alpha});
        draw_touch_frame(sdl.renderer,
                         {game_rect.x - 2.0F, game_rect.y - 2.0F,
                          game_rect.w + 4.0F, game_rect.h + 4.0F},
                         2.0F, SDL_Color{8, 15, 22, alpha});

        draw_branded_touch_dpad(
            sdl.renderer, point_for(0), size, alpha,
            {sdl.touch_buttons[0], sdl.touch_buttons[1],
             sdl.touch_buttons[2], sdl.touch_buttons[3]}, false);
        draw_branded_touch_circle(sdl.renderer, point_for(1), size, alpha,
                                  sdl.touch_buttons[4], "A");
        draw_branded_touch_circle(sdl.renderer, point_for(2), size, alpha,
                                  sdl.touch_buttons[5], "B");
        draw_branded_touch_system(sdl.renderer, point_for(3), size, alpha,
                                  sdl.touch_buttons[6], "SELECT");
        draw_branded_touch_system(sdl.renderer, point_for(4), size, alpha,
                                  sdl.touch_buttons[7], "START");

        if (!restore_video_presentation(sdl)) {
            sdl_error("Could not restore game presentation");
        }
        static_cast<void>(SDL_SetRenderDrawBlendMode(sdl.renderer,
                                                     SDL_BLENDMODE_NONE));
        return;
    }

    const auto safe = android_safe_area(sdl);
    const auto decoration_size = std::clamp(
        static_cast<float>(std::min(safe.w, safe.h)) * 0.055F, 32.0F, 72.0F);
    const auto decoration_stroke = std::clamp(decoration_size * 0.18F, 5.0F,
                                              10.0F);
    static_cast<void>(SDL_SetRenderDrawColor(sdl.renderer, 8, 175, 244, alpha));
    const auto draw_corner = [&](const float x, const float y,
                                 const bool right, const bool bottom) {
        const auto horizontal_x = right ? x - decoration_size : x;
        const auto vertical_y = bottom ? y - decoration_size : y;
        const SDL_FRect horizontal{horizontal_x, y - (bottom ? decoration_stroke : 0.0F),
                                   decoration_size, decoration_stroke};
        const SDL_FRect vertical{x - (right ? decoration_stroke : 0.0F), vertical_y,
                                 decoration_stroke, decoration_size};
        static_cast<void>(SDL_RenderFillRect(sdl.renderer, &horizontal));
        static_cast<void>(SDL_RenderFillRect(sdl.renderer, &vertical));
    };
    draw_corner(static_cast<float>(safe.x) + decoration_stroke,
                static_cast<float>(safe.y) + decoration_stroke, false, false);
    draw_corner(static_cast<float>(safe.x + safe.w) - decoration_stroke,
                static_cast<float>(safe.y + safe.h) - decoration_stroke, true,
                true);
    const auto pixel_size = std::max(4.0F, decoration_stroke * 0.9F);
    const auto draw_pixel_plus = [&](const float center_x, const float center_y) {
        const SDL_FRect vertical{center_x - pixel_size * 0.5F,
                                 center_y - pixel_size * 1.5F, pixel_size,
                                 pixel_size * 3.0F};
        const SDL_FRect horizontal{center_x - pixel_size * 1.5F,
                                   center_y - pixel_size * 0.5F,
                                   pixel_size * 3.0F, pixel_size};
        static_cast<void>(SDL_RenderFillRect(sdl.renderer, &vertical));
        static_cast<void>(SDL_RenderFillRect(sdl.renderer, &horizontal));
    };
    draw_pixel_plus(static_cast<float>(safe.x + safe.w) - decoration_size,
                    static_cast<float>(safe.y) + decoration_size);
    draw_pixel_plus(static_cast<float>(safe.x) + decoration_size,
                    static_cast<float>(safe.y + safe.h) - decoration_size);

    if (have_game_viewport) {
        const auto frame = std::clamp(size * 0.70F, 5.0F, 10.0F);
        const SDL_FRect shadow_frame{game_viewport.x - frame - 3.0F,
                                     game_viewport.y - frame - 3.0F,
                                     game_viewport.w + (frame + 3.0F) * 2.0F,
                                     game_viewport.h + (frame + 3.0F) * 2.0F};
        draw_touch_frame(sdl.renderer, shadow_frame, frame + 3.0F,
                         SDL_Color{0, 0, 0, 150});
        const SDL_FRect cyan_frame{game_viewport.x - frame,
                                   game_viewport.y - frame,
                                   game_viewport.w + frame * 2.0F,
                                   game_viewport.h + frame * 2.0F};
        draw_touch_frame(sdl.renderer, cyan_frame, frame,
                         SDL_Color{8, 175, 244, alpha});
        draw_touch_frame(sdl.renderer,
                         {game_viewport.x - 2.0F, game_viewport.y - 2.0F,
                          game_viewport.w + 4.0F, game_viewport.h + 4.0F},
                         2.0F, SDL_Color{8, 15, 22, alpha});
    }

    // The old per-arm renderer drew four complete shadows, borders, and faces
    // on top of one another. On Android that made the D-pad look like a stack
    // of translucent blobs. The unified renderer below draws each visual layer
    // once so the control remains a single crisp branded shape.
    draw_branded_touch_dpad(
        sdl.renderer, point_for(0), size, alpha,
        {sdl.touch_buttons[0], sdl.touch_buttons[1],
         sdl.touch_buttons[2], sdl.touch_buttons[3]}, true);
    draw_branded_touch_circle(sdl.renderer, point_for(1), size, alpha,
                              sdl.touch_buttons[4], "A");
    draw_branded_touch_circle(sdl.renderer, point_for(2), size, alpha,
                              sdl.touch_buttons[5], "B");
    draw_branded_touch_system(sdl.renderer, point_for(3), size, alpha,
                              sdl.touch_buttons[6], "SELECT");
    draw_branded_touch_system(sdl.renderer, point_for(4), size, alpha,
                              sdl.touch_buttons[7], "START");

    if (!restore_video_presentation(sdl)) {
        sdl_error("Could not restore game presentation");
    }
    static_cast<void>(SDL_SetRenderDrawBlendMode(sdl.renderer,
                                                 SDL_BLENDMODE_NONE));
    return;
}

} // namespace gbb::sdl

#endif
