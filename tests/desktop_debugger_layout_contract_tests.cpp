#include "desktop_debugger_layout.hpp"

#include <algorithm>
#include <array>
#include <iostream>
#include <string>

namespace {

int failures = 0;

void check(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

bool disjoint(const SDL_FRect& first, const SDL_FRect& second) {
    return first.x + first.w <= second.x || second.x + second.w <= first.x ||
           first.y + first.h <= second.y || second.y + second.h <= first.y;
}

bool inside(const SDL_FRect& rect, const float width, const float height) {
    return rect.x >= 0.0F && rect.y >= 0.0F && rect.x + rect.w <= width &&
           rect.y + rect.h <= height;
}

void test_layout_is_stable_at_supported_sizes() {
    constexpr std::array sizes{
        std::pair{1280, 900}, std::pair{1180, 820}, std::pair{960, 700},
        // Inputs below the minimum are clamped to the same safe layout.
        std::pair{640, 480}};
    for (const auto [width, height] : sizes) {
        const auto layout = gbb::sdl::desktop_debugger_layout(width, height);
        const auto effective_width = static_cast<float>(
            std::max(width, gbb::sdl::desktop_debugger_minimum_width));
        const auto effective_height = static_cast<float>(
            std::max(height, gbb::sdl::desktop_debugger_minimum_height));

        check(disjoint(layout.map, layout.register_area),
              "background map and registers do not overlap");
        check(disjoint(layout.register_area, layout.disassembly),
              "registers and disassembly do not overlap");
        check(disjoint(layout.inspector, layout.disassembly),
              "hardware inspector and disassembly do not overlap");
        check(layout.memory.x >= layout.inspector.x &&
                  layout.memory.y >= layout.inspector.y &&
                  layout.memory.x + layout.memory.w <=
                      layout.inspector.x + layout.inspector.w &&
                  layout.memory.y + layout.memory.h <=
                      layout.inspector.y + layout.inspector.h,
              "memory view stays inside the inspector panel");
        check(layout.map_scale > 0.0F && layout.map.w == layout.map.h,
              "calculated view remains square and drawable");

        const std::array buttons{
            layout.video_viewers, layout.hardware_inspector,
            layout.breakpoint_toggle, layout.breakpoint_clear, layout.record,
            layout.replay, layout.tas, layout.run_pause, layout.step_cpu,
            layout.step_frame, layout.sprite_editor};
        for (const auto& button : buttons) {
            check(inside(button, effective_width, effective_height),
                  "debugger control remains inside the window");
        }
        check(gbb::sdl::desktop_debugger_line_count(layout.disassembly) >= 4 &&
                  gbb::sdl::desktop_debugger_line_count(layout.disassembly) <= 20,
              "disassembly row count is bounded by the panel");
        check(gbb::sdl::desktop_debugger_memory_row_count(layout.memory) >= 1 &&
                  gbb::sdl::desktop_debugger_memory_row_count(layout.memory) <= 16,
              "memory row count is bounded by the panel");
    }
}

void test_hit_testing_uses_the_same_rectangles() {
    const auto layout = gbb::sdl::desktop_debugger_layout(960, 700);
    check(gbb::sdl::desktop_debugger_rect_contains(
              layout.record, layout.record.x + layout.record.w / 2.0F,
              layout.record.y + layout.record.h / 2.0F),
          "record button center is clickable");
    check(!gbb::sdl::desktop_debugger_rect_contains(
              layout.record, layout.record.x - 1.0F, layout.record.y),
          "outside of record button is not clickable");
}

} // namespace

int main() {
    test_layout_is_stable_at_supported_sizes();
    test_hit_testing_uses_the_same_rectangles();
    return failures == 0 ? 0 : 1;
}
