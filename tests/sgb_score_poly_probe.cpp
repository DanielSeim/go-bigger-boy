// SPDX-License-Identifier: GPL-3.0-or-later
#include "gameboy/snes_apu_audio_engine.hpp"

#include <array>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using Engine = gameboy::SnesApuAudioEngine;
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
void clock(Engine& engine, Audio& audio, Audio* steady = nullptr) {
    Engine::StereoSample sample;
    while (engine.pop_sample(sample)) {
        if (steady) {
            ++steady->frames;
            if (sample.left) ++steady->left_nonzero;
            if (sample.right) ++steady->right_nonzero;
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
    for (unsigned i = 0; i < track.size(); ++i) engine.bus().dsp_write_ram(0x2b00 + i, track[i]);
    engine.bus().dsp_write_ram(0x12, tempo);
    engine.bus().dsp_write_ram(0x20, track.size());
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
    unsigned tick, mask, affected, held;
    std::array<unsigned, 2> pitches;
    bool operator==(const Edge& other) const {
        return half == other.half && tick == other.tick && mask == other.mask && affected == other.affected && held == other.held && pitches == other.pitches;
    }
};
struct Result {
    unsigned status = 0, end_tick = 0, second_tick = 0;
    std::uint64_t completion_half = 0;
    std::vector<unsigned char> events;
    std::vector<std::uint64_t> halves;
    std::vector<Edge> keyons, keyoffs;
    Audio audio, steady;
    std::array<unsigned,2> peer_checks{};
    bool operator==(const Result& other) const {
        return status == other.status && end_tick == other.end_tick && second_tick == other.second_tick && completion_half == other.completion_half &&
            events == other.events && halves == other.halves && keyons == other.keyons && keyoffs == other.keyoffs &&
            audio == other.audio && steady == other.steady && peer_checks == other.peer_checks;
    }
};
Edge edge(Engine& engine, unsigned mask) {
    const auto& bus = engine.bus();
    return {engine.cpu().half_cycles(), unsigned(bus.dsp_read_ram(0x10) | bus.dsp_read_ram(0x11) << 8), mask, bus.dsp_read_ram(0x70), bus.dsp_register(0x5c),
            {unsigned(bus.dsp_register(0x22) | bus.dsp_register(0x23) << 8),
             unsigned(bus.dsp_register(0x32) | bus.dsp_register(0x33) << 8)}};
}
Result exercise(Engine& engine, bool restore) {
    Result result;
    unsigned offset = 0, previous_kon = 0, previous_kof = 0;
    std::array<unsigned,2> latest_opcodes{0xc9,0xc9};
    std::uint64_t second_start = 0;
    for (unsigned half = 0; half < 30'000'000; ++half) {
        const bool steady = second_start != 0 && engine.cpu().half_cycles() >= second_start + 20000;
        clock(engine, result.audio, steady ? &result.steady : nullptr);
        const auto& bus = engine.bus();
        const unsigned kon = bus.dsp_register(0x4c), kof = bus.dsp_register(0x5c);
        require((kon & ~12U) == 0, "polyphonic keyed unexpected voice");
        if (kon && previous_kon == 0) {
            require(bus.dsp_register(0x5d) == 16 && (kof & kon) == 0 && bus.dsp_register(0x6c) == 32 &&
                bus.dsp_register(0x2d) == 0 && bus.dsp_register(0x3d) == 0 && bus.dsp_register(0x4d) == 0,
                "polyphonic global DSP setup differs");
            for (unsigned channel : {2U, 3U}) {
                const unsigned base = channel * 16;
                require(bus.dsp_register(base) == (channel == 2 ? 80 : 0) &&
                    bus.dsp_register(base+1) == (channel == 3 ? 80 : 0) &&
                    bus.dsp_register(base+4) == 0 && bus.dsp_register(base+5) == 0 &&
                    bus.dsp_register(base+6) == 0 && bus.dsp_register(base+7) == 127,
                    "polyphonic owned source/envelope/routing differs");
            }
            result.keyons.push_back(edge(engine, kon));
        }
        const unsigned asserted = (kof & ~previous_kof) & 12;
        if (asserted && !result.events.empty()) {
            result.keyoffs.push_back(edge(engine, asserted));
            if (restore) checkpoint(engine);
        }
        previous_kon = kon;
        previous_kof = kof;
        const auto written = bus.dsp_read_ram(0x28);
        if (written != offset && written % 5 == 0) {
            require(written == offset+5 && written <= 80, "polyphonic event log overflow");
            for (unsigned i = offset; i < written; ++i) result.events.push_back(bus.dsp_read_ram(0x3000+i));
            result.halves.push_back(engine.cpu().half_cycles());
            offset = written;
            require(result.events[written-1] == 2 || result.events[written-1] == 3, "invalid event channel");
            latest_opcodes[result.events[written-1]-2] = result.events[written-3];
            if (bus.dsp_read_ram(0x30) == 32 && second_start == 0) {
                second_start = engine.cpu().half_cycles();
                result.second_tick = bus.dsp_read_ram(0x10) | bus.dsp_read_ram(0x11) << 8;
            }
            if (restore && written % 10 == 0) checkpoint(engine);
        }
        if (restore && (half == 73 || half == 1001 || half == 3001 || half == 5003 || half == 10037 ||
            half == 190001 || half == 230007 || half == 270011)) checkpoint(engine);
        const auto status = bus.dsp_read_ram(0x14);
        const auto affected = bus.dsp_read_ram(0x70);
        if (status == 1 && !result.keyons.empty() && engine.cpu().half_cycles() > result.keyons.front().half + 2000 &&
            (affected == 4 || affected == 8)) {
            const unsigned peer = affected == 4 ? 1 : 0;
            if (latest_opcodes[peer] != 0xc9) {
                require((kof & (4U << peer)) == 0, "single-track update keyed off its peer");
                require(bus.dsp_register((peer+2)*16+8) == 127, "single-track update restarted peer envelope");
                ++result.peer_checks[peer];
            }
        }
        if (status == 2 || status == 0xe1 || status == 0xe2) {
            result.status = status;
            result.end_tick = bus.dsp_read_ram(0x10) | bus.dsp_read_ram(0x11) << 8;
            result.completion_half = engine.cpu().half_cycles();
            for (unsigned i = 0; i < 40000; ++i) clock(engine, result.audio);
            require(bus.dsp_read_ram(0x14) == status && bus.dsp_read_ram(0x28) == offset,
                    "halted polyphonic changed state");
            require(bus.dsp_register(0x6c) == (status == 2 ? 32 : 224), "polyphonic DSP flags differ");
            require(result.audio.quiet_tail >= 64, "polyphonic failed to settle to silence");
            if (status == 2) require(bus.dsp_register(0x5c) == 12 && bus.dsp_register(0x4c) == 0,
                    "polyphonic failed to key off both voices");
            for (unsigned port = 0; port < 4; ++port)
                require(bus.host_read_port(port) == 0, "polyphonic advertised mailbox readiness");
            if (status != 2) require(result.events.empty() && result.keyons.empty() && result.keyoffs.empty() &&
                result.end_tick == 0 && result.audio.nonzero == 0, "invalid bank rendered audio");
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
            << ",\"mask\":" << value.mask << ",\"affected_mask\":" << value.affected << ",\"held_mask\":" << value.held << ",\"pitches\":[" << value.pitches[0] << ',' << value.pitches[1] << "]}";
    }
    std::cout << ']';
}
}
int main(int argc, char** argv) {
    try {
        require(argc == 4, "usage: score_poly_probe owned-program.bin owned-bank.bin tempo");
        const auto program = read(argv[1], 4096), bank = read(argv[2], 255);
        const auto tempo = std::stoul(argv[3]);
        require(tempo <= 255, "tempo outside byte bound");
        Engine engine;
        setup(engine, program, bank, tempo);
        const auto expected = exercise(engine, false);
        setup(engine, program, bank, tempo);
        require(exercise(engine, true) == expected, "restore altered polyphonic timeline/PCM");
        setup(engine, program, bank, tempo);
        require(exercise(engine, false) == expected, "reset altered polyphonic timeline/PCM");
        std::cout << "{\"schema\":\"gbb-spc-score-poly-v1\",\"qualification\":false,\"playback\":false,"
            "\"reset_equal\":true,\"restore_equal\":true,\"tempo\":" << tempo << ",\"status\":" << expected.status
            << ",\"second_pattern_tick\":" << expected.second_tick << ",\"end_tick\":" << expected.end_tick << ",\"completion_half_cycle\":" << expected.completion_half
            << ",\"events\":[";
        for (unsigned i = 0; i < expected.halves.size(); ++i) {
            if (i) std::cout << ',';
            const unsigned base = i*5;
            std::cout << "{\"tick\":" << (expected.events[base] | expected.events[base+1] << 8)
                << ",\"opcode\":" << unsigned(expected.events[base+2]) << ",\"duration\":" << unsigned(expected.events[base+3])
                << ",\"channel\":" << unsigned(expected.events[base+4]) << ",\"half_cycle\":" << expected.halves[i] << '}';
        }
        std::cout << "],\"keyons\":"; edges(expected.keyons);
        std::cout << ",\"keyoffs\":"; edges(expected.keyoffs);
        std::cout << ",\"peer_checks\":[" << expected.peer_checks[0] << "," << expected.peer_checks[1] << "]";
        std::cout << ",\"second_tail_pcm\":{\"frames\":" << expected.steady.frames
            << ",\"left_nonzero_frames\":" << expected.steady.left_nonzero
            << ",\"right_nonzero_frames\":" << expected.steady.right_nonzero << '}'
            << ",\"pcm\":{\"frames\":" << expected.audio.frames << ",\"nonzero_frames\":" << expected.audio.nonzero
            << ",\"peak\":" << expected.audio.peak << ",\"fnv1a64\":" << expected.audio.hash
            << ",\"quiet_tail_frames\":" << expected.audio.quiet_tail
            << ",\"left_nonzero_frames\":" << expected.audio.left_nonzero
            << ",\"right_nonzero_frames\":" << expected.audio.right_nonzero << "}}\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
