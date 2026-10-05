// SPDX-License-Identifier: GPL-3.0-or-later
// Original lettering and APU-synthesized behavior, not extracted firmware/assets.
#include "gameboy/boot_splash.hpp"
#include "gameboy/apu.hpp"
#include <algorithm>
#include <array>
#include <string_view>

namespace gameboy {
namespace {
// Hand-authored 4x7 glyphs. Lowercase stays lowercase, including the descenders.
constexpr std::array<std::array<unsigned, 7>, 8> glyphs{{
    {{6,9,8,11,9,9,6}},  // G
    {{0,0,6,9,9,9,6}},   // o
    {{14,9,9,14,9,9,14}},// B
    {{2,0,6,2,2,2,7}},   // i
    {{0,6,9,9,7,1,6}},   // g
    {{0,0,6,9,15,8,7}},  // e
    {{0,0,11,12,8,8,8}}, // r
    {{0,0,9,9,7,1,6}},   // y
}};
constexpr std::string_view logo = "Go Bigger Boy";

const std::vector<std::int16_t>& chime() {
    // Observable register writes from an execution-only DMG/MGB reference.
    // These are hardware parameters, not firmware instructions or recorded PCM.
    // Render once through GBB's real pulse/envelope/filter/resampler machinery.
    static const auto pcm = [] {
        struct Event { std::uint64_t cycle; std::uint16_t address; std::uint8_t value; };
        constexpr Event events[] = {
            {229436,0xff26,0x80}, {229444,0xff11,0x80},
            {229464,0xff12,0xf3}, {229472,0xff25,0xf3}, {229488,0xff24,0x77},
            {boot_splash_first_note_cycle-20,0xff13,0x83},
            {boot_splash_first_note_cycle,0xff14,0x87},
            {boot_splash_second_note_cycle-20,0xff13,0xc1},
            {boot_splash_second_note_cycle,0xff14,0x87},
        };
        constexpr std::uint64_t end = 24'000'000;
        constexpr auto warm = (boot_splash_first_note_cycle / 32768 - 16) * 32768;
        std::vector<std::int16_t> output(boot_splash_sample_time(end), 0);
        struct Sink {
            std::vector<std::int16_t>& output;
            std::uint64_t index;
            std::uint64_t first;
        } sink{output, boot_splash_sample_time(warm), boot_splash_sample_time(boot_splash_first_note_cycle)};
        Apu apu;
        apu.set_audio_enabled(false);
        apu.set_sample_sink([](void* context, std::int16_t left, std::int16_t) noexcept {
            auto& sink = *static_cast<Sink*>(context);
            // Warm up the analog filter silently, avoiding a DAC-on click.
            if (sink.index >= sink.first && sink.index < sink.output.size())
                sink.output[static_cast<std::size_t>(sink.index)] = left;
            ++sink.index;
        }, &sink);
        std::uint64_t cycle = 0;
        std::size_t event = 0;
        while (cycle < end) {
            auto next = std::min(end, (cycle / 8192 + 1) * 8192);
            if (event < std::size(events)) next = std::min(next, events[event].cycle);
            if (cycle < warm) next = std::min(next, warm);
            apu.tick(static_cast<unsigned>(next - cycle));
            cycle = next;
            if (cycle % 8192 == 0) apu.clock_frame_sequencer();
            if (cycle == warm) apu.set_audio_enabled(true);
            while (event < std::size(events) && events[event].cycle == cycle) {
                apu.write_register(events[event].address, events[event].value, (cycle & 4096) != 0);
                ++event;
            }
        }
        return output;
    }();
    return pcm;
}
}

std::uint64_t boot_splash_sample_time(const std::uint64_t cycles) noexcept {
    return (cycles / 4194304) * Apu::sample_rate +
           (cycles % 4194304) * Apu::sample_rate / 4194304;
}

void prepare_boot_splash_audio() { static_cast<void>(chime()); }

void render_boot_splash(Ppu::Framebuffer& pixels, const std::uint64_t frame) noexcept {
    pixels.fill(0xffffffffU);
    // One pixel every alternating 3/2 VBlanks, followed by a settled hold.
    // The first decrement is measured at 472820 clocks. Preserve GBB glyphs.
    const auto cycles = std::min(frame, std::uint64_t{400}) * boot_splash_frame_cycles;
    const auto elapsed = cycles < 472820 ? 0 : (cycles - 472820) / boot_splash_frame_cycles;
    const auto moved = cycles < 472820 ? 0 : std::min(std::uint64_t{100},
        1 + (elapsed / 5) * 2 + (elapsed % 5 >= 2 ? 1 : 0));
    const int top = -35 + static_cast<int>(moved);
    for (std::size_t character = 0; character < logo.size(); ++character) {
        if (logo[character] == ' ') continue;
        // The lookup intentionally excludes spaces from the glyph table.
        constexpr std::string_view lookup = "GoBigery";
        const auto glyph = lookup.find(logo[character]);
        if (glyph == lookup.npos) continue;
        for (int row = 0; row < 7; ++row) {
            for (int column = 0; column < 4; ++column) {
                if (!(glyphs[glyph][static_cast<std::size_t>(row)] & (8U >> column))) continue;
                for (int dy = 0; dy < 2; ++dy) {
                    const int y = top + row * 2 + dy;
                    if (y < 0 || y >= 144) continue;
                    for (int dx = 0; dx < 2; ++dx) {
                        const auto x = 16 + character * 10 + static_cast<std::size_t>(column * 2 + dx);
                        pixels[static_cast<std::size_t>(y) * 160 + x] = 0xff000000U;
                    }
                }
            }
        }
    }
}

std::int16_t boot_splash_sample(const std::uint64_t sample) {
    if (sample < boot_splash_sample_time(boot_splash_first_note_cycle)) return 0;
    const auto& pcm = chime();
    return sample < pcm.size() ? pcm[static_cast<std::size_t>(sample)] : 0;
}
}
