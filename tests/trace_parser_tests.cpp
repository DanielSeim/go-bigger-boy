#include "gbb/trace_parser.hpp"

#include <iostream>
#include <string>

namespace {

int failures = 0;

void check(const bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

void test_sdl_trace_replay() {
    const std::string trace =
        "GBB link trace\n"
        "session_start id=7 trace_version=1 counters_reset=1 transport=tcp "
        "role=host clock=monotonic elapsed_ms=0 session_id=7\n"
        "event=frame trace_version=1 session_id=7 frame=10 elapsed_ms=20 "
        "transport=tcp role=host cpu_cycles=702240\n"
        "event=serial_active trace_version=1 session_id=7 frame=11 elapsed_ms=21 "
        "transport=tcp role=host player=1 value=1\n"
        "event=serial_complete trace_version=1 session_id=7 frame=12 elapsed_ms=23 "
        "transport=tcp role=host player=1 count=1 tx=ff rx=01\n"
        "event=pokemon_state trace_version=1 session_id=7 frame=12 elapsed_ms=23 "
        "transport=tcp role=host player=1 link=60 battle=0\n"
        "session_end id=7 frames=12 elapsed_ms=24 session_id=7\n";
    const auto report = gbb::parse_trace(trace);
    check(report.valid(), "valid SDL trace has no parser errors");
    check(report.session_started && report.session_ended,
          "SDL replay observes complete session lifecycle");
    check(report.session_id == 7 && report.transport == "tcp" &&
              report.role == "host",
          "SDL replay retains session context");
    check(report.canonical_events == 4 && report.serial_completions == 1 &&
              report.serial_active_changes == 1 && report.pokemon_state_events == 1,
          "SDL replay summarizes canonical event classes");
    check(report.has_frame && report.first_frame == 10 && report.last_frame == 12 &&
              report.frames_monotonic,
          "SDL replay validates frame ordering");
    check(report.has_event("serial_complete"),
          "SDL replay can locate a specific event");
}

void test_harness_trace_replay() {
    const std::string trace =
        "GBB link scenario trace\n"
        "transport=local scenario=trade\n"
        "session_start id=3 trace_version=1 counters_reset=1 transport=local "
        "role=harness scenario=trade clock=monotonic elapsed_ms=0 session_id=3\n"
        "frame=1 marker=legacy-compatible\n"
        "event=frame trace_version=1 session_id=3 frame=30 elapsed_ms=40 "
        "transport=local role=harness p1_cycles=2106720 p2_cycles=2106720\n"
        "event=post_menu_serial_stall trace_version=1 session_id=3 frame=150 "
        "elapsed_ms=170 transport=local role=harness stalled_frames=120\n"
        "trace_end frames=150\n"
        "session_end id=3 frames=150 elapsed_ms=171 session_id=3\n";
    const auto report = gbb::parse_trace(trace);
    check(report.valid(), "valid harness trace has no parser errors");
    check(report.transport == "local" && report.role == "harness" &&
              report.scenario == "trade",
          "harness replay reads metadata and session context");
    check(report.canonical_events == 2 && report.stall_events == 1,
          "harness replay identifies watchdog events");
    check(report.has_frame && report.first_frame == 1 && report.last_frame == 150,
          "harness replay includes legacy and canonical frames");
    check(report.has_event("post_menu_serial_stall"),
          "harness replay can locate a stall marker");
}

void test_invalid_trace() {
    const auto report = gbb::parse_trace(
        "session_start id=1 trace_version=1 transport=tcp role=host\n"
        "event=frame trace_version=1 session_id=2 frame=9 elapsed_ms=nope "
        "transport=tcp role=join\n"
        "event=frame trace_version=1 session_id=2 frame=8 elapsed_ms=3 "
        "transport=tcp role=join\n");
    check(!report.valid(), "invalid canonical trace is rejected");
    check(!report.errors.empty(), "invalid trace reports concrete errors");
    check(!report.frames_monotonic,
          "invalid trace does not claim ordering is valid after a malformed frame");
}

void test_legacy_session_records() {
    const auto report = gbb::parse_trace(
        "GBB link trace\n"
        "session_start id=4 trace_version=1 counters_reset=1 transport=tcp "
        "role=join clock=monotonic elapsed_ms=0\n"
        "frame=1 marker=old-format\n"
        "session_end id=4 frames=1 elapsed_ms=2\n");
    check(report.valid(), "legacy session records remain parseable");
    check(report.session_id == 4 && report.transport == "tcp" &&
              report.role == "join",
          "legacy session records retain their context");
}

void test_bounded_and_mutated_input() {
    const std::string seed =
        "session_start id=1 trace_version=1 transport=tcp role=host\n"
        "event=frame trace_version=1 session_id=1 frame=1 elapsed_ms=2 "
        "transport=tcp role=host marker=ok\n";
    for (std::size_t index = 0; index < 512; ++index) {
        auto mutated = seed;
        mutated[index % mutated.size()] =
            static_cast<char>((index * 37U) & 0x7fU);
        const auto report = gbb::parse_trace(mutated);
        check(report.errors.size() <= 256U,
              "mutated trace keeps diagnostic errors bounded");
        check(report.records.size() <= gbb::trace_parser_max_records,
              "mutated trace keeps record storage bounded");
    }

    const std::string oversized_line(gbb::trace_parser_max_line_bytes + 1U, 'x');
    const auto line_report = gbb::parse_trace(oversized_line);
    check(!line_report.valid() && !line_report.errors.empty(),
          "oversized trace records are rejected");

    const std::string oversized_trace(gbb::trace_parser_max_bytes + 1U, '#');
    const auto trace_report = gbb::parse_trace(oversized_trace);
    check(!trace_report.valid() && !trace_report.errors.empty(),
          "oversized traces are rejected before unbounded parsing");
}

} // namespace

int main() {
    test_sdl_trace_replay();
    test_harness_trace_replay();
    test_invalid_trace();
    test_legacy_session_records();
    test_bounded_and_mutated_input();
    return failures == 0 ? 0 : 1;
}
