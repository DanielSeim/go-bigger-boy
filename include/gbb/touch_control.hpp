#pragma once

#include <cstddef>
#include <optional>

namespace gbb {

struct TouchControlState {
    std::optional<std::size_t> primary;
    std::optional<std::size_t> secondary;
};

// A finger keeps its last owned control while moving through neutral space.
// A newly hit control may take ownership; an empty hit never releases it.
[[nodiscard]] inline std::optional<std::size_t> retain_touch_control(
    const std::optional<std::size_t> owned,
    const std::optional<std::size_t> hit) noexcept {
    return hit.has_value() ? hit : owned;
}

// A and B can be held together by one finger while the finger moves between
// their hit targets. D-pad and system controls retain the original
// single-control ownership behavior.
[[nodiscard]] inline bool is_touch_action_control(
    const std::size_t control) noexcept {
    return control == 4U || control == 5U;
}

[[nodiscard]] inline TouchControlState update_touch_control_state(
    const TouchControlState state,
    const std::optional<std::size_t> hit) noexcept {
    if (!state.primary) return {hit, std::nullopt};
    if (!is_touch_action_control(*state.primary)) {
        return {hit.has_value() ? hit : state.primary, std::nullopt};
    }
    if (!hit.has_value()) return {state.primary, std::nullopt};
    if (!is_touch_action_control(*hit)) return {hit, std::nullopt};
    if (*hit == *state.primary) return {state.primary, std::nullopt};
    return {state.primary, hit};
}

} // namespace gbb
