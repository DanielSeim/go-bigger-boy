#include "link_trace_file.hpp"
#include "gbb/trace_format.hpp"

#include <filesystem>
#include <fstream>
#include <iterator>
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

} // namespace

int main() {
    const auto preference_path = std::filesystem::temp_directory_path() /
                                 "gbb-link-trace-file-contract";
    gbb::sdl::LinkTraceFile trace;
    trace.start(preference_path, "host");
    check(trace.is_open(), "SDL link trace opens a candidate path");
    if (trace.is_open()) {
        gbb::write_trace_event_prefix(trace.stream(), "frame", trace.session(),
                                      0, trace.elapsed_ms(), trace.transport(),
                                      trace.role());
        trace.stream() << " marker=session-schema\n";
        trace.stream() << "frame=1 marker=before-checkpoint\n";
        trace.advance_frame();
        for (unsigned frame = 2; frame <= 30; ++frame) {
            trace.stream() << "frame=" << frame << " marker=checkpoint\n";
            trace.advance_frame();
        }
        const auto path = trace.path();
        trace.stop();
        std::ifstream input(path);
        const std::string contents((std::istreambuf_iterator<char>(input)),
                                   std::istreambuf_iterator<char>());
        check(contents.find("GBB link trace\n") != std::string::npos,
              "SDL trace retains its human-readable header");
        check(contents.find(
                  "event=frame trace_version=1 session_id=1 frame=0 elapsed_ms=") !=
                  std::string::npos,
              "SDL trace events use the canonical event prefix");
        check(contents.find("transport=tcp role=host marker=session-schema") !=
                  std::string::npos,
              "SDL trace event carries transport and role metadata");
        check(contents.find(
                  "session_start id=1 trace_version=1 counters_reset=1 transport=tcp role=host") !=
                  std::string::npos,
              "SDL trace uses the shared session schema");
        check(contents.find("session_end id=1 frames=30 elapsed_ms=") !=
                  std::string::npos,
              "SDL trace records a flushed session end");
        std::filesystem::remove(path);
    }
    std::filesystem::remove_all(preference_path);
    return failures == 0 ? 0 : 1;
}
