#pragma once

#include "gameboy/link_endpoint.hpp"

namespace gameboy {

class Emulator;

// Adapter for the built-in Game Boy core.  Keeping this type outside
// LinkSession makes the session reusable by future cores without pulling the
// Game Boy Emulator API into the generic link layer.
class GameBoyLinkEndpoint final : public LinkEndpoint {
public:
    explicit GameBoyLinkEndpoint(Emulator& emulator) noexcept;

    [[nodiscard]] SerialPort& serial_port() noexcept override;

    [[nodiscard]] unsigned step() override;
    [[nodiscard]] std::uint64_t emulated_cycles() const noexcept override;

private:
    Emulator* emulator_;
};

} // namespace gameboy
