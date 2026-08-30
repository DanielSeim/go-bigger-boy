#include "gameboy/emulator.hpp"
#include "gameboy/link_session.hpp"
#include "gameboy/tcp_link_channel.hpp"
#include "gameboy/tcp_serial_endpoint.hpp"

#include <chrono>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr std::uint64_t cycles_per_frame = 70'224;
constexpr std::uint64_t poll_interval_cycles = 1'024;

struct Options {
    std::filesystem::path rom;
    std::filesystem::path save1;
    std::filesystem::path save2;
    std::filesystem::path state1;
    std::filesystem::path state2;
    std::filesystem::path report;
    std::filesystem::path capture_dir;
    std::uint64_t frames = 1'200;
    std::uint16_t port = 0;
    bool local{};
    bool auto_confirm{};
};

void usage() {
    std::cerr
        << "Usage: gbb_link_harness --rom ROM --save1 SAVE --save2 SAVE "
           "[--state1 STATE --state2 STATE] "
           "[--transport tcp|local] [--frames N] [--port N] "
           "[--auto-confirm] [--report PATH]\n";
}

std::uint64_t parse_positive(const std::string& text, const char* name) {
    std::size_t consumed = 0;
    const auto value = std::stoull(text, &consumed);
    if (consumed != text.size() || value == 0) {
        throw std::invalid_argument(std::string(name) +
                                    " must be a positive integer");
    }
    return value;
}

Options parse_options(const int argc, char** argv) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        const auto require_value = [&](const char* name) -> std::string {
            if (index + 1 >= argc) {
                throw std::invalid_argument(std::string(name) +
                                            " requires a value");
            }
            return argv[++index];
        };
        if (argument == "--rom") {
            options.rom = require_value("--rom");
        } else if (argument == "--save1") {
            options.save1 = require_value("--save1");
        } else if (argument == "--save2") {
            options.save2 = require_value("--save2");
        } else if (argument == "--state1") {
            options.state1 = require_value("--state1");
        } else if (argument == "--state2") {
            options.state2 = require_value("--state2");
        } else if (argument == "--frames") {
            options.frames = parse_positive(require_value("--frames"),
                                            "--frames");
        } else if (argument == "--port") {
            const auto value = parse_positive(require_value("--port"),
                                              "--port");
            if (value > 65'535) {
                throw std::invalid_argument("--port is out of range");
            }
            options.port = static_cast<std::uint16_t>(value);
        } else if (argument == "--transport") {
            const auto value = require_value("--transport");
            if (value == "local") {
                options.local = true;
            } else if (value == "tcp") {
                options.local = false;
            } else {
                throw std::invalid_argument("unknown transport: " + value);
            }
        } else if (argument == "--auto-confirm") {
            options.auto_confirm = true;
        } else if (argument == "--report") {
            options.report = require_value("--report");
        } else if (argument == "--capture-dir") {
            options.capture_dir = require_value("--capture-dir");
        } else if (argument == "--help" || argument == "-h") {
            usage();
            std::exit(EXIT_SUCCESS);
        } else {
            throw std::invalid_argument("unknown option: " + argument);
        }
    }
    if (options.rom.empty() || options.save1.empty() || options.save2.empty()) {
        throw std::invalid_argument("--rom, --save1, and --save2 are required");
    }
    if (options.state1.empty() != options.state2.empty()) {
        throw std::invalid_argument("--state1 and --state2 must be provided together");
    }
    return options;
}

std::vector<std::uint8_t> read_bytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("could not open " + path.string());
    std::vector<std::uint8_t> bytes{
        std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    if (input.bad()) throw std::runtime_error("could not read " + path.string());
    return bytes;
}

std::uint64_t fingerprint(const std::vector<std::uint8_t>& bytes) {
    std::uint64_t hash = UINT64_C(1469598103934665603);
    for (const auto byte : bytes) {
        hash ^= byte;
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

std::string hex(const std::uint64_t value) {
    std::ostringstream output;
    output << "0x" << std::hex << std::setfill('0') << std::setw(16) << value;
    return output.str();
}

void write_report(const std::filesystem::path& path, const std::string& report) {
    if (path.empty()) return;
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path());
    }
    std::ofstream output(path, std::ios::trunc);
    if (!output) throw std::runtime_error("could not open report " + path.string());
    output << report;
    if (!output) throw std::runtime_error("could not write report " + path.string());
}

void write_frame(const std::filesystem::path& path,
                 const gameboy::Ppu::Framebuffer& framebuffer) {
    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) throw std::runtime_error("could not open frame " + path.string());
    output << "P6\n" << gameboy::Ppu::screen_width << ' '
           << gameboy::Ppu::screen_height << "\n255\n";
    for (const auto pixel : framebuffer) {
        const std::array<char, 3> rgb{
            static_cast<char>((pixel >> 16) & 0xFF),
            static_cast<char>((pixel >> 8) & 0xFF),
            static_cast<char>(pixel & 0xFF)};
        output.write(rgb.data(), static_cast<std::streamsize>(rgb.size()));
    }
}

void poll_pair(gameboy::TcpSerialEndpoint& first,
               gameboy::TcpSerialEndpoint& second) {
    first.poll();
    second.poll();
}

void reset_pokemon_link_handshake(gameboy::Emulator& emulator) {
    emulator.bus().write8(0xFFAA, 0xFF);
    emulator.bus().write8(0xFFAB, 0x00);
    emulator.bus().write8(0xFFAC, 0x00);
    emulator.bus().write8(0xFFAD, 0x00);
    emulator.bus().write8(0xFF01, 0x02);
    emulator.bus().write8(0xFF02, 0x80);
    emulator.bus().write8(
        0xFFFF, static_cast<std::uint8_t>(emulator.bus().read8(0xFFFF) | 0x08U));
    emulator.bus().write8(
        0xFF0F, static_cast<std::uint8_t>(emulator.bus().read8(0xFF0F) & ~0x08U));
}

void apply_auto_inputs(const Options& options, const std::uint64_t frame,
                       gameboy::Emulator& first, gameboy::Emulator& second) {
    if (!options.auto_confirm) return;
    constexpr std::uint64_t confirm_interval = 24;
    if (!options.state1.empty()) {
        if (frame % confirm_interval == 0) {
            first.set_button(gameboy::Button::a, true);
            if (frame >= 24) second.set_button(gameboy::Button::a, true);
        } else if (frame % confirm_interval == 1) {
            first.set_button(gameboy::Button::a, false);
            if (frame >= 24) second.set_button(gameboy::Button::a, false);
        }
        return;
    }
    // A battery save contains cartridge RAM, not CPU/WRAM state. Without a
    // full state, start from post-boot and enter the title menu first.
    if (frame == 180) {
        first.set_button(gameboy::Button::start, true);
        second.set_button(gameboy::Button::start, true);
    } else if (frame == 181) {
        first.set_button(gameboy::Button::start, false);
        second.set_button(gameboy::Button::start, false);
    } else if (frame == 186) {
        first.set_button(gameboy::Button::down, true);
        second.set_button(gameboy::Button::down, true);
    } else if (frame == 187) {
        first.set_button(gameboy::Button::down, false);
        second.set_button(gameboy::Button::down, false);
    } else if (frame == 192) {
        first.set_button(gameboy::Button::a, true);
    } else if (frame == 193) {
        first.set_button(gameboy::Button::a, false);
    } else if (frame == 216) {
        second.set_button(gameboy::Button::a, true);
    } else if (frame == 217) {
        second.set_button(gameboy::Button::a, false);
    } else if (frame >= 300 &&
               (frame - 300) % confirm_interval == 0) {
        first.set_button(gameboy::Button::a, true);
        if ((frame - 300) >= 24) second.set_button(gameboy::Button::a, true);
    } else if (frame >= 300 &&
               (frame - 300) % confirm_interval == 1) {
        first.set_button(gameboy::Button::a, false);
        if ((frame - 300) >= 24) second.set_button(gameboy::Button::a, false);
    }
}

void advance_pair(gameboy::Emulator& first, gameboy::Emulator& second,
                  gameboy::TcpSerialEndpoint& first_endpoint,
                  gameboy::TcpSerialEndpoint& second_endpoint,
                  const std::uint64_t target_cycles) {
    std::uint64_t next_poll = std::min(first.cpu().total_cycles(),
                                       second.cpu().total_cycles()) +
                              poll_interval_cycles;
    while (first.cpu().total_cycles() < target_cycles ||
           second.cpu().total_cycles() < target_cycles) {
        if (first.cpu().total_cycles() <= second.cpu().total_cycles()) {
            static_cast<void>(first.step());
        } else {
            static_cast<void>(second.step());
        }
        const auto now = std::min(first.cpu().total_cycles(),
                                  second.cpu().total_cycles());
        if (now >= next_poll) {
            poll_pair(first_endpoint, second_endpoint);
            next_poll = now + poll_interval_cycles;
        }
    }
    poll_pair(first_endpoint, second_endpoint);
}

} // namespace

int main(int argc, char** argv) {
    try {
        const auto options = parse_options(argc, argv);
        const auto rom_bytes = read_bytes(options.rom);
        const auto save1 = read_bytes(options.save1);
        const auto save2 = read_bytes(options.save2);

        gameboy::Emulator first{gameboy::Cartridge{rom_bytes}};
        gameboy::Emulator second{gameboy::Cartridge{rom_bytes}};
        first.import_battery_save(save1);
        second.import_battery_save(save2);
        if (!options.state1.empty()) {
            first.load_state(read_bytes(options.state1));
            second.load_state(read_bytes(options.state2));
        }
        const auto initial_save1 = first.export_battery_save();
        const auto initial_save2 = second.export_battery_save();
        const auto is_pokemon = first.bus().cartridge().title() == "POKEMON BLUE" ||
                                first.bus().cartridge().title() == "POKEMON RED" ||
                                first.bus().cartridge().title() == "POKEMON YELLOW";

        if (options.local) {
            if (is_pokemon) {
                reset_pokemon_link_handshake(first);
                reset_pokemon_link_handshake(second);
            }
            gameboy::LinkSession session;
            session.start(first, second);
            for (std::uint64_t frame = 0; frame < options.frames; ++frame) {
                apply_auto_inputs(options, frame, first, second);
                session.advance(static_cast<unsigned>(cycles_per_frame));
            }
            if (options.auto_confirm) {
                first.set_button(gameboy::Button::a, false);
                second.set_button(gameboy::Button::a, false);
            }
            const auto final_save1 = first.export_battery_save();
            const auto final_save2 = second.export_battery_save();
            std::ostringstream report;
            report << "GBB link integration report\n"
                   << "transport=local\n"
                   << "rom=" << options.rom << '\n'
                   << "title=" << first.bus().cartridge().title() << '\n'
               << "rom_fingerprint="
               << hex(first.bus().cartridge().rom_fingerprint()) << '\n'
               << "state_inputs=" << (!options.state1.empty() ? "yes" : "no") << '\n'
                   << "frames=" << options.frames << '\n'
                   << "cycles=" << options.frames * cycles_per_frame << '\n'
                   << "transfers_completed=" << session.transfers_completed() << '\n'
                   << "save1_before=" << hex(fingerprint(initial_save1)) << '\n'
                   << "save1_after=" << hex(fingerprint(final_save1)) << '\n'
                   << "save2_before=" << hex(fingerprint(initial_save2)) << '\n'
                   << "save2_after=" << hex(fingerprint(final_save2)) << '\n';
            session.stop();
            if (!options.capture_dir.empty()) {
                write_frame(options.capture_dir / "player1.ppm", first.framebuffer());
                write_frame(options.capture_dir / "player2.ppm", second.framebuffer());
            }
            std::cout << report.str();
            write_report(options.report, report.str());
            return EXIT_SUCCESS;
        }

        gameboy::TcpLinkChannel server;
        gameboy::TcpLinkChannel client;
        if (!server.listen(options.port) || server.local_port() == 0) {
            throw std::runtime_error("could not listen for TCP link harness");
        }
        if (!client.connect("127.0.0.1", server.local_port())) {
            throw std::runtime_error("could not start TCP link harness client");
        }

        gameboy::TcpSerialEndpoint first_endpoint;
        gameboy::TcpSerialEndpoint second_endpoint;
        first_endpoint.set_arbitration_priority(true);
        second_endpoint.set_arbitration_priority(false);
        if (is_pokemon) {
            reset_pokemon_link_handshake(first);
            reset_pokemon_link_handshake(second);
        }
        first_endpoint.attach(first.bus().serial_port(), client);
        second_endpoint.attach(second.bus().serial_port(), server);

        for (unsigned attempt = 0;
             attempt < 500 && !first_endpoint.peer_ready_for_link(); ++attempt) {
            poll_pair(first_endpoint, second_endpoint);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (!first_endpoint.peer_ready_for_link()) {
            throw std::runtime_error("TCP link handshake did not become ready");
        }

        for (std::uint64_t frame = 0; frame < options.frames; ++frame) {
            apply_auto_inputs(options, frame, first, second);
            advance_pair(first, second, first_endpoint, second_endpoint,
                         (frame + 1) * cycles_per_frame);
        }
        if (options.auto_confirm) {
            first.set_button(gameboy::Button::a, false);
            second.set_button(gameboy::Button::a, false);
        }

        const auto final_save1 = first.export_battery_save();
        const auto final_save2 = second.export_battery_save();
        std::ostringstream report;
        report << "GBB link integration report\n"
               << "rom=" << options.rom << '\n'
               << "title=" << first.bus().cartridge().title() << '\n'
               << "rom_fingerprint=" << hex(first.bus().cartridge().rom_fingerprint())
               << '\n'
               << "frames=" << options.frames << '\n'
               << "cycles=" << options.frames * cycles_per_frame << '\n'
               << "tcp_port=" << server.local_port() << '\n'
               << "host_requests_sent=" << first_endpoint.requests_sent() << '\n'
               << "host_requests_received=" << first_endpoint.requests_received() << '\n'
               << "host_responses_sent=" << first_endpoint.responses_sent() << '\n'
               << "host_responses_received=" << first_endpoint.responses_received() << '\n'
               << "join_requests_sent=" << second_endpoint.requests_sent() << '\n'
               << "join_requests_received=" << second_endpoint.requests_received() << '\n'
               << "join_responses_sent=" << second_endpoint.responses_sent() << '\n'
               << "join_responses_received=" << second_endpoint.responses_received() << '\n'
               << "host_unmatched_responses=" << first_endpoint.responses_unmatched() << '\n'
               << "join_unmatched_responses=" << second_endpoint.responses_unmatched() << '\n'
               << "host_transfers_completed=" << first_endpoint.transfers_completed() << '\n'
               << "join_transfers_completed=" << second_endpoint.transfers_completed() << '\n'
               << "save1_before=" << hex(fingerprint(initial_save1)) << '\n'
               << "save1_after=" << hex(fingerprint(final_save1)) << '\n'
               << "save2_before=" << hex(fingerprint(initial_save2)) << '\n'
               << "save2_after=" << hex(fingerprint(final_save2)) << '\n';
        std::cout << report.str();
        if (!options.capture_dir.empty()) {
            write_frame(options.capture_dir / "player1.ppm", first.framebuffer());
            write_frame(options.capture_dir / "player2.ppm", second.framebuffer());
        }
        write_report(options.report, report.str());

        first_endpoint.detach();
        second_endpoint.detach();
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "Error: " << error.what() << '\n';
        usage();
        return EXIT_FAILURE;
    }
}
