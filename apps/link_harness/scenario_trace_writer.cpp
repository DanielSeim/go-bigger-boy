#include "scenario_trace_writer.hpp"

#include "gbb/trace_format.hpp"

#include <filesystem>
#include <ostream>
#include <stdexcept>
#include <string>

namespace gbb::link_harness {

ScenarioTraceWriter::ScenarioTraceWriter(const std::filesystem::path& path,
                                         const std::string_view transport,
                                         const std::string_view scenario) {
    if (path.empty()) return;
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path());
    }
    output_.open(path, std::ios::trunc);
    if (!output_) {
        throw std::runtime_error("could not open trace " + path.string());
    }
    output_ << "GBB link scenario trace\n"
            << "transport=" << transport << " scenario=" << scenario << "\n"
            << "# one key=value record per emulated frame; p1_/p2_ prefix the player\n";
    transport_ = std::string{transport};
    session_ = gbb::next_trace_session_id();
    started_at_ = std::chrono::steady_clock::now();
    gbb::write_trace_session_start(output_, session_, transport, "harness",
                                   scenario);
    output_.flush();
    if (!output_) {
        throw std::runtime_error("could not write trace " + path.string());
    }
    path_ = path;
}

ScenarioTraceWriter::~ScenarioTraceWriter() {
    finish();
}

std::uint64_t ScenarioTraceWriter::elapsed_ms() const noexcept {
    if (started_at_ == std::chrono::steady_clock::time_point{}) return 0;
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started_at_)
            .count());
}

void ScenarioTraceWriter::checkpoint_frame(const std::uint64_t frame) {
    if (!enabled()) return;
    frame_ = frame;
    if ((frame % 30) == 0) output_.flush();
}

void ScenarioTraceWriter::flush() {
    if (enabled()) output_.flush();
}

void ScenarioTraceWriter::finish() {
    if (finished_ || !enabled()) return;
    const auto elapsed_ms = this->elapsed_ms();
    gbb::write_trace_session_end(output_, session_, frame_, elapsed_ms);
    output_ << "trace_end frames=" << frame_ << '\n';
    output_.flush();
    finished_ = true;
}

} // namespace gbb::link_harness
