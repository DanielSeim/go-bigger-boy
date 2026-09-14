#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string>

namespace gbb::sdl {

inline constexpr std::size_t desktop_memory_view_rows = 16;
inline constexpr std::size_t desktop_memory_view_bytes_per_row = 16;
inline constexpr std::uint16_t desktop_memory_view_last_start = 0xFF00;

[[nodiscard]] inline std::uint16_t scroll_desktop_memory(
    const std::uint16_t start, const int rows) noexcept {
    const auto next = static_cast<int>(start) +
                      rows * static_cast<int>(desktop_memory_view_bytes_per_row);
    return static_cast<std::uint16_t>(
        std::clamp(next, 0, static_cast<int>(desktop_memory_view_last_start)));
}

[[nodiscard]] inline std::string format_desktop_memory_row(
    const std::uint16_t address,
    const std::array<std::uint8_t, desktop_memory_view_bytes_per_row>& bytes) {
    std::ostringstream output;
    output << std::uppercase << std::hex << std::setfill('0') << std::setw(4)
           << static_cast<unsigned>(address) << "  ";
    for (const auto byte : bytes) {
        output << std::setw(2) << static_cast<unsigned>(byte) << ' ';
    }
    output << "|";
    for (const auto byte : bytes) {
        output << ((byte >= 0x20 && byte <= 0x7E)
                       ? static_cast<char>(byte)
                       : '.');
    }
    return output.str();
}

} // namespace gbb::sdl
