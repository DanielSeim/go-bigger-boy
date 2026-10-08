// SPDX-License-Identifier: GPL-3.0-or-later
#include "gameboy/sgb_host.hpp"
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <tuple>

using Host = gameboy::SgbHost;
namespace {
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
std::vector<std::uint8_t> read(const char* path, std::size_t size) {
    std::ifstream input(path, std::ios::binary);
    std::vector<std::uint8_t> result(size);
    require(bool(input.read(reinterpret_cast<char*>(result.data()), size)) &&
            input.peek() == std::char_traits<char>::eof(), "image length");
    return result;
}
struct Result {
    std::uint64_t hash = 14695981039346656037ULL, frames = 0, nonzero = 0,
                  clocks = 0, last_nonzero_clock = 0;
    unsigned status = 0, transfers = 0, adoptions = 0, version = 0,
             bridge = 0, signature = 0, sounds = 0, error = 0, external = 0;
    unsigned interruptions = 0, active_env = 0, interrupt_tick = 0, interrupt_count = 0, kof = 0, flg = 0, score_tick = 0, selected_song = 0, admitted_roots = 0;
    std::vector<std::uint8_t> state;
    bool operator==(const Result& other) const {
        return std::tie(hash, frames, nonzero, clocks, last_nonzero_clock, status,
                        transfers, adoptions, version, bridge, signature, sounds,
                        error, external, interruptions, active_env, interrupt_tick, interrupt_count, kof, flg, score_tick, selected_song, admitted_roots, state) ==
               std::tie(other.hash, other.frames, other.nonzero, other.clocks,
                        other.last_nonzero_clock, other.status, other.transfers,
                        other.adoptions, other.version, other.bridge, other.signature,
                        other.sounds, other.error, other.external, other.interruptions, other.active_env,
                        other.interrupt_tick, other.interrupt_count, other.kof, other.flg, other.score_tick, other.selected_song, other.admitted_roots, other.state);
    }
};
struct Restores { unsigned count = 0, phases = 0, commands = 0, roots = 0; };
Result run(Host& host, std::uint64_t target, Restores* restores = nullptr) {
    Result result;
    unsigned steps = 0;
    std::uint64_t previous = ~std::uint64_t{0};
    Host::StereoSample sample;
    while (host.cpu().timing().clocks() < target) {
        require(host.step(), "whole-host execution fault");
        while (host.pop_sample(sample)) {
            ++result.frames;
            if (sample.left || sample.right) {
                ++result.nonzero;
                result.last_nonzero_clock = host.cpu().timing().clocks();
            }
            for (auto value : {sample.left, sample.right}) {
                for (unsigned i = 0; i < 2; ++i) {
                    result.hash ^= (std::uint16_t(value) >> (8*i)) & 255;
                    result.hash *= 1099511628211ULL;
                }
            }
        }
        if (restores) {
            auto status = host.cpu().debug_wram_byte(0x20);
            auto bridge = host.cpu().debug_wram_byte(0x31);
            unsigned commands = host.debug_spc_ram_byte(0xd3);
            const auto admitted = host.debug_spc_ram_byte(0xdc);
            std::uint64_t phase = status | (bridge << 8) | (commands << 16) |
                                  (std::uint64_t(host.debug_spc_ram_byte(0xd6)) << 24) |
                                  (std::uint64_t(admitted) << 32) |
                                  (std::uint64_t(host.debug_spc_ram_byte(0xdb)) << 40);
            if (++steps % 100003 == 0 || phase != previous) {
                require(++restores->count <= 4096, "snapshot bound");
                auto state = host.save_state();
                require(host.load_state(state), "mid-execution restore");
                require(host.save_state() == state, "exact state restoration");
                restores->commands |= commands;
                if (admitted > 0 && admitted <= 3) restores->roots |= (1U << admitted) - 1;
                if (status == 4) restores->phases |= 1; // IPL upload
                if (status == 2) restores->phases |= 2; // external restart
                if (bridge == 1) restores->phases |= 4; // validated, silent
                if (bridge == 3) restores->phases |= 8; // rendering
                if (bridge == 2) restores->phases |= 16; // completed
            }
            previous = phase;
        }
    }
    result.clocks = host.cpu().timing().clocks();
    result.status = host.cpu().debug_wram_byte(0x20);
    result.transfers = host.cpu().debug_wram_byte(0x26);
    result.adoptions = host.cpu().debug_wram_byte(0x2a);
    result.version = host.cpu().debug_wram_byte(0x30);
    result.bridge = host.cpu().debug_wram_byte(0x31);
    result.signature = host.cpu().debug_wram_byte(0x32);
    result.sounds = host.cpu().debug_wram_byte(0x23);
    result.error = host.cpu().debug_wram_byte(0x27);
    result.external = host.cpu().debug_wram_byte(0x24);
    result.interruptions = host.debug_spc_ram_byte(0xd3);
    result.active_env = host.debug_spc_ram_byte(0xd4);
    result.interrupt_tick = host.debug_spc_ram_byte(0xd5);
    result.interrupt_count = host.debug_spc_ram_byte(0xd6);
    result.score_tick = host.debug_spc_ram_byte(0x10) | (host.debug_spc_ram_byte(0x11) << 8);
    result.selected_song = host.debug_spc_ram_byte(0xdb);
    result.admitted_roots = host.debug_spc_ram_byte(0xdc);
    result.kof = host.debug_dsp_register(0x5c);
    result.flg = host.debug_dsp_register(0x6c);
    result.state = host.save_state();
    return result;
}
} // namespace
int main(int argc, char** argv) {
    try {
        require(argc == 6, "ROM GAME MODEL CLOCKS MODE");
        gameboy::SgbHostConfig config;
        config.program_rom = read(argv[1], 262144);
        config.game_rom = read(argv[2], 32768);
        const auto model = std::string(argv[3]);
        require(model == "sgb" || model == "sgb2", "model");
        config.model = model == "sgb" ? gameboy::HardwareModel::sgb : gameboy::HardwareModel::sgb2;
        config.gb_boot_rom[0] = 0xc3;
        config.gb_boot_rom[1] = 0;
        config.gb_boot_rom[2] = 1;
        const auto target = std::stoull(argv[4]);
        require(target > 0 && target <= 140000000, "clock bound");
        const auto mode = std::string(argv[5]);
        require(mode == "native" || mode == "combined" || mode == "scalar", "mode");
        config.combined_audio = mode == "combined";
        Host normal(config), restored(config);
        if (mode == "scalar") {
            normal.debug_set_apu_batch_enabled(false);
            restored.debug_set_apu_batch_enabled(false);
        }
        Restores restores;
        const auto result = run(normal, target);
        require(result == run(restored, target, &restores), "restored PCM and whole state parity");
        normal.reset();
        require(result == run(normal, target), "cold reset PCM and whole state parity");
        std::cout << "{\"schema\":\"gbb-score-transport-v1\",\"qualification\":false,"
                  << "\"playback\":false,\"reset_equal\":true,\"restore_equal\":true,"
                  << "\"restore_count\":" << restores.count << ",\"restore_phases\":" << restores.phases
                  << ",\"restore_commands\":" << restores.commands << ",\"restore_roots\":" << restores.roots
                  << ",\"status\":" << result.status << ",\"transfers\":" << result.transfers
                  << ",\"adoptions\":" << result.adoptions << ",\"version\":" << result.version
                  << ",\"bridge\":" << result.bridge << ",\"signature\":" << result.signature
                  << ",\"sounds\":" << result.sounds << ",\"error\":" << result.error
                  << ",\"external\":" << result.external << ",\"clocks\":" << result.clocks
                  << ",\"interruptions\":" << result.interruptions << ",\"active_env\":" << result.active_env
                  << ",\"interrupt_tick\":" << result.interrupt_tick << ",\"interrupt_count\":" << result.interrupt_count
                  << ",\"score_tick\":" << result.score_tick
                  << ",\"selected_song\":" << result.selected_song << ",\"admitted_roots\":" << result.admitted_roots
                  << ",\"kof\":" << result.kof << ",\"flg\":" << result.flg
                  << ",\"last_nonzero_clock\":" << result.last_nonzero_clock
                  << ",\"pcm\":{\"frames\":" << result.frames << ",\"nonzero_frames\":" << result.nonzero
                  << ",\"fnv1a64\":" << result.hash << "}}\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
