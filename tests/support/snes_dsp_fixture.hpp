#pragma once

#include <array>
#include <cstdio>
#include <cstdint>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

namespace sgb_test {

using DspStereoSample = std::array<std::int16_t, 2>;
struct DspClockResult {
    bool supported{true};
    std::optional<DspStereoSample> sample{};
};

// Shared, text-only stimulus protocol for independent DSP runners. A clock
// command advances individual DSP clocks and emits PCM only when available.
template <typename Sink>
int run_dsp_fixture(Sink& sink) {
#ifdef _WIN32
    // The protocol writes raw little-endian PCM to stdout. Windows text mode
    // expands 0x0a bytes and corrupts both sample counts and PCM hashes.
    if (_setmode(_fileno(stdout), _O_BINARY) == -1) return 2;
#endif
    std::string line;
    unsigned line_number = 0;
    unsigned sample_count = 0;
    while (std::getline(std::cin, line)) {
        ++line_number;
        const auto comment = line.find('#');
        if (comment != std::string::npos) line.erase(comment);
        std::istringstream fields(line);
        std::string operation;
        if (!(fields >> operation)) continue;
        if (operation == "endx") {
            std::string excess;
            if (fields >> excess) {
                std::cerr << "fixture line " << line_number
                          << ": invalid field count\n";
                return 2;
            }
            // Keep binary PCM on stdout; state probes are an ordered trace on
            // stderr so both test runners can be compared without new framing.
            std::cerr << "endx " << static_cast<unsigned>(sink.endx()) << '\n';
            continue;
        }
        std::string first;
        std::string second;
        std::string excess;
        if (!(fields >> first) ||
            (operation != "step" && operation != "clock" && !(fields >> second)) ||
            (fields >> excess)) {
            std::cerr << "fixture line " << line_number << ": invalid field count\n";
            return 2;
        }
        try {
            const auto parse = [](const std::string& token) {
                std::size_t used = 0;
                const auto value = std::stoul(token, &used, 0);
                if (used != token.size()) throw std::invalid_argument("trailing text");
                return value;
            };
            const auto address = parse(first);
            if (operation == "ram" || operation == "reg") {
                const auto value = parse(second);
                if (value > 0xffUL || address > (operation == "ram" ? 0xffffUL : 0x7fUL)) {
                    throw std::out_of_range("address or value");
                }
                if (operation == "ram") sink.ram(static_cast<std::uint16_t>(address),
                                                    static_cast<std::uint8_t>(value));
                else sink.reg(static_cast<std::uint8_t>(address),
                              static_cast<std::uint8_t>(value));
            } else if (operation == "step" || operation == "clock") {
                if (address == 0 || address > 100000UL ||
                    sample_count + address > 1000000UL) {
                    throw std::out_of_range("step count");
                }
                for (unsigned i = 0; i < address; ++i) {
                    const auto result = operation == "step"
                        ? sink.step_result() : sink.clock();
                    if (!result.supported) {
                        std::cerr << "fixture line " << line_number
                                  << ": unsupported DSP mode or no output\n";
                        return 3;
                    }
                    if (!result.sample) continue;
                    for (const auto channel : *result.sample) {
                        const auto bits = static_cast<std::uint16_t>(channel);
                        const char bytes[2]{static_cast<char>(bits & 0xffU),
                                            static_cast<char>(bits >> 8)};
                        std::cout.write(bytes, 2);
                    }
                    ++sample_count;
                }
            } else {
                throw std::invalid_argument("operation");
            }
        } catch (const std::exception&) {
            std::cerr << "fixture line " << line_number << ": invalid " << operation
                      << " arguments\n";
            return 2;
        }
    }
    if (sample_count == 0 || !std::cout) {
        std::cerr << "fixture: no samples or output error\n";
        return 2;
    }
    return 0;
}

} // namespace sgb_test
