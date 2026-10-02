#pragma once

#include <cstdint>
#include <iostream>
#include <sstream>
#include <string>

namespace sgb_test {

template<class Sink> int run_spc_write_fixture(Sink& sink) {
    std::string line;
    bool ran = false;
    while (std::getline(std::cin, line)) {
        std::istringstream fields(line);
        std::string operation, extra;
        unsigned address{}, value{};
        if (!(fields >> operation)) continue;
        if (operation == "ram" && !ran) {
            if (!(fields >> address >> value) || (fields >> extra) ||
                address > 65535 || value > 255) return 2;
            sink.load(static_cast<std::uint16_t>(address), static_cast<std::uint8_t>(value));
        } else if (operation == "run" && !ran) {
            if (!(fields >> value) || (fields >> extra) || value == 0 || value > 1000) return 2;
            ran = true;
            if (!sink.run(value)) return 3;
        } else return 2;
    }
    return ran ? 0 : 2;
}

} // namespace sgb_test
