#include "options.hpp"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

namespace gbb::link_harness {
namespace {

std::uint64_t parse_positive(const std::string& text, const char* name) {
    std::size_t consumed = 0;
    const auto value = std::stoull(text, &consumed);
    if (consumed != text.size() || value == 0) {
        throw std::invalid_argument(std::string(name) +
                                    " must be a positive integer");
    }
    return value;
}

} // namespace

void usage() {
    std::cerr
        << "Usage: gbb_link_harness --rom ROM --save1 SAVE --save2 SAVE "
           "[--state1 STATE --state2 STATE] "
           "[--transport tcp|local] [--frames N] [--port N] "
           "[--auto-confirm] [--scenario trade|battle] "
           "[--expect trade|battle] [--report PATH] [--trace PATH]\n";
}

Options parse_options(const int argc, char** argv) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        const auto require_value = [&](const char* name) -> std::string {
            if (index + 1 >= argc) {
                throw std::invalid_argument(std::string(name) +
                                            " requires a value");
            }
            return argv[++index];
        };
        if (argument == "--rom") {
            options.rom = require_value("--rom");
        } else if (argument == "--save1") {
            options.save1 = require_value("--save1");
        } else if (argument == "--save2") {
            options.save2 = require_value("--save2");
        } else if (argument == "--state1") {
            options.state1 = require_value("--state1");
        } else if (argument == "--state2") {
            options.state2 = require_value("--state2");
        } else if (argument == "--frames") {
            options.frames = parse_positive(require_value("--frames"),
                                            "--frames");
        } else if (argument == "--port") {
            const auto value = parse_positive(require_value("--port"),
                                              "--port");
            if (value > 65'535) {
                throw std::invalid_argument("--port is out of range");
            }
            options.port = static_cast<std::uint16_t>(value);
        } else if (argument == "--transport") {
            const auto value = require_value("--transport");
            if (value == "local") {
                options.local = true;
            } else if (value == "tcp") {
                options.local = false;
            } else {
                throw std::invalid_argument("unknown transport: " + value);
            }
        } else if (argument == "--auto-confirm") {
            options.auto_confirm = true;
        } else if (argument == "--scenario") {
            const auto value = require_value("--scenario");
            if (value == "trade") {
                options.scenario = Scenario::trade;
                options.expectation = Expectation::trade;
            } else if (value == "battle") {
                options.scenario = Scenario::battle;
                options.expectation = Expectation::battle;
            } else {
                throw std::invalid_argument("unknown scenario: " + value);
            }
            options.auto_confirm = true;
        } else if (argument == "--expect") {
            const auto value = require_value("--expect");
            if (value == "trade") {
                options.expectation = Expectation::trade;
            } else if (value == "battle") {
                options.expectation = Expectation::battle;
            } else {
                throw std::invalid_argument("unknown expectation: " + value);
            }
        } else if (argument == "--report") {
            options.report = require_value("--report");
        } else if (argument == "--trace") {
            options.trace = require_value("--trace");
        } else if (argument == "--capture-dir") {
            options.capture_dir = require_value("--capture-dir");
        } else if (argument == "--help" || argument == "-h") {
            usage();
            std::exit(EXIT_SUCCESS);
        } else {
            throw std::invalid_argument("unknown option: " + argument);
        }
    }
    if (options.rom.empty() || options.save1.empty() || options.save2.empty()) {
        throw std::invalid_argument("--rom, --save1, and --save2 are required");
    }
    if (options.state1.empty() != options.state2.empty()) {
        throw std::invalid_argument("--state1 and --state2 must be provided together");
    }
    if ((options.scenario == Scenario::trade &&
         options.expectation != Expectation::trade) ||
        (options.scenario == Scenario::battle &&
         options.expectation != Expectation::battle)) {
        throw std::invalid_argument(
            "--scenario and --expect must refer to the same outcome");
    }
    if (options.scenario != Scenario::none && options.state1.empty()) {
        throw std::invalid_argument(
            "--scenario requires --state1 and --state2 for reproducible input");
    }
    return options;
}

} // namespace gbb::link_harness
