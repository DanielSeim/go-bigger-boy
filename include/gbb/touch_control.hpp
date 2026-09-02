#pragma once

#include <cstddef>
#include <optional>

namespace gbb {

// A finger keeps its last owned control while moving through neutral space.
// A newly hit control may take ownership; an empty hit never releases it.
[[nodiscard]] inline std::optional<std::size_t> retain_touch_control(
    const std::optional<std::size_t> owned,
    const std::optional<std::size_t> hit) noexcept {
    return hit.has_value() ? hit : owned;
}

} // namespace gbb
