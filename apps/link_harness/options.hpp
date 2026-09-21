#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string_view>

namespace gbb::link_harness {

enum class Expectation { none, trade, battle };
enum class Scenario { none, trade, battle };
enum class FaultKind { drop, delay, duplicate, disconnect };

[[nodiscard]] const char* fault_kind_name(FaultKind fault) noexcept;
[[nodiscard]] std::optional<FaultKind> parse_fault_kind(
    std::string_view value) noexcept;

struct Options {
    std::filesystem::path rom;
    std::filesystem::path save1;
    std::filesystem::path save2;
    std::filesystem::path state1;
    std::filesystem::path state2;
    std::filesystem::path report;
    std::filesystem::path trace;
    std::filesystem::path fault_replay;
    std::filesystem::path capture_dir;
    std::uint64_t frames = 1'200;
    std::uint16_t port = 0;
    bool local{};
    bool auto_confirm{};
    std::optional<FaultKind> fault;
    Expectation expectation = Expectation::none;
    Scenario scenario = Scenario::none;
};

void usage();
[[nodiscard]] Options parse_options(int argc, char** argv);

} // namespace gbb::link_harness
