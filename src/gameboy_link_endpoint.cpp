#include "gameboy/gameboy_link_endpoint.hpp"

#include "gameboy/emulator.hpp"

namespace gameboy {

GameBoyLinkEndpoint::GameBoyLinkEndpoint(Emulator& emulator) noexcept
    : emulator_(&emulator) {}

SerialPort& GameBoyLinkEndpoint::serial_port() noexcept {
    return emulator_->bus().serial_port();
}

unsigned GameBoyLinkEndpoint::step() { return emulator_->step(); }

std::uint64_t GameBoyLinkEndpoint::emulated_cycles() const noexcept {
    return emulator_ == nullptr ? 0 : emulator_->cpu().total_cycles();
}

} // namespace gameboy
