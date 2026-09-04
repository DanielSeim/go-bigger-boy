#include "scenario_trace_writer.hpp"
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
    const auto trace_path = std::filesystem::temp_directory_path() /
                            "gbb-link-harness-trace-writer-test.log";
    {
        gbb::link_harness::ScenarioTraceWriter trace(trace_path, "local",
                                                      "trade");
        check(trace.enabled(), "trace writer opens a requested path");
        gbb::write_trace_event_prefix(trace.stream(), "frame", trace.session(),
                                      0, trace.elapsed_ms(), trace.transport(),
                                      trace.role());
        trace.stream() << " marker=session-schema\n";
        trace.stream() << "frame=1 marker=before-checkpoint\n";
        trace.checkpoint_frame(1);
        trace.stream() << "frame=30 marker=checkpoint\n";
        trace.checkpoint_frame(30);
    }

    std::ifstream trace_input(trace_path);
    const std::string trace_contents((std::istreambuf_iterator<char>(trace_input)),
                                      std::istreambuf_iterator<char>());
    check(trace_contents.find("transport=local scenario=trade") !=
              std::string::npos,
          "trace writer emits transport and scenario metadata");
    check(trace_contents.find(
              "event=frame trace_version=1 session_id=1 frame=0 elapsed_ms=") !=
              std::string::npos,
          "harness trace events use the canonical event prefix");
    check(trace_contents.find(
              "transport=local role=harness marker=session-schema") !=
              std::string::npos,
          "harness trace event carries transport and role metadata");
    check(trace_contents.find(
              "session_start id=1 trace_version=1 counters_reset=1 transport=local role=harness scenario=trade") !=
              std::string::npos,
          "trace writer emits the shared session schema");
    check(trace_contents.find("session_end id=1 frames=30 elapsed_ms=") !=
              std::string::npos,
          "trace writer flushes and closes with a shared session record");
    check(trace_contents.find("trace_end frames=30") != std::string::npos,
          "trace writer records the last checkpointed frame");
    check(trace_contents.find("trace_end frames=30") ==
              trace_contents.rfind("trace_end frames=30"),
          "trace writer terminates the trace exactly once");
    std::filesystem::remove(trace_path);
    return failures == 0 ? 0 : 1;
}
