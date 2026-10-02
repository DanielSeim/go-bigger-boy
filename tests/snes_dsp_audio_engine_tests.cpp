#include "gameboy/snes_dsp_audio_engine.hpp"
#include "gameboy/snes_audio_host.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <new>
#include <vector>

namespace {
bool watch_allocations{};
unsigned allocations{};
int failures{};
void check(bool condition, const char* message) {
    if (!condition) { std::cerr << "FAIL: " << message << '\n'; ++failures; }
}
using Engine = gameboy::SnesDspAudioEngine;
using Sample = Engine::StereoSample;
bool same(Sample a, Sample b) { return a.left == b.left && a.right == b.right; }

template<class Renderer> void setup(gameboy::SnesApuBus& bus, Renderer& engine) {
    gameboy::SnesApuBus::IplRom ipl{};
    bus.install_ipl(ipl); // Original zero-byte test IPL, not external firmware.
    bus.spc_write(0xF1, 0x87);
    bus.spc_write(0xFA, 7); bus.spc_write(0xFB, 11); bus.spc_write(0xFC, 3);
    bus.tick(123);
    bus.host_write_port(0, 0x5A); bus.spc_write(0xF5, 0xA5);
    for (unsigned source = 0; source < 2; ++source) {
        const unsigned address = 0x8000 + source * 16;
        for (unsigned word = 0; word < 2; ++word) {
            bus.spc_write(static_cast<std::uint16_t>(0x2800 + source * 4 + word * 2),
                          static_cast<std::uint8_t>(address));
            bus.spc_write(static_cast<std::uint16_t>(0x2801 + source * 4 + word * 2), 0x80);
        }
        bus.spc_write(static_cast<std::uint16_t>(address), 0xAB);
        for (unsigned i = 1; i < 9; ++i)
            bus.spc_write(static_cast<std::uint16_t>(address + i),
                          static_cast<std::uint8_t>(0x42 + i * 17 + source * 3));
    }
    for (unsigned voice = 0; voice < 8; ++voice) {
        const auto reg = [&](unsigned offset, unsigned value) {
            engine.write_dsp(static_cast<std::uint8_t>(voice * 16 + offset),
                             static_cast<std::uint8_t>(value));
        };
        reg(0, 0x40 + voice); reg(1, 0xC0 - voice); reg(2, voice * 17);
        reg(3, 0x10); reg(4, voice % 2); reg(5, voice % 2 ? 0x8F : 0);
        reg(6, 0xE0); reg(7, 0x7F);
    }
    engine.write_dsp(0x5D, 0x28);
    engine.write_dsp(0x0C, 0x7F); engine.write_dsp(0x1C, 0x7F);
    engine.write_dsp(0x2C, 0x30); engine.write_dsp(0x3C, 0xE0);
    engine.write_dsp(0x2D, 0xFE); engine.write_dsp(0x3D, 0x80);
    engine.write_dsp(0x4D, 0xFF); engine.write_dsp(0x0D, 0x20);
    engine.write_dsp(0x7F, 0x40); engine.write_dsp(0x6D, 0x40);
    engine.write_dsp(0x7D, 1); engine.write_dsp(0x6C, 0x1F);
    engine.write_dsp(0x4C, 0xFF);
}

template<class Renderer> void scheduled_writes(Renderer& engine, unsigned i) {
    // Dynamic writes exercise the staggered register/key/echo latches.
    if (i == 5) engine.write_dsp(0x4C, 0x21);
    if (i == 17) engine.write_dsp(0x5C, 0x10);
    if (i == 96) engine.write_dsp(0x6C, 0x7F);
    if (i == 159) engine.write_dsp(0x6C, 0x1F);
    if (i == 250) engine.write_dsp(0x02, 0xE3);
    if (i == 395) engine.write_dsp(0x0D, 0xDF);
    if (i == 409) engine.write_dsp(0x7D, 2);
    if (i == 711) engine.write_dsp(0x5C, 0);
}

std::vector<Sample> continue_audio(Engine& engine, unsigned clocks) {
    std::vector<Sample> samples;
    for (unsigned i = 0; i < clocks; ++i) {
        scheduled_writes(engine, i);
        check(engine.clock(), "continuation clock advances");
        Sample sample;
        while (engine.pop_sample(sample)) samples.push_back(sample);
    }
    return samples;
}

void test_buffering_preserves_raw_renderer_output() {
    gameboy::SnesApuBus buffered_bus, raw_bus;
    Engine buffered(buffered_bus);
    gameboy::SnesDspPcmRenderer raw(raw_bus);
    raw.set_live_readback_enabled(true);
    raw.reset();
    gameboy::SnesDspClock clock(raw, raw_bus);
    setup(buffered_bus, buffered); setup(raw_bus, raw);
    unsigned samples{}, audible{};
    for (unsigned i = 0; i < 100000; ++i) {
        scheduled_writes(buffered, i % 1024); scheduled_writes(raw, i % 1024);
        const auto expected = clock.clock();
        check(buffered.clock(), "unblocked buffered engine advances");
        Sample sample{};
        const bool emitted = buffered.pop_sample(sample);
        check(emitted == expected.has_value() && (!emitted || same(sample, *expected)),
              "buffered engine preserves every raw renderer PCM sample and output phase");
        if (emitted) {
            ++samples;
            if (sample.left || sample.right) ++audible;
        }
    }
    check(samples == 3125 && audible > 0, "renderer parity test covers non-silent stereo");
    for (unsigned address = 0; address < 65536; ++address)
        check(buffered_bus.dsp_read_ram(static_cast<std::uint16_t>(address)) ==
                  raw_bus.dsp_read_ram(static_cast<std::uint16_t>(address)),
              "buffering preserves all physical APU RAM/echo writeback");
    for (unsigned address = 0; address < 128; ++address)
        check(buffered_bus.dsp_register(static_cast<std::uint8_t>(address)) ==
                  raw_bus.dsp_register(static_cast<std::uint8_t>(address)),
              "buffering preserves all live DSP register readback");
}

void test_all_phase_restore() {
    // Both alternating KON epochs, every DSP phase, and non-empty PCM queues.
    for (unsigned phase = 0; phase < 64; ++phase) {
        gameboy::SnesApuBus bus;
        Engine engine(bus);
        setup(bus, engine);
        check(engine.run_clocks(1024 + phase) == 1024 + phase, "warmup stays within buffer capacity");
        const auto snapshot = engine.save_state();
        check(snapshot == engine.save_state(), "snapshot bytes deterministic");
        const auto expected = continue_audio(engine, 2048);
        const auto expected_state = engine.save_state();
        check(std::any_of(expected.begin(), expected.end(), [](Sample s) {
                  return s.left != 0 || s.right != 0;
              }), "original fixture produces non-silent stereo");
        check(engine.load_state(snapshot), "same-instance state accepted");
        const auto repeated = continue_audio(engine, 2048);
        check(expected.size() == repeated.size() &&
                  std::equal(expected.begin(), expected.end(), repeated.begin(), same) &&
                  expected_state == engine.save_state(), "same-instance continuation exact");

        gameboy::SnesApuBus other_bus;
        Engine other(other_bus);
        check(other.load_state(snapshot), "cross-instance state accepted");
        // The restored BRR walkers must read their destination bus, not old pointers.
        bus.dsp_write_ram(0x8001, 0);
        const auto restored = continue_audio(other, 2048);
        check(expected.size() == restored.size() &&
                  std::equal(expected.begin(), expected.end(), restored.begin(), same) &&
                  expected_state == other.save_state(), "cross-instance continuation and full bus state exact");
    }
}

void test_buffer_backpressure() {
    gameboy::SnesApuBus bus;
    Engine engine(bus);
    setup(bus, engine);
    constexpr auto clocks_to_full = (Engine::buffer_capacity - 1) * 32 + 28;
    check(engine.run_clocks(100000) == clocks_to_full &&
              engine.pending_samples() == Engine::buffer_capacity && engine.phase() == 28,
          "full buffer stops at the first unavailable slot");
    const auto full = engine.save_state();
    allocations = 0; watch_allocations = true;
    const bool blocked = !engine.clock() && engine.run_clocks(100000) == 0;
    watch_allocations = false;
    check(blocked && !allocations && full == engine.save_state(),
          "backpressure neither allocates nor advances any state");
    gameboy::SnesApuBus copied_bus;
    Engine copied(copied_bus);
    check(copied.load_state(full), "full queue state accepted");
    for (unsigned i = 0; i < 1024; ++i) {
        Sample a{}, b{};
        check(engine.pop_sample(a) && copied.pop_sample(b) && same(a, b),
              "wrapped queue preserves exact FIFO stereo ordering");
        check(engine.run_clocks(32) == 32 && copied.run_clocks(32) == 32,
              "one drained slot permits exactly one new sample");
    }
    check(engine.save_state() == copied.save_state(), "wrapped queue states agree");
    allocations = 0; watch_allocations = true;
    Sample out{};
    for (unsigned i = 0; i < Engine::buffer_capacity; ++i) (void)engine.pop_sample(out);
    const bool empty = !engine.pop_sample(out);
    (void)engine.run_clocks(3200);
    watch_allocations = false;
    check(empty && allocations == 0, "clocking and draining do not allocate");
}

void test_atomic_validation_and_observers() {
    gameboy::SnesApuBus bus;
    Engine engine(bus);
    setup(bus, engine);
    (void)engine.run_clocks(1039);
    const auto before = engine.save_state();
    const auto reject = [&](std::vector<std::uint8_t> invalid) {
        check(!engine.load_state(invalid) && engine.save_state() == before,
              "invalid snapshot leaves all live state unchanged");
    };
    auto bad = before; bad[7] = 2; reject(bad);
    bad = before; bad[0] ^= 1; reject(bad);
    bad = before; bad.push_back(0); reject(bad);
    bad.resize(70 * 1024 + 1); reject(bad);
    for (const auto length : {std::size_t{0}, std::size_t{7}, std::size_t{200}, before.size() - 1}) {
        bad = before; bad.resize(length); reject(bad);
    }
    // Version-1 field offsets (explicit wire sizes, never sizeof(host objects)).
    constexpr std::size_t voices = 8 + 65536 + 128 + 8 + 9 + 12 + 1 + 64 + 2 + 1;
    bad = before; bad[voices + 6] = 4; reject(bad); // Invalid BRR group index.
    bad = before; bad[voices + 33] = 1; reject(bad); // Misaligned ring write point.
    bad = before; bad[voices + 40] = 255; reject(bad); // Invalid envelope enum.
    bad = before; bad[voices + 41] = 255; reject(bad); // Invalid key-on sequence enum.
    bad = before; bad[voices + 42] = 2; reject(bad); // Non-canonical voice bool.
    bad = before; bad[voices + 7] = 0xFF; bad[voices + 8] = 0x7F;
    reject(bad); // Ring sample exceeds signed 15-bit BRR range.
    bad = before; bad[voices] = 0xFF; bad[voices + 1] = 0x7F;
    reject(bad); // BRR predictor exceeds signed 15-bit range.
    bad = before; bad[voices - 3] = 2; reject(bad); // Non-canonical has-IPL bool.
    bad = before; bad[before.size() - Engine::buffer_capacity * 4 - 4] = 0;
    bad[before.size() - Engine::buffer_capacity * 4 - 3] = 2; reject(bad); // Head=512.

    unsigned observed{};
    bus.set_dsp_write_observer([](void* context, std::uint8_t, std::uint8_t) noexcept {
        ++*static_cast<unsigned*>(context);
    }, &observed);
    allocations = 0; watch_allocations = true;
    const bool accepted = engine.load_state(before);
    watch_allocations = false;
    check(accepted && allocations == 0 && observed == 0, "restore neither allocates nor invokes observers");
    engine.write_dsp(0x0C, 0x55);
    check(observed == 1, "restore retains existing observer context");
    engine.reset();
    engine.write_dsp(0x0C, 0x66);
    check(observed == 2 && engine.pending_samples() == 0 && engine.phase() == 0,
          "reset clears phase and queue without detaching external observers");
}

void test_reset_repeatability_and_shared_write() {
    gameboy::SnesApuBus bus;
    Engine engine(bus);
    setup(bus, engine);
    (void)continue_audio(engine, 2048);
    engine.reset();
    Sample sample{123, -456};
    check(!engine.pop_sample(sample) && same(sample, {123, -456}),
          "reset flushes queued audio and empty pop preserves its destination");
    // Identical reset/setup produces identical complete snapshots, including PCM.
    bus.reset(); engine.reset(); setup(bus, engine);
    const auto first = continue_audio(engine, 2048);
    const auto state = engine.save_state();
    bus.reset(); engine.reset(); setup(bus, engine);
    const auto second = continue_audio(engine, 2048);
    check(first.size() == second.size() && std::equal(first.begin(), first.end(), second.begin(), same) &&
              state == engine.save_state(), "hard reset/setup repeats every audio/state byte");
    unsigned writes{};
    bus.set_dsp_write_observer([](void* c, std::uint8_t, std::uint8_t) noexcept {
        ++*static_cast<unsigned*>(c);
    }, &writes);
    bus.spc_write(0xF2, 0x4C); bus.spc_write(0xF3, 0x01);
    engine.accept_dsp_write(0x4C, 0x01);
    check(writes == 1, "accepting a committed shared-bus write does not write the ports twice");
}
} // namespace

// Count allocation calls only around the realtime/restore operations above.
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

int main() {
    test_buffering_preserves_raw_renderer_output();
    test_all_phase_restore();
    test_buffer_backpressure();
    test_atomic_validation_and_observers();
    test_reset_repeatability_and_shared_write();
    return failures ? 1 : 0;
}
