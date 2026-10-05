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
// Original compact block capitals, not glyphs decoded from a boot ROM.
// The Color presentation uses heavy stems and an oblique silhouette.
constexpr std::string_view color_lookup = "GOBIERY";
constexpr std::array<std::array<unsigned, 7>, 7> color_glyphs{{
    {{14,27,24,27,27,27,14}}, // G
    {{14,27,27,27,27,27,14}}, // O
    {{30,27,27,30,27,27,30}}, // B
    {{1,1,1,1,1,1,1}},       // I (narrow)
    {{31,24,24,30,24,24,31}},// E
    {{30,27,27,30,28,26,27}},// R
    {{27,27,27,14,6,6,6}},   // Y
}};
constexpr std::string_view color_logo = "GO BIGGER BOY";

std::vector<std::int16_t> synthesize_chime(HardwareModel model) {
    // Observable register writes from execution-only DMG0/DMG/MGB references.
    // These are hardware parameters, not firmware instructions or recorded PCM.
    // Render once through GBB's real pulse/envelope/filter/resampler machinery.
    const bool early = model == HardwareModel::dmg0;
    const bool color = is_cgb_hardware(model);
    const std::uint64_t first_note = color ? cgb_splash_first_note_cycle
        : early ? 18'523'904 : boot_splash_first_note_cycle;
    const std::uint64_t second_note = color ? cgb_splash_second_note_cycle
        : early ? 18'875'268 : boot_splash_second_note_cycle;
    struct Event { std::uint64_t cycle; std::uint16_t address; std::uint8_t value; };
    const Event events[] = {
        {229436,0xff26,0x80}, {229444,0xff11,0x80},
        {229464,0xff12,0xf3}, {229472,0xff25,0xf3}, {229488,0xff24,0x77},
        {first_note-20,0xff13,0x83},
        {first_note,0xff14,0x87},
        {second_note-20,0xff13,0xc1},
        {second_note,0xff14,0x87},
    };
    const std::uint64_t end = color ? 14'000'000 : early ? 25'500'000 : 24'000'000;
    const auto warm = (first_note / 32768 - 16) * 32768;
    std::vector<std::int16_t> output(boot_splash_sample_time(end), 0);
    struct Sink {
        std::vector<std::int16_t>& output;
        std::uint64_t index;
        std::uint64_t first;
    } sink{output, boot_splash_sample_time(warm), boot_splash_sample_time(first_note)};
    Apu apu;
    if (color) apu.initialize_power_on(HardwareModel::cgb);
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
}
const std::vector<std::int16_t>& chime(HardwareModel model) {
    if (is_cgb_hardware(model)) {
        static const auto color = synthesize_chime(HardwareModel::cgb);
        return color;
    }
    if (model == HardwareModel::dmg0) {
        static const auto early = synthesize_chime(HardwareModel::dmg0);
        return early;
    }
    static const auto later = synthesize_chime(HardwareModel::dmg);
    return later;
}
}

std::uint64_t boot_splash_sample_time(const std::uint64_t cycles) noexcept {
    return (cycles / 4194304) * Apu::sample_rate +
           (cycles % 4194304) * Apu::sample_rate / 4194304;
}

void prepare_boot_splash_audio(HardwareModel model) { static_cast<void>(chime(model)); }

void render_boot_splash(Ppu::Framebuffer& pixels, const std::uint64_t frame, HardwareModel model) noexcept {
    pixels.fill(0xffffffffU);
    if (is_cgb_hardware(model)) {
        // Black-box reference: white lead-in, small footer, successive rainbow
        // letters, blue hold, then a stepped palette fade. Original GBB artwork
        // replaces both marks; no reference pixels or firmware are consulted.
        const auto fade = frame < 146 ? 255ULL : frame < 177
            ? (31 - (frame - 146)) * 255 / 31 : 0ULL;
        constexpr std::array<std::uint32_t, 5> colors{
            0xffff00, 0xff0000, 0xff00ff, 0x00ff00, 0x0000ff};
        auto blend = [fade](std::uint32_t rgb) {
            std::uint32_t result = 0xff000000;
            for (unsigned shift : {0U, 8U, 16U}) {
                const auto channel = (rgb >> shift) & 255;
                result |= static_cast<std::uint32_t>(255 - (255 - channel) * fade / 255) << shift;
            }
            return result;
        };
        auto draw = [&](char letter, int left, int top, int sx, int sy,
                        bool oblique, std::uint32_t color) {
            const auto glyph = color_lookup.find(letter);
            const int width = letter == 'I' ? 1 : 5;
            for (int row = 0; row < 7; ++row) for (int column = 0; column < width; ++column) {
                if (!(color_glyphs[glyph][row] & (1U << (width - 1 - column)))) continue;
                for (int dy = 0; dy < sy; ++dy) for (int dx = 0; dx < sx; ++dx) {
                    const int y = top + row * sy + dy;
                    const int x = left + column * sx + dx + (oblique ? (20 - row * sy - dy) / 5 : 0);
                    pixels[static_cast<std::size_t>(y) * 160 + x] = color;
                }
            }
        };
        int left = 12;
        unsigned letter = 0;
        for (char c : color_logo) {
            if (c == ' ') { left += 5; continue; }
            const auto onset = 58 + 3 * letter++;
            if (frame >= onset) draw(c, left, 48, 2, 3, true,
                blend(colors[std::min<std::uint64_t>(4, (frame - onset) / 6)]));
            left += c == 'I' ? 4 : 12;
        }
        if (frame >= 35) {
            int x = 72;
            for (char c : std::string_view{"GBB"}) { draw(c, x, 104, 1, 1, false, blend(0)); x += 6; }
        }
        return;
    }
    // One pixel every alternating 3/2 VBlanks, followed by a settled hold.
    // The first decrement is measured at 472820 clocks. Preserve GBB glyphs.
    const auto cycles = std::min(frame, std::uint64_t{400}) * boot_splash_frame_cycles;
    const auto start = model == HardwareModel::dmg0 ? 476336U : 472820U;
    const auto elapsed = cycles < start ? 0 : (cycles - start) / boot_splash_frame_cycles;
    const auto descent = model == HardwareModel::dmg0
        ? (elapsed / 8) * 3 + (elapsed % 8 >= 3) + (elapsed % 8 >= 5)
        : (elapsed / 5) * 2 + (elapsed % 5 >= 2);
    const auto moved = cycles < start ? 0 : std::min(std::uint64_t{100}, 1 + descent);
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

std::int16_t boot_splash_sample(const std::uint64_t sample, HardwareModel model) {
    const auto first = is_cgb_hardware(model) ? cgb_splash_first_note_cycle
        : model == HardwareModel::dmg0 ? 18'523'904ULL : boot_splash_first_note_cycle;
    if (sample < boot_splash_sample_time(first)) return 0;
    const auto& pcm = chime(model);
    return sample < pcm.size() ? pcm[static_cast<std::size_t>(sample)] : 0;
}
}
