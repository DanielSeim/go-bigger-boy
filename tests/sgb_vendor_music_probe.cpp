// SPDX-License-Identifier: GPL-3.0-or-later
#include "gameboy/sgb_host.hpp"
#include "gameboy/boot_rom.hpp"
#include "../scripts/sgb_input_script.h"
#include <charconv>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <tuple>
using Host = gameboy::SgbHost;
namespace {
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
std::vector<std::uint8_t> read(const char* path, std::size_t min, std::size_t max) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    require(bool(input) && input.tellg() >= std::streamoff(min) &&
            input.tellg() <= std::streamoff(max), "invalid image length");
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(input.tellg()));
    input.seekg(0);
    require(bool(input.read(reinterpret_cast<char*>(bytes.data()), bytes.size())), "image read failed");
    return bytes;
}
struct Result {
    std::uint64_t hash = 14695981039346656037ULL, frames = 0, nonzero = 0,
        last_nonzero = 0, clocks = 0;
    unsigned state = 0, error = 0, transfers = 0, adoptions = 0, sounds = 0,
        starts = 0, completes = 0, notes = 0, selected = 0, rejected = 0,
        flg = 0, status = 0, external = 0, models_frames = 0;
    std::vector<std::uint8_t> snapshot;
    bool operator==(const Result& other) const {
        return std::tie(hash, frames, nonzero, last_nonzero, clocks, state, error, transfers,
                        adoptions, sounds, starts, completes, notes, selected, rejected,
                        flg, status, external, models_frames, snapshot) ==
               std::tie(other.hash, other.frames, other.nonzero, other.last_nonzero,
                        other.clocks, other.state, other.error, other.transfers,
                        other.adoptions, other.sounds, other.starts, other.completes,
                        other.notes, other.selected, other.rejected, other.flg,
                        other.status, other.external, other.models_frames, other.snapshot);
    }
};
Result run(Host& initial, std::uint64_t target, const gameboy::SgbHostConfig& config,
           unsigned* restore_count = nullptr, unsigned* unread_count = nullptr, bool scalar = false) {
    Result result;
    Host* host = &initial;
    std::unique_ptr<Host> continued;
    unsigned steps = 0, previous = ~0U;
    while (host->cpu().timing().clocks() < target) {
        if (!host->step()) throw std::runtime_error("host execution fault status=" +
            std::to_string(unsigned(host->status())) + " pc=" + std::to_string(host->fault().pc) +
            " host=" + std::to_string(host->cpu().debug_wram_byte(0x20)) +
            " phase=" + std::to_string(host->debug_spc_ram_byte(0xd2)));
        const unsigned phase = host->cpu().debug_wram_byte(0x20) |
            (unsigned(host->debug_spc_ram_byte(0xd2)) << 8) |
            (unsigned(host->debug_spc_ram_byte(0xd4)) << 16) |
            (unsigned(host->debug_spc_ram_byte(0xd5)) << 24);
        if (restore_count && ((++steps % 65536) == 0 || phase != previous)) {
            require(*restore_count < 2048, "restore budget exceeded");
            auto snapshot = host->save_state();
            // Load into a separately initialized instance before continuing this run.
            auto destination = std::make_unique<Host>(config);
            if (scalar) destination->debug_set_apu_batch_enabled(false);
            require(destination->load_state(snapshot) && destination->save_state() == snapshot,
                    "cross-instance snapshot mismatch");
            continued = std::move(destination);
            host = continued.get();
            ++*restore_count;
            if (host->pending_samples()) ++*unread_count;
        }
        previous = phase;
        Host::StereoSample sample;
        while (host->pop_sample(sample)) {
            ++result.frames;
            if (sample.left || sample.right) {
                ++result.nonzero;
                result.last_nonzero = host->cpu().timing().clocks();
            }
            for (auto value : {sample.left, sample.right}) for (unsigned i = 0; i < 2; ++i) {
                result.hash ^= (std::uint16_t(value) >> (8*i)) & 255;
                result.hash *= 1099511628211ULL;
            }
        }
        if (host->cpu().debug_wram_byte(0x20) == 0xff) break;
    }
    result.clocks = host->cpu().timing().clocks();
    const auto ram = [&](unsigned address) { return unsigned(host->cpu().debug_wram_byte(address)); };
    result.state = ram(0x20); result.error = ram(0x27);
    result.transfers = ram(0x26); result.adoptions = ram(0x2a);
    result.sounds = host->icd().sound_packets_delivered();
    result.starts = host->debug_spc_ram_byte(0xd4);
    result.completes = host->debug_spc_ram_byte(0xd5);
    result.notes = host->debug_spc_ram_byte(0xd7) | (unsigned(host->debug_spc_ram_byte(0xd8)) << 8);
    result.selected = host->debug_spc_ram_byte(0xd3);
    result.rejected = host->debug_spc_ram_byte(0xd6);
    result.flg = host->debug_dsp_register(0x6c);
    result.status = unsigned(host->status()); result.external = ram(0x24);
    result.models_frames = host->icd().completed_frames();
    result.snapshot = host->save_state();
    return result;
}
}
int main(int argc, char** argv) {
    try {
        require(argc == 7 || argc == 8, "ROM GAME sgb|sgb2 CLOCKS native|scalar|combined fixture|bundled [INPUT_SCRIPT]");
        gameboy::SgbHostConfig config;
        config.program_rom = read(argv[1], 262144, 262144);
        config.game_rom = read(argv[2], 336, 16*1024*1024);
        const std::string_view model = argv[3], mode = argv[5], boot = argv[6];
        require(model == "sgb" || model == "sgb2", "invalid model");
        require(mode == "native" || mode == "scalar" || mode == "combined", "invalid mode");
        require(boot == "fixture" || boot == "bundled", "invalid boot profile");
        config.model = model == "sgb" ? gameboy::HardwareModel::sgb : gameboy::HardwareModel::sgb2;
        if (boot == "bundled") config.gb_boot_rom = model == "sgb" ? gameboy::sgb_boot_rom() : gameboy::sgb2_boot_rom();
        else { config.gb_boot_rom[0] = 0xc3; config.gb_boot_rom[2] = 1; }
        std::uint64_t target = 0;
        const std::string_view number = argv[4];
        const auto parsed = std::from_chars(number.data(), number.data()+number.size(), target);
        require(parsed.ec == std::errc{} && parsed.ptr == number.data()+number.size() &&
                target && target <= 2000000000, "clock bound must be 1..2000000000");
        if (argc == 8) {
            gbb_sgb_input_script script{}; char error[256]{};
            require(gbb_sgb_input_load(argv[7], &script, error, sizeof(error)), "invalid input script");
            for (std::size_t i = 0; i < script.count; ++i)
                config.input_events.push_back({script.events[i].frame, script.events[i].mask});
        }
        config.combined_audio = mode == "combined";
        Host host(config), restored(config);
        if (mode == "scalar") { host.debug_set_apu_batch_enabled(false); restored.debug_set_apu_batch_enabled(false); }
        const auto result = run(host, target, config);
        unsigned restores = 0, unread = 0;
        require(result == run(restored, target, config, &restores, &unread, mode == "scalar"), "restored state/PCM mismatch");
        host.reset();
        require(result == run(host, target, config), "reset state/PCM mismatch");
        // Aggregate metadata only: no private score, waveform, sample or state export.
        std::cout << "{\"schema\":\"gbb-sgb-vendor-music-v1\",\"qualification\":false,"
                  << "\"model\":\"" << model << "\",\"mode\":\"" << mode
                  << "\",\"reset_equal\":true,\"restore_equal\":true,\"restores\":" << restores
                  << ",\"unread_restores\":" << unread << ",\"clocks\":" << result.clocks
                  << ",\"frames\":" << result.frames << ",\"nonzero\":" << result.nonzero
                  << ",\"pcm_fnv64\":" << result.hash << ",\"last_nonzero_clock\":" << result.last_nonzero
                  << ",\"firmware_state\":" << result.state << ",\"transfer_error\":" << result.error
                  << ",\"transfers\":" << result.transfers << ",\"adoptions\":" << result.adoptions
                  << ",\"sounds\":" << result.sounds << ",\"starts\":" << result.starts
                  << ",\"completes\":" << result.completes << ",\"notes\":" << result.notes
                  << ",\"selected\":" << result.selected << ",\"rejected\":" << result.rejected
                  << ",\"flg\":" << result.flg << ",\"host_status\":" << result.status
                  << ",\"external\":" << result.external << ",\"gb_frames\":" << result.models_frames << "}\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 2; }
}
