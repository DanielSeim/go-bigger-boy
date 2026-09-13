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

// A and B can be held together by one finger while the finger is inside both
// action hit targets in succession. D-pad and system controls retain the
// original single-control ownership behavior.
[[nodiscard]] inline bool is_touch_action_control(
    const std::size_t control) noexcept {
    return control == 4U || control == 5U;
}

[[nodiscard]] inline TouchControlState update_touch_control_state(
    const TouchControlState state,
    const std::optional<std::size_t> hit,
    const bool secondary_in_release_zone = false) noexcept {
    if (!state.primary) return {hit, std::nullopt};
    if (!is_touch_action_control(*state.primary)) {
        return {hit.has_value() ? hit : state.primary, std::nullopt};
    }
    // Neutral motion means the secondary action is no longer under the
    // finger. Allow a small release zone around it so touch sampling jitter
    // cannot turn a held A/B handoff into a short click. Keep the primary
    // action held so B can continue running while A is released and pressed
    // again for another jump.
    if (!hit.has_value()) {
        return {state.primary,
                state.secondary && secondary_in_release_zone
                    ? state.secondary
                    : std::nullopt};
    }
    if (!is_touch_action_control(*hit)) return {hit, std::nullopt};
    // Returning to the primary button is the release gesture for the
    // secondary button. This preserves B while allowing A to be released and
    // pressed again without lifting the finger.
    if (*hit == *state.primary) return {state.primary, std::nullopt};
    if (state.secondary && *hit == *state.secondary) {
        return state;
    }
    return {state.primary, hit};
}

} // namespace gbb
