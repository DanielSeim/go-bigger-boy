#include "gameboy/emulator.hpp"
#include "gameboy/boot_splash.hpp"
#include "../src/save_state_container.hpp"
#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
static_assert(sizeof(gameboy::Emulator) < 64 * 1024,
              "presentation buffers must not inflate the core's stack footprint");
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
gameboy::Cartridge cartridge() {
    std::vector<std::uint8_t> rom(32768, 0);
    rom[0x100] = 0x76;
    std::uint8_t sum = 0;
    for (unsigned i = 0x134; i <= 0x14c; ++i) sum = static_cast<std::uint8_t>(sum - rom[i] - 1);
    rom[0x14d] = sum;
    return gameboy::Cartridge(std::move(rom));
}
void same_hardware(gameboy::Emulator& a, gameboy::Emulator& b) {
    // Drain the presentation notification in both; the animated test polls
    // frames during startup whereas the fast baseline deliberately does not.
    a.consume_frame(); b.consume_frame();
    auto left = a.save_state(), right = b.save_state();
    // Animated firmware adds whole DIV wraps before initialization. Only
    // the total CPU clock may differ at handoff, not the hardware payload.
    std::fill(left.begin() + 52, left.begin() + 60, 0);
    std::fill(right.begin() + 52, right.begin() + 60, 0);
    require(left.size() == right.size(), "state sizes differ");
    require(std::equal(left.begin() + 28, left.end() - 25, right.begin() + 28),
            "animation changed hardware state");
}
}
int main() {
    try {
        using namespace gameboy;
        Ppu::Framebuffer start{}, settled{}, held{};
        render_boot_splash(start, 0);
        render_boot_splash(settled, 256);
        render_boot_splash(held, 320);
        require(std::all_of(start.begin(), start.end(), [](auto p){return p == 0xffffffffU;}), "initial logo is not offscreen");
        require(settled == held && settled != start, "logo does not descend and settle");
        const auto first_note = boot_splash_sample_time(boot_splash_first_note_cycle);
        const auto second_note = boot_splash_sample_time(boot_splash_second_note_cycle);
        for (std::uint64_t i = 0; i < boot_splash_sample_time(24'000'000); ++i) {
            const auto sample = boot_splash_sample(i);
            if (sample <= -32767 || sample >= 32767)
                throw std::runtime_error("chime clips at sample " + std::to_string(i));
            if (i < first_note)
                require(sample == 0, "chime outside note windows");
        }
        require(boot_splash_sample(first_note + 500) != 0 && boot_splash_sample(second_note + 500) != 0, "missing notes");
        for (auto note : {first_note, second_note}) {
            unsigned crossings = 0;
            for (auto i = note + 100; i < note + 1060; ++i)
                crossings += boot_splash_sample(i) > 0 && boot_splash_sample(i - 1) <= 0;
            require(note == first_note ? crossings >= 19 && crossings <= 23
                                      : crossings >= 39 && crossings <= 44,
                    "chime pitch differs from the reference pulse periods");
        }
        Emulator fast(cartridge(), HardwareModel::dmg, BootRomMode::replacement_dmg);
        Emulator animated(cartridge(), HardwareModel::dmg, BootRomMode::animated_dmg);
        while (fast.bus().boot_rom_enabled()) static_cast<void>(fast.step());
        unsigned instructions = 0;
        bool saw_frame = false, saw_audio = false, restored = false;
        while (animated.bus().boot_rom_enabled()) {
            require(++instructions < 5000000, "boot failed to finish");
            static_cast<void>(animated.step());
            if (animated.frame_ready()) {
                saw_frame = true;
                animated.consume_frame();
                static_cast<void>(animated.framebuffer());
                const auto audio = animated.take_audio_samples();
                saw_audio |= std::any_of(audio.begin(), audio.end(), [](auto s){return s != 0;});
                for (std::size_t i = 0; i + 1 < audio.size(); i += 2)
                    require(audio[i] == audio[i+1], "chime stereo differs");
                if (!restored && animated.cpu().total_cycles() > boot_splash_first_note_cycle + 10000) {
                    Emulator receiver(cartridge(), HardwareModel::dmg);
                    receiver.load_state(animated.save_state());
                    require(receiver.startup_animation_active(), "saved animation not restored");
                    require(receiver.framebuffer() == animated.framebuffer(), "restored logo differs");
                    for (unsigned i = 0; i < 1000; ++i) {
                        require(receiver.step() == animated.step(), "restore timing differs");
                    }
                    require(receiver.take_audio_samples() == animated.take_audio_samples(), "restored chime differs");
                    // Save with undrained PCM: restore discards that queue,
                    // and must not repeat its earlier part of the chime.
                    for (unsigned i = 0; i < 1000; ++i) {
                        static_cast<void>(animated.step());
                    }
                    receiver.load_state(animated.save_state());
                    static_cast<void>(animated.take_audio_samples());
                    for (unsigned i = 0; i < 1000; ++i) {
                        static_cast<void>(receiver.step());
                        static_cast<void>(animated.step());
                    }
                    require(receiver.take_audio_samples() == animated.take_audio_samples(), "restore repeated undrained chime");
                    const auto saved = receiver.save_state();
                    auto decoded = save_state_container::decode(saved, receiver.rom_fingerprint());
                    decoded.payload[decoded.payload.size() - 25] = 0x80;
                    bool rejected = false;
                    try { receiver.load_state(save_state_container::encode(receiver.rom_fingerprint(), decoded.payload)); }
                    catch (const SaveStateError&) { rejected = true; }
                    require(rejected && receiver.save_state() == saved, "malformed splash state not rejected atomically");
                    decoded = save_state_container::decode(saved, receiver.rom_fingerprint());
                    decoded.payload.resize(decoded.payload.size() - 25);
                    auto legacy = save_state_container::encode(receiver.rom_fingerprint(), decoded.payload);
                    legacy[8] = 41;
                    receiver.load_state(legacy);
                    require(!receiver.startup_animation_active() && receiver.bus().boot_rom_enabled(), "legacy state incorrectly adds animation");
                    receiver.reset();
                    require(!receiver.startup_animation_active(), "reset ignored configured instant mode");
                    restored = true;
                }
            }
        }
        require(saw_frame && saw_audio && restored, "animation paths not exercised");
        require(animated.cpu().total_cycles() == fast.cpu().total_cycles() + boot_splash_delay_cycles,
                "original-cadence boot delay differs");
        same_hardware(fast, animated);
        require(!animated.startup_animation_active() && animated.framebuffer() == fast.framebuffer(), "handoff presentation differs");
        // The pending buffer still contains boot samples, intentionally replaced.
        static_cast<void>(animated.take_audio_samples());
        static_cast<void>(fast.take_audio_samples());
        for (unsigned i = 0; i < 1000; ++i) {
            require(animated.step() == fast.step(), "gameplay timing differs");
        }
        require(animated.take_audio_samples() == fast.take_audio_samples(), "gameplay audio differs");
        animated.reset();
        require(animated.startup_animation_active(), "reset does not replay intro");
        animated.set_button(Button::start, true);
        require(!animated.startup_animation_active() && animated.bus().boot_rom_enabled(), "skip bypassed firmware");
        animated.bus().write8(0xff00, 0x10);
        require((animated.bus().read8(0xff00) & 0x0f) == 0x0f, "skip leaked button into cartridge");
        animated.set_audio_enabled(false);
        for (unsigned i = 0; i < 1000; ++i) static_cast<void>(animated.step());
        require(animated.take_audio_samples().empty(), "muted intro has samples");
        animated.reset();
        animated.set_player_button(0, Button::a, true);
        require(!animated.startup_animation_active() && animated.bus().boot_rom_enabled(), "A skip bypassed firmware");
        Emulator muted(cartridge(), HardwareModel::dmg, BootRomMode::animated_dmg);
        Emulator muted_fast(cartridge(), HardwareModel::dmg, BootRomMode::replacement_dmg);
        muted.set_audio_enabled(false); muted_fast.set_audio_enabled(false);
        while (muted_fast.bus().boot_rom_enabled()) static_cast<void>(muted_fast.step());
        while (muted.bus().boot_rom_enabled()) {
            static_cast<void>(muted.step());
        }
        require(muted.take_audio_samples().empty(), "muted boot accumulated chime");
        muted.set_audio_enabled(true); muted_fast.set_audio_enabled(true);
        for (unsigned i = 0; i < 1000; ++i) {
            static_cast<void>(muted.step()); static_cast<void>(muted_fast.step());
        }
        require(muted.take_audio_samples() == muted_fast.take_audio_samples(), "unmute replayed chime");
        // Different drain cadences must generate the same original chime.
        Emulator frequent(cartridge(), HardwareModel::dmg, BootRomMode::animated_dmg);
        Emulator batched(cartridge(), HardwareModel::dmg, BootRomMode::animated_dmg);
        std::vector<std::int16_t> chunks, batches;
        for (unsigned i = 0; frequent.cpu().total_cycles() < boot_splash_second_note_cycle + 1000000; ++i) {
            static_cast<void>(frequent.step()); static_cast<void>(batched.step());
            if (i % 127 == 0) {
                const auto part = frequent.take_audio_samples();
                chunks.insert(chunks.end(), part.begin(), part.end());
            }
            if (i % 20000 == 0) {
                const auto part = batched.take_audio_samples();
                batches.insert(batches.end(), part.begin(), part.end());
            }
        }
        const auto tail = frequent.take_audio_samples();
        chunks.insert(chunks.end(), tail.begin(), tail.end());
        const auto batch_tail = batched.take_audio_samples();
        batches.insert(batches.end(), batch_tail.begin(), batch_tail.end());
        require(chunks == batches, "audio drain cadence changes chime");
        std::cout << "Original boot presentation and unchanged hardware handoff passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
