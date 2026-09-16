#include "windows_dashboard_model.hpp"

#include <iostream>

namespace {

int failures = 0;

void check(const bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

void test_settings_return_contract() {
    check(gbb_desktop::settings_return_action(true) ==
              gbb_desktop::DashboardResultAction::resume,
          "settings returns to an active emulator when one can resume");
    check(gbb_desktop::settings_return_action(false) ==
              gbb_desktop::DashboardResultAction::library,
          "startup settings returns to the library when no game is active");
}

void test_dashboard_actions_remain_distinct() {
    using Action = gbb_desktop::DashboardResultAction;
    check(Action::resume != Action::library,
          "resume and library actions remain distinct");
    check(Action::open_rom != Action::quit,
          "opening a ROM and quitting remain distinct");
    check(gbb_desktop::DashboardResult{}.action == Action::resume,
          "dashboard results default to the safe resume action");
}

} // namespace

int main() {
    test_settings_return_contract();
    test_dashboard_actions_remain_distinct();
    return failures == 0 ? 0 : 1;
}
