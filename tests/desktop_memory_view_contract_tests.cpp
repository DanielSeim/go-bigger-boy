#include "desktop_memory_view.hpp"

#include <array>
#include <cstdint>
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

void test_scrolling_is_page_aligned_and_clamped() {
    check(gbb::sdl::scroll_desktop_memory(0x0000, 1) == 0x0010,
          "scrolls down by one memory row");
    check(gbb::sdl::scroll_desktop_memory(0x0020, -2) == 0x0000,
          "scrolls up by multiple rows");
    check(gbb::sdl::scroll_desktop_memory(0x0000, -1) == 0x0000,
          "clamps at the start of address space");
    check(gbb::sdl::scroll_desktop_memory(0xFFF0, 1) == 0xFF00,
          "clamps at the final complete memory page");
}

void test_formats_hex_and_ascii_columns() {
    std::array<std::uint8_t, gbb::sdl::desktop_memory_view_bytes_per_row> bytes{};
    bytes[0] = 0x00;
    bytes[1] = 0x41;
    bytes[2] = 0x7E;
    bytes[3] = 0x7F;
    const auto row = gbb::sdl::format_desktop_memory_row(0x01A0, bytes);
    check(row.compare(0, 18, "01A0  00 41 7E 7F ") == 0,
          "formats the address and hexadecimal bytes");
    check(row.size() >= 17 &&
              row.compare(row.size() - 17, 17, "|.A~.............") == 0,
          "formats printable bytes and replaces control bytes");
}

} // namespace

int main() {
    test_scrolling_is_page_aligned_and_clamped();
    test_formats_hex_and_ascii_columns();
    return failures == 0 ? 0 : 1;
}
