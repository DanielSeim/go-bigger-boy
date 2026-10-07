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
void clock(Engine& engine) {
    Engine::StereoSample sample;
    while (engine.pop_sample(sample)) {
        require(sample.left == 0 && sample.right == 0, "track experiment must remain silent");
    }
    require(engine.clock_half(), "native track CPU stopped");
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
struct Result {
    unsigned status = 0, end_tick = 0;
    std::vector<unsigned char> events;
    std::vector<std::uint64_t> halves;
    bool operator==(const Result& other) const {
        return status == other.status && end_tick == other.end_tick &&
               events == other.events && halves == other.halves;
    }
};
void checkpoint(Engine& engine) {
    const auto saved = engine.save_state();
    Engine other;
    require(other.load_state(saved), "cross-instance track restore failed");
    for (unsigned i = 0; i < 4103; ++i) { clock(engine); clock(other); }
    require(engine.save_state() == other.save_state(), "native parser restore continuation differs");
    require(engine.load_state(saved) && engine.save_state() == saved, "track rewind failed");
}
Result exercise(Engine& engine, bool restore) {
    Result result;
    unsigned offset = 0;
    for (unsigned half = 0; half < 30'000'000; ++half) {
        clock(engine);
        const auto written = engine.bus().dsp_read_ram(0x28);
        if (written != offset && written % 5 == 0) {
            require(written == offset + 5 && written <= 80, "native event log overflow");
            for (unsigned i = offset; i < written; ++i)
                result.events.push_back(engine.bus().dsp_read_ram(0x3000 + i));
            result.halves.push_back(engine.cpu().half_cycles());
            offset = written;
        }
        if (restore && (half == 73 || half == 4097 || half == 10037 || half == 17005)) checkpoint(engine);
        const auto status = engine.bus().dsp_read_ram(0x14);
        if (status == 2 || status == 0xe1 || status == 0xe2) {
            result.status = status;
            result.end_tick = engine.bus().dsp_read_ram(0x10) | engine.bus().dsp_read_ram(0x11) << 8;
            for (unsigned i = 0; i < 1000; ++i) clock(engine);
            require(engine.bus().dsp_read_ram(0x14) == status &&
                    engine.bus().dsp_read_ram(0x28) == offset, "halted track changed state");
            require(engine.bus().dsp_register(0x6c) == 0xe0, "track changed DSP mute/reset");
            for (unsigned port = 0; port < 4; ++port)
                require(engine.bus().host_read_port(port) == 0, "track advertised mailbox readiness");
            if (status != 2) require(result.events.empty() && result.end_tick == 0, "invalid stream emitted events");
            return result;
        }
    }
    throw std::runtime_error("native track exceeded physical-half bound");
}
}
int main(int argc, char** argv) {
    try {
        require(argc == 4, "usage: score_track_probe owned-program.bin owned-track.bin tempo");
        const auto program = read(argv[1], 1024);
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
        std::cout << "{\"schema\":\"gbb-spc-score-track-v1\",\"qualification\":false,\"playback\":false,"
                     "\"reset_equal\":true,\"restore_equal\":true,\"status\":" << expected.status
                  << ",\"end_tick\":" << expected.end_tick << ",\"events\":[";
        for (unsigned i = 0; i < expected.halves.size(); ++i) {
            const auto base = i * 5;
            if (i) std::cout << ',';
            std::cout << "{\"tick\":" << (expected.events[base] | expected.events[base + 1] << 8)
                      << ",\"opcode\":" << unsigned(expected.events[base + 2])
                      << ",\"duration\":" << unsigned(expected.events[base + 3])
                      << ",\"articulation\":" << unsigned(expected.events[base + 4])
                      << ",\"half_cycle\":" << expected.halves[i] << '}';
        }
        std::cout << "]}\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
