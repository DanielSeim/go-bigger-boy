// Test-only probe for the public Gambatte ly0 fixture's result routine.
// Inputs stay local; report only the computed byte, never ROM or RAM contents.
#include "gameboy/emulator.hpp"

#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    try {
        if (argc < 2 || argc > 4)
            throw std::runtime_error("usage: gbb_stat_irq_probe ROM [dmg|mgb|cgb-c|cgb-e|sgb|sgb2] [--dmg-boot]");
        const std::string model = argc >= 3 ? argv[2] : "dmg";
        const auto hardware = model == "dmg" ? gameboy::HardwareModel::dmg :
            model == "mgb" ? gameboy::HardwareModel::mgb :
            model == "cgb-c" ? gameboy::HardwareModel::cgb_c :
            model == "cgb-e" ? gameboy::HardwareModel::cgb_e :
            model == "sgb" ? gameboy::HardwareModel::sgb :
            model == "sgb2" ? gameboy::HardwareModel::sgb2 : throw std::runtime_error("invalid model");
        if (argc == 4 && std::string(argv[3]) != "--dmg-boot")
            throw std::runtime_error("invalid boot option");
        std::ifstream input(argv[1], std::ios::binary);
        if (!input) throw std::runtime_error("cannot open fixture");
        std::vector<std::uint8_t> rom{std::istreambuf_iterator<char>(input), {}};
        if (input.bad()) throw std::runtime_error("cannot read fixture");
        // Bytes-only construction never imports/persists adjacent save files.
        gameboy::Emulator emulator(gameboy::Cartridge(std::move(rom)), hardware,
            argc == 4 ? gameboy::BootRomMode::replacement : gameboy::BootRomMode::post_boot);
        while (emulator.cpu().total_cycles() < 20'000'000) {
            if (!emulator.bus().boot_rom_enabled() && emulator.cpu().registers().pc == 0x7000) {
                std::cout << "{\"schema\":1,\"result\":" << +emulator.cpu().registers().a
                          << ",\"cycles\":" << emulator.cpu().total_cycles() << "}\n";
                return 0;
            }
            (void)emulator.step();
            (void)emulator.take_audio_samples();
        }
        throw std::runtime_error("fixture did not reach result routine within budget");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 2;
    }
}
