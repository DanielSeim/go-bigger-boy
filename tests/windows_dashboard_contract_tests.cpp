#include "windows_dashboard_model.hpp"
#include "gbb/core_registry.hpp"
#include "gbb/frontend_logging.hpp"

#include <chrono>
#include <fstream>
#include <iostream>
#include <iterator>

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

void test_failed_rom_launch_keeps_error_visible() {
    const auto directory = std::filesystem::temp_directory_path() /
        ("gbb-launch-error-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    auto& logger = gbb::Logger::instance();
    logger.close_file();
    check(gbb::initialize_frontend_file_logging(directory), "default launch log opens without environment configuration");
    std::string detail;
    try {
        static_cast<void>(gbb::built_in_core_registry().create_from_file(directory / "missing.gb"));
    } catch (const std::exception& error) {
        detail = error.what();
        gbb::log_rom_launch_error("missing.gb", "cgb-e", detail);
    }
    check(!detail.empty(), "missing ROM fails with useful technical details");
    // Return-to-library state must not cause the native modal loop to hide
    // the SDL error. Repeated UI ticks are not a dismissal action.
    for (unsigned tick = 0; tick < 120; ++tick) {
        check(!gbb_desktop::should_open_windows_dashboard(true, false, true),
              "library transition waits while the launch error is open");
    }
    check(gbb_desktop::should_open_windows_dashboard(true, false, false),
          "library opens after the user dismisses the error");
    check(!gbb_desktop::should_open_windows_dashboard(false, false, false) &&
          !gbb_desktop::should_open_windows_dashboard(true, true, false),
          "gameplay and update download retain their dashboard behavior");
    // Error records are flushed while the app remains open, not only at exit.
    std::ifstream input(directory / "logs" / "gbb.log", std::ios::binary);
    const std::string contents((std::istreambuf_iterator<char>(input)), {});
    check(contents.find("ROM launch failed: rom=missing.gb model=cgb-e error=" + detail) != std::string::npos,
          "launch log retains ROM, model and exact exception details");
    input.close();
    logger.close_file();
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
}

} // namespace

int main() {
    test_settings_return_contract();
    test_dashboard_actions_remain_distinct();
    test_failed_rom_launch_keeps_error_visible();
    return failures == 0 ? 0 : 1;
}
