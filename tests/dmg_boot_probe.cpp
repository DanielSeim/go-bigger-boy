// Execution-only boot reference probe. Never disassembles or exports firmware.
#include "gameboy/emulator.hpp"
#include "dmg_boot_serial_probe.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cmath>
#include <fstream>
#include <filesystem>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
struct AudioStats {
    std::uint64_t samples = 0, nonzero = 0, hash = 14695981039346656037ULL;
    unsigned peak = 0;
    double squares = 0;
    void consume(const std::vector<std::int16_t>& pcm) {
        for (const auto sample : pcm) {
            const auto magnitude = static_cast<unsigned>(std::abs(static_cast<int>(sample)));
            ++samples;
            nonzero += sample != 0;
            peak = std::max(peak, magnitude);
            squares += static_cast<double>(sample) * sample;
            const auto bits = static_cast<std::uint16_t>(sample);
            for (unsigned byte = 0; byte < 2; ++byte) {
                hash ^= (bits >> (8 * byte)) & 255U;
                hash *= 1099511628211ULL;
            }
        }
    }
    void print() const {
        std::cout << "{\"samples\":" << samples << ",\"nonzero_samples\":" << nonzero
                  << ",\"peak\":" << peak << ",\"rms\":"
                  << (samples ? std::sqrt(squares / samples) : 0)
                  << ",\"pcm_fnv64\":" << hash << '}';
    }
};
std::vector<std::uint8_t> read_file(const std::string& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) throw std::runtime_error("cannot open input: " + path);
    std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>(stream), {}};
    if (stream.bad()) throw std::runtime_error("cannot read input: " + path);
    return bytes;
}

template<class Range> void array(const Range& values) {
    std::cout << '[';
    bool first = true;
    for (const auto value : values) {
        if (!first) std::cout << ',';
        first = false;
        std::cout << +value;
    }
    std::cout << ']';
}

void snapshot(gameboy::Emulator& emulator) {
    const auto& cpu = emulator.cpu();
    const auto& r = cpu.registers();
    const auto& bus = emulator.bus();
    std::cout << "{\"cycles\":" << cpu.total_cycles()
              << ",\"cpu\":{";
    std::cout << "\"a\":" << +r.a << ",\"f\":" << +r.f
              << ",\"b\":" << +r.b << ",\"c\":" << +r.c
              << ",\"d\":" << +r.d << ",\"e\":" << +r.e
              << ",\"h\":" << +r.h << ",\"l\":" << +r.l
              << ",\"sp\":" << r.sp << ",\"pc\":" << r.pc
              << ",\"ime\":" << cpu.interrupts_enabled()
              << ",\"halted\":" << cpu.halted()
              << ",\"stopped\":" << cpu.stopped() << "},\"io\":";
    std::array<std::uint8_t, 128> io{};
    for (unsigned i = 0; i < io.size(); ++i) io[i] = bus.read8(0xFF00 + i);
    array(io);
    std::cout << ",\"ie\":" << +bus.read8(0xFFFF)
              << ",\"divider_counter\":" << bus.debug_divider_counter()
              << ",\"ppu_dot\":" << bus.debug_ppu_dot()
              << ",\"ppu_mode\":" << +bus.debug_ppu_mode()
              << ",\"serial_phase\":" << bus.serial_port().phase()
              << ",\"serial_bits\":" << +bus.serial_port().bits_shifted()
              << ",\"apu_clocks\":";
    array(bus.debug_apu_clock_state());
    // Use unrestricted diagnostic reads for video memory even in mode 3.
    std::array<std::uint8_t, 8192> ram{};
    std::cout << ",\"vram\":";
    for (unsigned i = 0; i < ram.size(); ++i) ram[i] = bus.debug_read_vram(0, i);
    array(ram);
    std::cout << ",\"wram\":";
    for (unsigned i = 0; i < ram.size(); ++i) ram[i] = bus.read8(0xC000 + i);
    array(ram);
    std::array<std::uint8_t, 160> oam{};
    for (unsigned i = 0; i < oam.size(); ++i) oam[i] = bus.debug_read_oam(i);
    std::cout << ",\"oam\":"; array(oam);
    std::array<std::uint8_t, 127> hram{};
    for (unsigned i = 0; i < hram.size(); ++i) hram[i] = bus.read8(0xFF80 + i);
    std::cout << ",\"hram\":"; array(hram);
    // Hash visible output without exporting cartridge graphics into reports.
    std::uint64_t frame_hash = 14695981039346656037ULL;
    for (const auto pixel : emulator.framebuffer()) {
        for (unsigned byte = 0; byte < 4; ++byte) {
            frame_hash ^= (pixel >> (8 * byte)) & 255U;
            frame_hash *= 1099511628211ULL;
        }
    }
    std::cout << ",\"framebuffer_fnv64\":" << frame_hash;
    std::cout << '}';
}
}

int main(int argc, char** argv) {
    try {
        if (argc < 2) throw std::runtime_error(
            "usage: gbb_dmg_boot_probe CARTRIDGE [--boot-rom FILE] [--max-cycles N] [--run-cycles N] [--align-frame] [--cold-clock-cycles N] [--audio-directory NEW_DIRECTORY] [--press-start-cycle N] [--serial-check] [--boot-trace]");
        std::string reference;
        std::string audio_directory;
        std::vector<std::uint64_t> start_presses;
        bool align_frame = false, serial_check = false, boot_trace = false;
        auto model = gameboy::HardwareModel::dmg;
        std::uint64_t max_cycles = 40'000'000, run_cycles = 0;
        unsigned cold_clock_cycles = 0;
        for (int i = 2; i < argc; ++i) {
            const std::string option = argv[i];
            if (option == "--align-frame") { align_frame = true; continue; }
            if (option == "--serial-check") { serial_check = true; continue; }
            if (option == "--boot-trace") { boot_trace = true; continue; }
            if (i + 1 >= argc) throw std::runtime_error("missing option value");
            const std::string value = argv[++i];
            if (option == "--model") {
                if (value == "sgb") model = gameboy::HardwareModel::sgb;
                else if (value == "sgb2") model = gameboy::HardwareModel::sgb2;
                else if (value != "dmg") throw std::runtime_error("probe model must be dmg, sgb or sgb2");
            }
            else if (option == "--boot-rom") reference = value;
            else if (option == "--audio-directory") audio_directory = value;
            else if (option == "--max-cycles" || option == "--run-cycles" || option == "--cold-clock-cycles" || option == "--press-start-cycle") {
                std::size_t used = 0;
                if (value.empty() || value[0] == '-') throw std::runtime_error("invalid cycle count");
                const auto count = std::stoull(value, &used);
                if (used != value.size() || count > 1'000'000'000ULL)
                    throw std::runtime_error("invalid cycle count");
                if (option == "--max-cycles") max_cycles = count;
                else if (option == "--run-cycles") run_cycles = count;
                else if (option == "--press-start-cycle") start_presses.push_back(count);
                else {
                    if (count > 16) throw std::runtime_error("cold clock offset must be 0..16");
                    cold_clock_cycles = static_cast<unsigned>(count);
                }
            } else throw std::runtime_error("unknown option: " + option);
        }
        std::sort(start_presses.begin(), start_presses.end());
        for (std::size_t i = 0; i < start_presses.size(); ++i) {
            if (start_presses[i] + 70'224 + 24 > run_cycles ||
                (i && start_presses[i] - start_presses[i - 1] < 70'224))
                throw std::runtime_error("Start pulses must be separated by a frame and inside followup");
        }
        // Bytes-only cartridge construction avoids adjacent save/RTC reads and
        // persistence writes. Both runs use exactly the same cold baseline.
        const auto cartridge_bytes = read_file(argv[1]);
        gameboy::Emulator emulator(gameboy::Cartridge(cartridge_bytes),
            model, gameboy::BootRomMode::replacement_dmg);
        if (!reference.empty()) {
            const auto bytes = read_file(reference);
            if (bytes.size() != gameboy::diagnostic_boot_rom_size)
                throw std::runtime_error("reference boot ROM must be exactly 256 bytes");
            gameboy::DiagnosticBootRom image{};
            std::copy(bytes.begin(), bytes.end(), image.begin());
            emulator.bus().install_boot_rom(image);
        }
        // Diagnostic sensitivity experiment, not a production reset change.
        // Record it explicitly; CPU instruction cycle totals exclude this offset.
        emulator.bus().tick(cold_clock_cycles);
        AudioStats boot_audio, followup_audio;
        if (boot_trace) emulator.bus().debug_enable_io_trace(true);
        while (emulator.bus().boot_rom_enabled() && emulator.cpu().total_cycles() < max_cycles) {
            (void)emulator.step();
            boot_audio.consume(emulator.take_audio_samples());
        }
        if (emulator.bus().boot_rom_enabled()) throw std::runtime_error("boot handoff timed out");
        if (emulator.cpu().registers().pc != 0x100)
            throw std::runtime_error("boot did not hand off at PC=0100");
        const auto boot_writes = boot_trace ? emulator.bus().debug_take_io_trace()
                                           : std::vector<gameboy::MemoryBus::IoTraceEvent>{};
        emulator.bus().debug_enable_io_trace(false);
        const auto serial_handoff = serial_check ? emulator.save_state() : std::vector<std::uint8_t>{};
        std::ofstream pcm_output;
        if (!audio_directory.empty()) {
            // Reserve a fresh directory rather than truncate an existing file.
            if (!std::filesystem::create_directory(audio_directory))
                throw std::runtime_error("audio directory already exists");
            pcm_output.open(std::filesystem::path(audio_directory) / "followup.pcm",
                            std::ios::binary);
            if (!pcm_output) throw std::runtime_error("cannot create audio capture");
            emulator.bus().debug_enable_io_trace(true);
        }
        std::vector<gameboy::MemoryBus::IoTraceEvent> apu_writes;
        std::cout << "{\"schema\":1,\"cold_clock_cycles\":" << cold_clock_cycles
                  << ",\"handoff\":";
        snapshot(emulator);
        const auto start = emulator.cpu().total_cycles();
        std::vector<std::array<std::uint64_t, 2>> input_events;
        std::size_t next_press = 0;
        bool start_held = false;
        std::uint64_t release_cycle = 0;
        const auto capture_audio = [&] {
            const auto pcm = emulator.take_audio_samples();
            followup_audio.consume(pcm);
            if (!audio_directory.empty()) {
                for (const auto sample : pcm) {
                    const auto bits = static_cast<std::uint16_t>(sample);
                    const char bytes[] = {static_cast<char>(bits & 255U),
                                          static_cast<char>(bits >> 8)};
                    pcm_output.write(bytes, 2);
                }
                for (auto event : emulator.bus().debug_take_io_trace()) {
                    if (event.address >= 0xFF10 && event.address <= 0xFF3F) {
                        if (apu_writes.size() == 250'000)
                            throw std::runtime_error("audio trace event limit exceeded");
                        event.cycle -= start + cold_clock_cycles;
                        apu_writes.push_back(event);
                    }
                }
            }
        };
        while (emulator.cpu().total_cycles() - start < run_cycles) {
            const auto elapsed = emulator.cpu().total_cycles() - start;
            if (start_held && elapsed >= release_cycle) {
                emulator.bus().set_button(gameboy::Button::start, false);
                input_events.push_back({elapsed, 0});
                start_held = false;
            }
            if (next_press < start_presses.size() && elapsed >= start_presses[next_press]) {
                emulator.bus().set_button(gameboy::Button::start, true);
                input_events.push_back({elapsed, 1});
                release_cycle = start_presses[next_press++] + 70'224;
                start_held = true;
            }
            (void)emulator.step();
            capture_audio();
        }
        if (align_frame) {
            emulator.consume_frame();
            const auto alignment_start = emulator.cpu().total_cycles();
            while (!emulator.frame_ready() &&
                   emulator.cpu().total_cycles() - alignment_start < 2 * 70'224) {
                (void)emulator.step();
                capture_audio();
            }
            if (!emulator.frame_ready())
                throw std::runtime_error("followup frame alignment timed out");
        }
        if (!audio_directory.empty()) {
            pcm_output.close();
            if (!pcm_output) throw std::runtime_error("audio capture write failed");
        }
        std::cout << ",\"followup\":"; snapshot(emulator);
        std::cout << ",\"boot_writes\":[";
        for (std::size_t index = 0; index < boot_writes.size(); ++index) {
            if (index) std::cout << ',';
            const auto& event = boot_writes[index];
            std::cout << '[' << event.cycle << ',' << event.address << ',' << +event.value << ']';
        }
        std::cout << "],\"audio\":{\"boot\":"; boot_audio.print();
        std::cout << ",\"followup\":"; followup_audio.print();
        std::cout << "},\"apu_writes\":[";
        bool first = true;
        for (const auto& event : apu_writes) {
            if (!first) std::cout << ',';
            first = false;
            std::cout << '[' << event.cycle << ',' << event.address << ',' << +event.value << ']';
        }
        std::cout << "],\"start_input_events\":[";
        first = true;
        for (const auto& event : input_events) {
            if (!first) std::cout << ',';
            first = false;
            array(event);
        }
        std::cout << ']';
        if (serial_check) {
            std::cout << ",\"serial_check\":";
            dmg_serial_probe::print(std::cout, cartridge_bytes, serial_handoff);
        }
        std::cout << "}\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 2;
    }
}
