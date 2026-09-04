#pragma once

#include <cstdint>
#include <filesystem>

namespace gbb::link_harness {

enum class Expectation { none, trade, battle };
enum class Scenario { none, trade, battle };

struct Options {
    std::filesystem::path rom;
    std::filesystem::path save1;
    std::filesystem::path save2;
    std::filesystem::path state1;
    std::filesystem::path state2;
    std::filesystem::path report;
    std::filesystem::path trace;
    std::filesystem::path capture_dir;
    std::uint64_t frames = 1'200;
    std::uint16_t port = 0;
    bool local{};
    bool auto_confirm{};
    Expectation expectation = Expectation::none;
    Scenario scenario = Scenario::none;
};

void usage();
[[nodiscard]] Options parse_options(int argc, char** argv);

} // namespace gbb::link_harness
