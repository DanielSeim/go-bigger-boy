#pragma once

#include <algorithm>
#include <array>
#include <cstddef>

namespace gbb::sdl {

// These values are the design dimensions used by the Android overlay. They
// are deliberately independent of the Game Boy framebuffer dimensions.
inline constexpr float android_touch_dpad_dimension = 48.0F;
inline constexpr float android_touch_action_diameter = 24.0F;
inline constexpr float android_touch_system_width = 38.0F;
inline constexpr float android_touch_system_height = 10.0F;
inline constexpr float android_touch_minimum_target_dp = 48.0F;

struct AndroidTouchPoint {
    float x{};
    float y{};
};

struct AndroidTouchRect {
    float x{};
    float y{};
    float w{};
    float h{};
};

struct AndroidTouchGeometry {
    bool landscape{};
    float safe_x{};
    float safe_y{};
    float safe_w{};
    float safe_h{};
    bool has_viewport{};
    float viewport_x{};
    float viewport_y{};
    float viewport_w{};
    float viewport_h{};
    float scale{1.0F};
    std::array<float, 10> positions{};
};

inline AndroidTouchPoint android_touch_control_center(
    const AndroidTouchGeometry& geometry, const std::size_t control) {
    const auto position = control * 2;
    AndroidTouchPoint point{
        geometry.safe_x + geometry.positions[position] * geometry.safe_w,
        geometry.safe_y + geometry.positions[position + 1] * geometry.safe_h};
    if (!geometry.landscape || !geometry.has_viewport ||
        geometry.viewport_w <= 0.0F || geometry.viewport_h <= 0.0F) {
        return point;
    }

    const auto gutter = 8.0F * geometry.scale;
    const auto left_edge = geometry.safe_x;
    const auto right_edge = geometry.safe_x + geometry.safe_w;
    if (control == 0) {
        const auto half_width = android_touch_dpad_dimension * geometry.scale *
                                0.5F;
        const auto minimum = left_edge + half_width + gutter;
        const auto maximum = geometry.viewport_x - half_width - gutter;
        point.x = minimum <= maximum ? std::clamp(point.x, minimum, maximum)
                                     : minimum;
    } else if (control == 1 || control == 2) {
        const auto radius =
            (android_touch_action_diameter * 0.5F + 3.0F) * geometry.scale;
        const auto minimum = geometry.viewport_x + geometry.viewport_w +
                             radius + gutter;
        const auto maximum = right_edge - radius - gutter;
        if (minimum <= maximum) {
            point.x = std::clamp(point.x, minimum, maximum);
        } else {
            const auto gap = 4.0F * geometry.scale;
            const auto outer = right_edge - radius - gutter;
            const auto inner = std::max(left_edge + radius + gutter,
                                        outer - radius * 2.0F - gap);
            point.x = control == 1 ? outer : inner;
        }
    } else if (control == 3 || control == 4) {
        const auto half_width =
            (android_touch_system_width * 0.5F + 2.0F) * geometry.scale;
        const auto left_minimum = left_edge + half_width + gutter;
        const auto left_maximum = geometry.viewport_x - half_width - gutter;
        const auto right_minimum = geometry.viewport_x + geometry.viewport_w +
                                   half_width + gutter;
        const auto right_maximum = right_edge - half_width - gutter;
        if (control == 3) {
            point.x = left_minimum <= left_maximum
                          ? std::clamp(point.x, left_minimum, left_maximum)
                          : left_minimum;
        } else {
            point.x = right_minimum <= right_maximum
                          ? std::clamp(point.x, right_minimum, right_maximum)
                          : right_maximum;
        }
    }
    return point;
}

inline AndroidTouchRect android_touch_visual_bounds(
    const AndroidTouchGeometry& geometry, const std::size_t control) {
    const auto center = android_touch_control_center(geometry, control);
    float width = android_touch_system_width;
    float height = android_touch_system_height;
    if (control == 0) {
        width = height = android_touch_dpad_dimension;
    } else if (control == 1 || control == 2) {
        width = height = android_touch_action_diameter + 6.0F;
    } else {
        width += 4.0F;
        height += 4.0F;
    }
    width *= geometry.scale;
    height *= geometry.scale;
    return {center.x - width * 0.5F, center.y - height * 0.5F, width, height};
}

inline float android_touch_minimum_target_px(const float density) {
    return android_touch_minimum_target_dp * std::max(1.0F, density);
}

inline float android_touch_visual_half_width(const AndroidTouchGeometry& geometry,
                                             const std::size_t control) {
    if (control == 0) return android_touch_dpad_dimension * geometry.scale * 0.5F;
    if (control == 1 || control == 2) {
        return android_touch_action_diameter * geometry.scale * 0.5F;
    }
    return android_touch_system_width * geometry.scale * 0.5F;
}

inline float android_touch_visual_half_height(
    const AndroidTouchGeometry& geometry, const std::size_t control) {
    if (control == 0) return android_touch_dpad_dimension * geometry.scale * 0.5F;
    if (control == 1 || control == 2) {
        return android_touch_action_diameter * geometry.scale * 0.5F;
    }
    return android_touch_system_height * geometry.scale * 0.5F;
}

inline float android_touch_hit_radius(const AndroidTouchGeometry& geometry,
                                      const std::size_t control,
                                      const float density) {
    const auto visual = android_touch_visual_half_width(geometry, control);
    return std::max(visual, android_touch_minimum_target_px(density) * 0.5F);
}

inline AndroidTouchRect android_touch_label_bounds(
    const AndroidTouchGeometry& geometry, const std::size_t control,
    const float density) {
    const auto center = android_touch_control_center(geometry, control);
    const auto half_width = android_touch_hit_radius(geometry, control, density);
    const auto visual_height =
        android_touch_visual_half_height(geometry, control);
    const auto label_height = 18.0F * std::max(1.0F, density);
    return {center.x - half_width,
            center.y + visual_height + 7.0F * geometry.scale,
            half_width * 2.0F, label_height};
}

inline bool android_touch_rects_overlap(const AndroidTouchRect first,
                                        const AndroidTouchRect second) {
    return first.x < second.x + second.w &&
           first.x + first.w > second.x && first.y < second.y + second.h &&
           first.y + first.h > second.y;
}

}  // namespace gbb::sdl
