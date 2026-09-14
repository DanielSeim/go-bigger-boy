#include "desktop_breakpoints.hpp"

#include <cstdint>
#include <iostream>

namespace {

int failures = 0;

void check(const bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

void test_sorted_toggle_and_clear() {
    gbb::sdl::DesktopBreakpoints breakpoints;
    check(breakpoints.toggle(0x4000), "adding a PC breakpoint reports enabled");
    check(breakpoints.toggle(0x0100), "adding a lower PC breakpoint reports enabled");
    check(breakpoints.toggle(0xC000), "adding a higher PC breakpoint reports enabled");
    check(breakpoints.addresses().size() == 3 &&
              breakpoints.addresses()[0] == 0x0100 &&
              breakpoints.addresses()[1] == 0x4000 &&
              breakpoints.addresses()[2] == 0xC000,
          "PC breakpoints remain sorted for stable presentation");
    check(!breakpoints.toggle(0x4000) && !breakpoints.contains(0x4000) &&
              breakpoints.size() == 2,
          "toggling an existing PC breakpoint removes it");
    breakpoints.clear();
    check(breakpoints.empty() && !breakpoints.last_hit(),
          "clearing breakpoints removes addresses and hit state");
}

void test_hit_and_resume_semantics() {
    gbb::sdl::DesktopBreakpoints breakpoints;
    static_cast<void>(breakpoints.toggle(0x1234));
    static_cast<void>(breakpoints.toggle(0x4567));

    check(!breakpoints.check(0x1000),
          "non-breakpoint PC continues without a hit");
    check(breakpoints.check(0x1234) == std::optional<std::uint16_t>{0x1234} &&
              breakpoints.last_hit() == std::optional<std::uint16_t>{0x1234},
          "matching PC reports the breakpoint address");

    breakpoints.resume_after_hit();
    check(!breakpoints.check(0x1234),
          "resuming from a hit executes the current instruction once");
    check(breakpoints.check(0x1234) == std::optional<std::uint16_t>{0x1234},
          "the same breakpoint fires again after one instruction");

    breakpoints.resume_after_hit();
    check(breakpoints.check(0x4567) == std::optional<std::uint16_t>{0x4567},
          "moving to another breakpoint does not lose the breakpoint hit");
}

}  // namespace

int main() {
    test_sorted_toggle_and_clear();
    test_hit_and_resume_semantics();
    return failures == 0 ? 0 : 1;
}
