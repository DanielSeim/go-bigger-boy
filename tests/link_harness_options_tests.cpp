#include "options.hpp"

#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(const bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

gbb::link_harness::Options parse(
    const std::vector<std::string>& arguments) {
    std::vector<char*> pointers;
    pointers.reserve(arguments.size());
    for (const auto& argument : arguments) {
        pointers.push_back(const_cast<char*>(argument.c_str()));
    }
    return gbb::link_harness::parse_options(
        static_cast<int>(pointers.size()), pointers.data());
}

template <typename Function>
void expects_invalid(Function&& function, const char* message) {
    try {
        function();
        check(false, message);
    } catch (const std::invalid_argument&) {
        check(true, message);
    } catch (...) {
        check(false, message);
    }
}

} // namespace

int main() {
    const auto options = parse({
        "gbb_link_harness", "--rom", "game.gb", "--save1", "one.sav",
        "--save2", "two.sav", "--state1", "one.gbbs", "--state2",
        "two.gbbs", "--transport", "local", "--frames", "42", "--port",
        "1234", "--scenario", "trade", "--report", "report.txt",
        "--trace", "trace.log", "--capture-dir", "captures"});
    check(options.local, "local transport is parsed");
    check(options.frames == 42 && options.port == 1234,
          "numeric link options are parsed");
    check(options.scenario == gbb::link_harness::Scenario::trade &&
              options.expectation == gbb::link_harness::Expectation::trade &&
              options.auto_confirm,
          "trade scenario selects its expectation and auto-confirm");
    check(options.state1 == "one.gbbs" && options.state2 == "two.gbbs",
          "paired state paths are retained");

    expects_invalid(
        [] { parse({"gbb_link_harness", "--rom", "game.gb"}); },
        "required save paths are validated");
    expects_invalid(
        [] {
            parse({"gbb_link_harness", "--rom", "game.gb", "--save1",
                   "one.sav", "--save2", "two.sav", "--state1", "one.gbbs"});
        },
        "state paths must be supplied as a pair");
    expects_invalid(
        [] {
            parse({"gbb_link_harness", "--rom", "game.gb", "--save1",
                   "one.sav", "--save2", "two.sav", "--frames", "0"});
        },
        "frame count must be positive");
    expects_invalid(
        [] {
            parse({"gbb_link_harness", "--rom", "game.gb", "--save1",
                   "one.sav", "--save2", "two.sav", "--transport", "udp"});
        },
        "transport names are validated");

    return failures == 0 ? 0 : 1;
}
