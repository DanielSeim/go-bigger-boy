// SPDX-License-Identifier: GPL-3.0-or-later
#include "gameboy/snes_apu_audio_engine.hpp"

#include <fstream>
#include <iostream>
#include <stdexcept>
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
    bool operator==(const Audio& other) const {
        return hash == other.hash && frames == other.frames && nonzero == other.nonzero &&
               peak == other.peak && quiet_tail == other.quiet_tail;
    }
};
void clock(Engine& engine, Audio& audio) {
    Engine::StereoSample sample;
    while (engine.pop_sample(sample)) {
        require(sample.left == sample.right, "centered owned source must have equal stereo output");
        ++audio.frames;
        require(audio.frames <= 500000, "owned PCM frame bound");
        if (sample.left || sample.right) { ++audio.nonzero; audio.quiet_tail = 0; }
        else ++audio.quiet_tail;
        const auto magnitude = unsigned(sample.left < 0 ? -int(sample.left) : int(sample.left));
        if (magnitude > audio.peak) audio.peak = magnitude;
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
struct KeyOn {
    std::uint64_t half;
    unsigned tick, opcode, pitch, quiet_tail;
    bool operator==(const KeyOn& other) const {
        return half == other.half && tick == other.tick && opcode == other.opcode &&
               pitch == other.pitch && quiet_tail == other.quiet_tail;
    }
};
struct Result {
    unsigned status = 0, end_tick = 0;
    std::vector<unsigned char> events;
    std::vector<std::uint64_t> halves;
    Audio audio;
    std::vector<std::uint64_t> keyoffs;
    std::vector<KeyOn> keyons;
    bool operator==(const Result& other) const {
        return status == other.status && end_tick == other.end_tick &&
               events == other.events && halves == other.halves && audio == other.audio && keyons == other.keyons && keyoffs == other.keyoffs;
    }
};
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
Result exercise(Engine& engine, bool restore) {
    Result result;
    unsigned offset = 0, previous_kon = 0, previous_kof = 0;
    for (unsigned half = 0; half < 30'000'000; ++half) {
        clock(engine, result.audio);
        const unsigned kon = engine.bus().dsp_register(0x4c);
        require(kon == 0 || kon == 4, "renderer keyed unexpected voice");
        if (kon == 4 && previous_kon == 0) {
            const auto& bus = engine.bus();
            require(bus.dsp_register(0x24) == 0 && bus.dsp_register(0x25) == 0 &&
                    bus.dsp_register(0x26) == 0 && bus.dsp_register(0x27) == 127 &&
                    bus.dsp_register(0x20) == 80 && bus.dsp_register(0x21) == 80 &&
                    bus.dsp_register(0x5d) == 16 && bus.dsp_register(0x5c) == 0 &&
                    bus.dsp_register(0x2d) == 0 && bus.dsp_register(0x3d) == 0 &&
                    bus.dsp_register(0x4d) == 0 && bus.dsp_register(0x6c) == 32,
                    "native instrument setup differs");
            result.keyons.push_back({engine.cpu().half_cycles(),
                unsigned(bus.dsp_read_ram(0x10) | bus.dsp_read_ram(0x11) << 8),
                bus.dsp_read_ram(0x26), unsigned(bus.dsp_register(0x22) | bus.dsp_register(0x23) << 8),
                result.audio.quiet_tail});
        }
        previous_kon = kon;
        const unsigned kof = engine.bus().dsp_register(0x5c);
        if ((kof & 4) && !(previous_kof & 4) && !result.keyons.empty()) {
            require(result.keyoffs.size()+1 == result.keyons.size(), "key-off count differs");
            result.keyoffs.push_back(engine.cpu().half_cycles());
            if (restore && result.keyoffs.size() == 1) checkpoint(engine);
        }
        previous_kof = kof;
        const auto written = engine.bus().dsp_read_ram(0x28);
        if (written != offset && written % 5 == 0) {
            require(written == offset + 5 && written <= 80, "native event log overflow");
            for (unsigned i = offset; i < written; ++i)
                result.events.push_back(engine.bus().dsp_read_ram(0x3000 + i));
            result.halves.push_back(engine.cpu().half_cycles());
            offset = written;
        }
        if (restore && (half == 73 || half == 4097 || half == 10037 || half == 17005 || half == 190001 || half == 230007 || half == 270011)) checkpoint(engine);
        const auto status = engine.bus().dsp_read_ram(0x14);
        if (status == 2 || status == 0xe1 || status == 0xe2) {
            result.status = status;
            result.end_tick = engine.bus().dsp_read_ram(0x10) | engine.bus().dsp_read_ram(0x11) << 8;
            for (unsigned i = 0; i < 40000; ++i) clock(engine, result.audio);
            require(engine.bus().dsp_read_ram(0x14) == status &&
                    engine.bus().dsp_read_ram(0x28) == offset, "halted track changed state");
            require(engine.bus().dsp_register(0x6c) == (status == 2 ? 0x20 : 0xe0), "renderer DSP flags differ");
            require(result.audio.quiet_tail >= 64, "renderer did not settle to silence");
            if (status == 2) require(engine.bus().dsp_register(0x5c) == 4, "renderer failed to key off at completion");
            for (unsigned port = 0; port < 4; ++port)
                require(engine.bus().host_read_port(port) == 0, "track advertised mailbox readiness");
            if (status != 2) require(result.events.empty() && result.keyons.empty() && result.end_tick == 0 && result.audio.nonzero == 0, "invalid stream rendered audio");
            return result;
        }
    }
    throw std::runtime_error("native track exceeded physical-half bound");
}
}
int main(int argc, char** argv) {
    try {
        require(argc == 4, "usage: score_gate_probe owned-program.bin owned-track.bin tempo");
        const auto program = read(argv[1], 4096);
        const auto track = read(argv[2], 255);
        const auto tempo = std::stoul(argv[3]);
        require(tempo <= 255, "tempo outside byte bound");
        Engine engine;
        setup(engine, program, track, tempo);
        const auto expected = exercise(engine, false);
        setup(engine, program, track, tempo);
        require(exercise(engine, true) == expected, "restore altered track timeline");
        setup(engine, program, track, tempo);
        require(exercise(engine, false) == expected, "reset altered track timeline");
        std::cout << "{\"schema\":\"gbb-spc-score-gate-v1\",\"qualification\":false,\"playback\":false,"
                     "\"reset_equal\":true,\"restore_equal\":true,\"status\":" << expected.status
                  << ",\"tempo\":" << tempo << ",\"end_tick\":" << expected.end_tick << ",\"events\":[";
        for (unsigned i = 0; i < expected.halves.size(); ++i) {
            const auto base = i * 5;
            if (i) std::cout << ',';
            std::cout << "{\"tick\":" << (expected.events[base] | expected.events[base + 1] << 8)
                      << ",\"opcode\":" << unsigned(expected.events[base + 2])
                      << ",\"duration\":" << unsigned(expected.events[base + 3])
                      << ",\"articulation\":" << unsigned(expected.events[base + 4])
                      << ",\"half_cycle\":" << expected.halves[i] << '}';
        }
        std::cout << "],\"keyons\":[";
        for (unsigned i = 0; i < expected.keyons.size(); ++i) {
            const auto& keyon = expected.keyons[i];
            if (i) std::cout << ',';
            std::cout << "{\"tick\":" << keyon.tick << ",\"opcode\":" << keyon.opcode
                      << ",\"pitch\":" << keyon.pitch << ",\"half_cycle\":" << keyon.half
                      << ",\"quiet_tail_frames\":" << keyon.quiet_tail << '}';
        }
        std::cout << "],\"keyoff_half_cycles\":[";
        for (unsigned i = 0; i < expected.keyoffs.size(); ++i) {
            if (i) std::cout << ',';
            std::cout << expected.keyoffs[i];
        }
        std::cout << "],\"pcm\":{\"frames\":" << expected.audio.frames
                  << ",\"nonzero_frames\":" << expected.audio.nonzero << ",\"peak\":" << expected.audio.peak
                  << ",\"fnv1a64\":" << expected.audio.hash << ",\"quiet_tail_frames\":" << expected.audio.quiet_tail
                  << "}}\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
