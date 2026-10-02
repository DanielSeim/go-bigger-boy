#pragma once

#include "gameboy/snes_apu_audio_engine.hpp"

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <vector>

namespace sgb_test {

// Diagnostic only: replay the same private live APU/music state three times.
// No SNES CPU, frontend, DAC/resampling, or future host commands run here.
// The state remains in memory, and the report contains metrics/hashes only.
inline void benchmark_apu_firmware(const std::vector<std::uint8_t>& state,
                                  unsigned apu_hz, const char* model,
                                  const std::filesystem::path& output) {
    if (state.empty()) throw std::runtime_error("no active music state for firmware benchmark");
    if (std::filesystem::exists(output)) throw std::runtime_error("firmware benchmark output already exists");
    struct Trial { double elapsed; std::uint64_t samples, nonzero, hash; };
    std::array<Trial, 3> trials{};
    gameboy::SnesApuAudioEngine engine;
    for (auto& trial : trials) {
        if (!engine.load_state(state)) throw std::runtime_error("invalid firmware benchmark state");
        trial.hash = 14695981039346656037ULL;
        const auto begin = std::chrono::steady_clock::now();
        for (std::uint64_t half = 0; half < std::uint64_t(apu_hz) * 4; ++half) {
            if (!engine.clock_half()) throw std::runtime_error("firmware benchmark CPU stopped");
            gameboy::SnesApuAudioEngine::StereoSample sample;
            while (engine.pop_sample(sample)) {
                ++trial.samples;
                if (sample.left || sample.right) ++trial.nonzero;
                for (auto channel : {sample.left, sample.right}) {
                    const auto bits = static_cast<std::uint16_t>(channel);
                    for (unsigned shift : {0U, 8U}) {
                        trial.hash ^= static_cast<std::uint8_t>(bits >> shift);
                        trial.hash *= 1099511628211ULL;
                    }
                }
            }
        }
        trial.elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
        if (trial.samples != apu_hz / 16 || !trial.nonzero)
            throw std::runtime_error("firmware benchmark did not produce complete nonsilent PCM");
    }
    for (const auto& trial : trials)
        if (trial.hash != trials[0].hash || trial.nonzero != trials[0].nonzero)
            throw std::runtime_error("firmware benchmark state continuation is not deterministic");
    std::ofstream file(output);
    if (!file) throw std::runtime_error("cannot create firmware benchmark report");
    file << "{\"format\":\"gbb-apu-firmware-performance-v1\",\"model\":\"" << model
         << "\",\"apu_hz\":" << apu_hz << ",\"emulated_seconds_per_trial\":2,\"trials\":[";
    for (unsigned i = 0; i < trials.size(); ++i) {
        const auto& trial = trials[i];
        if (i) file << ',';
        file << "{\"seconds\":" << trial.elapsed << ",\"realtime_ratio\":" << 2 / trial.elapsed
             << ",\"samples\":" << trial.samples << ",\"nonzero_samples\":" << trial.nonzero
             << ",\"pcm_fnv64\":" << trial.hash << '}';
    }
    file << "]}\n";
    if (!file) throw std::runtime_error("cannot finish firmware benchmark report");
}

} // namespace sgb_test
