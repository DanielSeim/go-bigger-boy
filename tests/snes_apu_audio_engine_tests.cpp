#include "gameboy/snes_apu_audio_engine.hpp"
#include "support/snes_apu_firmware_benchmark.hpp"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <limits>
#include <string_view>
#include <cstdlib>
#include <new>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

namespace {
using Engine = gameboy::SnesApuAudioEngine;
using Sample = Engine::StereoSample;
int failures{};
bool watch_allocations{};
unsigned allocations{};
void check(bool value, const char* message) {
    if (!value) { std::cerr << "FAIL: " << message << '\n'; ++failures; }
}
bool same(Sample a, Sample b) { return a.left == b.left && a.right == b.right; }

// Original program: poll the host, read port values, commit a DSP write, then
// acknowledge. Exercises first-half port reads and second-half DSP writes.
gameboy::SnesApuBus::IplRom program() {
    gameboy::SnesApuBus::IplRom image{};
    constexpr std::uint8_t code[]{
        0xe4, 0xf4, 0xf0, 0xfc, 0xe4, 0xf5, 0xc4, 0xf2,
        0xe4, 0xf6, 0xc4, 0xf3, 0x8f, 0, 0xf4, 0x2f, 0xfe};
    std::copy(std::begin(code), std::end(code), image.begin());
    return image;
}

void setup(Engine& e, bool fixture = false) {
    e.install_ipl(program());
    const auto ram = [&](unsigned address, unsigned value) {
        e.bus().dsp_write_ram(static_cast<std::uint16_t>(address), static_cast<std::uint8_t>(value));
        if (fixture) std::cout << "ram " << address << ' ' << value << '\n';
    };
    const auto reg = [&](unsigned address, unsigned value) {
        e.bus().spc_write(0xf2, static_cast<std::uint8_t>(address));
        e.bus().spc_write(0xf3, static_cast<std::uint8_t>(value));
        if (fixture) std::cout << "reg " << address << ' ' << value << '\n';
    };
    // One looping original BRR waveform, eight voices, noise and pitch modulation.
    ram(0x2800, 0); ram(0x2801, 0x80); ram(0x2802, 0); ram(0x2803, 0x80);
    ram(0x8000, 0xab);
    for (unsigned i = 1; i < 9; ++i) ram(0x8000 + i, (0x42 + i * 17) & 255);
    reg(0x6c, 0xe0);
    for (unsigned v = 0; v < 8; ++v) {
        reg(v * 16, 64 + v); reg(v * 16 + 1, 64 - v);
        reg(v * 16 + 2, v * 13); reg(v * 16 + 3, 0x10);
        reg(v * 16 + 7, 0x7f);
    }
    reg(0x5d, 0x28); reg(0x0c, 0x7f); reg(0x1c, 0x7f);
    reg(0x2d, 0xfe); reg(0x3d, 0x80); reg(0x4d, 0xff);
    reg(0x2c, 0x30); reg(0x3c, 0x20); reg(0x0d, 0x20);
    reg(0x7f, 0x40); reg(0x6d, 0x40); reg(0x7d, 1); reg(0x6c, 0x1f);
    e.bus().host_write_port(0, 1);
    e.bus().host_write_port(1, 0x4c);
    e.bus().host_write_port(2, 0xff);
}

std::vector<Sample> run(Engine& e, unsigned halves) {
    std::vector<Sample> output;
    Sample sample{};
    while (e.pop_sample(sample)) output.push_back(sample);
    for (unsigned i = 0; i < halves; ++i) {
        if (!e.clock_half()) { check(false, "supported synthetic half clock"); break; }
        while (e.pop_sample(sample)) output.push_back(sample);
    }
    return output;
}

void restore_tests() {
    Engine e; setup(e);
    for (unsigned offset = 0; offset < 256; ++offset) {
        if (offset == 128) (void)run(e, 4096); // Repeat all phases with live echo/envelopes.
        // Cover all DSP/key phases, both half phases, pending instructions,
        // read latches, stores and FIFO slots. Same and cross-instance restore.
        const auto state = e.save_state();
        const auto expected = run(e, 1024);
        const auto final = e.save_state();
        check(e.load_state(state), "same-instance composite restore");
        const auto repeated = run(e, 1024);
        check(expected.size() == repeated.size() &&
              std::equal(expected.begin(), expected.end(), repeated.begin(), same) &&
              e.save_state() == final, "same-instance CPU/DSP/bus/PCM continuation exact");
        Engine other;
        check(other.load_state(state), "cross-instance composite restore");
        const auto restored = run(other, 1024);
        check(expected.size() == restored.size() &&
              std::equal(expected.begin(), expected.end(), restored.begin(), same) &&
              other.save_state() == final, "cross-instance complete continuation exact");
        check(e.load_state(state) && e.clock_half(), "move snapshot by one half clock");
    }
    const auto audible = run(e, 4096);
    check(std::any_of(audible.begin(), audible.end(), [](Sample s) { return s.left || s.right; }),
          "restored integrated voices produce nonsilent PCM");
}

void direct_clock_equivalence() {
    Engine direct, callback;
    callback.debug_set_direct_dsp_clock_enabled(false);
    callback.debug_set_dsp_phase_dispatch_enabled(false);
    setup(direct); setup(callback);
    // Live voices, echo, noise and pitch modulation, every half-clock phase,
    // destination-owned restore bindings, and a full output FIFO.
    for (unsigned half = 0; half < 40000; ++half) {
        if (half % 137 == 0) {
            direct.bus().host_write_port(0, half & 255);
            callback.bus().host_write_port(0, half & 255);
        }
        const bool a = direct.clock_half(), b = callback.clock_half();
        check(a == b, "direct and callback clocks have identical backpressure");
        if (half % 97 == 0 || !a) {
            check(direct.save_state() == callback.save_state(),
                  "direct DSP clock preserves complete live APU state");
            const auto state = direct.save_state();
            check(direct.load_state(state) && callback.load_state(state),
                  "restore retains both clock driver implementations");
        }
        if (!a) {
            Sample x{}, y{};
            check(direct.pop_sample(x) && callback.pop_sample(y) && same(x,y),
                  "backpressured clock paths drain identical PCM");
        }
    }
    direct.reset(); callback.reset(); setup(direct); setup(callback);
    const auto x = run(direct, 8192), y = run(callback, 8192);
    check(x.size() == y.size() && std::equal(x.begin(),x.end(),y.begin(),same) &&
          direct.save_state() == callback.save_state(),
          "reset reinstalls direct and callback DSP bindings exactly");
}

void integrated_clock_opcode_equivalence() {
    for (unsigned opcode = 0; opcode < 256; ++opcode)
    for (unsigned seed = 0; seed < 2; ++seed) {
        Engine direct, reported;
        reported.debug_set_direct_dsp_clock_enabled(false);
        reported.debug_set_dsp_phase_dispatch_enabled(false);
        gameboy::SnesApuBus::IplRom image{};
        image[0] = static_cast<std::uint8_t>(opcode);
        image[1] = static_cast<std::uint8_t>(0xF4 + seed);
        image[2] = seed ? 0xFF : 0x00;
        for (auto* engine : {&direct, &reported}) {
            engine->install_ipl(image);
            engine->bus().spc_write(0xF1, 0x87);
            engine->bus().spc_write(0xFA, 1);
            engine->bus().spc_write(0xFB, 3);
            engine->bus().spc_write(0xFC, 2);
        }
        for (unsigned half = 0; half < 64; ++half) {
            for (auto* engine : {&direct, &reported})
                engine->bus().host_write_port(half % 4, static_cast<std::uint8_t>(half * 7));
            const bool a = direct.clock_half(), b = reported.clock_half();
            check(a == b && direct.save_state() == reported.save_state(),
                  "direct/callback clocks preserve all-opcode half-clock state and status");
            if (!a) break;
            if (half % 13 == 0) {
                const auto state = direct.save_state();
                check(direct.load_state(state) && reported.load_state(state),
                      "direct/callback scheduler choices survive half-clock restoration");
            }
        }
    }
}

void boundaries() {
    Engine missing;
    check(!missing.clock_half() && missing.status() == Engine::Status::missing_ipl &&
          !missing.cpu().half_cycles(), "missing IPL fails explicitly without executing zero-filled RAM");
    missing.install_ipl(program());
    check(missing.clock_half(), "caller-supplied IPL enables execution");
    Engine e; setup(e);
    const auto advanced = e.run_half_clocks(100000);
    check(advanced > 0 && e.pending_samples() == 512 && e.status() == Engine::Status::buffer_full,
          "bounded FIFO stops CPU and DSP together");
    const auto full = e.save_state();
    check(!e.clock_half() && e.save_state() == full, "backpressure has no repeated bus effects");
    Engine other;
    check(other.load_state(full), "full FIFO snapshot accepted");
    const auto expected = run(e, 4096), restored = run(other, 4096);
    check(expected.size() == restored.size() &&
          std::equal(expected.begin(), expected.end(), restored.begin(), same) &&
          e.save_state() == other.save_state(), "FIFO resume preserves scheduler state");
    const auto good = e.save_state();
    for (const auto length : {std::size_t(0), std::size_t(8), good.size() - 1}) {
        auto bad = good; bad.resize(length);
        check(!e.load_state(bad) && e.save_state() == good, "truncation rejected atomically");
    }
    auto bad = good; bad[7] = 2;
    check(!e.load_state(bad) && e.save_state() == good, "version rejected atomically");
    bad = good; bad.push_back(0);
    check(!e.load_state(bad) && e.save_state() == good, "trailing bytes rejected atomically");
    bad = good; bad.back() = 255;
    check(!e.load_state(bad) && e.save_state() == good, "status rejected atomically");
    e.reset(); setup(e); const auto reset_state = e.save_state();
    const auto baseline = run(e, 8192);
    e.reset(); setup(e); const auto again = run(e, 8192);
    check(baseline.size() == again.size() && std::equal(baseline.begin(), baseline.end(), again.begin(), same),
          "full reset reinstalls DSP routing and produces identical audio");
    e.reset(); setup(e);
    check(reset_state == e.save_state(), "reset clears stale CPU replay and instruction metadata");
    check(std::any_of(baseline.begin(), baseline.end(), [](Sample s) { return s.left || s.right; }),
          "integrated CPU actually starts nonsilent DSP output");
    Engine rendezvous; setup(rendezvous);
    check(rendezvous.advance_to(10000) && rendezvous.cpu().half_cycles() == 10000ULL * 2048000 / 21477273,
          "exact nominal master-clock rendezvous");
    const auto before = rendezvous.cpu().half_cycles();
    check(!rendezvous.advance_to(0) && rendezvous.cpu().half_cycles() == before,
          "backward target rejected without clocks");
    check(!rendezvous.advance_to(std::numeric_limits<std::uint64_t>::max(), 1) &&
          rendezvous.cpu().half_cycles() == before, "overflow target rejected");
    Engine reference_profile; setup(reference_profile);
    check(reference_profile.advance_to(10000, 21477273, 1025280) &&
          reference_profile.cpu().half_cycles() == 10000ULL * 2050560 / 21477273,
          "explicit diagnostic oscillator uses an exact half-clock target");
    check(!reference_profile.advance_to(10000, 21477273, 1025281), "invalid oscillator profile rejected");
    Engine paced, unbuffered; setup(paced); setup(unbuffered);
    constexpr std::uint64_t master_target = 1000000;
    constexpr auto half_target = master_target * 2048000 / 21477273;
    std::vector<Sample> paced_output;
    Sample sample;
    unsigned retries{};
    bool arrived{};
    do {
        arrived = paced.advance_to(master_target);
        check(arrived || paced.status() == Engine::Status::buffer_full, "rendezvous pauses only for capacity");
        while (paced.pop_sample(sample)) paced_output.push_back(sample);
    } while (!arrived && ++retries < 10);
    const auto unbuffered_output = run(unbuffered, static_cast<unsigned>(half_target));
    check(arrived && retries > 0 && paced.cpu().half_cycles() == half_target &&
          paced_output.size() == unbuffered_output.size() &&
          std::equal(paced_output.begin(), paced_output.end(), unbuffered_output.begin(), same) &&
          paced.save_state() == unbuffered.save_state(),
          "repeated absolute target after backpressure neither loses clocks nor duplicates audio");
    Engine fault; auto unsupported = program(); unsupported[0] = 0xff;
    fault.install_ipl(unsupported);
    for (unsigned i = 0; i < 8 && fault.clock_half(); ++i) {}
    check(fault.status() == Engine::Status::unsupported_instruction, "unsupported opcode fails closed");
    const auto stopped = fault.save_state();
    check(!fault.clock_half() && fault.save_state() == stopped, "fault cannot repeatedly advance DSP");
    check(!fault.advance_to(0, 0, 0) && fault.save_state() == stopped,
          "invalid rendezvous must not clear a terminal unsupported-instruction fault");
    Engine stopped_copy;
    check(stopped_copy.load_state(stopped) && !stopped_copy.clock_half(), "fault state remains stopped after restore");
    fault.reset();
    const auto fault_halves = fault.run_half_clocks(8);
    check(fault_halves == fault.cpu().half_cycles() && fault_halves > 0 && fault_halves < 8 &&
          fault.status() == Engine::Status::unsupported_instruction, "run reports terminal fetch clocks accurately");
}

void malformed_cpu_and_realtime() {
    Engine e; setup(e);
    check(e.clock_half(), "suspend first opcode fetch");
    const auto good = e.save_state();
    gameboy::SnesApuBus bus;
    gameboy::SnesDspAudioEngine dsp(bus);
    const auto cpu_offset = dsp.save_state().size(); // Same 8-byte outer header.
    for (const auto [offset, value] : std::array<std::pair<unsigned, unsigned>, 6>{{
            {7, 127}, // CPU clock count no longer agrees with DSP count.
            {20, 0}, // Cycle-level CPU mode must be enabled.
            {25, 'X'}, // Invalid replay access kind.
            {29, 3}, // More than two halves in a physical access.
            {184, 17}, // Replay exceeds fixed capacity.
            {195, 2}, // Invalid boolean encoding.
        }}) {
        auto bad = good; bad[cpu_offset + offset] = static_cast<std::uint8_t>(value);
        check(!e.load_state(bad) && e.save_state() == good, "malformed CPU continuation rejected atomically");
    }
    unsigned writes{};
    e.bus().set_spc_ram_write_observer([](void* context, std::uint16_t, std::uint8_t) noexcept {
        ++*static_cast<unsigned*>(context);
    }, &writes);
    allocations = 0; watch_allocations = true;
    const bool accepted = e.load_state(good);
    const bool callback_free = writes == 0;
    Sample sample{};
    bool advanced = true;
    for (unsigned i = 0; i < 32768 && advanced; ++i) {
        advanced = e.clock_half();
        while (e.pop_sample(sample)) {}
    }
    watch_allocations = false;
    check(accepted && callback_free && advanced && writes != 0 && allocations == 0,
          "composite restore/clock/drain allocate nothing and retain destination observers");
    // Attached external objects use the same bus and CPU, and preserve IPL.
    gameboy::SnesApuBus external_bus;
    external_bus.install_ipl(program());
    gameboy::SnesSpc700 external_cpu(external_bus);
    {
        Engine attached(external_cpu); setup(attached);
        check(attached.load_state(good), "attached scheduler accepts composite state");
        const auto output = run(attached, 4096);
        check(external_cpu.half_cycles() == 4097 && !output.empty(), "attached CPU advances only with scheduler");
    }
    // No dangling callbacks remain after detaching the scheduler.
    external_bus.spc_write(0xf2, 0x4c); external_bus.spc_write(0xf3, 1);
    (void)external_cpu.clock_half();
}

void benchmark() {
    Engine e; setup(e); (void)run(e, 65536);
    constexpr unsigned halves = 2048000 * 2;
    Sample sample{};
    unsigned samples{};
    const auto start = std::chrono::steady_clock::now();
    for (unsigned i = 0; i < halves; ++i) {
        if (!e.clock_half()) { check(false, "benchmark CPU supported"); return; }
        while (e.pop_sample(sample)) ++samples;
    }
    const auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::cout << "Integrated eight-voice APU: " << samples << " stereo samples, "
              << seconds << " seconds, " << 2 / seconds << "x nominal realtime\n";
    check(samples == 64000, "benchmark advances exact native output count");
}

int export_synthetic(bool fixture) {
#ifdef _WIN32
    if (!fixture && _setmode(_fileno(stdout), _O_BINARY) == -1) return 2;
#endif
    Engine e; setup(e, fixture);
    if (fixture) e.bus().set_spc_ram_write_observer([](void* context, std::uint16_t address,
                                                     std::uint8_t value) noexcept {
        auto& engine = *static_cast<Engine*>(context);
        if (address == 0xf3)
            std::cout << "reg " << unsigned(engine.bus().spc_read(0xf2)) << ' ' << unsigned(value) << '\n';
    }, &e);
    for (unsigned i = 0; i < 16384; ++i) {
        // Full-clock DSP work precedes the SPC access at this boundary.
        if (fixture && e.cpu().half_cycles() % 2) std::cout << "clock 1\n";
        if (!e.clock_half()) return 3;
        Sample sample;
        while (e.pop_sample(sample)) if (!fixture) {
            for (auto channel : {sample.left, sample.right}) {
                const auto bits = static_cast<std::uint16_t>(channel);
                const char bytes[]{static_cast<char>(bits), static_cast<char>(bits >> 8)};
                std::cout.write(bytes, 2);
            }
        }
    }
    return std::cout ? 0 : 2;
}
} // namespace

void* operator new(std::size_t size) {
    if (watch_allocations) ++allocations;
    if (auto* p = std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

int main(int argc, char** argv) {
    if (argc == 3 && std::string_view(argv[1]) == "--benchmark-fixture") {
        try {
            Engine engine; setup(engine);
            (void)run(engine, 8192);
            sgb_test::benchmark_apu_firmware(engine.save_state(), 1024000, "synthetic", argv[2]);
            return failures ? 1 : 0;
        } catch (const std::exception& error) {
            std::cerr << error.what() << '\n'; return 1;
        }
    }
    if (argc == 2 && std::string_view(argv[1]) == "--fixture") return export_synthetic(true);
    if (argc == 2 && std::string_view(argv[1]) == "--pcm") return export_synthetic(false);
    if (argc != 1) return 2;
    restore_tests(); direct_clock_equivalence(); integrated_clock_opcode_equivalence();
    boundaries(); malformed_cpu_and_realtime(); benchmark();
    return failures ? 1 : 0;
}
