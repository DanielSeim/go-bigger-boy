#pragma once

#include "gameboy/serial.hpp"

namespace gameboy {

// Core-neutral endpoint consumed by LinkSession.  A frontend or another
// emulator core only needs to expose its serial port and advance one unit of
// emulation; the link scheduler does not need to know the concrete machine
// type.
class LinkEndpoint {
public:
    virtual ~LinkEndpoint() = default;

    [[nodiscard]] virtual SerialPort& serial_port() noexcept = 0;
    [[nodiscard]] virtual unsigned step() = 0;

    // Optional monotonic cycle position used only for diagnostics. A
    // core-neutral endpoint may leave this at zero; scheduling never depends
    // on the value.
    [[nodiscard]] virtual std::uint64_t emulated_cycles() const noexcept {
        return 0;
    }
};

} // namespace gameboy
