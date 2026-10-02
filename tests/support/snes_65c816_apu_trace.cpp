#include "snes_65c816_trace_cpu.hpp"
#include "snes_icd_gb_source.hpp"
#include "snes_dsp_pcm_renderer.hpp"
#include "snes_dsp_clock.hpp"
#include "gameboy/snes_apu_audio_engine.hpp"
#include "snes_apu_firmware_benchmark.hpp"
#include "gameboy/snes_spc700.hpp"

#include <array>
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace {

void write_pcm_wav(const std::filesystem::path& path,
                   const std::vector<sgb_test::SnesDspPcmRenderer::StereoSample>& samples,
                   unsigned sample_rate) {
    if (std::filesystem::exists(path))
        throw std::runtime_error("PCM output already exists: " + path.string());
    if (samples.size() > (UINT32_MAX - 44U) / 4U)
        throw std::runtime_error("PCM output exceeds WAV size limit");
    if (!path.parent_path().empty())
        std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    if (!output) throw std::runtime_error("could not create PCM output: " + path.string());
    const auto u16 = [&output](std::uint16_t value) {
        output.put(static_cast<char>(value));
        output.put(static_cast<char>(value >> 8));
    };
    const auto u32 = [&output](std::uint32_t value) {
        for (unsigned shift = 0; shift < 32; shift += 8)
            output.put(static_cast<char>(value >> shift));
    };
    const auto data_bytes = static_cast<std::uint32_t>(samples.size() * 4U);
    output.write("RIFF", 4);
    u32(data_bytes + 36U);
    output.write("WAVEfmt ", 8);
    u32(16);
    u16(1);
    u16(2);
    u32(sample_rate);
    u32(sample_rate * 4U);
    u16(4);
    u16(16);
    output.write("data", 4);
    u32(data_bytes);
    for (const auto& sample : samples) {
        u16(static_cast<std::uint16_t>(sample.left));
        u16(static_cast<std::uint16_t>(sample.right));
    }
    if (!output) throw std::runtime_error("could not finish PCM output: " + path.string());
}

} // namespace

int main(int argc, char** argv) {
    const bool trace = argc >= 3 && std::string_view(argv[argc - 1]) == "--trace";
    const bool upload = argc >= 3 && std::string_view(argv[argc - 1]) == "--upload";
    const bool upload_two = argc >= 3 &&
        std::string_view(argv[argc - 1]) == "--upload-two";
    const bool upload_three = argc >= 3 &&
        std::string_view(argv[argc - 1]) == "--upload-three";
    const bool upload_boot = argc >= 3 &&
        std::string_view(argv[argc - 1]) == "--upload-boot";
    const bool driver_probe = argc >= 3 &&
        std::string_view(argv[argc - 1]) == "--driver-probe";
    const bool sync_probe = argc >= 3 &&
        std::string_view(argv[argc - 1]) == "--sync-probe";
    const bool sync_gb = argc >= 6 &&
        (std::string_view(argv[3]) == "--sync-gb-sgb1" ||
         std::string_view(argv[3]) == "--sync-gb-sgb2");
    bool audible_sound_probe = false;
    bool clocked_dsp = false;
    bool bus_clocked_dsp = false;
    bool cycle_bus_dsp = false;
    bool shared_bus_dsp = false;
    bool cycle_apu_sync = false;
    bool fractional_apu_sync = false;
    bool core_apu_engine = false;
    bool core_apu_state_roundtrip = false;
    bool allow_unanchored_pcm = false;
    bool ppu_dma_timing = false;
    unsigned apu_clock_hz = 1024000;
    bool apu_clock_set = false;
    bool host_bus_timing = false;
    bool timer_poll_trace = false;
    bool native_gb_input = false;
    std::array<std::uint64_t, 2> history_window{};
    std::filesystem::path apu_bus_output_path;
    std::filesystem::path boot_timeline_path;
    std::filesystem::path host_startup_path;
    std::filesystem::path input_script_path;
    std::filesystem::path pcm_output_path;
    std::filesystem::path core_apu_benchmark_path;
        std::filesystem::path sound_event_trace_path;
        std::filesystem::path apu_ram_output_path;
    unsigned requested_instruction_limit = 0;
    if (sync_gb) {
        for (int index = 6; index < argc; ++index) {
            const std::string_view option(argv[index]);
            if (option == "--audible-sound-probe" && !audible_sound_probe) {
                audible_sound_probe = true;
            } else if (option == "--core-apu-engine" && !core_apu_engine) {
                core_apu_engine = true;
                fractional_apu_sync = cycle_apu_sync = shared_bus_dsp = cycle_bus_dsp =
                    bus_clocked_dsp = clocked_dsp = true;
            } else if (option == "--allow-unanchored-pcm" && !allow_unanchored_pcm) {
                allow_unanchored_pcm = true;
            } else if (option == "--core-apu-state-roundtrip" && !core_apu_state_roundtrip) {
                core_apu_state_roundtrip = true;
            } else if (option == "--core-apu-benchmark-output" && core_apu_benchmark_path.empty() && index + 1 < argc) {
                core_apu_benchmark_path = argv[++index];
            } else if (option == "--clocked-dsp" && !clocked_dsp) {
                clocked_dsp = true;
            } else if (option == "--bus-clocked-dsp" && !bus_clocked_dsp) {
                bus_clocked_dsp = true;
                clocked_dsp = true;
            } else if (option == "--cycle-bus-dsp" && !cycle_bus_dsp) {
                cycle_bus_dsp = true;
                bus_clocked_dsp = clocked_dsp = true;
            } else if (option == "--shared-bus-dsp" && !shared_bus_dsp) {
                shared_bus_dsp = cycle_bus_dsp = bus_clocked_dsp = clocked_dsp = true;
            } else if (option == "--cycle-apu-sync" && !cycle_apu_sync) {
                cycle_apu_sync = shared_bus_dsp = cycle_bus_dsp = bus_clocked_dsp = clocked_dsp = true;
            } else if (option == "--fractional-apu-sync" && !fractional_apu_sync) {
                fractional_apu_sync = cycle_apu_sync = shared_bus_dsp = cycle_bus_dsp = bus_clocked_dsp = clocked_dsp = true;
            } else if (option == "--apu-bus-output" && apu_bus_output_path.empty() && index + 1 < argc) {
                apu_bus_output_path = argv[++index];
            } else if (option == "--boot-timeline-output" && boot_timeline_path.empty() && index + 1 < argc) {
                boot_timeline_path = argv[++index];
            } else if (option == "--host-startup-output" && host_startup_path.empty() && index + 1 < argc) {
                host_startup_path = argv[++index];
            } else if (option == "--ppu-dma-timing" && !ppu_dma_timing) {
                ppu_dma_timing = true;
            } else if (option == "--host-bus-timing" && !host_bus_timing) {
                host_bus_timing = true;
            } else if (option == "--apu-clock-hz" && !apu_clock_set && index + 1 < argc) {
                apu_clock_set = true;
                const std::string_view number(argv[++index]);
                const auto [end, error] = std::from_chars(number.data(), number.data() + number.size(), apu_clock_hz);
                if (error != std::errc{} || end != number.data() + number.size() ||
                    apu_clock_hz < 1000000 || apu_clock_hz > 1100000 || apu_clock_hz % 32) {
                    std::cerr << "APU clock must be 1000000..1100000 Hz and divisible by 32\n"; return 2;
                }
            } else if (option == "--timer-poll-trace" && !timer_poll_trace) {
                timer_poll_trace = true;
            } else if (option == "--native-gb-input" && !native_gb_input) {
                native_gb_input = true;
            } else if (option == "--apu-history-window-half" && history_window[1] == 0 && index + 2 < argc) {
                for (auto& value : history_window) {
                    const std::string_view number(argv[++index]);
                    const auto [end, error] = std::from_chars(number.data(), number.data() + number.size(), value);
                    if (error != std::errc{} || end != number.data() + number.size()) return 2;
                }
                if (history_window[1] <= history_window[0] || history_window[1] - history_window[0] > 200000) {
                    std::cerr << "APU history window must span 1..200000 half clocks\n";
                    return 2;
                }
            } else if (option == "--input-script" &&
                       input_script_path.empty() && index + 1 < argc) {
                input_script_path = argv[++index];
            } else if (option == "--pcm-output" &&
                       pcm_output_path.empty() && index + 1 < argc) {
                pcm_output_path = argv[++index];
            } else if (option == "--sound-event-trace-output" &&
                       sound_event_trace_path.empty() && index + 1 < argc) {
                sound_event_trace_path = argv[++index];
            } else if (option == "--apu-ram-output" &&
                       apu_ram_output_path.empty() && index + 1 < argc) {
                apu_ram_output_path = argv[++index];
            } else if (option == "--instruction-limit" &&
                       requested_instruction_limit == 0 && index + 1 < argc) {
                const std::string_view number(argv[++index]);
                const auto [end, error] = std::from_chars(
                    number.data(), number.data() + number.size(),
                    requested_instruction_limit);
                if (error != std::errc{} || end != number.data() + number.size() ||
                    requested_instruction_limit == 0 ||
                    requested_instruction_limit > 500000000U) {
                    std::cerr << "instruction limit must be 1..500000000\n";
                    return 2;
                }
            } else {
                std::cerr << "unsupported synchronized GB option: " << option
                          << '\n';
                return 2;
            }
        }
    }
    if (apu_clock_set && !cycle_apu_sync) {
        std::cerr << "APU clock profile requires cycle or fractional APU synchronization\n";
        return 2;
    }
    if (core_apu_engine && (timer_poll_trace || history_window[1] ||
        !apu_bus_output_path.empty() || !boot_timeline_path.empty() ||
        !sound_event_trace_path.empty() || !host_startup_path.empty())) {
        std::cerr << "core APU engine mode currently exports PCM/RAM only, not legacy observer traces\n";
        return 2;
    }
    if ((core_apu_state_roundtrip || !core_apu_benchmark_path.empty()) && !core_apu_engine) {
        std::cerr << "core APU state roundtrip/benchmark requires --core-apu-engine\n";
        return 2;
    }
    const bool synchronized = sync_probe || sync_gb;
    const int path_count = sync_gb ? 2 : argc - 1 -
        (trace || upload || upload_two || upload_three || upload_boot || driver_probe ||
         sync_probe ? 1 : 0);
    if (path_count < 1 || path_count > 2 ||
        ((upload || upload_two || upload_three || upload_boot || driver_probe ||
          synchronized) && path_count != 2)) {
        std::cerr << "usage: gameboy_snes_65c816_apu_trace <SGB-program-ROM> "
                     "[64-byte-SPC700-IPL] [--trace|--upload|--upload-two|"
                     "--upload-three|--upload-boot|--driver-probe|--sync-probe|"
                     "--sync-gb-sgb1 GB-ROM GB-BOOT|"
                     "--sync-gb-sgb2 GB-ROM GB-BOOT"
                     " [--input-script PATH] [--pcm-output WAV] [--instruction-limit N]"
                     " [--sound-event-trace-output CSV]"
                     " [--clocked-dsp]"
                     " [--core-apu-engine]"
                     " [--core-apu-state-roundtrip]"
                     " [--core-apu-benchmark-output JSON]"
                     " [--allow-unanchored-pcm]"
                     " [--bus-clocked-dsp]"
                     " [--cycle-bus-dsp]"
                     " [--shared-bus-dsp]"
                     " [--cycle-apu-sync]"
                     " [--fractional-apu-sync]"
                     " [--native-gb-input] [--ppu-dma-timing] [--host-bus-timing]"
                     " [--apu-clock-hz HZ] [--host-startup-output JSON]"
                     " [--apu-bus-output JSON]"
                     " [--boot-timeline-output JSON]"
                     " [--timer-poll-trace]"
                     " [--apu-history-window-half START END]"
                     " [--audible-sound-probe]]\n";
        return 2;
    }
    try {
        const auto rom = gameboy::SgbProgramRom::from_file(
            std::filesystem::path(argv[1]));
        gameboy::SnesApuBus apu;
        if (path_count == 2) {
            if (std::filesystem::file_size(argv[2]) != 64) {
                std::cerr << "SPC700 IPL must be exactly 64 bytes\n";
                return 2;
            }
            gameboy::SnesApuBus::IplRom image{};
            std::ifstream input(argv[2], std::ios::binary);
            if (!input.read(reinterpret_cast<char*>(image.data()), image.size())) {
                std::cerr << "could not read complete SPC700 IPL\n";
                return 2;
            }
            apu.install_ipl(image);
        }
        gameboy::SnesSpc700 spc(apu);
        std::optional<gameboy::SnesApuAudioEngine> core_apu;
        if (core_apu_engine) core_apu.emplace(spc);
        sgb_test::Snes65c816TraceCpu cpu(rom, apu,
                                          path_count == 2 ? &spc : nullptr);
        struct BootTimeline {
            struct Event { char kind; std::uint64_t master, half, value, count, digest; };
            std::vector<Event> events;
            sgb_test::Snes65c816TraceCpu* cpu;
            gameboy::SnesSpc700* spc;
            bool enabled{}, overflow{}, command_seen{};
            bool ipl_a{}, ipl_ready{};
            unsigned host_ipl_reads{};
            void record(char kind, std::uint64_t master, std::uint64_t value,
                        std::uint64_t count = 0, std::uint64_t digest = 0) noexcept {
                if (!enabled) return;
                if (events.size() == 128) { overflow = true; return; }
                events.push_back({kind, master, spc->half_cycles(), value, count, digest});
            }
        } boot_timeline{{}, &cpu, &spc, !boot_timeline_path.empty()};
        std::vector<std::array<std::uint64_t, 11>> host_startup;
        std::vector<std::array<std::uint64_t, 7>> startup_dma;
        bool host_startup_finished = false;
        if (!host_startup_path.empty()) {
            if (boot_timeline_path.empty() || std::filesystem::exists(host_startup_path))
                throw std::runtime_error("host startup requires a boot timeline and unused output path");
            host_startup.reserve(131072);
            startup_dma.reserve(128);
            const auto host = std::filesystem::weakly_canonical(host_startup_path);
            for (const auto& path : {boot_timeline_path, apu_bus_output_path, pcm_output_path,
                                    sound_event_trace_path, apu_ram_output_path, input_script_path})
                if (!path.empty() && host == std::filesystem::weakly_canonical(path))
                    throw std::runtime_error("host startup output paths must differ");
        }
        if (boot_timeline.enabled) boot_timeline.events.reserve(128);
        std::unique_ptr<sgb_test::SnesIcdGbSource> icd;
        if (sync_gb) {
            const auto model = std::string_view(argv[3]) == "--sync-gb-sgb2"
                ? gameboy::HardwareModel::sgb2 : gameboy::HardwareModel::sgb;
            icd = std::make_unique<sgb_test::SnesIcdGbSource>(
                argv[4], argv[5], model);
            icd->set_native_gb_input(native_gb_input);
            icd->set_audible_sound_substitution(audible_sound_probe);
            if (boot_timeline.enabled) icd->set_boot_observer(
                [](void* context, char kind, std::uint64_t master, std::uint32_t value, std::uint64_t count) noexcept {
                    static_cast<BootTimeline*>(context)->record(kind, master, value, count);
                }, &boot_timeline);
            if (!input_script_path.empty())
                icd->load_input_script(input_script_path);
            if (audible_sound_probe)
                std::cerr << "SYNTHETIC SOUND payload substitution enabled;"
                             " not a title-authentic packet\n";
            cpu.set_icd_source(icd.get());
        }
        std::size_t printed = 0;
        std::uint8_t ports[4]{};
        bool transfer_started = false;
        unsigned completed_blocks = 0;
        std::array<sgb_test::Snes65c816TraceCpu::ApuWrite, 16> recent_apu_writes{};
        std::size_t recent_apu_next = 0;
        std::size_t recent_apu_count = 0;
        const unsigned requested_blocks = upload_boot || driver_probe || synchronized ? 0U :
            upload_three ? 3U : upload_two ? 2U : 1U;
        bool entry_command_seen = false;
        bool boot_handed_off = false;
        std::uint16_t entry_address = 0;
        struct SoundTraceEvent {
            char kind{};
            std::uint64_t master_clock{};
            std::uint64_t spc_cycle{};
            std::uint64_t pcm_sample{};
            std::uint16_t address{};
            std::uint32_t value{};
        };
        std::vector<SoundTraceEvent> sound_trace;
        struct ApuBusEvent {
            char kind;
            std::uint64_t master_clock, spc_half_clock, pcm_sample;
            std::uint16_t address;
            std::uint8_t value;
            unsigned dsp_clock64;
        };
        std::vector<ApuBusEvent> apu_trace;
        if (!boot_timeline_path.empty() && (!fractional_apu_sync || apu_bus_output_path.empty()))
            throw std::runtime_error("boot timeline requires fractional APU synchronization and APU bus output");
        if (native_gb_input && input_script_path.empty())
            throw std::runtime_error("native GB input requires an input script");
        if (!core_apu_benchmark_path.empty() &&
            (std::filesystem::exists(core_apu_benchmark_path) ||
             core_apu_benchmark_path == pcm_output_path || core_apu_benchmark_path == apu_ram_output_path))
            throw std::runtime_error("core APU benchmark requires an unused, distinct output path");
        if (!boot_timeline_path.empty() && std::filesystem::exists(boot_timeline_path))
            throw std::runtime_error("boot timeline already exists");
        if (!boot_timeline_path.empty() && (boot_timeline_path == apu_bus_output_path ||
                boot_timeline_path == pcm_output_path || boot_timeline_path == sound_event_trace_path))
            throw std::runtime_error("boot timeline output paths must differ");
        if (timer_poll_trace && apu_bus_output_path.empty())
            throw std::runtime_error("timer polling requires APU bus output");
        if (history_window[1] && !timer_poll_trace)
            throw std::runtime_error("APU history window requires timer polling");
        if (!apu_bus_output_path.empty()) {
            if (!cycle_apu_sync) throw std::runtime_error("APU bus trace requires cycle APU synchronization");
            apu_trace.reserve(262144);
        }
        if (timer_poll_trace)
            std::cout << "Timer polling and bounded driver-state trace enabled\n";
        gameboy::SnesApuBus pcm_bus;
        auto& render_bus = shared_bus_dsp ? apu : pcm_bus;
        sgb_test::SnesDspPcmRenderer pcm(render_bus);
        pcm.set_live_readback_enabled(shared_bus_dsp);
        sgb_test::SnesDspClock pcm_clock(pcm, render_bus);
        struct DspObservation {
            struct Event {
                std::uint64_t completed_cycle{};
                std::uint8_t opcode{};
                std::uint8_t address{};
                std::uint8_t value{};
            };
            std::array<Event, 16> events{};
            std::size_t count{};
            std::size_t printed{};
            std::uint64_t dsp_writes{};
            std::uint64_t dsp_hash{14695981039346656037ULL};
            unsigned pending{};
            std::uint8_t address{};
            std::uint8_t value{};
            std::array<std::pair<std::uint16_t, std::uint8_t>, 4> pending_ram{};
            unsigned pending_ram_count{};
            std::uint64_t ram_writes{};
            std::uint64_t ram_hash{14695981039346656037ULL};
            std::uint64_t first_ram_cycle{};
            bool unsupported{};
            bool pcm_unsupported{};
            sgb_test::SnesDspPcmRenderer* pcm{};
            sgb_test::SnesDspClock* clock{};
            gameboy::SnesApuBus* pcm_bus{};
            bool shared_bus{};
            bool fractional_bus{};
            bool timer_trace{};
            std::array<std::uint64_t, 2> history_window{};
            sgb_test::Snes65c816TraceCpu* cpu{};
            gameboy::SnesSpc700* spc{};
            std::vector<ApuBusEvent>* apu_trace{};
            BootTimeline* boot_timeline{};
            bool apu_trace_overflow{};
            const sgb_test::SnesIcdGbSource* icd{};
            std::uint64_t next_sample_cycle{};
            std::uint64_t pcm_samples{};
            std::uint64_t pcm_nonzero{};
            std::uint64_t post_sound_nonzero{};
            std::uint64_t post_audible_sound_nonzero{};
            std::uint64_t pcm_hash{14695981039346656037ULL};
            std::vector<sgb_test::SnesDspPcmRenderer::StereoSample>* pcm_export{};
            std::optional<std::uint64_t> first_audible_sample;
            std::optional<std::uint64_t> first_keyon_sample;
            std::optional<std::uint64_t> second_keyon_sample;
            unsigned state_checkpoints{};
            std::vector<SoundTraceEvent>* sound_trace{};
            sgb_test::Snes65c816TraceCpu::SpcStepObserver advance{};
            void record_bus(char kind, std::uint64_t half_clock,
                            std::uint16_t address, std::uint8_t value) noexcept {
                if (boot_timeline && kind == 'h' &&
                    ((address == 0x2140 && value == 0xaa) || (address == 0x2141 && value == 0xbb))) {
                    const unsigned flag = 1U << (address - 0x2140);
                    if (!(boot_timeline->host_ipl_reads & flag)) {
                        boot_timeline->host_ipl_reads |= flag;
                        const auto& r = cpu->registers();
                        boot_timeline->record('R', cpu->timing().clocks(), (unsigned(address) << 8) | value,
                                              (unsigned(r.pb) << 16) | r.pc);
                    }
                }
                if (boot_timeline && !boot_timeline->command_seen && kind == 'H' && address == 0x2140 &&
                    value == 1 && icd && icd->audible_sound_packets_delivered() != 0) {
                    boot_timeline->record('P', cpu->timing().clocks(), value);
                    boot_timeline->command_seen = true;
                }
                if (boot_timeline && kind == 'W' && (address == 0xf1 || address == 0xfa))
                    boot_timeline->record('T', cpu->timing().clocks(), (std::uint64_t(address) << 8) | value);
                if (boot_timeline && !boot_timeline->ipl_ready && kind == 'W') {
                    if (address == 0xf4 && value == 0xaa) boot_timeline->ipl_a = true;
                    if (address == 0xf5 && value == 0xbb && boot_timeline->ipl_a) {
                        boot_timeline->ipl_ready = true;
                        boot_timeline->record('I', cpu->timing().clocks(), 0xaabb);
                    }
                }
                const bool configuration = timer_trace && kind == 'W' &&
                    (address == 0xf0 || address == 0xf1 || (address >= 0xfa && address <= 0xfc));
                const bool phase_write = timer_trace && kind == 'W' && (address == 0x43 || address == 0xd8);
                const bool history = history_window[1] && half_clock >= history_window[0] && half_clock < history_window[1];
                if (!apu_trace || !icd || (!configuration && !phase_write && !history && icd->audible_sound_packets_delivered() == 0)) return;
                if (first_keyon_sample && pcm_samples >= *first_keyon_sample + 27200) return;
                const bool driver = timer_trace && (kind == 'R' || kind == 'W') && address < 0xf0;
                if (driver && first_keyon_sample && pcm_samples >= *first_keyon_sample + 250) return;
                if (driver) kind = kind == 'R' ? 'r' : 'w';
                if (kind == 'R' && (address < 0xf4 || address > 0xf7) &&
                    !(timer_trace && address >= 0xfd && address <= 0xff)) return;
                if (kind == 'W' && address != 0xf1 && address != 0xf3 &&
                    (address < 0xf4 || address > 0xf7) && !configuration) return;
                if (kind == 'W' && address == 0xf3 && pcm_bus->spc_read(0xf2) == 0x4c) kind = 'K';
                if (apu_trace->size() >= 262144) { apu_trace_overflow = true; return; }
                apu_trace->push_back({kind, cpu->timing().clocks(), half_clock,
                    pcm_samples, address, value, clock->key_poll_clock()});
            }
        } dsp_observation;
        std::vector<sgb_test::SnesDspPcmRenderer::StereoSample> pcm_export;
        struct ObserverScope {
            gameboy::SnesApuBus& apu;
            gameboy::SnesSpc700& spc;
            sgb_test::Snes65c816TraceCpu& cpu;
            ~ObserverScope() {
                apu.set_dsp_write_observer(nullptr);
                apu.set_spc_ram_write_observer(nullptr);
                spc.set_write_cycle_observer(nullptr);
                spc.set_bus_cycle_observer(nullptr);
                spc.set_half_cycle_observer(nullptr);
                cpu.set_spc_step_observer(nullptr);
                cpu.set_apu_port_observer(nullptr);
            }
        } observer_scope{apu, spc, cpu};
        if (sync_gb) {
            dsp_observation.pcm = &pcm;
            if (clocked_dsp) dsp_observation.clock = &pcm_clock;
            dsp_observation.pcm_bus = &render_bus;
            dsp_observation.shared_bus = shared_bus_dsp;
            dsp_observation.fractional_bus = fractional_apu_sync;
            dsp_observation.timer_trace = timer_poll_trace;
            dsp_observation.history_window = history_window;
            dsp_observation.boot_timeline = boot_timeline.enabled ? &boot_timeline : nullptr;
            dsp_observation.cpu = &cpu;
            dsp_observation.spc = &spc;
            if (!apu_bus_output_path.empty()) {
                dsp_observation.apu_trace = &apu_trace;
                cpu.set_apu_port_observer(
                    [](void* context, std::uint64_t, char kind, std::uint16_t address,
                       std::uint8_t value) noexcept {
                        auto& observed = *static_cast<DspObservation*>(context);
                        observed.record_bus(kind, observed.spc->half_cycles(), address, value);
                    }, &dsp_observation);
            }
            dsp_observation.icd = icd.get();
            if (!pcm_output_path.empty()) dsp_observation.pcm_export = &pcm_export;
            if (!sound_event_trace_path.empty())
                dsp_observation.sound_trace = &sound_trace;
        }
        std::vector<std::pair<std::uint16_t, std::size_t>> uploaded_ranges;
        std::uint8_t expected_index = 0;
        std::uint16_t destination = 0;
        std::vector<std::uint8_t> transferred;
        const unsigned instruction_bound = requested_instruction_limit != 0
            ? requested_instruction_limit : sync_gb ? 20000000U :
            ((upload || upload_two || upload_three || upload_boot ||
              driver_probe || synchronized) ? 5000000U : 1000000U);
        std::uint64_t logged_sound_deliveries{};
        const auto install_observation = [&] {
            apu.set_dsp_write_observer(
                [](void* context, std::uint8_t address,
                   std::uint8_t value) noexcept {
                    auto& observed = *static_cast<DspObservation*>(context);
                    ++observed.pending;
                    observed.address = address;
                    observed.value = value;
                }, &dsp_observation);
            apu.set_spc_ram_write_observer(
                [](void* context, std::uint16_t address,
                   std::uint8_t value) noexcept {
                    auto& observed = *static_cast<DspObservation*>(context);
                    if (observed.pending_ram_count ==
                        observed.pending_ram.size()) {
                        observed.unsupported = true;
                        return;
                    }
                    observed.pending_ram[observed.pending_ram_count++] =
                        {address, value};
                    if (observed.sound_trace != nullptr &&
                        observed.first_keyon_sample &&
                        observed.pcm_samples >=
                            *observed.first_keyon_sample + 22400 &&
                        observed.pcm_samples <
                            *observed.first_keyon_sample + 27200 &&
                        observed.sound_trace->size() < 32768) {
                        observed.sound_trace->push_back({
                            'R', 0, 0, observed.pcm_samples,
                            address, value});
                    }
                }, &dsp_observation);
            dsp_observation.advance =
                [](void* context, std::uint64_t cycle,
                   std::uint8_t opcode, unsigned) noexcept {
                    auto& observed = *static_cast<DspObservation*>(context);
                    if (observed.pcm != nullptr) {
                        if (observed.next_sample_cycle == 0)
                            observed.next_sample_cycle = cycle +
                                (observed.clock != nullptr ? 1 : 32);
                        while (observed.next_sample_cycle <= cycle &&
                               !observed.unsupported) {
                            const bool output = observed.clock == nullptr ||
                                observed.clock->phase() == 27;
                            const auto sample = observed.clock != nullptr
                                ? observed.clock->clock()
                                : observed.pcm->next_sample();
                            if (!output) {
                                ++observed.next_sample_cycle;
                                continue;
                            }
                            if (!sample) {
                                observed.unsupported = true;
                                observed.pcm_unsupported = true;
                                break;
                            }
                            if (observed.icd->audible_sound_packets_delivered() != 0 &&
                                !observed.first_audible_sample)
                                observed.first_audible_sample = observed.pcm_samples;
                            if (observed.pcm_export != nullptr)
                                observed.pcm_export->push_back(*sample);
                            ++observed.pcm_samples;
                            if (observed.sound_trace != nullptr &&
                                observed.first_keyon_sample &&
                                observed.state_checkpoints < 3) {
                                constexpr std::array<std::uint64_t, 2> offsets{
                                    24000, 26240}; // +0.75s, +0.82s at 32kHz
                                const auto target = observed.state_checkpoints < 2
                                    ? std::optional<std::uint64_t>{
                                        *observed.first_keyon_sample +
                                        offsets[observed.state_checkpoints]}
                                    : observed.second_keyon_sample
                                        ? std::optional<std::uint64_t>{
                                            *observed.second_keyon_sample + 640}
                                        : std::nullopt;
                                if (target && observed.pcm_samples >= *target) {
                                    for (unsigned voice = 0; voice < 8; ++voice)
                                        for (unsigned field = 0; field < 3; ++field)
                                            observed.sound_trace->push_back({
                                                'V', 0,
                                                observed.clock != nullptr ? observed.next_sample_cycle : 0,
                                                observed.pcm_samples,
                                                static_cast<std::uint16_t>(voice * 3 + field),
                                                observed.pcm->diagnostic_state(voice, field)});
                                    observed.sound_trace->push_back({
                                        'V', 0,
                                        observed.clock != nullptr ? observed.next_sample_cycle : 0,
                                        observed.pcm_samples, 24,
                                        observed.pcm->diagnostic_state(8, 0)});
                                    ++observed.state_checkpoints;
                                }
                            }
                            if (sample->left != 0 || sample->right != 0) {
                                ++observed.pcm_nonzero;
                                if (observed.icd->sound_packets_delivered() != 0)
                                    ++observed.post_sound_nonzero;
                                if (observed.icd->audible_sound_packets_delivered() != 0)
                                    ++observed.post_audible_sound_nonzero;
                            }
                            for (const auto value : {sample->left,
                                                     sample->right}) {
                                const auto bits = static_cast<std::uint16_t>(value);
                                observed.pcm_hash =
                                    (observed.pcm_hash ^
                                     static_cast<std::uint8_t>(bits)) *
                                    1099511628211ULL;
                                observed.pcm_hash =
                                    (observed.pcm_hash ^
                                     static_cast<std::uint8_t>(bits >> 8)) *
                                    1099511628211ULL;
                            }
                            observed.next_sample_cycle +=
                                observed.clock != nullptr ? 1 : 32;
                        }
                    }
                    for (unsigned index = 0;
                         index < observed.pending_ram_count; ++index) {
                        const auto [address, value] =
                            observed.pending_ram[index];
                        if (observed.pcm_bus != nullptr && !observed.shared_bus)
                            observed.pcm_bus->dsp_write_ram(address, value);
                        if (observed.ram_writes == 0)
                            observed.first_ram_cycle = cycle;
                        ++observed.ram_writes;
                        const auto fold = [&](std::uint8_t byte) {
                            observed.ram_hash =
                                (observed.ram_hash ^ byte) *
                                1099511628211ULL;
                        };
                        fold(static_cast<std::uint8_t>(address));
                        fold(static_cast<std::uint8_t>(address >> 8));
                        fold(value);
                        for (unsigned shift = 0; shift < 64; shift += 8)
                            fold(static_cast<std::uint8_t>(cycle >> shift));
                    }
                    observed.pending_ram_count = 0;
                    if (observed.pending != 0) {
                        if (observed.pending != 1) {
                            observed.unsupported = true;
                        } else {
                            ++observed.dsp_writes;
                            const auto fold = [&](std::uint8_t byte) {
                                observed.dsp_hash =
                                    (observed.dsp_hash ^ byte) *
                                    1099511628211ULL;
                            };
                            fold(opcode);
                            fold(observed.address);
                            fold(observed.value);
                            for (unsigned shift = 0; shift < 64; shift += 8)
                                fold(static_cast<std::uint8_t>(cycle >> shift));
                            if (observed.count < observed.events.size()) {
                                observed.events[observed.count++] = {
                                    cycle, opcode, observed.address,
                                    observed.value};
                            }
                            if (observed.address == 0x4c &&
                                observed.value != 0 &&
                                observed.icd->audible_sound_packets_delivered() != 0) {
                                if (!observed.first_keyon_sample)
                                    observed.first_keyon_sample = observed.pcm_samples;
                                else if (!observed.second_keyon_sample)
                                    observed.second_keyon_sample = observed.pcm_samples;
                            }
                            if (observed.sound_trace != nullptr &&
                                observed.icd->audible_sound_packets_delivered() != 0 &&
                                observed.sound_trace->size() < 32768) {
                                observed.sound_trace->push_back({
                                    'D', 0, cycle, observed.pcm_samples,
                                    observed.address, observed.value});
                            }
                            if (observed.pcm != nullptr) {
                                if (observed.shared_bus)
                                    observed.pcm->accept_dsp_write(observed.address, observed.value);
                                else
                                    observed.pcm->write_dsp(observed.address, observed.value);
                            }
                            if (observed.clock != nullptr &&
                                observed.sound_trace != nullptr &&
                                observed.address == 0x4c && observed.value != 0 &&
                                observed.icd->audible_sound_packets_delivered() != 0 &&
                                observed.sound_trace->size() < 32768)
                                observed.sound_trace->push_back({
                                    'Q', 0, cycle, observed.pcm_samples,
                                    static_cast<std::uint16_t>(observed.clock->key_poll_clock()),
                                    observed.value});
                        }
                    }
                    observed.pending = 0;
                };
            cpu.set_spc_step_observer(dsp_observation.advance, &dsp_observation);
        };
        if (bus_clocked_dsp && !core_apu_engine) {
            if (!shared_bus_dsp) pcm_bus = apu;
            pcm.reset();
            pcm_clock.reset();
            if (shared_bus_dsp) {
                // Start muted, soft-reset and with echo writes disabled. A
                // zeroed FLG would let the DSP overwrite the live IPL upload.
                render_bus.dsp_publish_register(0x6C, 0xE0);
                pcm.accept_dsp_write(0x6C, 0xE0);
            }
            dsp_observation.next_sample_cycle = 1;
            install_observation();
            spc.set_write_cycle_observer(
                [](void* context, std::uint64_t cycle, std::uint8_t opcode,
                   std::uint16_t, std::uint8_t, bool) noexcept {
                    auto& observed = *static_cast<DspObservation*>(context);
                    if (cycle == 0) { observed.unsupported = true; return; }
                    // Before: advance DSP up to the write. After: commit RAM
                    // and DSP writes at the same boundary, before any idle tail.
                    observed.advance(context, cycle, opcode, 0);
                }, &dsp_observation);
            if (cycle_bus_dsp) {
                spc.set_cycle_bus_enabled(true);
                spc.set_bus_cycle_observer(
                    [](void* context, std::uint64_t cycle, char kind,
                       std::uint16_t address, std::uint8_t value) noexcept {
                        auto& observed = *static_cast<DspObservation*>(context);
                        if (kind == 'T') observed.advance(context, cycle, 0, 0);
                        else if (!observed.fractional_bus && (kind == 'R' || kind == 'W'))
                            observed.record_bus(kind, cycle * 2, address, value);
                    }, &dsp_observation);
                std::cout << "SPC cycle-level reads, dummy accesses and timers enabled\n";
            }
            if (shared_bus_dsp)
                std::cout << "Shared SPC/DSP APU RAM and live register readback enabled\n";
            if (cycle_apu_sync) {
                cpu.set_cycle_apu_sync_enabled(true);
                std::cout << "Cycle-level SNES/SPC APU rendezvous enabled\n";
            }
            if (fractional_apu_sync) {
                cpu.set_fractional_apu_sync_enabled(true);
                spc.set_half_cycle_observer(
                    [](void* context, std::uint64_t half, char kind,
                       std::uint16_t address, std::uint8_t value) noexcept {
                        if (kind == 'R' || kind == 'W')
                            static_cast<DspObservation*>(context)->record_bus(kind, half, address, value);
                    }, &dsp_observation);
                std::cout << "Fractional APU ports: SPC midpoint reads and SNES four-clock read tail enabled\n";
            }
            std::cout << "DSP clock from SPC reset; write-boundary observation enabled\n";
        }
        struct CoreApuDriver {
            gameboy::SnesApuAudioEngine* engine;
            DspObservation* observed;
            bool roundtrip;
            unsigned until_restore{8192};
            bool benchmark{};
            std::vector<std::uint8_t> benchmark_state;
        } core_driver{core_apu ? &*core_apu : nullptr, &dsp_observation, core_apu_state_roundtrip,
                      8192, false, {}};
        core_driver.benchmark = !core_apu_benchmark_path.empty();
        if (core_apu_engine) {
            cpu.set_fractional_apu_sync_enabled(true);
            cpu.set_apu_half_driver([](void* context) noexcept {
                auto& driver = *static_cast<CoreApuDriver*>(context);
                if (!driver.engine->clock_half()) return false;
                gameboy::SnesApuAudioEngine::StereoSample sample;
                auto& observed = *driver.observed;
                while (driver.engine->pop_sample(sample)) {
                    if (observed.icd->audible_sound_packets_delivered() && !observed.first_audible_sample)
                        observed.first_audible_sample = observed.pcm_samples;
                    if (observed.pcm_export) observed.pcm_export->push_back(sample);
                    ++observed.pcm_samples;
                    if (sample.left || sample.right) ++observed.pcm_nonzero;
                    if (driver.roundtrip && !--driver.until_restore) {
                        const auto state = driver.engine->save_state();
                        if (!driver.engine->load_state(state)) return false;
                        driver.until_restore = 8192;
                    }
                    if (driver.benchmark && driver.benchmark_state.empty() && observed.first_audible_sample &&
                        observed.pcm_samples >= *observed.first_audible_sample + 8192)
                        driver.benchmark_state = driver.engine->save_state();
                }
                return true;
            }, &core_driver);
            std::cout << "Reusable core SPC700/DSP scheduler enabled; frontend playback remains disabled\n";
        }
        cpu.set_ppu_dma_timing_enabled(ppu_dma_timing);
        cpu.set_host_bus_timing_enabled(host_bus_timing);
        if (!cpu.set_apu_clock_hz(apu_clock_hz)) throw std::runtime_error("invalid APU clock profile");
        for (unsigned i = 0; i < instruction_bound; ++i) {
            if (!host_startup_path.empty() && boot_timeline.host_ipl_reads == 3 && !host_startup_finished) {
                if (host_startup.size() == 131072) throw std::runtime_error("host startup trace overflowed");
                const auto& r = cpu.registers();
                host_startup.push_back({cpu.timing().clocks(), (unsigned(r.pb) << 16) | r.pc,
                    r.a, r.x, r.y, r.s, r.d, r.db, r.p, cpu.timing().line(), cpu.timing().horizontal_clock()});
            }
            const auto dma_before = cpu.dma_start_count();
            const auto result = cpu.step();
            if (!host_startup_path.empty() && !host_startup_finished &&
                boot_timeline.host_ipl_reads == 3 && cpu.dma_start_count() != dma_before) {
                for (unsigned ch = 0; ch < 8; ++ch) {
                    if (!(cpu.last_dma_mask() & (1U << ch))) continue;
                    if (startup_dma.size() == 128) throw std::runtime_error("startup DMA trace overflowed");
                    startup_dma.push_back({cpu.timing().clocks(), cpu.last_dma_mask(), ch,
                        cpu.dma_register(ch, 0), cpu.dma_register(ch, 1),
                        (unsigned(cpu.dma_register(ch, 4)) << 16) |
                        (unsigned(cpu.dma_register(ch, 3)) << 8) | cpu.dma_register(ch, 2),
                        (unsigned(cpu.dma_register(ch, 6)) << 8) | cpu.dma_register(ch, 5)});
                }
            }
            if (icd && icd->sound_packets_delivered() > logged_sound_deliveries) {
                logged_sound_deliveries = icd->sound_packets_delivered();
                if (!sound_event_trace_path.empty() &&
                    icd->audible_sound_packets_delivered() == 1)
                    sound_trace.push_back({'P', cpu.timing().clocks(), 0,
                                           dsp_observation.pcm_samples, 0, 0});
                std::cout << "host SOUND delivery index="
                          << logged_sound_deliveries - 1
                          << " GB_frame=" << icd->completed_frames()
                          << " GB_cycle=" << icd->gb_cycles()
                          << " SNES_master_clock=" << cpu.timing().clocks()
                          << " PCM_sample=" << dsp_observation.pcm_samples
                          << " packet=";
                for (const auto byte : icd->last_delivered_sound_packet())
                    std::cout << ' ' << std::hex << std::setw(2)
                              << std::setfill('0') << static_cast<unsigned>(byte);
                std::cout << std::dec << '\n';
            }
            if (sync_gb) {
                for (std::size_t event = 0; event < cpu.apu_write_count(); ++event) {
                    const auto write = cpu.apu_write(event);
                    recent_apu_writes[recent_apu_next] = write;
                    recent_apu_next = (recent_apu_next + 1) % recent_apu_writes.size();
                    if (recent_apu_count < recent_apu_writes.size())
                        ++recent_apu_count;
                    if (!sound_event_trace_path.empty() &&
                        icd->audible_sound_packets_delivered() != 0 &&
                        sound_trace.size() < 32768) {
                        sound_trace.push_back({'H', cpu.timing().clocks(), 0,
                                               dsp_observation.pcm_samples,
                                               write.port, write.value});
                    }
                }
            }
            if (icd) {
                icd->advance_to(cpu.timing().clocks());
                if (icd->missing_address() != 0) {
                    std::cerr << "GB ICD source stopped at missing/overflowed $"
                              << std::hex << std::setw(4) << std::setfill('0')
                              << icd->missing_address() << std::dec
                              << " after GB frame " << icd->completed_frames()
                              << "; host consumed " << icd->packets_delivered()
                              << " of " << icd->packets_completed()
                              << " packets; audible SOUND generated="
                              << icd->audible_sound_commands()
                              << "; observed PCM samples="
                              << dsp_observation.pcm_samples
                              << " nonzero=" << dsp_observation.pcm_nonzero
                              << " post_initial_SOUND_nonzero="
                              << dsp_observation.post_sound_nonzero << '\n';
                    if (icd->audible_sound_commands() != 0) {
                        std::cerr << "first audible SOUND frame="
                                  << icd->first_audible_frame() << " packet=";
                        for (const auto byte : icd->first_audible_sound_packet())
                            std::cerr << ' ' << std::hex << std::setw(2)
                                      << static_cast<unsigned>(byte);
                        std::cerr << std::dec << '\n';
                    }
                    return 5;
                }
            }
            if (synchronized) {
                while (dsp_observation.printed < dsp_observation.count) {
                    const auto& event = dsp_observation.events[dsp_observation.printed++];
                    std::cout << (bus_clocked_dsp
                                      ? "synchronized DSP write at SPC bus write cycle "
                                      : "synchronized DSP write at SPC instruction end cycle ")
                              << std::dec << event.completed_cycle << " opcode $"
                              << std::hex << std::setw(2) << std::setfill('0')
                              << static_cast<unsigned>(event.opcode) << " register $"
                              << std::setw(2) << static_cast<unsigned>(event.address)
                              << "=$" << std::setw(2)
                              << static_cast<unsigned>(event.value) << std::dec << '\n';
                }
                if (dsp_observation.unsupported) {
                    apu.set_dsp_write_observer(nullptr);
                    apu.set_spc_ram_write_observer(nullptr);
                    cpu.set_spc_step_observer(nullptr);
                    std::cerr << (dsp_observation.pcm_unsupported
                                      ? "synchronized PCM renderer rejected a sample\n"
                                      : "synchronized DSP observation multi-write"
                                        " SPC instruction\n");
                    return 5;
                }
            }
            if ((upload_boot || driver_probe || synchronized) && entry_command_seen &&
                result.error == sgb_test::Snes65c816TraceCpu::Error::none &&
                spc.registers().pc < 0xFFC0) {
                if (!boot_handed_off) {
                    std::cout << "SPC700 entered uploaded program at $" << std::hex
                              << std::setw(4) << std::setfill('0') << spc.registers().pc
                              << " (entry $" << std::setw(4) << entry_address
                              << ") after " << std::dec << cpu.steps()
                              << " SNES steps\n";
                }
                if (synchronized) {
                    if (!boot_handed_off) {
                        boot_handed_off = true;
                        if (sync_gb && !bus_clocked_dsp) {
                            pcm_bus = apu;
                            pcm.reset();
                            pcm_clock.reset();
                        }
                        if (!bus_clocked_dsp) install_observation();
                    }
                    cpu.clear_apu_writes();
                    continue;
                }
                if (driver_probe) {
                    struct FirstDspWrite {
                        bool seen{};
                        std::uint8_t address{};
                        std::uint8_t value{};
                    } first_dsp;
                    apu.set_dsp_write_observer(
                        [](void* context, std::uint8_t address,
                           std::uint8_t value) noexcept {
                            auto& first = *static_cast<FirstDspWrite*>(context);
                            if (!first.seen) {
                                first.seen = true;
                                first.address = address;
                                first.value = value;
                            }
                        }, &first_dsp);
                    for (unsigned step = 0; step < 100000 && !first_dsp.seen; ++step) {
                        const auto pc = spc.registers().pc;
                        const auto next = spc.step();
                        if (!next.supported) {
                            apu.set_dsp_write_observer(nullptr);
                            std::cerr << "SPC700 driver stopped at $" << std::hex
                                      << std::setw(4) << std::setfill('0') << pc
                                      << " unsupported opcode $" << std::setw(2)
                                      << static_cast<unsigned>(next.opcode)
                                      << std::dec << " after " << step
                                      << " driver instructions\n";
                            return 3;
                        }
                    }
                    apu.set_dsp_write_observer(nullptr);
                    if (!first_dsp.seen) {
                        std::cerr << "SPC700 driver reached instruction bound"
                                     " before first DSP write\n";
                        return 4;
                    }
                    std::cout << "first uploaded-driver DSP write: $" << std::hex
                              << std::setw(2) << std::setfill('0')
                              << static_cast<unsigned>(first_dsp.address)
                              << "=$" << std::setw(2)
                              << static_cast<unsigned>(first_dsp.value)
                              << std::dec << '\n';
                }
                return 0;
            }
            if (result.error != sgb_test::Snes65c816TraceCpu::Error::none) {
                if (synchronized) {
                    apu.set_dsp_write_observer(nullptr);
                    apu.set_spc_ram_write_observer(nullptr);
                    cpu.set_spc_step_observer(nullptr);
                    std::cout << "post-handoff SPC RAM writes=" << std::dec
                              << dsp_observation.ram_writes
                              << " first_instruction_end_cycle="
                              << dsp_observation.first_ram_cycle << " fnv64="
                              << std::hex << std::setw(16) << std::setfill('0')
                              << dsp_observation.ram_hash << std::dec << '\n';
                    std::cout << "post-handoff DSP writes="
                              << dsp_observation.dsp_writes << " fnv64="
                              << std::hex << std::setw(16) << std::setfill('0')
                              << dsp_observation.dsp_hash << std::dec << '\n';
                    if (sync_gb)
                        std::cout << "post-handoff PCM samples="
                                  << dsp_observation.pcm_samples
                                  << " nonzero=" << dsp_observation.pcm_nonzero
                                  << " post_SOUND_nonzero="
                                  << dsp_observation.post_sound_nonzero
                                  << " post_audible_SOUND_nonzero="
                                  << dsp_observation.post_audible_sound_nonzero
                                  << " fnv64=" << std::hex << std::setw(16)
                                  << std::setfill('0') << dsp_observation.pcm_hash
                                  << std::dec << '\n';
                }
                if (icd) {
                    std::cout << "GB ICD source cycles=" << icd->gb_cycles()
                              << " packets=" << icd->packets_completed()
                              << " SOUND=" << icd->sound_commands()
                              << " delivered=" << icd->packets_delivered()
                              << " SOUND_delivered="
                              << icd->sound_packets_delivered()
                              << " audible_SOUND_delivered="
                              << icd->audible_sound_packets_delivered()
                              << " audible_SOUND="
                              << icd->audible_sound_commands()
                              << " GB_frames=" << icd->completed_frames()
                              << " input_events=" << icd->input_events_applied()
                              << " SOU_TRN=" << icd->transfer_commands()
                              << " control_writes=" << icd->control_writes()
                              << " last_control=$" << std::hex << std::setw(2)
                              << static_cast<unsigned>(icd->last_control())
                              << " missing=$" << std::hex << std::setw(4)
                              << std::setfill('0') << icd->missing_address()
                              << std::dec << '\n';
                    if (icd->sound_commands() != 0) {
                        std::cout << "first SOUND packet=";
                        for (const auto byte : icd->first_sound_packet())
                            std::cout << ' ' << std::hex << std::setw(2)
                                      << std::setfill('0')
                                      << static_cast<unsigned>(byte);
                        std::cout << std::dec << '\n';
                    }
                    if (icd->audible_sound_commands() != 0) {
                        std::cout << "first audible SOUND frame="
                                  << icd->first_audible_frame() << " packet=";
                        for (const auto byte : icd->first_audible_sound_packet())
                            std::cout << ' ' << std::hex << std::setw(2)
                                      << std::setfill('0')
                                      << static_cast<unsigned>(byte);
                        std::cout << std::dec << '\n';
                    }
                }
                std::cerr << "SNES CPU trace stopped after " << cpu.steps()
                          << " instructions at $" << std::hex << std::setw(2)
                          << std::setfill('0') << static_cast<unsigned>(result.bank)
                          << ':' << std::setw(4) << result.pc;
                if (result.error == sgb_test::Snes65c816TraceCpu::Error::unsupported_opcode) {
                    std::cerr << " unsupported opcode $" << std::setw(2)
                              << static_cast<unsigned>(result.opcode);
                } else if (result.error ==
                           sgb_test::Snes65c816TraceCpu::Error::unsupported_spc_opcode) {
                    std::cerr << " unsupported SPC700 opcode at $"
                              << std::setw(4) << result.address << " ($"
                              << std::setw(2) << static_cast<unsigned>(
                                     apu.dsp_read_ram(static_cast<std::uint16_t>(
                                         result.address))) << ')';
                } else {
                    std::cerr << " unsupported I/O/mapping access $"
                              << std::setw(6) << result.address;
                }
                std::cerr << std::dec << '\n';
                if ((result.address & 0xFFFF) == 0x420B) {
                    std::cerr << "Rejected DMA mask=$" << std::hex << unsigned(cpu.last_dma_mask());
                    for (unsigned ch = 0; ch < 8; ++ch) {
                        if (!(cpu.last_dma_mask() & (1U << ch))) continue;
                        std::cerr << " channel=" << ch << " registers=";
                        for (unsigned j = 0; j < 7; ++j)
                            std::cerr << std::setw(2) << std::setfill('0') << unsigned(cpu.dma_register(ch, j));
                    }
                    std::cerr << std::dec << '\n';
                }
                std::cerr << "master clocks " << cpu.timing().clocks()
                          << " V=" << cpu.timing().line()
                          << " H=" << cpu.timing().horizontal_clock()
                          << " IRQ_entries=" << cpu.irq_entries() << '\n';
                return 3;
            }
            for (std::size_t event = 0; event < cpu.apu_write_count(); ++event) {
                const auto write = cpu.apu_write(event);
                ++printed;
                if (synchronized && boot_handed_off) continue;
                if (upload || upload_two || upload_three || upload_boot || driver_probe ||
                    synchronized) {
                    ports[write.port] = write.value;
                    if (!transfer_started && write.port == 0 &&
                        write.value == 0xCC && ports[1] == 1) {
                        transfer_started = true;
                        destination = static_cast<std::uint16_t>(
                            ports[2] | (static_cast<unsigned>(ports[3]) << 8));
                        boot_timeline.record('A', cpu.timing().clocks(), destination);
                        host_startup_finished = true;
                    } else if (transfer_started && write.port == 0) {
                        if (write.value == expected_index) {
                            transferred.push_back(ports[1]);
                            ++expected_index;
                        } else if (write.value ==
                                   static_cast<std::uint8_t>(expected_index + 1)) {
                            if (transferred.empty()) {
                                std::cerr << "empty APU upload block\n";
                                return 5;
                            }
                            std::uint64_t hash = 14695981039346656037ULL;
                            for (std::size_t index = 0; index < transferred.size(); ++index) {
                                const auto actual = apu.dsp_read_ram(
                                    static_cast<std::uint16_t>(destination + index));
                                if (actual != transferred[index]) {
                                    std::cerr << "APU upload RAM mismatch at block "
                                              << completed_blocks + 1 << " byte " << index << '\n';
                                    return 5;
                                }
                                hash = (hash ^ actual) * 1099511628211ULL;
                            }
                            if (completed_blocks < 3) {
                                std::cout << (completed_blocks == 0 ? "first" :
                                              completed_blocks == 1 ? "second" : "third")
                                          << " APU upload block: ";
                            } else {
                                std::cout << "APU upload block " << completed_blocks + 1
                                          << ": ";
                            }
                            std::cout << "destination $" << std::hex
                                      << std::setw(4) << std::setfill('0') << destination
                                      << " bytes=" << std::dec << transferred.size()
                                      << " fnv64=" << std::hex << std::setw(16) << hash
                                      << std::dec << " SNES_steps=" << cpu.steps()
                                      << " master_clocks=" << cpu.timing().clocks()
                                      << " nmitimen=$" << std::hex
                                      << static_cast<unsigned>(cpu.interrupt_enable())
                                      << std::dec << " nmi_ever_enabled="
                                      << (cpu.nmi_was_enabled() ? 1 : 0)
                                      << " p=$" << std::hex
                                      << static_cast<unsigned>(cpu.registers().p)
                                      << std::dec << '\n';
                            uploaded_ranges.emplace_back(destination, transferred.size());
                            boot_timeline.record('U', cpu.timing().clocks(), destination, transferred.size(), hash);
                            ++completed_blocks;
                            if (requested_blocks != 0 &&
                                completed_blocks == requested_blocks) return 0;
                            if ((upload_boot || driver_probe || synchronized) &&
                                ports[1] == 0) {
                                entry_address = static_cast<std::uint16_t>(
                                    ports[2] | (static_cast<unsigned>(ports[3]) << 8));
                                boot_timeline.record('E', cpu.timing().clocks(), entry_address);
                                bool entry_uploaded = false;
                                for (const auto& [start, length] : uploaded_ranges) {
                                    if (entry_address >= start &&
                                        static_cast<std::uint32_t>(entry_address) <
                                            static_cast<std::uint32_t>(start) + length) {
                                        entry_uploaded = true;
                                        break;
                                    }
                                }
                                if (!entry_uploaded) {
                                    std::cerr << "SPC700 entry is outside verified upload blocks\n";
                                    return 5;
                                }
                                entry_command_seen = true;
                                continue;
                            }
                            destination = static_cast<std::uint16_t>(
                                ports[2] | (static_cast<unsigned>(ports[3]) << 8));
                            transferred.clear();
                            expected_index = 0;
                        } else {
                            std::cerr << "unexpected APU upload sequence at byte "
                                      << transferred.size() << " expected index "
                                      << static_cast<unsigned>(expected_index)
                                      << " got " << static_cast<unsigned>(write.value)
                                      << " port1 " << static_cast<unsigned>(ports[1])
                                      << " PC $" << std::hex
                                      << static_cast<unsigned>(cpu.registers().pb)
                                      << ':' << cpu.registers().pc
                                      << " SPC $" << spc.registers().pc << std::dec << '\n';
                            return 5;
                        }
                    }
                    continue;
                }
                if (trace) {
                    std::cout << write.step << ' ' << static_cast<unsigned>(write.port)
                              << ' ' << static_cast<unsigned>(write.value) << '\n';
                    if (printed == 64) return 0;
                } else {
                    std::cout << "first APU port write after " << write.step
                              << " instructions: port " << static_cast<unsigned>(write.port)
                              << " value $" << std::hex << std::setw(2)
                              << std::setfill('0') << static_cast<unsigned>(write.value)
                              << std::dec << '\n';
                    return 0;
                }
            }
            cpu.clear_apu_writes();
        }
        std::cerr << "SNES CPU trace reached instruction bound at $"
                  << std::hex << std::setw(2) << std::setfill('0')
                  << static_cast<unsigned>(cpu.registers().pb) << ':'
                  << std::setw(4) << cpu.registers().pc << std::dec
                  << " after " << cpu.timing().clocks() << " master clocks"
                  << " (V=" << cpu.timing().line()
                  << " H=" << cpu.timing().horizontal_clock() << ")\n";
        if (cycle_apu_sync) {
            const auto target = cpu.timing().clocks() * apu_clock_hz / 21'477'273ULL;
            std::cout << "APU rendezvous target=" << target
                      << " completed=" << cpu.spc_cycles()
                      << " SPC_completed=" << spc.cycles()
                      << " instruction_pending=" << spc.instruction_pending() << '\n';
            if (cpu.spc_cycles() != target || spc.cycles() != target)
                throw std::runtime_error("cycle APU rendezvous overshot its master-clock target");
            if (fractional_apu_sync) {
                const auto half_target = cpu.timing().clocks() * (std::uint64_t(apu_clock_hz) * 2) / 21'477'273ULL;
                std::cout << "APU fractional target_half=" << half_target
                          << " completed_half=" << spc.half_cycles() << '\n';
                if (spc.half_cycles() != half_target)
                    throw std::runtime_error("fractional APU rendezvous overshot its half-clock target");
            }
        }
        if (!sound_event_trace_path.empty()) {
            if (std::filesystem::exists(sound_event_trace_path))
                throw std::runtime_error("sound event trace already exists");
            if (!sound_event_trace_path.parent_path().empty())
                std::filesystem::create_directories(sound_event_trace_path.parent_path());
            std::ofstream output(sound_event_trace_path);
            if (!output) throw std::runtime_error("could not create sound event trace");
            output << "kind,master_clock,spc_cycle,pcm_sample,address,value\n";
            for (const auto& event : sound_trace)
                output << event.kind << ',' << event.master_clock << ','
                       << event.spc_cycle << ',' << event.pcm_sample << ','
                       << static_cast<unsigned>(event.address) << ','
                       << static_cast<unsigned>(event.value) << '\n';
            if (!output) throw std::runtime_error("could not finish sound event trace");
        }
        if (!host_startup_path.empty()) {
            if (host_startup.empty() || !host_startup_finished) throw std::runtime_error("incomplete host startup trace");
            if (!host_startup_path.parent_path().empty()) std::filesystem::create_directories(host_startup_path.parent_path());
            std::ofstream output(host_startup_path);
            output << "{\"format\":\"gbb-sgb-host-startup-v1\",\"source\":\"gbb\",\"master_hz\":21477273,\"ppu_dma_timing\":"
                   << (ppu_dma_timing ? "true" : "false") << ",\"host_bus_timing\":" << (host_bus_timing ? "true" : "false")
                   << ",\"apu_half_hz\":" << apu_clock_hz * 2 << ",\"instructions\":[";
            for (std::size_t i = 0; i < host_startup.size(); ++i) {
                if (i) output << ',';
                output << '[';
                for (std::size_t j = 0; j < 11; ++j) { if (j) output << ','; output << host_startup[i][j]; }
                output << ']';
            }
            output << "],\"dma_requests\":[";
            for (std::size_t i = 0; i < startup_dma.size(); ++i) {
                if (i) output << ',';
                output << '[';
                for (std::size_t j = 0; j < 7; ++j) { if (j) output << ','; output << startup_dma[i][j]; }
                output << ']';
            }
            output << "]}\n";
            if (!output) throw std::runtime_error("could not finish host startup trace");
        }
        if (!boot_timeline_path.empty()) {
            if (boot_timeline.overflow) throw std::runtime_error("boot timeline overflowed");
            if (!boot_timeline_path.parent_path().empty()) std::filesystem::create_directories(boot_timeline_path.parent_path());
            std::ofstream output(boot_timeline_path);
            if (!output) throw std::runtime_error("could not create boot timeline");
            output << "{\"format\":\"gbb-sgb-boot-timeline-v1\",\"source\":\"gbb\","
                "\"master_hz\":21477273,\"apu_half_hz\":" << apu_clock_hz * 2 << ",\"input_mode\":\""
                << (native_gb_input ? "gb-lcd-frame-held-v1" : "legacy-direct-gb-v1")
                << "\",\"ppu_dma_timing\":" << (ppu_dma_timing ? "true" : "false")
                << ",\"host_bus_timing\":" << (host_bus_timing ? "true" : "false")
                << ",\"external_boot_reset\":" << (native_gb_input ? "true" : "false") << ",\"events\":[";
            for (std::size_t index = 0; index < boot_timeline.events.size(); ++index) {
                const auto& e = boot_timeline.events[index];
                if (index) output << ',';
                output << "{\"kind\":\"" << e.kind << "\",\"master_clock_snapshot\":" << e.master
                    << ",\"spc_half_clock_snapshot\":" << e.half << ",\"value\":" << e.value
                    << ",\"count\":" << e.count << ",\"digest_fnv64\":" << e.digest << '}';
            }
            output << "]}\n";
            if (!output) throw std::runtime_error("could not finish boot timeline");
        }
        if (!apu_bus_output_path.empty()) {
            if (dsp_observation.apu_trace_overflow || apu_trace.empty())
                throw std::runtime_error("APU bus trace is empty or overflowed");
            if (std::filesystem::exists(apu_bus_output_path))
                throw std::runtime_error("APU bus trace already exists");
            if (!apu_bus_output_path.parent_path().empty())
                std::filesystem::create_directories(apu_bus_output_path.parent_path());
            std::ofstream output(apu_bus_output_path);
            if (!output) throw std::runtime_error("could not create APU bus trace");
            output << "{\"format\":\"" << (timer_poll_trace ? "gbb-apu-bus-v2" : "gbb-apu-bus-v1")
                   << "\",\"source\":\"gbb\","
                      "\"master_hz\":21477273,\"apu_half_hz\":" << apu_clock_hz * 2 << ',';
            output << "\"ppu_dma_timing\":" << (ppu_dma_timing ? "true" : "false") << ',';
            output << "\"host_bus_timing\":" << (host_bus_timing ? "true" : "false") << ',';
            if (timer_poll_trace) output << "\"phase_writes_from_reset\":true,";
            if (history_window[1]) output << "\"history_window_half_clocks\":[" << history_window[0] << ',' << history_window[1] << "],";
            output << "\"events\":[";
            for (std::size_t i = 0; i < apu_trace.size(); ++i) {
                const auto& event = apu_trace[i];
                if (i) output << ',';
                output << "{\"kind\":\"" << event.kind << "\",\"master_clock\":" << event.master_clock
                    << ",\"spc_half_clock\":" << event.spc_half_clock << ",\"pcm_sample\":" << event.pcm_sample
                    << ",\"address\":" << event.address << ",\"value\":" << unsigned(event.value)
                    << ",\"dsp_clock64\":" << event.dsp_clock64 << '}';
            }
            output << "]}\n";
            if (!output) throw std::runtime_error("could not finish APU bus trace");
        }
        if (!apu_ram_output_path.empty()) {
            if (std::filesystem::exists(apu_ram_output_path))
                throw std::runtime_error("APU RAM output already exists");
            if (!apu_ram_output_path.parent_path().empty())
                std::filesystem::create_directories(apu_ram_output_path.parent_path());
            std::ofstream output(apu_ram_output_path, std::ios::binary);
            if (!output) throw std::runtime_error("could not create APU RAM output");
            for (unsigned address = 0; address < 65536; ++address)
                output.put(static_cast<char>(render_bus.dsp_read_ram(
                    static_cast<std::uint16_t>(address))));
            if (!output) throw std::runtime_error("could not finish APU RAM output");
        }
        if (!pcm_output_path.empty()) {
            if (!dsp_observation.first_audible_sample && !allow_unanchored_pcm)
                throw std::runtime_error(
                    "PCM output requested but no audible SOUND packet was delivered");
            write_pcm_wav(pcm_output_path, pcm_export, apu_clock_hz / 32);
            std::cerr << "PCM output=" << pcm_output_path
                      << " rate=" << apu_clock_hz / 32
                      << " frames=" << pcm_export.size()
                      << " first_audible_delivery_sample="
                      << (dsp_observation.first_audible_sample
                              ? std::to_string(*dsp_observation.first_audible_sample)
                              : "unanchored-firmware-startup") << '\n';
        }
        if (!core_apu_benchmark_path.empty())
            sgb_test::benchmark_apu_firmware(core_driver.benchmark_state, apu_clock_hz,
                std::string_view(argv[3]) == "--sync-gb-sgb1" ? "sgb1" : "sgb2", core_apu_benchmark_path);
        if (synchronized)
            std::cerr << "APU wait context SPC_PC=$" << std::hex
                      << std::setw(4) << std::setfill('0')
                      << spc.registers().pc << " host_port0=$"
                      << std::setw(2)
                      << static_cast<unsigned>(apu.host_read_port(0))
                      << " spc_input0=$" << std::setw(2)
                      << static_cast<unsigned>(apu.spc_read(0xF4))
                      << std::dec << '\n';
        if (synchronized && sync_gb) {
            const auto& registers = spc.registers();
            std::cerr << "SPC registers A=$" << std::hex << std::setw(2)
                      << static_cast<unsigned>(registers.a) << " X=$"
                      << std::setw(2) << static_cast<unsigned>(registers.x)
                      << " Y=$" << std::setw(2)
                      << static_cast<unsigned>(registers.y) << " PSW=$"
                      << std::setw(2) << static_cast<unsigned>(registers.psw)
                      << std::dec << '\n';
            const auto& host = cpu.registers();
            const auto base = host.d;
            std::cerr << "host transfer context D=$" << std::hex
                      << std::setw(4) << base << " Y=$" << std::setw(4)
                      << host.y << " src=$";
            for (unsigned offset = 0x98; offset <= 0x9A; ++offset)
                std::cerr << std::setw(2)
                          << static_cast<unsigned>(cpu.debug_wram_byte(
                                 static_cast<std::uint16_t>(base + offset)));
            std::cerr << " remaining=$" << std::setw(4)
                      << static_cast<unsigned>(cpu.debug_wram_byte(
                             static_cast<std::uint16_t>(base + 0x9C)))
                      << std::dec << '\n';
            std::cerr << "DMA starts=" << cpu.dma_start_count()
                      << " last_mask=$" << std::hex << std::setw(2)
                      << static_cast<unsigned>(cpu.last_dma_mask());
            for (unsigned channel = 0; channel < 8; ++channel) {
                if ((cpu.last_dma_mask() & (1U << channel)) == 0) continue;
                std::cerr << " ch" << channel << '=';
                for (unsigned offset = 0; offset <= 6; ++offset)
                    std::cerr << std::setw(2)
                              << static_cast<unsigned>(cpu.dma_register(
                                     static_cast<std::uint8_t>(channel),
                                     static_cast<std::uint8_t>(offset)));
            }
            std::cerr << std::dec << '\n';
            std::cerr << "DMA destinations:";
            for (unsigned destination = 0; destination < 256; ++destination) {
                const auto count = cpu.dma_destination_count(
                    static_cast<std::uint8_t>(destination));
                if (count != 0)
                    std::cerr << " $" << std::hex << std::setw(2)
                              << std::setfill('0') << destination << std::dec
                              << '=' << count;
            }
            std::cerr << '\n';
            std::cerr << "last WRAM DMA target=$" << std::hex
                      << cpu.last_wram_dma_target() << " regs=";
            for (unsigned offset = 0; offset < 7; ++offset)
                std::cerr << std::setw(2) << std::setfill('0')
                          << static_cast<unsigned>(cpu.last_wram_dma_register(
                                 static_cast<std::uint8_t>(offset)));
            std::cerr << " WMADD=$" << cpu.wram_port_address()
                      << std::dec << '\n';
            std::cerr << "recent host APU writes:";
            for (std::size_t index = 0; index < recent_apu_count; ++index) {
                const auto& write = recent_apu_writes[
                    (recent_apu_next + recent_apu_writes.size() -
                     recent_apu_count + index) % recent_apu_writes.size()];
                std::cerr << ' ' << write.step << '/'
                          << static_cast<unsigned>(write.port) << "=$"
                          << std::hex << std::setw(2) << std::setfill('0')
                          << static_cast<unsigned>(write.value) << std::dec;
            }
            std::cerr << '\n';
        }
        if (sync_gb) {
            std::cerr << "GB ICD source cycles=" << icd->gb_cycles()
                      << " packets=" << icd->packets_completed()
                      << " SOUND=" << icd->sound_commands()
                      << " delivered=" << icd->packets_delivered()
                      << " SOUND_delivered=" << icd->sound_packets_delivered()
                      << " audible_SOUND_delivered="
                      << icd->audible_sound_packets_delivered()
                      << " audible_SOUND=" << icd->audible_sound_commands()
                      << " GB_frames=" << icd->completed_frames()
                      << " input_events=" << icd->input_events_applied()
                      << " SOU_TRN=" << icd->transfer_commands()
                      << " control_writes=" << icd->control_writes()
                      << " last_control=$" << std::hex << std::setw(2)
                      << std::setfill('0')
                      << static_cast<unsigned>(icd->last_control())
                      << " missing=$" << std::setw(4)
                      << icd->missing_address() << std::dec
                      << " PCM_samples=" << dsp_observation.pcm_samples
                      << " PCM_nonzero=" << dsp_observation.pcm_nonzero
                      << " post_SOUND_nonzero="
                      << dsp_observation.post_sound_nonzero
                      << " post_audible_SOUND_nonzero="
                      << dsp_observation.post_audible_sound_nonzero << '\n';
            if (icd->sound_commands() != 0) {
                std::cerr << "first SOUND packet=";
                for (const auto byte : icd->first_sound_packet())
                    std::cerr << ' ' << std::hex << std::setw(2)
                              << std::setfill('0') << static_cast<unsigned>(byte);
                std::cerr << std::dec << '\n';
            }
            if (icd->audible_sound_commands() != 0) {
                std::cerr << "first audible SOUND frame="
                          << icd->first_audible_frame() << " packet=";
                for (const auto byte : icd->first_audible_sound_packet())
                    std::cerr << ' ' << std::hex << std::setw(2)
                              << std::setfill('0') << static_cast<unsigned>(byte);
                std::cerr << std::dec << '\n';
            }
        }
        return 4;
    } catch (const std::exception& error) {
        std::cerr << "could not run SGB trace: " << error.what() << '\n';
        return 2;
    }
}
