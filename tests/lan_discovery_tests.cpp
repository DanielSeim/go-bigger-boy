#include "gameboy/lan_discovery.hpp"

#include <chrono>
#include <iostream>
#include <thread>
#include <vector>

namespace {
int failures = 0;

void check(const bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}
} // namespace

int main() {
    gameboy::LanDiscovery host;
    gameboy::LanDiscovery scanner;
    const auto fingerprint = UINT64_C(0x0123456789abcdef);
    const auto host_started = host.start_host(8765, fingerprint, "Test Host");
    const auto scanner_started = scanner.start_scan(fingerprint);
    if (!host_started || !scanner_started) {
        // Some hermetic CI/sandbox environments deny UDP sockets entirely.
        // Keep the test visible as skipped there; real desktop CI exercises
        // the full host/query exchange.
        std::cout << "SKIP: UDP discovery sockets unavailable\n";
        return 77;
    }
    if (host.active() && scanner.active()) {
        std::vector<gameboy::LanPeer> peers;
        for (int attempt = 0; attempt < 100 && peers.empty(); ++attempt) {
            host.poll();
            scanner.poll();
            peers = scanner.take_peers();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        host.poll();
        scanner.poll();
        if (peers.empty()) peers = scanner.take_peers();
        check(!peers.empty(), "scanner receives a matching host");
        if (!peers.empty()) {
            check(peers.front().port == 8765 &&
                      peers.front().rom_fingerprint == fingerprint &&
                      peers.front().name == "Test Host",
                  "discovery response carries host metadata");
        }
    }
    scanner.stop();
    host.stop();
    return failures == 0 ? 0 : 1;
}
