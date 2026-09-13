#include "android_touch_geometry.hpp"

#include <array>
#include <cassert>

namespace {

constexpr std::array<float, 10> portrait_positions{
    0.28F, 0.71F, 0.80F, 0.62F, 0.58F, 0.78F, 0.35F, 0.91F, 0.65F,
    0.91F};
constexpr std::array<float, 10> landscape_positions{
    0.10F, 0.46F, 0.94F, 0.34F, 0.76F, 0.60F, 0.18F, 0.84F, 0.88F,
    0.84F};

void check(bool condition) {
    assert(condition);
}

void check_no_viewport_overlap(const gbb::sdl::AndroidTouchGeometry& geometry) {
    const gbb::sdl::AndroidTouchRect viewport{
        geometry.viewport_x, geometry.viewport_y, geometry.viewport_w,
        geometry.viewport_h};
    for (std::size_t control = 0; control < 5; ++control) {
        check(!gbb::sdl::android_touch_rects_overlap(
            gbb::sdl::android_touch_visual_bounds(geometry, control),
            viewport));
    }
}

void check_inside_safe_area(const gbb::sdl::AndroidTouchGeometry& geometry) {
    const gbb::sdl::AndroidTouchRect safe{geometry.safe_x, geometry.safe_y,
                                          geometry.safe_w, geometry.safe_h};
    for (std::size_t control = 0; control < 5; ++control) {
        const auto bounds =
            gbb::sdl::android_touch_visual_bounds(geometry, control);
        check(bounds.x >= safe.x && bounds.y >= safe.y &&
              bounds.x + bounds.w <= safe.x + safe.w &&
              bounds.y + bounds.h <= safe.y + safe.h);
    }
}

void check_hit_targets(const gbb::sdl::AndroidTouchGeometry& geometry,
                       const float density) {
    const auto minimum =
        gbb::sdl::android_touch_minimum_target_px(density);
    for (std::size_t control = 0; control < 5; ++control) {
        const auto radius = gbb::sdl::android_touch_hit_radius(
            geometry, control, density);
        check(radius * 2.0F >= minimum);
        const auto label = gbb::sdl::android_touch_label_bounds(
            geometry, control, density);
        check(label.w >= minimum && label.h >= 18.0F * density);
    }
}

void test_portrait_geometry() {
    const gbb::sdl::AndroidTouchGeometry geometry{
        false, 12.0F, 24.0F, 1056.0F, 2280.0F, false, 0.0F, 0.0F, 0.0F,
        0.0F, 5.5F, portrait_positions};
    check_hit_targets(geometry, 3.0F);
    check_inside_safe_area(geometry);
    const auto select = gbb::sdl::android_touch_control_center(geometry, 3);
    const auto start = gbb::sdl::android_touch_control_center(geometry, 4);
    check(start.x > select.x);
    check(!gbb::sdl::android_touch_rects_overlap(
        gbb::sdl::android_touch_visual_bounds(geometry, 3),
        gbb::sdl::android_touch_visual_bounds(geometry, 4)));
}

void test_landscape_geometry() {
    const gbb::sdl::AndroidTouchGeometry geometry{
        true, 0.0F, 0.0F, 2340.0F, 1080.0F, true, 570.0F, 0.0F, 1200.0F,
        1080.0F, 6.0F, landscape_positions};
    check_hit_targets(geometry, 3.0F);
    check_inside_safe_area(geometry);
    check_no_viewport_overlap(geometry);

    const auto a = gbb::sdl::android_touch_control_center(geometry, 1);
    const auto b = gbb::sdl::android_touch_control_center(geometry, 2);
    check(a.x > b.x);
    check(a.x - b.x >= 4.0F * geometry.scale);
    check(!gbb::sdl::android_touch_rects_overlap(
        gbb::sdl::android_touch_label_bounds(geometry, 1, 3.0F),
        gbb::sdl::android_touch_label_bounds(geometry, 2, 3.0F)));
}

void test_landscape_narrow_side_columns() {
    const gbb::sdl::AndroidTouchGeometry geometry{
        true, 0.0F, 0.0F, 1920.0F, 1080.0F, true, 360.0F, 0.0F, 1200.0F,
        1080.0F, 4.0F, landscape_positions};
    check_no_viewport_overlap(geometry);
    check_inside_safe_area(geometry);
    check_hit_targets(geometry, 2.75F);
}

}  // namespace

int main() {
    test_portrait_geometry();
    test_landscape_geometry();
    test_landscape_narrow_side_columns();
    return 0;
}
