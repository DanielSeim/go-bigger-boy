#include "gameboy/sgb_trace.hpp"

#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>

namespace {

std::string read_trace(const char* path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("could not open trace: " + std::string{path});
    return {std::istreambuf_iterator<char>{input}, {}};
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "Usage: gbb_sgb_trace_diff LEFT.trace RIGHT.trace\n";
        return 2;
    }
    try {
        std::string error;
        const auto left = gameboy::SgbTrace::parse(read_trace(argv[1]), &error);
        if (!left.has_value()) {
            std::cerr << "Could not parse left trace: " << error << '\n';
            return 2;
        }
        const auto right = gameboy::SgbTrace::parse(read_trace(argv[2]), &error);
        if (!right.has_value()) {
            std::cerr << "Could not parse right trace: " << error << '\n';
            return 2;
        }
        const auto result = gameboy::SgbTrace::diff(*left, *right);
        if (result.equal) {
            std::cout << "SGB traces match: " << left->writes.size()
                      << " writes, " << left->checkpoints.size()
                      << " checkpoints\n";
            return 0;
        }
        std::cout << "SGB traces differ at index " << result.index << ": "
                  << result.description << '\n';
        return 1;
    } catch (const std::exception& exception) {
        std::cerr << "Could not compare SGB traces: " << exception.what() << '\n';
        return 2;
    }
}
