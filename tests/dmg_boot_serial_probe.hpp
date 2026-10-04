// Test-only, host-driven bus diagnostics. No cartridge code runs in these
// checks, and no boot image, cartridge bytes or save-state payload is exported.
#pragma once

#include "gameboy/emulator.hpp"

#include <array>
#include <ostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace dmg_serial_probe {
inline bool equal(const gameboy::MemoryBus& a, const gameboy::MemoryBus& b) {
    return a.read8(0xFF01) == b.read8(0xFF01) &&
           a.read8(0xFF02) == b.read8(0xFF02) &&
           a.read8(0xFF0F) == b.read8(0xFF0F) &&
           a.serial_port().link_state_signature() == b.serial_port().link_state_signature() &&
           a.serial_port().transfer_byte() == b.serial_port().transfer_byte();
}

inline void print(std::ostream& out, const std::vector<std::uint8_t>& rom,
                  const std::vector<std::uint8_t>& handoff) {
    out << "{\"schema\":1,\"internal\":[";
    bool first = true;
    for (const unsigned idle : {0U, 1U, 4U, 59U, 60U, 119U, 511U, 512U, 4095U, 8192U}) {
        gameboy::Emulator original{gameboy::Cartridge(rom), gameboy::HardwareModel::dmg};
        gameboy::Emulator restored{gameboy::Cartridge(rom), gameboy::HardwareModel::dmg};
        original.load_state(handoff);
        auto& bus = original.bus();
        bus.tick(idle);
        const auto phase = bus.serial_port().phase();
        bus.write8(0xFF0F, 0);
        bus.write8(0xFF01, 0xA5);
        bus.write8(0xFF02, 0x81);
        unsigned bits = 0;
        bool restore_started = false, restore_exact = true;
        if (!first) out << ',';
        first = false;
        out << "{\"idle\":" << idle << ",\"phase\":" << phase << ",\"edges\":[";
        for (unsigned elapsed = 1; bus.serial_port().transfer_active(); ++elapsed) {
            if (elapsed > 4096) throw std::runtime_error("serial internal transfer timed out");
            bus.tick(1);
            if (restore_started) {
                restored.bus().tick(1);
                restore_exact = restore_exact && equal(bus, restored.bus());
            }
            // Save after three bits plus 37 clocks, not at a bit boundary.
            if (!restore_started && bits == 3 && bus.serial_port().phase() == 37) {
                restored.load_state(original.save_state());
                restore_started = true;
                restore_exact = equal(bus, restored.bus());
            }
            if (bus.serial_port().bits_shifted() == bits) continue;
            if (bits) out << ',';
            bits = bus.serial_port().bits_shifted();
            out << '[' << elapsed << ',' << +bus.read8(0xFF01) << ','
                << +bus.read8(0xFF02) << ',' << +bus.read8(0xFF0F) << ']';
        }
        const auto output = bus.take_serial_output();
        const bool output_once = output == std::string(1, static_cast<char>(0xA5)) &&
                                 bus.take_serial_output().empty();
        restore_exact = restore_started && restore_exact &&
                        restored.bus().take_serial_output() == output &&
                        restored.bus().take_serial_output().empty();
        // An abort must not publish a partial byte or request an interrupt.
        bus.write8(0xFF0F, 0);
        bus.write8(0xFF01, 0x5A);
        bus.write8(0xFF02, 0x81);
        bus.tick(1024);
        bus.write8(0xFF02, 0);
        const bool abort_clean = !bus.serial_port().transfer_active() &&
            bus.serial_port().bits_shifted() == 0 && !(bus.read8(0xFF0F) & 8) &&
            bus.take_serial_output().empty();
        bus.write8(0xFF01, 0x5A);
        bus.write8(0xFF02, 0x81);
        bus.tick(511);
        const bool rearm_waits = bus.serial_port().bits_shifted() == 0;
        bus.tick(1);
        out << "],\"restore_exact\":" << restore_exact << ",\"output_once\":" << output_once
            << ",\"abort_clean\":" << abort_clean << ",\"rearm_edge\":"
            << (rearm_waits && bus.serial_port().bits_shifted() == 1) << '}';
    }

    // No internal clock may advance an externally clocked transfer. Inject
    // irregular peer edges, saving/restoring with five bits still pending.
    gameboy::Emulator external{gameboy::Cartridge(rom), gameboy::HardwareModel::dmg};
    gameboy::Emulator resumed{gameboy::Cartridge(rom), gameboy::HardwareModel::dmg};
    external.load_state(handoff);
    auto& bus = external.bus();
    bus.write8(0xFF0F, 0);
    bus.write8(0xFF01, 0x3C);
    bus.write8(0xFF02, 0x80);
    bus.tick(8192);
    const bool waits = bus.serial_port().bits_shifted() == 0 &&
                       bus.serial_port().transfer_active() && !(bus.read8(0xFF0F) & 8);
    bool restore_exact = true;
    out << "],\"external\":{\"waits\":" << waits << ",\"edges\":[";
    for (unsigned bit = 0; bit < 8; ++bit) {
        const unsigned gap = std::array<unsigned, 8>{1, 511, 1024, 7, 4096, 3, 512, 19}[bit];
        bus.tick(gap);
        const bool incoming = (0xA5U & (0x80U >> bit)) != 0;
        const bool outgoing = bus.serial_port().clock_external_bit(incoming);
        if (bit >= 3) {
            resumed.bus().tick(gap);
            restore_exact = (resumed.bus().serial_port().clock_external_bit(incoming) == outgoing)
                            && restore_exact && equal(bus, resumed.bus());
        }
        if (bit == 2) {
            resumed.load_state(external.save_state());
            restore_exact = equal(bus, resumed.bus());
        }
        if (bit) out << ',';
        out << '[' << outgoing << ',' << +bus.read8(0xFF01) << ','
            << +bus.read8(0xFF02) << ',' << +bus.read8(0xFF0F) << ']';
    }
    const auto output = bus.take_serial_output();
    restore_exact = restore_exact && resumed.bus().take_serial_output() == output;
    out << "],\"restore_exact\":" << restore_exact << ",\"output_once\":"
        << (output == std::string(1, static_cast<char>(0x3C)) && bus.take_serial_output().empty())
        << "},\"linked\":[";

    first = true;
    // Mix a just-booted port with the unchanged production post-boot path,
    // and exercise each as master, including an initially unarmed peer.
    for (const bool boot_master : {true, false}) {
        for (const unsigned late : {0U, 4096U}) {
            gameboy::Emulator booted{gameboy::Cartridge(rom), gameboy::HardwareModel::dmg};
            gameboy::Emulator fast{gameboy::Cartridge(rom), gameboy::HardwareModel::dmg};
            booted.load_state(handoff);
            auto& master = boot_master ? booted.bus() : fast.bus();
            auto& peer = boot_master ? fast.bus() : booted.bus();
            master.tick(119);
            peer.tick(511);
            gameboy::SerialCable cable;
            cable.connect(master.serial_port(), peer.serial_port());
            master.write8(0xFF0F, 0); peer.write8(0xFF0F, 0);
            master.write8(0xFF01, 0xA5); peer.write8(0xFF01, 0x3C);
            master.write8(0xFF02, 0x81);
            bool held = true;
            if (late) {
                master.tick(late);
                held = master.serial_port().bits_shifted() == 0 &&
                       master.serial_port().transfer_active() && !(master.read8(0xFF0F) & 8);
            }
            peer.write8(0xFF02, 0x80);
            if (!first) out << ',';
            first = false;
            out << "{\"boot_master\":" << boot_master << ",\"late\":" << late
                << ",\"held\":" << held << ",\"edges\":[";
            unsigned bits = 0;
            for (unsigned elapsed = 1; master.serial_port().transfer_active(); ++elapsed) {
                if (elapsed > 4096) throw std::runtime_error("linked serial transfer timed out");
                master.tick(1);
                if (master.serial_port().bits_shifted() == bits) continue;
                if (bits) out << ',';
                bits = master.serial_port().bits_shifted();
                out << '[' << elapsed << ',' << +master.read8(0xFF01) << ','
                    << +peer.read8(0xFF01) << ',' << +master.read8(0xFF02) << ','
                    << +peer.read8(0xFF02) << ',' << +master.read8(0xFF0F) << ','
                    << +peer.read8(0xFF0F) << ']';
            }
            out << "],\"completion_once\":"
                << (master.serial_port().transfers_completed() == 1 &&
                    peer.serial_port().transfers_completed() == 1 &&
                    master.serial_port().last_transmitted() == 0xA5 &&
                    peer.serial_port().last_transmitted() == 0x3C &&
                    master.serial_port().last_received() == 0x3C &&
                    peer.serial_port().last_received() == 0xA5 &&
                    master.take_serial_output().empty() && peer.take_serial_output().empty()) << '}';
        }
    }
    out << "]}";
}
} // namespace dmg_serial_probe
