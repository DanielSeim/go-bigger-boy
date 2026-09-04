#include "gameboy/link_transport.hpp"

namespace gameboy {

void LocalLinkTransport::attach(SerialPort& first,
                                SerialPort& second) noexcept {
    detach();
    cable_.connect(first, second);
    cable_connected_ = true;
}

void LocalLinkTransport::detach() noexcept {
    cable_.disconnect();
    cable_connected_ = false;
}

} // namespace gameboy
