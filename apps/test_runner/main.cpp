#include "gameboy/emulator.hpp"
#include "gameboy/sgb_trace.hpp"
#include "sgb_input_script.h"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

enum class Protocol {
    automatic,
    mooneye,
    mooneye_wilbertpol,
    serial,
    blargg,
    gbmicrotest
};

struct Options {
    std::string rom_path;
    std::uint64_t max_cycles = 100'000'000;
    Protocol protocol = Protocol::automatic;
    gameboy::HardwareModel model = gameboy::HardwareModel::automatic;
    std::uint64_t frames{};
    std::filesystem::path frame_output;
    std::uint64_t frame_series_first{};
    std::uint64_t frame_series_last{};
    std::filesystem::path frame_series_prefix;
    std::filesystem::path input_script;
    std::filesystem::path apu_trace_output;
    std::filesystem::path ppu_trace_output;
    std::filesystem::path io_trace_output;
    std::filesystem::path cpu_trace_output;
    std::filesystem::path sgb_trace_output;
    std::filesystem::path sgb_replay_input;
    std::uint64_t trace_limit{};
    std::optional<std::uint16_t> watched_wram;
    bool dmg_compatibility_colors{};
    bool frame_on_ld_bb{};
    bool sgb_frame{};
    bool frame_state_series{};
    bool diagnostic_boot{};
};

void usage() {
    std::cerr << "Usage: gbb_test_runner <rom.gb> "
                 "[--max-cycles N] [--protocol auto|mooneye|"
                 "mooneye-wilbertpol|serial|blargg|gbmicrotest] "
                 "[--model auto|dmg0|dmg|mgb|sgb|sgb2|cgb0|cgb-c|cgb-e] "
                 "[--frames N --frame-output capture.ppm [--sgb-frame]] "
                 "[--frame-series FIRST LAST PREFIX [--sgb-frame]] "
                 "[--frame-state-series] "
                 "[--watch-wram 0xC000..0xDFFF] "
                 "[--input-script PATH] "
                 "[--trace-apu PATH] [--trace-ppu PATH] [--trace-io PATH] "
                 "[--trace-cpu PATH] [--trace-limit N] "
                 "[--sgb-trace PATH] [--replay-sgb-trace PATH] "
                 "[--frame-on-ld-bb --frame-output capture.ppm] "
                 "[--dmg-compatibility-colors] [--diagnostic-boot]\n";
}

gameboy::HardwareModel parse_model(const std::string& value) {
    if (value == "auto") return gameboy::HardwareModel::automatic;
    if (value == "dmg0") return gameboy::HardwareModel::dmg0;
    if (value == "dmg") return gameboy::HardwareModel::dmg;
    if (value == "mgb") return gameboy::HardwareModel::mgb;
    if (value == "sgb") return gameboy::HardwareModel::sgb;
    if (value == "sgb2") return gameboy::HardwareModel::sgb2;
    if (value == "cgb0") return gameboy::HardwareModel::cgb0;
    if (value == "cgb-c" || value == "cgbc") return gameboy::HardwareModel::cgb_c;
    if (value == "cgb-e" || value == "cgb" || value == "cgbe") return gameboy::HardwareModel::cgb_e;
    throw std::invalid_argument("unknown hardware model: " + value);
}

std::uint64_t parse_cycles(const std::string& text) {
    std::size_t consumed = 0;
    const auto value = std::stoull(text, &consumed);
    if (consumed != text.size() || value == 0) {
        throw std::invalid_argument("max cycles must be a positive integer");
    }
    return value;
}

Options parse_options(const int argc, char** argv) {
    if (argc < 2) {
        throw std::invalid_argument("missing ROM path");
    }
    Options options;
    options.rom_path = argv[1];
    for (int index = 2; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--max-cycles" && index + 1 < argc) {
            options.max_cycles = parse_cycles(argv[++index]);
        } else if (argument == "--protocol" && index + 1 < argc) {
            const std::string value = argv[++index];
            if (value == "auto") options.protocol = Protocol::automatic;
            else if (value == "mooneye") options.protocol = Protocol::mooneye;
            else if (value == "mooneye-wilbertpol")
                options.protocol = Protocol::mooneye_wilbertpol;
            else if (value == "serial") options.protocol = Protocol::serial;
            else if (value == "blargg") options.protocol = Protocol::blargg;
            else if (value == "gbmicrotest")
                options.protocol = Protocol::gbmicrotest;
            else throw std::invalid_argument("unknown protocol: " + value);
        } else if (argument == "--model" && index + 1 < argc) {
            options.model = parse_model(argv[++index]);
        } else if (argument == "--frames" && index + 1 < argc) {
            options.frames = parse_cycles(argv[++index]);
        } else if (argument == "--frame-series" && index + 3 < argc) {
            options.frame_series_first = parse_cycles(argv[++index]);
            options.frame_series_last = parse_cycles(argv[++index]);
            options.frame_series_prefix = argv[++index];
        } else if (argument == "--frame-output" && index + 1 < argc) {
            options.frame_output = argv[++index];
        } else if (argument == "--input-script" && index + 1 < argc) {
            options.input_script = argv[++index];
        } else if (argument == "--trace-apu" && index + 1 < argc) {
            options.apu_trace_output = argv[++index];
        } else if (argument == "--trace-ppu" && index + 1 < argc) {
            options.ppu_trace_output = argv[++index];
        } else if (argument == "--trace-io" && index + 1 < argc) {
            options.io_trace_output = argv[++index];
        } else if (argument == "--trace-cpu" && index + 1 < argc) {
            options.cpu_trace_output = argv[++index];
        } else if (argument == "--sgb-trace" && index + 1 < argc) {
            options.sgb_trace_output = argv[++index];
        } else if (argument == "--replay-sgb-trace" && index + 1 < argc) {
            options.sgb_replay_input = argv[++index];
        } else if (argument == "--trace-limit" && index + 1 < argc) {
            options.trace_limit = parse_cycles(argv[++index]);
        } else if (argument == "--dmg-compatibility-colors") {
            options.dmg_compatibility_colors = true;
        } else if (argument == "--frame-on-ld-bb") {
            options.frame_on_ld_bb = true;
        } else if (argument == "--sgb-frame") {
            options.sgb_frame = true;
        } else if (argument == "--frame-state-series") {
            options.frame_state_series = true;
        } else if (argument == "--watch-wram" && index + 1 < argc) {
            if (options.watched_wram.has_value()) {
                throw std::invalid_argument("--watch-wram specified twice");
            }
            const auto value = std::string{argv[++index]};
            std::size_t consumed = 0;
            const auto parsed = std::stoul(value, &consumed, 0);
            if (consumed != value.size() || parsed < 0xC000 ||
                parsed > 0xDFFF) {
                throw std::invalid_argument("--watch-wram requires a WRAM address");
            }
            options.watched_wram = static_cast<std::uint16_t>(parsed);
        } else if (argument == "--diagnostic-boot") {
            options.diagnostic_boot = true;
        } else {
            throw std::invalid_argument("unknown or incomplete option: " + argument);
        }
    }
    if ((options.frames != 0 && options.frame_on_ld_bb) ||
        (options.frame_series_last != 0 &&
         (options.frames != 0 || options.frame_on_ld_bb ||
          !options.frame_output.empty()))) {
        throw std::invalid_argument(
            "--frame-series, --frames, and --frame-on-ld-bb are mutually exclusive");
    }
    if (options.frame_series_last != 0 &&
        (options.frame_series_last < options.frame_series_first ||
         options.frame_series_last - options.frame_series_first > 1000 ||
         options.frame_series_prefix.empty())) {
        throw std::invalid_argument(
            "--frame-series requires a prefix, FIRST <= LAST, and at most 1001 frames");
    }
    if (!options.sgb_trace_output.empty() && !options.sgb_replay_input.empty()) {
        throw std::invalid_argument(
            "--sgb-trace and --replay-sgb-trace are mutually exclusive");
    }
    if (!options.input_script.empty() && !options.sgb_replay_input.empty()) {
        throw std::invalid_argument(
            "--input-script and --replay-sgb-trace are mutually exclusive");
    }
    const auto captures_single_frame =
        options.frames != 0 || options.frame_on_ld_bb;
    const auto captures_frame = captures_single_frame ||
                                options.frame_series_last != 0;
    if (captures_single_frame && options.frame_output.empty()) {
        throw std::invalid_argument("frame capture requires --frame-output");
    }
    if (!captures_single_frame && !options.frame_output.empty()) {
        throw std::invalid_argument(
            "--frame-output requires --frames or --frame-on-ld-bb");
    }
    if (options.sgb_frame && !captures_frame) {
        throw std::invalid_argument(
            "--sgb-frame requires --frames or --frame-on-ld-bb");
    }
    if (options.frame_state_series && options.frame_series_last == 0) {
        throw std::invalid_argument(
            "--frame-state-series requires --frame-series");
    }
    if (options.watched_wram.has_value() && options.frame_series_last == 0) {
        throw std::invalid_argument("--watch-wram requires --frame-series");
    }
    return options;
}

void write_frame(const std::filesystem::path& path,
                 const std::uint32_t* framebuffer,
                 const std::size_t width, const std::size_t height) {
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path());
    }
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("could not open frame output: " + path.string());
    }
    output << "P6\n" << width << ' ' << height << "\n255\n";
    for (std::size_t index = 0; index < width * height; ++index) {
        const auto pixel = framebuffer[index];
        const std::array<char, 3> rgb{
            static_cast<char>((pixel >> 16) & 0xFF),
            static_cast<char>((pixel >> 8) & 0xFF),
            static_cast<char>(pixel & 0xFF),
        };
        output.write(rgb.data(), static_cast<std::streamsize>(rgb.size()));
    }
    if (!output) {
        throw std::runtime_error("could not write frame output: " + path.string());
    }
}

void write_capture(const std::filesystem::path& path,
                   const gameboy::Emulator& emulator,
                   const bool sgb_frame) {
    if (sgb_frame) {
        const auto& frame = emulator.sgb_framebuffer();
        write_frame(path, frame.data(), gameboy::Ppu::sgb_border_width,
                    gameboy::Ppu::sgb_border_height);
    } else {
        const auto& frame = emulator.framebuffer();
        write_frame(path, frame.data(), gameboy::Ppu::screen_width,
                    gameboy::Ppu::screen_height);
    }
}

void write_frame_state(const std::filesystem::path& path,
                       const gameboy::Emulator& emulator) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) throw std::runtime_error("could not open frame state: " + path.string());
    for (std::uint32_t address = 0xC000; address < 0xE000; ++address) {
        output.put(static_cast<char>(emulator.bus().read8(
            static_cast<std::uint16_t>(address))));
    }
    for (std::uint16_t offset = 0; offset < 0xA0; ++offset) {
        output.put(static_cast<char>(emulator.bus().debug_read_oam(
            static_cast<std::uint8_t>(offset))));
    }
    if (!output) throw std::runtime_error("could not write frame state: " + path.string());
}

void set_held_buttons(gameboy::Emulator& emulator, const std::uint8_t previous,
                      const std::uint8_t next) {
    constexpr std::array buttons{
        gameboy::Button::right, gameboy::Button::left,
        gameboy::Button::up, gameboy::Button::down,
        gameboy::Button::a, gameboy::Button::b,
        gameboy::Button::select, gameboy::Button::start};
    for (unsigned bit = 0; bit < buttons.size(); ++bit) {
        const auto flag = static_cast<std::uint8_t>(1U << bit);
        if ((previous & flag) != (next & flag)) {
            emulator.set_button(buttons[bit], (next & flag) != 0);
        }
    }
}

bool mooneye_success(const gameboy::CpuRegisters& registers) {
    return registers.b == 3 && registers.c == 5 && registers.d == 8 &&
           registers.e == 13 && registers.h == 21 && registers.l == 34;
}

void print_state(const gameboy::Cpu& cpu) {
    const auto& r = cpu.registers();
    std::cerr << std::hex << std::setfill('0')
              << "PC=" << std::setw(4) << r.pc
              << " SP=" << std::setw(4) << r.sp
              << " AF=" << std::setw(2) << static_cast<unsigned>(r.a)
              << std::setw(2) << static_cast<unsigned>(r.f)
              << " BC=" << std::setw(2) << static_cast<unsigned>(r.b)
              << std::setw(2) << static_cast<unsigned>(r.c)
              << " DE=" << std::setw(2) << static_cast<unsigned>(r.d)
              << std::setw(2) << static_cast<unsigned>(r.e)
              << " HL=" << std::setw(2) << static_cast<unsigned>(r.h)
              << std::setw(2) << static_cast<unsigned>(r.l)
              << std::dec << " cycles=" << cpu.total_cycles() << '\n';
}

void print_result_diagnostics(const gameboy::Cpu& cpu,
                              const gameboy::MemoryBus& bus) {
    const auto& r = cpu.registers();
    std::cerr << "RESULT expected_registers=B=03,C=05,D=08,E=0d,H=15,L=22 "
              << "observed_registers=B=" << std::hex << std::setw(2)
              << static_cast<unsigned>(r.b) << ",C=" << std::setw(2)
              << static_cast<unsigned>(r.c) << ",D=" << std::setw(2)
              << static_cast<unsigned>(r.d) << ",E=" << std::setw(2)
              << static_cast<unsigned>(r.e) << ",H=" << std::setw(2)
              << static_cast<unsigned>(r.h) << ",L=" << std::setw(2)
              << static_cast<unsigned>(r.l) << std::dec << '\n';
    std::cerr << "RESULT wram_c000=" << std::hex << std::setfill('0');
    for (std::uint16_t address = 0xC000; address < 0xC020; ++address) {
        std::cerr << std::setw(2)
                  << static_cast<unsigned>(bus.read8(address));
    }
    std::cerr << std::dec << '\n';
}

void print_recent_pcs(const std::array<std::uint16_t, 64>& pcs,
                      const std::size_t next, const std::size_t count) {
    std::cerr << "Recent PCs:" << std::hex << std::setfill('0');
    const auto begin = (next + pcs.size() - count) % pcs.size();
    for (std::size_t index = 0; index < count; ++index) {
        std::cerr << ' ' << std::setw(4) << pcs[(begin + index) % pcs.size()];
    }
    std::cerr << std::dec << '\n';
}

void print_hram_head(const gameboy::MemoryBus& bus) {
    std::cerr << "HRAM FF80:" << std::hex << std::setfill('0');
    for (std::uint16_t address = 0xFF80; address < 0xFF90; ++address) {
        std::cerr << ' ' << std::setw(2)
                  << static_cast<unsigned>(bus.read8(address));
    }
    std::cerr << std::dec << '\n';
}

void print_hex_window(const char* label, const std::uint16_t base,
                      const std::uint16_t size,
                      const gameboy::MemoryBus& bus) {
    std::cerr << label << ' ' << std::hex << std::setfill('0');
    for (std::uint16_t offset = 0; offset < size; ++offset) {
        if ((offset % 16) == 0) {
            std::cerr << '\n' << std::setw(4)
                      << static_cast<unsigned>(base + offset) << ':';
        }
        std::cerr << ' ' << std::setw(2)
                  << static_cast<unsigned>(bus.read8(
                         static_cast<std::uint16_t>(base + offset)));
    }
    std::cerr << std::dec << '\n';
}

void print_video_diagnostics(const gameboy::MemoryBus& bus) {
    std::cerr << "Video registers: LCDC=" << std::hex << std::setfill('0')
              << std::setw(2) << static_cast<unsigned>(bus.read8(0xFF40))
              << " STAT=" << std::setw(2)
              << static_cast<unsigned>(bus.read8(0xFF41))
              << " LY=" << std::setw(2)
              << static_cast<unsigned>(bus.read8(0xFF44))
              << " LYC=" << std::setw(2)
              << static_cast<unsigned>(bus.read8(0xFF45))
              << " SCY=" << std::setw(2)
              << static_cast<unsigned>(bus.read8(0xFF42))
              << " SCX=" << std::setw(2)
              << static_cast<unsigned>(bus.read8(0xFF43))
              << " WY=" << std::setw(2)
              << static_cast<unsigned>(bus.read8(0xFF4A))
              << " WX=" << std::setw(2)
              << static_cast<unsigned>(bus.read8(0xFF4B))
              << " VBK=" << std::setw(2)
              << static_cast<unsigned>(bus.read8(0xFF4F))
              << " HDMA1-5=" << std::setw(2)
              << static_cast<unsigned>(bus.read8(0xFF51)) << ' '
              << std::setw(2) << static_cast<unsigned>(bus.read8(0xFF52))
              << ' ' << std::setw(2)
              << static_cast<unsigned>(bus.read8(0xFF53)) << ' '
              << std::setw(2) << static_cast<unsigned>(bus.read8(0xFF54))
              << ' ' << std::setw(2)
              << static_cast<unsigned>(bus.read8(0xFF55)) << std::dec << '\n';
    print_hex_window("VRAM 8800:", 0x8800, 0x40, bus);
    print_hex_window("WRAM C000:", 0xC000, 0x40, bus);
}

void print_audio_diagnostics(const gameboy::MemoryBus& bus) {
    print_hex_window("APU FF10:", 0xFF10, 0x17, bus);
    std::cerr << "APU PCM12=" << std::hex << std::setfill('0') << std::setw(2)
              << static_cast<unsigned>(bus.read8(0xFF76))
              << " PCM34=" << std::setw(2)
              << static_cast<unsigned>(bus.read8(0xFF77)) << std::dec << '\n';
}

void write_apu_trace(std::ofstream& output, const gameboy::Cpu& cpu,
                     const gameboy::MemoryBus& bus) {
    const auto& r = cpu.registers();
    output << "cycle=" << cpu.total_cycles() << " pc=" << std::hex
           << std::setw(4) << std::setfill('0') << r.pc
           << " div=" << std::setw(2) << static_cast<unsigned>(bus.read8(0xFF04))
           << " nr10_26=";
    for (std::uint16_t address = 0xFF10; address <= 0xFF26; ++address) {
        output << std::setw(2) << static_cast<unsigned>(bus.read8(address));
    }
    output << " pcm12=" << std::setw(2)
           << static_cast<unsigned>(bus.read8(0xFF76))
           << " pcm34=" << std::setw(2)
           << static_cast<unsigned>(bus.read8(0xFF77)) << std::dec << '\n';
}

void write_ppu_trace(std::ofstream& output, const gameboy::Cpu& cpu,
                     const gameboy::MemoryBus& bus) {
    const auto& r = cpu.registers();
    output << "cycle=" << cpu.total_cycles() << " pc=" << std::hex
           << std::setw(4) << std::setfill('0') << r.pc
           << " dot=" << std::setw(3) << bus.debug_ppu_dot()
           << " mode=" << static_cast<unsigned>(bus.debug_ppu_mode())
           << " m3end=" << std::setw(3) << bus.debug_ppu_mode3_end_dot()
           << " requests=" << std::setw(2)
           << static_cast<unsigned>(bus.debug_last_ppu_requests())
           << " lcdc=" << std::setw(2) << static_cast<unsigned>(bus.read8(0xFF40))
           << " stat=" << std::setw(2) << static_cast<unsigned>(bus.read8(0xFF41))
           << " ly=" << std::setw(2) << static_cast<unsigned>(bus.read8(0xFF44))
           << " lyc=" << std::setw(2) << static_cast<unsigned>(bus.read8(0xFF45))
           << " scy=" << std::setw(2) << static_cast<unsigned>(bus.read8(0xFF42))
           << " scx=" << std::setw(2) << static_cast<unsigned>(bus.read8(0xFF43))
           << " wy=" << std::setw(2) << static_cast<unsigned>(bus.read8(0xFF4A))
           << " wx=" << std::setw(2) << static_cast<unsigned>(bus.read8(0xFF4B))
           << " vbk=" << std::setw(2) << static_cast<unsigned>(bus.read8(0xFF4F))
           << " if=" << std::setw(2) << static_cast<unsigned>(bus.read8(0xFF0F))
           << std::dec << '\n';
}

void write_io_trace(
    std::ofstream& output,
    const std::vector<gameboy::MemoryBus::IoTraceEvent>& events) {
    for (const auto& event : events) {
        output << "cycle=" << event.cycle << " address=" << std::hex
               << std::setw(4) << std::setfill('0') << event.address
               << " value=" << std::setw(2)
               << static_cast<unsigned>(event.value)
               << " ly=" << std::setw(2) << static_cast<unsigned>(event.ly)
               << " dot=" << std::setw(3)
               << static_cast<unsigned>(event.dot)
               << " mode=" << static_cast<unsigned>(event.mode)
               << std::dec << '\n';
    }
}

bool write_sgb_trace(const std::filesystem::path& path,
                     std::optional<gameboy::SgbTrace::Recorder>& recorder,
                     const gameboy::Emulator& emulator,
                     const std::uint64_t frame, std::string& error) {
    if (!recorder.has_value()) return true;
    try {
        if (!recorder->checkpoint(emulator.cpu().total_cycles(), frame, emulator)) {
            error = "SGB trace checkpoint limit or ordering was exceeded";
            return false;
        }
        const auto trace = std::move(*recorder).finish();
        if (path.has_parent_path()) {
            std::filesystem::create_directories(path.parent_path());
        }
        std::ofstream output(path, std::ios::out | std::ios::trunc);
        if (!output) {
            error = "could not open SGB trace: " + path.string();
            return false;
        }
        if (!gameboy::SgbTrace::serialize(trace, output, &error)) return false;
        recorder.reset();
        return true;
    } catch (const std::exception& exception) {
        error = exception.what();
        return false;
    }
}

std::string read_text_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::in | std::ios::binary);
    if (!input) throw std::runtime_error("could not open trace: " + path.string());
    return {std::istreambuf_iterator<char>{input}, {}};
}

void write_cpu_trace(std::ofstream& output, const gameboy::Cpu& cpu,
                     const gameboy::MemoryBus& bus, const std::uint64_t cycle,
                     const std::uint16_t pc, const std::uint8_t opcode,
                     const unsigned cycles) {
    const auto& r = cpu.registers();
    output << "cycle=" << cycle << " duration=" << cycles << " pc="
           << std::hex << std::setw(4) << std::setfill('0') << pc
           << " opcode=" << std::setw(2) << static_cast<unsigned>(opcode)
           << " next_pc=" << std::setw(4) << r.pc
           << " af=" << std::setw(4)
           << static_cast<unsigned>((static_cast<unsigned>(r.a) << 8) | r.f)
           << " bc=" << std::setw(4)
           << static_cast<unsigned>((static_cast<unsigned>(r.b) << 8) | r.c)
           << " de=" << std::setw(4)
           << static_cast<unsigned>((static_cast<unsigned>(r.d) << 8) | r.e)
           << " hl=" << std::setw(4)
           << static_cast<unsigned>((static_cast<unsigned>(r.h) << 8) | r.l)
           << " sp=" << std::setw(4) << r.sp
           << " div=" << std::setw(2)
           << static_cast<unsigned>(bus.read8(0xFF04))
           << " tima=" << std::setw(2)
           << static_cast<unsigned>(bus.read8(0xFF05))
           << " tac=" << std::setw(2)
           << static_cast<unsigned>(bus.read8(0xFF07))
           << " if=" << std::setw(2)
           << static_cast<unsigned>(bus.read8(0xFF0F))
           << " ie=" << std::setw(2)
           << static_cast<unsigned>(bus.read8(0xFFFF))
           << " stat=" << std::setw(2)
           << static_cast<unsigned>(bus.read8(0xFF41))
           << " ly=" << std::setw(2)
           << static_cast<unsigned>(bus.read8(0xFF44))
           << " halted=" << (cpu.halted() ? 1 : 0)
           << " stopped=" << (cpu.stopped() ? 1 : 0) << std::dec << '\n';
}

bool contains_failure(const std::string& output) {
    return output.find("Failed") != std::string::npos ||
           output.find("FAILED") != std::string::npos ||
           output.find("failed") != std::string::npos;
}

bool has_blargg_signature(const gameboy::MemoryBus& bus) {
    return bus.read8(0xA001) == 0xDE && bus.read8(0xA002) == 0xB0 &&
           bus.read8(0xA003) == 0x61;
}

// GBMicrotest publishes its result in HRAM so a harness does not need to
// scrape the test's display. FF80 is the observed value, FF81 is the expected
// value, and FF82 is the completion flag (0x01 pass, 0xFF fail).
bool has_gbmicrotest_result(const gameboy::MemoryBus& bus) {
    const auto status = bus.read8(0xFF82);
    return status == 0x01 || status == 0xFF;
}

std::string blargg_output(const gameboy::MemoryBus& bus) {
    std::string output;
    for (std::uint16_t address = 0xA004; address < 0xC000; ++address) {
        const auto value = bus.read8(address);
        if (value == 0) break;
        output.push_back(static_cast<char>(value));
    }
    return output;
}

} // namespace

int main(int argc, char** argv) {
    try {
        const auto options = parse_options(argc, argv);
        std::ifstream input(options.rom_path, std::ios::binary);
        if (!input) {
            throw std::runtime_error("could not open ROM: " + options.rom_path);
        }
        std::vector<std::uint8_t> rom{
            std::istreambuf_iterator<char>{input}, {}};
        if (input.bad()) {
            throw std::runtime_error("could not read ROM: " + options.rom_path);
        }
        // Conformance runs must never load or create battery files beside the
        // fixtures: prior results would otherwise make a later run appear to
        // pass without executing the test.
        auto emulator = gameboy::Emulator{
            gameboy::Cartridge{std::move(rom)}, options.model,
            options.diagnostic_boot ? gameboy::BootRomMode::diagnostic
                                    : gameboy::BootRomMode::post_boot};
        if (options.sgb_frame &&
            emulator.hardware_model() != gameboy::HardwareModel::sgb &&
            emulator.hardware_model() != gameboy::HardwareModel::sgb2) {
            throw std::invalid_argument(
                "--sgb-frame requires an SGB or SGB2 hardware model");
        }
        gbb_sgb_input_script input_script{};
        if (!options.input_script.empty()) {
            char error[128]{};
            if (!gbb_sgb_input_load(options.input_script.string().c_str(),
                                    &input_script, error, sizeof(error))) {
                throw std::invalid_argument(std::string{"input script: "} + error);
            }
        }
        std::size_t next_input_event = 0;
        std::uint8_t held_buttons = 0;
        const auto apply_input = [&](const std::uint64_t frame) {
            if (next_input_event < input_script.count &&
                input_script.events[next_input_event].frame == frame) {
                const auto mask = input_script.events[next_input_event++].mask;
                set_held_buttons(emulator, held_buttons, mask);
                held_buttons = mask;
            }
        };
        apply_input(0);
        if (!options.sgb_replay_input.empty()) {
            const auto trace_text = read_text_file(options.sgb_replay_input);
            std::string error;
            const auto trace = gameboy::SgbTrace::parse(trace_text, &error);
            if (!trace.has_value()) {
                throw std::runtime_error("could not parse SGB trace: " + error);
            }
            const auto replay = gameboy::SgbTrace::replay(*trace, emulator);
            if (!replay.success) {
                std::cerr << "SGB replay failed after " << replay.writes_applied
                          << " writes and " << replay.checkpoints_checked
                          << " checkpoints: " << replay.error << '\n';
                return EXIT_FAILURE;
            }
            std::cout << "SGB replay passed: " << replay.writes_applied
                      << " writes, " << replay.checkpoints_checked
                      << " checkpoints\n";
            return EXIT_SUCCESS;
        }
        std::ofstream apu_trace;
        std::ofstream ppu_trace;
        std::ofstream io_trace;
        std::ofstream cpu_trace;
        std::optional<gameboy::SgbTrace::Recorder> sgb_recorder;
        if (!options.sgb_trace_output.empty()) {
            if (emulator.hardware_model() != gameboy::HardwareModel::sgb &&
                emulator.hardware_model() != gameboy::HardwareModel::sgb2) {
                throw std::runtime_error(
                    "--sgb-trace requires an SGB or SGB2 hardware model");
            }
            sgb_recorder.emplace(emulator.rom_fingerprint(),
                                 emulator.hardware_model());
        }
        if (!options.apu_trace_output.empty()) {
            if (options.apu_trace_output.has_parent_path()) {
                std::filesystem::create_directories(
                    options.apu_trace_output.parent_path());
            }
            apu_trace.open(options.apu_trace_output,
                           std::ios::out | std::ios::trunc);
            if (!apu_trace) throw std::runtime_error(
                "could not open APU trace: " + options.apu_trace_output.string());
            apu_trace << "trace_version=1 kind=apu\n";
        }
        if (!options.ppu_trace_output.empty()) {
            if (options.ppu_trace_output.has_parent_path()) {
                std::filesystem::create_directories(
                    options.ppu_trace_output.parent_path());
            }
            ppu_trace.open(options.ppu_trace_output,
                           std::ios::out | std::ios::trunc);
            if (!ppu_trace) throw std::runtime_error(
                "could not open PPU trace: " + options.ppu_trace_output.string());
            ppu_trace << "trace_version=1 kind=ppu\n";
        }
        if (!options.io_trace_output.empty()) {
            if (options.io_trace_output.has_parent_path()) {
                std::filesystem::create_directories(
                    options.io_trace_output.parent_path());
            }
            io_trace.open(options.io_trace_output,
                          std::ios::out | std::ios::trunc);
            if (!io_trace) throw std::runtime_error(
                "could not open I/O trace: " + options.io_trace_output.string());
            io_trace << "trace_version=1 kind=io\n";
        }
        if (!options.io_trace_output.empty() || sgb_recorder.has_value()) {
            emulator.bus().debug_enable_io_trace(true);
        }
        if (!options.cpu_trace_output.empty()) {
            if (options.cpu_trace_output.has_parent_path()) {
                std::filesystem::create_directories(
                    options.cpu_trace_output.parent_path());
            }
            cpu_trace.open(options.cpu_trace_output,
                           std::ios::out | std::ios::trunc);
            if (!cpu_trace) throw std::runtime_error(
                "could not open CPU trace: " + options.cpu_trace_output.string());
            cpu_trace << "trace_version=1 kind=cpu\n";
        }
        emulator.set_dmg_compatibility_colors(options.dmg_compatibility_colors);
        std::string serial_output;
        std::string memory_output;
        auto saw_blargg = false;
        std::array<std::uint16_t, 64> recent_pcs{};
        std::size_t recent_pc_next = 0;
        std::size_t recent_pc_count = 0;
        std::uint16_t last_low_rom_pc = 0x0100;
        std::uint64_t completed_frames = 0;
        std::uint64_t sgb_trace_frames = 0;
        std::uint64_t trace_records = 0;
        const bool captures_frame = options.frames != 0 ||
                                    options.frame_on_ld_bb ||
                                    options.frame_series_last != 0;
        const bool tracks_input = !options.input_script.empty();

        if (sgb_recorder.has_value()) {
            if (!sgb_recorder->checkpoint(0, 0, emulator)) {
                throw std::runtime_error("could not record initial SGB checkpoint");
            }
        }
        const auto finish_run = [&](const int status) {
            if (!sgb_recorder.has_value()) return status;
            std::string error;
            if (!write_sgb_trace(options.sgb_trace_output, sgb_recorder,
                                 emulator, completed_frames, error)) {
                std::cerr << "Could not write SGB trace: " << error << '\n';
                return EXIT_FAILURE;
            }
            return status;
        };

        while (emulator.cpu().total_cycles() < options.max_cycles) {
            if (options.frame_on_ld_bb &&
                emulator.bus().read8(emulator.cpu().registers().pc) == 0x40) {
                write_capture(options.frame_output, emulator,
                              options.sgb_frame);
                std::cout << "Captured LD B,B framebuffer to "
                          << options.frame_output << '\n';
                return finish_run(EXIT_SUCCESS);
            }
            const bool watches_blargg =
                !captures_frame &&
                (options.protocol == Protocol::automatic ||
                 options.protocol == Protocol::blargg);
            if (watches_blargg && has_blargg_signature(emulator.bus())) {
                saw_blargg = true;
                const auto current_output = blargg_output(emulator.bus());
                if (current_output.size() > memory_output.size()) {
                    std::cout << current_output.substr(memory_output.size())
                              << std::flush;
                    memory_output = current_output;
                }
                const auto status = emulator.bus().read8(0xA000);
                if ((status == 0 &&
                     memory_output.find("Passed") != std::string::npos) ||
                    (status != 0 && status != 0x80 &&
                     contains_failure(memory_output))) {
                    if (status == 0) {
                        std::cout << "\nPASS (Blargg memory)\n";
                        return finish_run(EXIT_SUCCESS);
                    }
                    std::cerr << "\nFAIL (Blargg result code "
                              << static_cast<unsigned>(status) << ")\n";
                    print_state(emulator.cpu());
                    print_recent_pcs(recent_pcs, recent_pc_next,
                                     recent_pc_count);
                    std::cerr << "Last low ROM PC=" << std::hex
                              << last_low_rom_pc << std::dec << '\n';
                    return finish_run(EXIT_FAILURE);
                }
            }

            const auto& registers = emulator.cpu().registers();
            const bool watches_mooneye =
                !captures_frame &&
                (options.protocol == Protocol::mooneye ||
                 options.protocol == Protocol::mooneye_wilbertpol ||
                 (options.protocol == Protocol::automatic && !saw_blargg));
            const auto mooneye_breakpoint =
                options.protocol == Protocol::mooneye_wilbertpol ? 0xED : 0x40;
            if (watches_mooneye &&
                emulator.bus().read8(registers.pc) == mooneye_breakpoint) {
                const auto automatic_failure_signature =
                    registers.b == 0x42 && registers.c == 0x42 &&
                    registers.d == 0x42 && registers.e == 0x42 &&
                    registers.h == 0x42 && registers.l == 0x42;
                if (mooneye_success(registers)) {
                    std::cout << "PASS (Mooneye)\n";
                    return finish_run(EXIT_SUCCESS);
                }
                if (options.protocol == Protocol::mooneye ||
                    options.protocol == Protocol::mooneye_wilbertpol ||
                    automatic_failure_signature) {
                    std::cerr << "FAIL (Mooneye result registers)\n";
                    print_state(emulator.cpu());
                    print_recent_pcs(recent_pcs, recent_pc_next,
                                     recent_pc_count);
                    std::cerr << "Last low ROM PC=" << std::hex
                              << last_low_rom_pc << std::dec << '\n';
                    print_hram_head(emulator.bus());
                    print_result_diagnostics(emulator.cpu(), emulator.bus());
                    print_video_diagnostics(emulator.bus());
                    print_audio_diagnostics(emulator.bus());
                    return finish_run(EXIT_FAILURE);
                }
            }

            recent_pcs[recent_pc_next] = registers.pc;
            if (registers.pc < 0x4000) last_low_rom_pc = registers.pc;
            recent_pc_next = (recent_pc_next + 1) % recent_pcs.size();
            recent_pc_count = std::min(recent_pc_count + 1,
                                       recent_pcs.size());
            const auto trace_cycle = emulator.cpu().total_cycles();
            const auto trace_pc = registers.pc;
            const bool watching_wram = options.watched_wram.has_value() &&
                completed_frames >= options.frame_series_first - 1;
            const auto watched_before = watching_wram
                ? emulator.bus().read8(*options.watched_wram) : std::uint8_t{};
            const auto trace_opcode = cpu_trace
                                          ? emulator.bus().read8(trace_pc)
                                          : std::uint8_t{};
            const auto stepped_cycles = emulator.step();
            if (watching_wram) {
                const auto watched_after = emulator.bus().read8(*options.watched_wram);
                if (watched_before != watched_after) {
                    std::cerr << "WRAM watch frame=" << completed_frames
                              << " cycle=" << trace_cycle << " pc=" << std::hex
                              << trace_pc << " address=" << *options.watched_wram
                              << " old=" << static_cast<unsigned>(watched_before)
                              << " new=" << static_cast<unsigned>(watched_after)
                              << std::dec << '\n';
                }
            }
            const auto tracing = options.trace_limit == 0 ||
                                 trace_records < options.trace_limit;
            if (tracing) {
                if (apu_trace) write_apu_trace(apu_trace, emulator.cpu(),
                                               emulator.bus());
                if (ppu_trace) write_ppu_trace(ppu_trace, emulator.cpu(),
                                               emulator.bus());
                if (cpu_trace) write_cpu_trace(
                    cpu_trace, emulator.cpu(), emulator.bus(), trace_cycle,
                    trace_pc, trace_opcode, stepped_cycles);
                ++trace_records;
            }
            const auto io_events = emulator.bus().debug_take_io_trace();
            if (sgb_recorder.has_value()) {
                for (const auto& event : io_events) {
                    if (event.address != 0xFF00 ||
                        !sgb_recorder->record_joypad_write(event.cycle,
                                                           event.value)) {
                        if (event.address == 0xFF00) {
                            throw std::runtime_error(
                                "SGB trace write limit or ordering was exceeded");
                        }
                        continue;
                    }
                }
            }
            if (io_trace && tracing) write_io_trace(io_trace, io_events);
            if (emulator.frame_ready()) {
                if (sgb_recorder.has_value()) {
                    ++sgb_trace_frames;
                    if (!sgb_recorder->checkpoint(emulator.cpu().total_cycles(),
                                                  sgb_trace_frames, emulator)) {
                        throw std::runtime_error(
                            "SGB trace checkpoint limit or ordering was exceeded");
                    }
                }
                if (captures_frame || tracks_input) {
                    ++completed_frames;
                    if (options.frame_series_last != 0 &&
                        completed_frames >= options.frame_series_first) {
                        const auto path = std::filesystem::path{
                            options.frame_series_prefix.string() + "-" +
                            std::to_string(completed_frames) + ".ppm"};
                        write_capture(path, emulator, options.sgb_frame);
                        if (options.frame_state_series) {
                            write_frame_state(std::filesystem::path{
                                options.frame_series_prefix.string() + "-" +
                                std::to_string(completed_frames) + ".state"},
                                emulator);
                        }
                        if (completed_frames == options.frame_series_last) {
                            std::cout << "Captured frame series through "
                                      << completed_frames << '\n';
                            return finish_run(EXIT_SUCCESS);
                        }
                    }
                    if (completed_frames == options.frames) {
                        write_capture(options.frame_output, emulator,
                                      options.sgb_frame);
                        std::cout << "Captured frame " << completed_frames << " to "
                                  << options.frame_output << '\n';
                        return finish_run(EXIT_SUCCESS);
                    }
                    emulator.consume_frame();
                    apply_input(completed_frames);
                } else if (sgb_recorder.has_value()) {
                    emulator.consume_frame();
                }
            }
            auto bytes = emulator.bus().take_serial_output();
            if (!bytes.empty()) {
                serial_output += bytes;
                std::cout << bytes << std::flush;
            }

            if (options.protocol == Protocol::gbmicrotest &&
                has_gbmicrotest_result(emulator.bus())) {
                const auto result = emulator.bus().read8(0xFF80);
                const auto expected = emulator.bus().read8(0xFF81);
                const auto status = emulator.bus().read8(0xFF82);
                // The GBMicrotest contract defines FF82 as the authoritative
                // completion/result flag. FF80/FF81 are diagnostic values and
                // are intentionally not required to match on every failing
                // or passing ROM (see the upstream test-roms-howto.md).
                if (status == 0x01) {
                    std::cout << "PASS (GBMicrotest) result=0x" << std::hex
                              << static_cast<unsigned>(result) << std::dec << '\n';
                    return finish_run(EXIT_SUCCESS);
                }
                std::cerr << "FAIL (GBMicrotest status=0x" << std::hex
                          << static_cast<unsigned>(status)
                          << " result=0x" << static_cast<unsigned>(result)
                          << " expected=0x" << static_cast<unsigned>(expected)
                          << std::dec << ")\n";
                print_state(emulator.cpu());
                print_recent_pcs(recent_pcs, recent_pc_next, recent_pc_count);
                print_hram_head(emulator.bus());
                print_result_diagnostics(emulator.cpu(), emulator.bus());
                print_video_diagnostics(emulator.bus());
                print_audio_diagnostics(emulator.bus());
                return finish_run(EXIT_FAILURE);
            }

            const bool watches_serial =
                !captures_frame &&
                (options.protocol == Protocol::serial ||
                 options.protocol == Protocol::automatic);
            if (watches_serial && serial_output.find("Passed") != std::string::npos) {
                std::cout << "\nPASS (serial)\n";
                return finish_run(EXIT_SUCCESS);
            }
            if (watches_serial && contains_failure(serial_output)) {
                std::cerr << "\nFAIL (serial)\n";
                print_state(emulator.cpu());
                print_recent_pcs(recent_pcs, recent_pc_next, recent_pc_count);
                std::cerr << "Last low ROM PC=" << std::hex << last_low_rom_pc
                          << std::dec << '\n';
                return finish_run(EXIT_FAILURE);
            }
        }

        std::cerr << "TIMEOUT after reaching the cycle limit\n";
        print_state(emulator.cpu());
        // Keep the same diagnostic memory window used by protocol failures.
        // Many hardware ROMs deliberately park in HRAM after a mismatch, so
        // this exposes their last observed value instead of leaving a timeout
        // indistinguishable from a harness hang.
        print_hram_head(emulator.bus());
        print_result_diagnostics(emulator.cpu(), emulator.bus());
        print_video_diagnostics(emulator.bus());
        print_audio_diagnostics(emulator.bus());
        return finish_run(2);
    } catch (const std::exception& error) {
        usage();
        std::cerr << "Error: " << error.what() << '\n';
        return 2;
    }
}
