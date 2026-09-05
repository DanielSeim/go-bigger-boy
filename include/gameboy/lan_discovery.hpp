#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace gameboy {

struct LanPeer {
    std::string address;
    std::string name;
    std::uint16_t port{};
    std::uint64_t rom_fingerprint{};
};

// Opt-in UDP discovery for desktop LAN links. A host answers queries while a
// scanner sends one broadcast and collects replies. Discovery never opens the
// TCP link or starts emulation by itself.
class LanDiscovery final {
public:
    static constexpr std::uint16_t discovery_port = 8764;

    LanDiscovery() noexcept = default;
    LanDiscovery(const LanDiscovery&) = delete;
    LanDiscovery& operator=(const LanDiscovery&) = delete;
    ~LanDiscovery();

    [[nodiscard]] bool start_host(std::uint16_t tcp_port,
                                  std::uint64_t rom_fingerprint,
                                  const std::string& name) noexcept;
    [[nodiscard]] bool start_scan(std::uint64_t rom_fingerprint) noexcept;
    void poll() noexcept;
    void stop() noexcept;

    [[nodiscard]] bool active() const noexcept { return socket_ != -1; }
    [[nodiscard]] std::vector<LanPeer> take_peers();

private:
    enum class Mode { host, scan };
    void receive_available() noexcept;
    bool send_message(const std::string& message, const char* address,
                      std::uint16_t port) noexcept;

    std::intptr_t socket_{-1};
    Mode mode_{Mode::scan};
    std::uint16_t tcp_port_{};
    std::uint64_t rom_fingerprint_{};
    std::string name_;
    std::vector<LanPeer> peers_;
};

} // namespace gameboy
