// SPDX-License-Identifier: GPL-3.0-or-later
#include "gameboy/snes_apu_audio_engine.hpp"

#include <array>
#include <algorithm>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>
#include <tuple>

namespace {
using Engine = gameboy::SnesApuAudioEngine;
#ifdef GBB_SCORE_LIST_PROBE
constexpr auto schema = "gbb-spc-score-list-v1";
constexpr unsigned log_bound = 320, second_pattern_offset = 64;
constexpr unsigned log_base = 0x4000, bank_input_bound = 2049, upper_guard = 0x4a00;
#elif defined(GBB_SCORE_BANK_PROBE)
constexpr auto schema = "gbb-spc-score-bank-v1";
constexpr unsigned log_bound = 160, second_pattern_offset = 64;
constexpr unsigned log_base = 0x4000, bank_input_bound = 2049, upper_guard = 0x4900;
#elif defined(GBB_SCORE_ENVELOPE_PROBE)
constexpr auto schema = "gbb-spc-score-envelope-v1";
constexpr unsigned log_bound = 160, second_pattern_offset = 64;
#elif defined(GBB_SCORE_MIX_PROBE)
constexpr auto schema = "gbb-spc-score-mix-v1";
constexpr unsigned log_bound = 160, second_pattern_offset = 64;
#elif defined(GBB_SCORE_CHROMATIC_PROBE)
constexpr auto schema = "gbb-spc-score-chromatic-v1";
constexpr unsigned log_bound = 160, second_pattern_offset = 64;
#elif defined(GBB_SCORE_CALLGATE_PROBE)
constexpr auto schema = "gbb-spc-score-callgate-v1";
constexpr unsigned log_bound = 160, second_pattern_offset = 64;
#else
constexpr auto schema = "gbb-spc-score-polygate-v1";
constexpr unsigned log_bound = 80, second_pattern_offset = 32;
#endif
#ifndef GBB_SCORE_BANK_PROBE
constexpr unsigned log_base = 0x3000, bank_input_bound = 255;
#endif
void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}
std::vector<unsigned char> read(const char* path, unsigned bound) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    require(input && input.tellg() > 0 && input.tellg() <= bound, "input outside byte bound");
    std::vector<unsigned char> data(static_cast<std::size_t>(input.tellg()));
    input.seekg(0);
    require(bool(input.read(reinterpret_cast<char*>(data.data()), data.size())), "input read failed");
    return data;
}
struct Audio {
    std::uint64_t hash = 14695981039346656037ULL;
    unsigned frames = 0, nonzero = 0, peak = 0, quiet_tail = 0;
    unsigned left_nonzero = 0, right_nonzero = 0, left_peak = 0, right_peak = 0;
    bool stereo_equal = true;
    bool operator==(const Audio& other) const {
        return hash == other.hash && frames == other.frames && nonzero == other.nonzero &&
               peak == other.peak && quiet_tail == other.quiet_tail && left_nonzero == other.left_nonzero &&
               right_nonzero == other.right_nonzero && left_peak == other.left_peak &&
               right_peak == other.right_peak && stereo_equal == other.stereo_equal;
    }
};
void clock(Engine& engine, Audio& audio, Audio* steady = nullptr, unsigned settled = 0,
           std::array<unsigned,2>* settled_frames = nullptr, std::array<unsigned,2>* peer_pcm = nullptr) {
    Engine::StereoSample sample;
    while (engine.pop_sample(sample)) {
        if (steady) {
            ++steady->frames;
            if (sample.left) ++steady->left_nonzero;
            if (sample.right) ++steady->right_nonzero;
        }
        for (unsigned channel = 0; channel < 2; ++channel) {
            if (settled & (4U << channel)) {
                require((channel == 0 ? sample.left : sample.right) == 0, "expired voice did not settle while peer plays");
                ++(*settled_frames)[channel];
                if (channel == 0 ? sample.right != 0 : sample.left != 0) ++(*peer_pcm)[channel];
            }
        }
        audio.stereo_equal = audio.stereo_equal && sample.left == sample.right;
        if (sample.left) ++audio.left_nonzero;
        if (sample.right) ++audio.right_nonzero;
        ++audio.frames;
        require(audio.frames <= 500000, "owned PCM frame bound");
        if (sample.left || sample.right) { ++audio.nonzero; audio.quiet_tail = 0; }
        else ++audio.quiet_tail;
        const auto magnitude = unsigned(sample.left < 0 ? -int(sample.left) : int(sample.left));
        if (magnitude > audio.left_peak) audio.left_peak = magnitude;
        const auto right_magnitude = unsigned(sample.right < 0 ? -int(sample.right) : int(sample.right));
        if (right_magnitude > audio.right_peak) audio.right_peak = right_magnitude;
        audio.peak = audio.left_peak > audio.right_peak ? audio.left_peak : audio.right_peak;
        for (const auto channel : {sample.left, sample.right}) {
            const auto value = static_cast<std::uint16_t>(channel);
            for (unsigned shift : {0U, 8U}) {
                audio.hash ^= (value >> shift) & 255;
                audio.hash *= 1099511628211ULL;
            }
        }
    }
    require(engine.clock_half(), "native renderer CPU stopped");
}
void setup(Engine& engine, const std::vector<unsigned char>& program,
           const std::vector<unsigned char>& track, unsigned tempo) {
    gameboy::SnesApuBus::IplRom entry{};
    entry[0] = 0x5f; entry[1] = 0; entry[2] = 8;
    engine.install_ipl(entry);
    engine.reset();
    for (unsigned i = 0; i < program.size(); ++i) engine.bus().dsp_write_ram(0x0800 + i, program[i]);
#ifdef GBB_SCORE_BANK_PROBE
    // Poison unprovided source bytes and guard the cache/source separation.
    for (unsigned address = 0x2b00; address < 0x4000; ++address) engine.bus().dsp_write_ram(address, 0xa5);
    for (unsigned address = upper_guard; address < upper_guard+0x100; ++address) engine.bus().dsp_write_ram(address, 0xa5);
    engine.bus().dsp_write_ram(0x8e, track.size() >> 8);
#endif
    for (unsigned i = 0; i < track.size(); ++i) engine.bus().dsp_write_ram(0x2b00 + i, track[i]);
    engine.bus().dsp_write_ram(0x12, tempo);
    engine.bus().dsp_write_ram(0x20, track.size());
}
unsigned log_count(Engine& engine) {
    const auto& bus = engine.bus();
#ifdef GBB_SCORE_LIST_PROBE
    return bus.dsp_read_ram(0x28) | bus.dsp_read_ram(0x9b) << 8;
#else
    return bus.dsp_read_ram(0x28);
#endif
}
void checkpoint(Engine& engine) {
    const auto saved = engine.save_state();
    Engine other;
    require(other.load_state(saved), "cross-instance track restore failed");
    Audio first, second;
    for (unsigned i = 0; i < 4103; ++i) { clock(engine, first); clock(other, second); }
    require(first == second, "restored renderer PCM differs");
    require(engine.save_state() == other.save_state(), "native parser restore continuation differs");
    require(engine.load_state(saved) && engine.save_state() == saved, "track rewind failed");
}
struct Edge {
    std::uint64_t half;
    unsigned tick, mask, affected, held, cause;
    std::array<unsigned,2> pending;
    std::array<unsigned, 2> pitches;
    std::array<std::array<unsigned,2>,2> volumes;
    std::array<std::array<unsigned,4>,2> instrument;
    bool operator==(const Edge& other) const {
        return half == other.half && tick == other.tick && mask == other.mask && affected == other.affected && held == other.held && cause == other.cause && pending == other.pending && pitches == other.pitches && volumes == other.volumes && instrument == other.instrument;
    }
};
struct Envelope {
    unsigned voice;
    std::uint64_t on, off = 0;
    unsigned peak = 0, decay_min = 127, off_env = 0, release_steps = 0;
    bool attack_zero = false, release_zero = false;
    bool operator==(const Envelope& other) const {
        return std::tie(voice,on,off,peak,decay_min,off_env,release_steps,attack_zero,release_zero) ==
               std::tie(other.voice,other.on,other.off,other.peak,other.decay_min,other.off_env,other.release_steps,other.attack_zero,other.release_zero);
    }
};
struct Result {
    unsigned status = 0, end_tick = 0, second_tick = 0;
    std::uint64_t completion_half = 0;
    std::vector<unsigned char> events;
    std::vector<unsigned char> articulations;
    std::vector<unsigned> pattern_ticks;
    std::vector<std::array<unsigned,5>> controls;
    std::vector<std::uint64_t> halves;
    std::vector<Edge> keyons, keyoffs;
    std::vector<Envelope> envelopes;
    Audio audio, steady;
    std::array<unsigned,2> peer_checks{}, settled_frames{}, peer_pcm{};
    std::array<unsigned,2> settled_envelopes{};
    bool operator==(const Result& other) const {
        return status == other.status && end_tick == other.end_tick && second_tick == other.second_tick && completion_half == other.completion_half &&
            events == other.events && articulations == other.articulations && pattern_ticks == other.pattern_ticks && controls == other.controls && halves == other.halves && keyons == other.keyons && keyoffs == other.keyoffs && envelopes == other.envelopes &&
            audio == other.audio && steady == other.steady && peer_checks == other.peer_checks && settled_frames == other.settled_frames && peer_pcm == other.peer_pcm && settled_envelopes == other.settled_envelopes;
    }
};
Edge edge(Engine& engine, unsigned mask) {
    const auto& bus = engine.bus();
    return {engine.cpu().half_cycles(), unsigned(bus.dsp_read_ram(0x10) | bus.dsp_read_ram(0x11) << 8), mask, bus.dsp_read_ram(0x70), bus.dsp_register(0x5c), bus.dsp_read_ram(0x7a),
            {bus.dsp_read_ram(0x78), bus.dsp_read_ram(0x79)},
            {unsigned(bus.dsp_register(0x22) | bus.dsp_register(0x23) << 8),
             unsigned(bus.dsp_register(0x32) | bus.dsp_register(0x33) << 8)},
            {{{bus.dsp_register(0x20),bus.dsp_register(0x21)}, {bus.dsp_register(0x30),bus.dsp_register(0x31)}}},
            {{{bus.dsp_register(0x24),bus.dsp_register(0x25),bus.dsp_register(0x26),bus.dsp_register(0x27)},
              {bus.dsp_register(0x34),bus.dsp_register(0x35),bus.dsp_register(0x36),bus.dsp_register(0x37)}}}};
}
Result exercise(Engine& engine, bool restore) {
    Result result;
#ifdef GBB_SCORE_BANK_PROBE
    std::array<unsigned char,2049> source_before{};
    for (unsigned i = 0; i < source_before.size(); ++i) source_before[i] = engine.bus().dsp_read_ram(0x2b00+i);
#endif
    unsigned offset = 0, previous_kon = 0, previous_kof = 0;
#ifdef GBB_SCORE_LIST_PROBE
    std::array<bool,3> log_carry_restored{};
#endif
    std::array<unsigned,2> latest_opcodes{0xc9,0xc9};
    std::uint64_t second_start = 0;
    std::array<std::uint64_t,2> expired_since{};
    std::array<std::uint64_t,2> latest_on{};
    std::array<std::array<unsigned,2>,2> latest_volumes{};
    std::array<unsigned,2> envelope_index{}, previous_env{};
#ifdef GBB_SCORE_ENVELOPE_PROBE
    std::array<std::uint64_t,2> last_release_drop{};
    const auto observe_envelopes = [&]() {
        const auto& bus = engine.bus();
        // Read-only ENVX observations, including while the other voice updates.
        for (unsigned channel = 0; channel < 2; ++channel) if (latest_on[channel]) {
            auto& envelope = result.envelopes[envelope_index[channel]];
            const unsigned env = bus.dsp_register((channel+2)*16+8);
            const auto age = engine.cpu().half_cycles()-envelope.on;
            if (age <= 1024) envelope.attack_zero = envelope.attack_zero || env == 0;
            if (envelope.attack_zero) envelope.peak = std::max(envelope.peak, env);
            if (age > 1024) require(envelope.attack_zero && envelope.peak == 127, "instrument-2 attack failed to reach peak");
            if (age > 1024 && !envelope.off) {
                require(env > 0 && env <= previous_env[channel], "ADSR decay or untouched peer restarted");
                envelope.decay_min = std::min(envelope.decay_min, env);
            } else if (envelope.off && engine.cpu().half_cycles() > envelope.off+128) {
                require(env <= previous_env[channel], "released ADSR envelope increased");
                if (env < previous_env[channel]) {
                    require(previous_env[channel]-env == 1, "ADSR release ENVX step differs");
                    if (last_release_drop[channel]) require(engine.cpu().half_cycles()-last_release_drop[channel] == 128,
                            "ADSR release rate differs");
                    last_release_drop[channel] = engine.cpu().half_cycles();
                    ++envelope.release_steps;
                }
                envelope.release_zero = envelope.release_zero || env == 0;
            }
            previous_env[channel] = env;
        }
    };
#endif
    for (unsigned half = 0; half < 30'000'000; ++half) {
        const bool steady = second_start != 0 && engine.cpu().half_cycles() >= second_start + 20000;
        unsigned settled = 0;
        for (unsigned channel = 0; channel < 2; ++channel) {
            const auto& current = engine.bus();
            if (expired_since[channel] && engine.cpu().half_cycles() >= expired_since[channel]+20000 &&
                current.dsp_read_ram(0x72+channel) == 0 && current.dsp_read_ram(0x72+(1-channel)) > 0 &&
                (current.dsp_register(0x5c) & (4U << channel))) {
#ifdef GBB_SCORE_MIX_PROBE
                const unsigned own = (channel+2)*16, peer = (3-channel)*16;
                if (engine.cpu().half_cycles() >= latest_on[1-channel]+2000) {
                    require(current.dsp_register(own+8) == 0 && current.dsp_register(peer+8)
#ifdef GBB_SCORE_ENVELOPE_PROBE
                            > 0,
#else
                            == 127,
#endif
                            "expired mix envelope or active peer differs");
                    ++result.settled_envelopes[channel];
                    if (current.dsp_register(own) && !current.dsp_register(own+1) &&
                        !current.dsp_register(peer) && current.dsp_register(peer+1)) settled |= 4;
                    if (current.dsp_register(own+1) && !current.dsp_register(own) &&
                        !current.dsp_register(peer+1) && current.dsp_register(peer)) settled |= 8;
                }
#else
                settled |= 4U << channel;
#endif
            }
        }
        clock(engine, result.audio, steady ? &result.steady : nullptr,
              settled, &result.settled_frames, &result.peer_pcm);
        const auto& bus = engine.bus();
        const unsigned kon = bus.dsp_register(0x4c), kof = bus.dsp_register(0x5c);
        require((kon & ~12U) == 0, "polyphonic keyed unexpected voice");
        if (kon && previous_kon == 0) {
            require(bus.dsp_register(0x5d) == 16 && (kof & kon) == 0 && bus.dsp_register(0x6c) == 32 &&
                bus.dsp_register(0x2d) == 0 && bus.dsp_register(0x3d) == 0 && bus.dsp_register(0x4d) == 0,
                "polyphonic global DSP setup differs");
            for (unsigned channel : {2U, 3U}) {
                const unsigned base = channel * 16;
#ifdef GBB_SCORE_MIX_PROBE
                require(bus.dsp_register(base) <= 11 && bus.dsp_register(base+1) <= 11 &&
#else
                require(bus.dsp_register(base) == (channel == 2 ? 80 : 0) &&
                    bus.dsp_register(base+1) == (channel == 3 ? 80 : 0) &&
#endif
#ifdef GBB_SCORE_ENVELOPE_PROBE
                    bus.dsp_register(base+4) == 2 && bus.dsp_register(base+5) == 0x8f &&
                    bus.dsp_register(base+6) == 0x6f && bus.dsp_register(base+7) == 0xb8,
#else
                    bus.dsp_register(base+4) == 0 && bus.dsp_register(base+5) == 0 &&
                    bus.dsp_register(base+6) == 0 && bus.dsp_register(base+7) == 127,
#endif
                    "polyphonic owned source/envelope/routing differs");
            }
            result.keyons.push_back(edge(engine, kon));
            for (unsigned channel = 0; channel < 2; ++channel)
                if (kon & (4U << channel)) {
                    expired_since[channel] = 0;
                    latest_on[channel] = engine.cpu().half_cycles();
#ifdef GBB_SCORE_ENVELOPE_PROBE
                    last_release_drop[channel] = 0;
                    envelope_index[channel] = result.envelopes.size();
                    result.envelopes.push_back({channel+2, latest_on[channel]});
#endif
                    latest_volumes[channel] = {bus.dsp_register((channel+2)*16),bus.dsp_register((channel+2)*16+1)};
                }
        }
        const unsigned asserted = (kof & ~previous_kof) & 12;
        if (asserted && !result.events.empty()) {
            result.keyoffs.push_back(edge(engine, asserted));
#ifdef GBB_SCORE_ENVELOPE_PROBE
            for (unsigned channel = 0; channel < 2; ++channel) if (asserted & (4U << channel)) {
                auto& envelope = result.envelopes[envelope_index[channel]];
                envelope.off = engine.cpu().half_cycles();
                envelope.off_env = bus.dsp_register((channel+2)*16+8);
            }
#endif
            if (bus.dsp_read_ram(0x7a) == 1) {
                for (unsigned channel = 0; channel < 2; ++channel)
                    if (asserted & (4U << channel)) expired_since[channel] = engine.cpu().half_cycles();
            }
            if (restore) checkpoint(engine);
        }
#ifdef GBB_SCORE_ENVELOPE_PROBE
        observe_envelopes();
#endif
        previous_kon = kon;
        previous_kof = kof;
        const auto written = log_count(engine);
#ifdef GBB_SCORE_LIST_PROBE
        const bool committed = bus.dsp_read_ram(0xa1) == 0;
        if (restore && !committed && offset == 255) {
            // Resume inside the record, on both sides of the low-byte carry
            // and during its transient low=0/high=0 count. Publication waits
            // for the complete record in all three saved continuations.
            const int stage = written == 255 ? 0 : written == 0 ? 1 : written == 256 ? 2 : -1;
            if (stage >= 0 && !log_carry_restored[stage]) {
                checkpoint(engine);
                log_carry_restored[stage] = true;
            }
        }
#else
        const bool committed = true;
#endif
        if (committed && written != offset && written % 5 == 0) {
            require(written == offset+5 && written <= log_bound, "polyphonic event log overflow");
            for (unsigned i = offset; i < written; ++i) result.events.push_back(bus.dsp_read_ram(log_base+i));
            result.halves.push_back(engine.cpu().half_cycles());
            result.articulations.push_back(bus.dsp_read_ram(0x7b));
#ifdef GBB_SCORE_MIX_PROBE
            result.controls.push_back({bus.dsp_read_ram(0x89),bus.dsp_read_ram(0x8a),bus.dsp_read_ram(0x8b),bus.dsp_read_ram(0x8c),bus.dsp_read_ram(0x8d)});
#endif
#ifdef GBB_SCORE_LIST_PROBE
            const unsigned pattern = bus.dsp_read_ram(0x9c);
            if (pattern == result.pattern_ticks.size()) {
                require(pattern < 4, "phrase-list pattern overflow");
                result.pattern_ticks.push_back(bus.dsp_read_ram(0x10) | bus.dsp_read_ram(0x11) << 8);
            } else require(pattern+1 == result.pattern_ticks.size(), "phrase-list skipped/repeated pattern entry");
#endif
            offset = written;
            require(result.events[written-1] == 2 || result.events[written-1] == 3, "invalid event channel");
            latest_opcodes[result.events[written-1]-2] = result.events[written-3];
            if (bus.dsp_read_ram(0x30) == second_pattern_offset && second_start == 0) {
                second_start = engine.cpu().half_cycles();
                result.second_tick = bus.dsp_read_ram(0x10) | bus.dsp_read_ram(0x11) << 8;
            }
            if (restore && written % 10 == 0) checkpoint(engine);
        }
        if (restore && (half == 73 || half == 1001 || half == 3001 || half == 5003 || half == 10037 ||
            half == 190001 || half == 230007 || half == 270011)) checkpoint(engine);
        if (bus.dsp_read_ram(0x7a) == 0) {
            for (unsigned channel = 0; channel < 2; ++channel)
                if (bus.dsp_read_ram(0x70) & (4U << channel)) expired_since[channel] = 0;
        }
        const auto status = bus.dsp_read_ram(0x14);
        const auto affected = bus.dsp_read_ram(0x70);
        if (status == 1 && !result.keyons.empty() && engine.cpu().half_cycles() > result.keyons.front().half + 2000 &&
            (affected == 4 || affected == 8)) {
            const unsigned peer = affected == 4 ? 1 : 0;
            if (latest_opcodes[peer] != 0xc9 && bus.dsp_read_ram(0x72+peer) > 0) {
                require((kof & (4U << peer)) == 0, "single-track update keyed off its peer");
#ifdef GBB_SCORE_ENVELOPE_PROBE
                require(bus.dsp_register((peer+2)*16+8) > 0, "single-track update cut peer envelope");
#else
                require(bus.dsp_register((peer+2)*16+8) == 127, "single-track update restarted peer envelope");
#endif
#ifdef GBB_SCORE_MIX_PROBE
                require(latest_volumes[peer] == std::array<unsigned,2>{bus.dsp_register((peer+2)*16),bus.dsp_register((peer+2)*16+1)},
                        "single-track mix update changed peer volumes");
#endif
                ++result.peer_checks[peer];
            }
        }
        if (status == 2 || status == 0xe1 || status == 0xe2) {
            result.status = status;
            result.end_tick = bus.dsp_read_ram(0x10) | bus.dsp_read_ram(0x11) << 8;
            result.completion_half = engine.cpu().half_cycles();
            for (unsigned i = 0; i < 40000; ++i) {
                clock(engine, result.audio);
#ifdef GBB_SCORE_ENVELOPE_PROBE
                observe_envelopes();
#endif
            }
            require(bus.dsp_read_ram(0x14) == status && log_count(engine) == offset,
                    "halted polyphonic changed state");
#ifdef GBB_SCORE_LIST_PROBE
            if (restore && offset > 255)
                require(std::all_of(log_carry_restored.begin(), log_carry_restored.end(), [](bool value) { return value; }),
                        "phrase-list log carry restore stages missing");
#endif
            require(bus.dsp_register(0x6c) == (status == 2 ? 32 : 224), "polyphonic DSP flags differ");
            require(result.audio.quiet_tail >= 64, "polyphonic failed to settle to silence");
            if (status == 2) require(bus.dsp_register(0x5c) == 12 && bus.dsp_register(0x4c) == 0,
                    "polyphonic failed to key off both voices");
            for (unsigned port = 0; port < 4; ++port)
                require(bus.host_read_port(port) == 0, "polyphonic advertised mailbox readiness");
            if (status != 2) require(result.events.empty() && result.keyons.empty() && result.keyoffs.empty() &&
                result.end_tick == 0 && result.audio.nonzero == 0, "invalid bank rendered audio");
#ifdef GBB_SCORE_BANK_PROBE
            for (unsigned i = 0; i < source_before.size(); ++i)
                require(engine.bus().dsp_read_ram(0x2b00+i) == source_before[i], "native cache overwrote score source");
            for (unsigned address = 0x3301; address < 0x4000; ++address)
                require(engine.bus().dsp_read_ram(address) == 0xa5, "native score touched lower cache guard");
            for (unsigned address = upper_guard; address < upper_guard+0x100; ++address)
                require(engine.bus().dsp_read_ram(address) == 0xa5, "native score touched upper cache guard");
#endif
            return result;
        }
    }
    throw std::runtime_error("polyphonic exceeded physical-half bound");
}
void edges(const std::vector<Edge>& values) {
    std::cout << '[';
    for (unsigned i = 0; i < values.size(); ++i) {
        if (i) std::cout << ',';
        const auto& value = values[i];
        std::cout << "{\"half_cycle\":" << value.half << ",\"tick\":" << value.tick
            << ",\"mask\":" << value.mask << ",\"affected_mask\":" << value.affected << ",\"held_mask\":" << value.held << ",\"cause\":" << value.cause << ",\"pending_pulses\":[" << value.pending[0] << "," << value.pending[1] << "]" << ",\"pitches\":[" << value.pitches[0] << ',' << value.pitches[1] << "]";
#ifdef GBB_SCORE_MIX_PROBE
        std::cout << ",\"volumes\":[[" << value.volumes[0][0] << ',' << value.volumes[0][1] << "],[" << value.volumes[1][0] << ',' << value.volumes[1][1] << "]]";
#endif
#ifdef GBB_SCORE_ENVELOPE_PROBE
        std::cout << ",\"instrument_setup\":[";
        for (unsigned voice = 0; voice < 2; ++voice) {
            if (voice) std::cout << ',';
            std::cout << '[';
            for (unsigned field = 0; field < 4; ++field) {
                if (field) std::cout << ',';
                std::cout << value.instrument[voice][field];
            }
            std::cout << ']';
        }
        std::cout << ']';
#endif
        std::cout << '}';
    }
    std::cout << ']';
}
}
int main(int argc, char** argv) {
    try {
        require(argc == 4, "usage: score_polygate_probe owned-program.bin owned-bank.bin tempo");
        const auto program = read(argv[1], 4096), bank = read(argv[2], bank_input_bound);
        const auto tempo = std::stoul(argv[3]);
        require(tempo <= 255, "tempo outside byte bound");
        Engine engine;
        setup(engine, program, bank, tempo);
        const auto expected = exercise(engine, false);
        setup(engine, program, bank, tempo);
        require(exercise(engine, true) == expected, "restore altered polyphonic timeline/PCM");
        setup(engine, program, bank, tempo);
        require(exercise(engine, false) == expected, "reset altered polyphonic timeline/PCM");
        std::cout << "{\"schema\":\"" << schema << "\",\"qualification\":false,\"playback\":false,"
            "\"reset_equal\":true,\"restore_equal\":true,\"tempo\":" << tempo << ",\"status\":" << expected.status
            << ",\"second_pattern_tick\":" << expected.second_tick << ",\"end_tick\":" << expected.end_tick << ",\"completion_half_cycle\":" << expected.completion_half
            << ",\"events\":[";
        for (unsigned i = 0; i < expected.halves.size(); ++i) {
            if (i) std::cout << ',';
            const unsigned base = i*5;
            std::cout << "{\"tick\":" << (expected.events[base] | expected.events[base+1] << 8)
                << ",\"opcode\":" << unsigned(expected.events[base+2]) << ",\"duration\":" << unsigned(expected.events[base+3])
                << ",\"articulation\":" << unsigned(expected.articulations[i])
                << ",\"channel\":" << unsigned(expected.events[base+4]) << ",\"half_cycle\":" << expected.halves[i];
#ifdef GBB_SCORE_MIX_PROBE
            const auto& control = expected.controls[i];
            std::cout << ",\"volumes\":[" << control[0] << ',' << control[1] << "],\"pan\":" << control[2]
                      << ",\"track_volume\":" << control[3] << ",\"song_volume\":" << control[4];
#endif
            std::cout << '}';
        }
#ifdef GBB_SCORE_BANK_PROBE
        std::cout << "],\"source_unmodified\":true,\"cache_guards_equal\":true,\"keyons\":";
#else
        std::cout << "],\"keyons\":";
#endif
        edges(expected.keyons);
        std::cout << ",\"keyoffs\":"; edges(expected.keyoffs);
#ifdef GBB_SCORE_ENVELOPE_PROBE
        std::cout << ",\"envelopes\":[";
        for (unsigned i = 0; i < expected.envelopes.size(); ++i) {
            if (i) std::cout << ',';
            const auto& env = expected.envelopes[i];
            std::cout << "{\"voice\":" << env.voice << ",\"on_half_cycle\":" << env.on << ",\"off_half_cycle\":" << env.off
                << ",\"peak\":" << env.peak << ",\"decay_min\":" << env.decay_min << ",\"off_env\":" << env.off_env
                << ",\"release_steps\":" << env.release_steps << ",\"attack_zero\":" << (env.attack_zero ? "true" : "false")
                << ",\"release_zero\":" << (env.release_zero ? "true" : "false") << '}';
        }
        std::cout << ']';
#endif
#ifdef GBB_SCORE_LIST_PROBE
        std::cout << ",\"pattern_ticks\":[";
        for (unsigned i = 0; i < expected.pattern_ticks.size(); ++i) {
            if (i) std::cout << ',';
            std::cout << expected.pattern_ticks[i];
        }
        std::cout << ']';
#endif
        std::cout << ",\"peer_checks\":[" << expected.peer_checks[0] << "," << expected.peer_checks[1] << "]";
#ifdef GBB_SCORE_MIX_PROBE
        std::cout << ",\"settled_envelope_checks\":[" << expected.settled_envelopes[0] << ',' << expected.settled_envelopes[1] << ']';
#endif
        std::cout << ",\"settled_gate_frames\":[" << expected.settled_frames[0] << "," << expected.settled_frames[1] << "]"
            << ",\"settled_peer_nonzero_frames\":[" << expected.peer_pcm[0] << "," << expected.peer_pcm[1] << "]";
        std::cout << ",\"second_tail_pcm\":{\"frames\":" << expected.steady.frames
            << ",\"left_nonzero_frames\":" << expected.steady.left_nonzero
            << ",\"right_nonzero_frames\":" << expected.steady.right_nonzero << '}'
            << ",\"pcm\":{\"frames\":" << expected.audio.frames << ",\"nonzero_frames\":" << expected.audio.nonzero
            << ",\"peak\":" << expected.audio.peak << ",\"fnv1a64\":" << expected.audio.hash
            << ",\"quiet_tail_frames\":" << expected.audio.quiet_tail
            << ",\"left_nonzero_frames\":" << expected.audio.left_nonzero
            << ",\"right_nonzero_frames\":" << expected.audio.right_nonzero
#ifdef GBB_SCORE_MIX_PROBE
            << ",\"left_peak\":" << expected.audio.left_peak << ",\"right_peak\":" << expected.audio.right_peak
            << ",\"stereo_equal\":" << (expected.audio.stereo_equal ? "true" : "false")
#endif
            << "}}\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
