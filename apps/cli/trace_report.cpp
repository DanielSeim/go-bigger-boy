#include "gbb/trace_parser.hpp"

#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "Usage: gbb_trace_report TRACE_PATH\n";
        return 2;
    }

    std::ifstream input(argv[1], std::ios::binary);
    if (!input) {
        std::cerr << "Could not open trace: " << argv[1] << '\n';
        return 2;
    }
    const std::string contents((std::istreambuf_iterator<char>(input)),
                               std::istreambuf_iterator<char>());
    const auto report = gbb::parse_trace(contents);
    std::cout << "valid=" << (report.valid() ? "yes" : "no") << '\n'
              << "session_started=" << (report.session_started ? "yes" : "no") << '\n'
              << "session_ended=" << (report.session_ended ? "yes" : "no") << '\n'
              << "trace_version=" << report.trace_version << '\n'
              << "session_id="
              << (report.session_id.has_value() ? std::to_string(*report.session_id)
                                                 : "unknown")
              << '\n'
              << "transport=" << (report.transport.empty() ? "unknown" : report.transport)
              << '\n'
              << "role=" << (report.role.empty() ? "unknown" : report.role) << '\n'
              << "scenario=" << (report.scenario.empty() ? "unknown" : report.scenario)
              << '\n'
              << "canonical_events=" << report.canonical_events << '\n'
              << "serial_completions=" << report.serial_completions << '\n'
              << "serial_active_changes=" << report.serial_active_changes << '\n'
              << "pokemon_state_events=" << report.pokemon_state_events << '\n'
              << "trade_phase_events=" << report.trade_phase_events << '\n'
              << "stall_events=" << report.stall_events << '\n'
              << "frames=";
    if (report.has_frame) {
        std::cout << report.first_frame << '-' << report.last_frame;
    } else {
        std::cout << "unknown";
    }
    std::cout << '\n'
              << "frames_monotonic=" << (report.frames_monotonic ? "yes" : "no") << '\n';
    for (const auto& error : report.errors) std::cout << "error=" << error << '\n';
    return report.valid() ? 0 : 1;
}
