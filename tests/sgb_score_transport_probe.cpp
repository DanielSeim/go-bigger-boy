// SPDX-License-Identifier: GPL-3.0-or-later
#include "gameboy/sgb_host.hpp"
#include <array>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <tuple>

#ifdef GBB_SCORE_ADSR_PROBE
#include "support/sgb_score_adsr_observer.hpp"
#endif
#ifdef GBB_SCORE_ACOUSTIC_PROBE
#ifdef GBB_SCORE_COMBINED_PITCH_PROBE
#include "support/sgb_combined_sample_pitch_observer.hpp"
#else
#include "support/sgb_host_sample_pitch_observer.hpp"
#endif
#endif
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
    unsigned atomic_clears = 0, atomic_invalidated = 0, atomic_phase = 0;
    std::uint64_t score_hash = 0, asset_hash = 0, blocked_frames = 0, blocked_nonzero = 0;
    unsigned recovery_rejections = 0, suppressed_sound = 0, recovery_blocked = 0,
             host_stack = 0, clear_events = 0, clear_verified = 0, clear_invalidated = 0;
    unsigned instruments = 0, source2 = 0, source3 = 0;
    std::array<unsigned, 9> prefix_ids{}, prefix_counts{};
    std::array<unsigned, 2> sample_counts{}, sample_loops{}, sample_starts{}, profile_env{}, profile_pitch{};
    std::array<unsigned, 8> voice_profiles{}, sample_headers{};
    std::array<unsigned, 2> voice_tuning{}, voice_env{}, voice_pitch{};
    unsigned end_windows = 0, natural_ends = 0;
    std::array<unsigned, 4> end_env{}, end_gates{}, end_sources{};
    std::array<unsigned, 2> endx{}, end_kof{}, sample_modes{};
#ifdef GBB_SCORE_TUNING_PROBE
    unsigned tuning_seen = 0;
    std::array<unsigned, 26> tuning_pitches{};
#endif
#ifdef GBB_SCORE_ADSR_PROBE
    ScoreAdsrObserver envelopes;
#endif
#ifdef GBB_SCORE_ACOUSTIC_PROBE
    HostSamplePitchObserver acoustic;
#endif
    std::vector<std::uint8_t> state;
    bool operator==(const Result& other) const {
#ifdef GBB_SCORE_ACOUSTIC_PROBE
        if (!(acoustic == other.acoustic)) return false;
#endif
#ifdef GBB_SCORE_TUNING_PROBE
        if (tuning_seen != other.tuning_seen || tuning_pitches != other.tuning_pitches) return false;
#endif
#ifdef GBB_SCORE_ADSR_PROBE
        if (!(envelopes == other.envelopes)) return false;
#endif
        return std::tie(hash, frames, nonzero, clocks, last_nonzero_clock, status,
                        transfers, adoptions, version, bridge, signature, sounds,
                        error, external, interruptions, active_env, interrupt_tick, interrupt_count, kof, flg, score_tick, selected_song, admitted_roots, instruments, source2, source3, prefix_ids, prefix_counts, sample_counts, sample_loops, sample_starts, profile_env, profile_pitch, voice_profiles, voice_tuning, voice_env, voice_pitch, end_windows, natural_ends, end_env, end_gates, end_sources, endx, end_kof, sample_modes, sample_headers, atomic_clears, atomic_invalidated, atomic_phase, score_hash, asset_hash, blocked_frames, blocked_nonzero, recovery_rejections, suppressed_sound, recovery_blocked, host_stack, clear_events, clear_verified, clear_invalidated, state) ==
               std::tie(other.hash, other.frames, other.nonzero, other.clocks,
                        other.last_nonzero_clock, other.status, other.transfers,
                        other.adoptions, other.version, other.bridge, other.signature,
                        other.sounds, other.error, other.external, other.interruptions, other.active_env,
                        other.interrupt_tick, other.interrupt_count, other.kof, other.flg, other.score_tick, other.selected_song, other.admitted_roots, other.instruments, other.source2, other.source3, other.prefix_ids, other.prefix_counts, other.sample_counts, other.sample_loops, other.sample_starts, other.profile_env, other.profile_pitch, other.voice_profiles, other.voice_tuning, other.voice_env, other.voice_pitch, other.end_windows, other.natural_ends, other.end_env, other.end_gates, other.end_sources, other.endx, other.end_kof, other.sample_modes, other.sample_headers, other.atomic_clears, other.atomic_invalidated, other.atomic_phase, other.score_hash, other.asset_hash, other.blocked_frames, other.blocked_nonzero, other.recovery_rejections, other.suppressed_sound, other.recovery_blocked, other.host_stack, other.clear_events, other.clear_verified, other.clear_invalidated, other.state);
    }
};
struct Restores { unsigned count = 0, phases = 0, commands = 0, roots = 0, instruments = 0, ends = 0, rejections = 0, suppressed = 0, clears = 0; };
Result run(Host& host, std::uint64_t target, Restores* restores = nullptr) {
    Result result;
#ifdef GBB_SCORE_ACOUSTIC_PROBE
    result.acoustic.rate=host.sample_rate();
    unsigned previous_acoustic_checkpoint=0;
#endif
    unsigned steps = 0, previous_atomic = 0, previous_rejections = 0,
             previous_suppressed = 0, previous_clears = 0;
    std::uint64_t previous = ~std::uint64_t{0};
#ifdef GBB_SCORE_TUNING_PROBE
    unsigned previous_tuning_seen = 0;
#endif
#ifdef GBB_SCORE_ADSR_PROBE
    unsigned previous_envelope_checkpoints = 0;
#endif
    Host::StereoSample sample;
    while (host.cpu().timing().clocks() < target) {
        require(host.step(), "whole-host execution fault");
        while (host.pop_sample(sample)) {
            ++result.frames;
#ifdef GBB_SCORE_ACOUSTIC_PROBE
#ifdef GBB_SCORE_COMBINED_PITCH_PROBE
            result.acoustic.sample(result.frames,sample,host.apu_half_clocks());
#else
            result.acoustic.sample(result.frames,sample);
#endif
#endif
            if (host.cpu().debug_wram_byte(0x56)) {
                ++result.blocked_frames;
                if (sample.left || sample.right) ++result.blocked_nonzero;
            }
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
#ifdef GBB_SCORE_ADSR_PROBE
        result.envelopes.capture(host);
#ifdef GBB_SCORE_ACOUSTIC_PROBE
#ifdef GBB_SCORE_COMBINED_PITCH_PROBE
        result.acoustic.sync(result.envelopes,result.frames,host.apu_half_clocks());
#else
        result.acoustic.sync(result.envelopes);
#endif
#endif
#endif
        // Observe live source IDs only while this diagnostic bridge renders.
        if (host.debug_spc_ram_byte(0xd2) == 2) {
#ifdef GBB_SCORE_TUNING_PROBE
            // Settled actual DSP registers ten ticks into each authored 16-tick
            // note, before its gate ends. No inferred pitch or timestamp edits.
            const unsigned tuning_tick = host.debug_spc_ram_byte(0x10) |
                (unsigned(host.debug_spc_ram_byte(0x11)) << 8);
            if (host.debug_spc_ram_byte(0xdb) == 1 && tuning_tick < 208 && tuning_tick % 16 == 10) {
                const unsigned index = tuning_tick / 16;
                result.tuning_seen |= 1U << index;
                for (unsigned voice = 2; voice <= 3; ++voice)
                    result.tuning_pitches[2*index + voice-2] = host.debug_dsp_register(16*voice+2) |
                        (unsigned(host.debug_dsp_register(16*voice+3)) << 8);
            }
#endif
            // First score, first note: observe a stable tick before its gate ends.
            if (host.debug_spc_ram_byte(0xdb) == 1 &&
                host.debug_spc_ram_byte(0x10) == 10 && host.debug_spc_ram_byte(0x11) == 0) {
                for (unsigned voice = 2; voice <= 3; ++voice) {
                    const auto index = voice - 2;
                    result.profile_env[index] = std::max(result.profile_env[index],
                        unsigned(host.debug_dsp_register(voice * 16 + 8)));
                    result.profile_pitch[index] = host.debug_dsp_register(voice * 16 + 2) |
                        (host.debug_dsp_register(voice * 16 + 3) << 8);
                }
            }
            // Observe settled first/second notes before their gates expire.
            const auto tick = host.debug_spc_ram_byte(0x10);
            if (host.debug_spc_ram_byte(0xdb) == 1 && host.debug_spc_ram_byte(0x11) == 0 &&
                (tick == 10 || tick == 26)) {
                const unsigned window = tick == 10 ? 0 : 1;
                result.end_windows |= 1U << window;
                result.endx[window] |= host.debug_dsp_register(0x7c) & 12;
                result.end_kof[window] |= host.debug_dsp_register(0x5c) & 12;
                for (unsigned voice = 2; voice <= 3; ++voice) {
                    const auto index = window * 2 + voice - 2;
                    const auto env = unsigned(host.debug_dsp_register(voice * 16 + 8));
                    const auto gate = unsigned(host.debug_spc_ram_byte(0x72 + voice - 2));
                    const auto source = unsigned(host.debug_dsp_register(voice * 16 + 4));
                    result.end_env[index] = std::max(result.end_env[index], env);
                    result.end_gates[index] = std::max(result.end_gates[index], gate);
                    result.end_sources[index] = source;
                    if ((source == 2 || source == 3) && env == 0 && gate > 0 &&
                        (host.debug_dsp_register(0x7c) & (1U << voice)) != 0 &&
                        (host.debug_dsp_register(0x5c) & (1U << voice)) == 0 &&
                        host.debug_spc_ram_byte(0x5013 + 4 * (source - 2)) == 1)
                        result.natural_ends |= 1U << ((voice - 2) * 2 + source - 2);
                }
            }
            for (unsigned voice = 2; voice <= 3; ++voice) {
                const auto source = host.debug_dsp_register(voice * 16 + 4);
                if ((source == 2 || source == 3) && host.debug_dsp_register(voice * 16 + 8) > 0)
                    result.instruments |= 1U << ((voice - 2) * 2 + source - 2);
            }
        }
        // D4 archive phase is published only after both bounded regions clear.
        // Observe once per generation, without changing RAM or execution.
        const auto atomic_phase = host.debug_spc_ram_byte(0x0504);
        const auto generation = host.cpu().debug_wram_byte(0x26);
        if (atomic_phase == 0xa4 && generation < 3 &&
            (result.atomic_clears & (1U << generation)) == 0) {
            bool zero = true;
            for (unsigned address = 0x2b00; zero && address < 0x3300; ++address)
                zero = host.debug_spc_ram_byte(address) == 0;
            for (unsigned address = 0x5000; zero && address < 0x50c0; ++address)
                zero = host.debug_spc_ram_byte(address) == 0;
            if (zero) {
                result.atomic_clears |= 1U << generation;
                if (host.debug_spc_ram_byte(0xd2) == 0 &&
                    host.debug_spc_ram_byte(0xdc) == 0 && host.debug_spc_ram_byte(0xdb) == 0)
                    result.atomic_invalidated |= 1U << generation;
            }
        }
        if (atomic_phase == 0xa4 && previous_atomic != 0xa4) {
            require(result.clear_events < 8, "clear-event bound");
            const unsigned bit = 1U << result.clear_events++;
            bool zero = true;
            for (unsigned address = 0x2b00; zero && address < 0x3300; ++address)
                zero = host.debug_spc_ram_byte(address) == 0;
            for (unsigned address = 0x5000; zero && address < 0x50c0; ++address)
                zero = host.debug_spc_ram_byte(address) == 0;
            if (zero) result.clear_verified |= bit;
            if (zero && host.debug_spc_ram_byte(0xd2) == 0 &&
                host.debug_spc_ram_byte(0xdc) == 0 && host.debug_spc_ram_byte(0xdb) == 0 &&
                (host.debug_dsp_register(0x6c) & 0xe0) == 0xe0)
                result.clear_invalidated |= bit;
        }
        previous_atomic = atomic_phase;
        if (restores) {
            const auto rejections = host.cpu().debug_wram_byte(0x57);
            const auto suppressed = host.cpu().debug_wram_byte(0x59);
            auto status = host.cpu().debug_wram_byte(0x20);
            auto bridge = host.cpu().debug_wram_byte(0x31);
            unsigned commands = host.debug_spc_ram_byte(0xd3);
            const auto admitted = host.debug_spc_ram_byte(0xdc);
            // Save around physical sample-end/KON and envelope-zero transitions,
            // in addition to settled natural ends observed before native gates.
            const unsigned sample_phase = host.debug_spc_ram_byte(0xd2) == 2
                ? ((host.debug_dsp_register(0x7c) >> 2) & 3U) |
                  (unsigned(host.debug_dsp_register(0x28) == 0) << 2) |
                  (unsigned(host.debug_dsp_register(0x38) == 0) << 3)
                : 0;
            std::uint64_t phase = status | (bridge << 8) | (commands << 16) |
                                  (std::uint64_t(host.debug_spc_ram_byte(0xd6)) << 24) |
                                  (std::uint64_t(admitted) << 32) |
                                  (std::uint64_t(host.debug_spc_ram_byte(0xdb)) << 40) |
                                  (std::uint64_t(result.instruments) << 48) |
                                  (std::uint64_t(result.natural_ends) << 52) |
                                  (std::uint64_t(sample_phase) << 56) |
                                  (std::uint64_t(atomic_phase == 0xa4 ? 1 : atomic_phase == 0xa5 ? 2 : 0) << 60) |
                                  (std::uint64_t(host.cpu().debug_wram_byte(0x56) != 0) << 62) |
                                  (std::uint64_t(suppressed & 1U) << 63);
            if (++steps % 100003 == 0 || phase != previous ||
                rejections != previous_rejections || suppressed != previous_suppressed ||
                result.clear_events != previous_clears
#ifdef GBB_SCORE_TUNING_PROBE
                || result.tuning_seen != previous_tuning_seen
#endif
#ifdef GBB_SCORE_ADSR_PROBE
                || result.envelopes.checkpoints != previous_envelope_checkpoints
#ifdef GBB_SCORE_ACOUSTIC_PROBE
                || result.acoustic.checkpoint() != previous_acoustic_checkpoint
#endif
#endif
                ) {
                require(++restores->count <= 4096, "snapshot bound");
                auto state = host.save_state();
                require(host.load_state(state), "mid-execution restore");
                require(host.save_state() == state, "exact state restoration");
                restores->rejections = std::max(restores->rejections, unsigned(rejections));
                restores->suppressed = std::max(restores->suppressed, unsigned(suppressed));
                restores->clears |= result.clear_verified;
                restores->commands |= commands;
                restores->instruments |= result.instruments;
                restores->ends |= result.natural_ends;
                if (admitted > 0 && admitted <= 3) restores->roots |= (1U << admitted) - 1;
                if (status == 4) restores->phases |= 1; // IPL upload
                if (status == 2) restores->phases |= 2; // external restart
                if (bridge == 1) restores->phases |= 4; // validated, silent
                if (bridge == 3) restores->phases |= 8; // rendering
                if (bridge == 2) restores->phases |= 16; // completed
            }
#ifdef GBB_SCORE_TUNING_PROBE
            previous_tuning_seen = result.tuning_seen;
#endif
#ifdef GBB_SCORE_ADSR_PROBE
            previous_envelope_checkpoints = result.envelopes.checkpoints;
#endif
#ifdef GBB_SCORE_ACOUSTIC_PROBE
            previous_acoustic_checkpoint=result.acoustic.checkpoint();
#endif
            previous = phase;
            previous_rejections = rejections;
            previous_suppressed = suppressed;
            previous_clears = result.clear_events;
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
    for (unsigned i = 0; i < 2; ++i) {
        result.voice_tuning[i] = host.debug_spc_ram_byte(0xec + i);
        result.voice_pitch[i] = host.debug_dsp_register(0x22 + 16*i) |
            (host.debug_dsp_register(0x23 + 16*i) << 8);
        result.voice_env[i] = host.debug_dsp_register(0x28 + 16*i);
        for (unsigned field = 0; field < 4; ++field)
            result.voice_profiles[4*i + field] = host.debug_dsp_register(0x24 + 16*i + field);
    }
    result.source2 = host.debug_dsp_register(0x24);
    result.source3 = host.debug_dsp_register(0x34);
    constexpr std::array<unsigned, 9> ids{0x6000, 0x6100, 0x6200, 0x6002,
                                          0x6020, 0x6120, 0x6220, 0x6022, 0x6024};
    constexpr std::array<unsigned, 9> counts{0x4a00, 0x4a02, 0x4a20, 0x4a22,
                                             0x4a60, 0x4a62, 0x4a80, 0x4a82, 0x4a24};
    for (std::size_t i = 0; i < ids.size(); ++i) {
        result.prefix_ids[i] = host.debug_spc_ram_byte(ids[i]);
        result.prefix_counts[i] = host.debug_spc_ram_byte(counts[i]);
    }
    for (unsigned i = 0; i < 2; ++i) {
        for (unsigned block = 0; block < 4; ++block)
            result.sample_headers[4*i + block] = host.debug_spc_ram_byte(0x5040 + 64*i + 9*block);
        result.sample_starts[i] = host.debug_spc_ram_byte(0x5008 + 4 * i) |
                                 (unsigned(host.debug_spc_ram_byte(0x5009 + 4 * i)) << 8);
        result.sample_modes[i] = host.debug_spc_ram_byte(0x5013 + 4 * i);
        result.sample_counts[i] = host.debug_spc_ram_byte(0x5010 + i);
        result.sample_loops[i] = host.debug_spc_ram_byte(0x500a + 4 * i) |
                                (unsigned(host.debug_spc_ram_byte(0x500b + 4 * i)) << 8);
    }
    result.recovery_rejections = host.cpu().debug_wram_byte(0x57);
    result.suppressed_sound = host.cpu().debug_wram_byte(0x59);
    result.recovery_blocked = host.cpu().debug_wram_byte(0x56);
    result.host_stack = host.cpu().registers().s;
    result.atomic_phase = host.debug_spc_ram_byte(0x0504);
    auto region_hash = [&](unsigned begin, unsigned end) {
        std::uint64_t hash = 14695981039346656037ULL;
        for (unsigned address = begin; address < end; ++address) {
            hash ^= host.debug_spc_ram_byte(address);
            hash *= 1099511628211ULL;
        }
        return hash;
    };
    result.score_hash = region_hash(0x2b00, 0x3300);
    result.asset_hash = region_hash(0x5000, 0x50c0);
#ifdef GBB_SCORE_ACOUSTIC_PROBE
#ifdef GBB_SCORE_COMBINED_PITCH_PROBE
    result.acoustic.gb_samples=host.gb_samples_captured();
    result.acoustic.clipped_samples=host.clipped_samples();
#endif
    result.acoustic.validate(result.envelopes);
#endif
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
#if defined(GBB_SCORE_ACOUSTIC_PROBE) && !defined(GBB_SCORE_COMBINED_PITCH_PROBE)
        require(mode != "combined", "native pitch capture requires native/scalar output");
#endif
#ifdef GBB_SCORE_COMBINED_PITCH_PROBE
        require(mode == "combined" || mode == "scalar", "combined pitch mode");
        config.combined_audio = true;
#else
        config.combined_audio = mode == "combined";
#endif
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
                  << ",\"restore_instruments\":" << restores.instruments << ",\"restore_ends\":" << restores.ends
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
                  << ",\"instruments\":" << result.instruments
                  << ",\"source2\":" << result.source2 << ",\"source3\":" << result.source3;
#ifdef GBB_SCORE_ACOUSTIC_PROBE
        result.acoustic.emit();
#endif
        std::cout << ",\"recovery_rejections\":" << result.recovery_rejections
                  << ",\"suppressed_sound\":" << result.suppressed_sound
                  << ",\"recovery_blocked\":" << result.recovery_blocked
                  << ",\"host_stack\":" << result.host_stack
                  << ",\"clear_events\":" << result.clear_events
                  << ",\"clear_verified\":" << result.clear_verified
                  << ",\"clear_invalidated\":" << result.clear_invalidated
                  << ",\"blocked_frames\":" << result.blocked_frames
                  << ",\"blocked_nonzero\":" << result.blocked_nonzero
                  << ",\"restore_rejections\":" << restores.rejections
                  << ",\"restore_suppressed\":" << restores.suppressed
                  << ",\"restore_clears\":" << restores.clears;
        std::cout << ",\"atomic_clears\":" << result.atomic_clears
                  << ",\"atomic_invalidated\":" << result.atomic_invalidated
                  << ",\"atomic_phase\":" << result.atomic_phase
                  << ",\"score_hash\":" << result.score_hash
                  << ",\"asset_hash\":" << result.asset_hash;
#ifdef GBB_SCORE_TUNING_PROBE
        std::cout << ",\"tuning_seen\":" << result.tuning_seen << ",\"tuning_pitches\":[";
        for (std::size_t i = 0; i < result.tuning_pitches.size(); ++i)
            std::cout << (i ? "," : "") << result.tuning_pitches[i];
        std::cout << ']';
#endif
#ifdef GBB_SCORE_ADSR_PROBE
        result.envelopes.print();
#endif
        std::cout << ",\"sample_starts\":[" << result.sample_starts[0] << ',' << result.sample_starts[1] << ']';
        std::cout << ",\"sample_headers\":[";
        for (std::size_t i = 0; i < result.sample_headers.size(); ++i)
            std::cout << (i ? "," : "") << result.sample_headers[i];
        std::cout << ']';
        std::cout << ",\"end_windows\":" << result.end_windows << ",\"natural_ends\":" << result.natural_ends
                  << ",\"endx\":[" << result.endx[0] << ',' << result.endx[1] << ']'
                  << ",\"end_kof\":[" << result.end_kof[0] << ',' << result.end_kof[1] << ']'
                  << ",\"sample_modes\":[" << result.sample_modes[0] << ',' << result.sample_modes[1] << ']';
        std::cout << ",\"end_env\":[";
        for (std::size_t i = 0; i < result.end_env.size(); ++i)
            std::cout << (i ? "," : "") << result.end_env[i];
        std::cout << "],\"end_gates\":[";
        for (std::size_t i = 0; i < result.end_gates.size(); ++i)
            std::cout << (i ? "," : "") << result.end_gates[i];
        std::cout << "],\"end_sources\":[";
        for (std::size_t i = 0; i < result.end_sources.size(); ++i)
            std::cout << (i ? "," : "") << result.end_sources[i];
        std::cout << ']';
        std::cout << ",\"voice_profiles\":[";
        for (std::size_t i = 0; i < result.voice_profiles.size(); ++i)
            std::cout << (i ? "," : "") << result.voice_profiles[i];
        std::cout << "],\"voice_tuning\":[" << result.voice_tuning[0] << ',' << result.voice_tuning[1] << ']'
                  << ",\"voice_env\":[" << result.voice_env[0] << ',' << result.voice_env[1] << ']'
                  << ",\"voice_pitch\":[" << result.voice_pitch[0] << ',' << result.voice_pitch[1] << ']';
        std::cout << ",\"prefix_ids\":[";
        for (std::size_t i = 0; i < result.prefix_ids.size(); ++i)
            std::cout << (i ? "," : "") << result.prefix_ids[i];
        std::cout << "],\"prefix_counts\":[";
        for (std::size_t i = 0; i < result.prefix_counts.size(); ++i)
            std::cout << (i ? "," : "") << result.prefix_counts[i];
        std::cout << "],\"sample_counts\":[" << result.sample_counts[0] << ',' << result.sample_counts[1]
                  << "],\"sample_loops\":[" << result.sample_loops[0] << ',' << result.sample_loops[1] << ']'
                  << ",\"profile_env\":[" << result.profile_env[0] << ',' << result.profile_env[1] << ']'
                  << ",\"profile_pitch\":[" << result.profile_pitch[0] << ',' << result.profile_pitch[1] << ']'
                  << ",\"pcm\":{\"frames\":" << result.frames << ",\"nonzero_frames\":" << result.nonzero
                  << ",\"fnv1a64\":" << result.hash << "}}\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
