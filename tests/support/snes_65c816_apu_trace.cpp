#include "snes_65c816_trace_cpu.hpp"
#include "snes_icd_gb_source.hpp"
#include "snes_dsp_pcm_renderer.hpp"
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
                   const std::vector<sgb_test::SnesDspPcmRenderer::StereoSample>& samples) {
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
    u32(sgb_test::SnesDspPcmRenderer::sample_rate);
    u32(sgb_test::SnesDspPcmRenderer::sample_rate * 4U);
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
    std::filesystem::path input_script_path;
    std::filesystem::path pcm_output_path;
    std::filesystem::path sound_event_trace_path;
    unsigned requested_instruction_limit = 0;
    if (sync_gb) {
        for (int index = 6; index < argc; ++index) {
            const std::string_view option(argv[index]);
            if (option == "--audible-sound-probe" && !audible_sound_probe) {
                audible_sound_probe = true;
            } else if (option == "--input-script" &&
                       input_script_path.empty() && index + 1 < argc) {
                input_script_path = argv[++index];
            } else if (option == "--pcm-output" &&
                       pcm_output_path.empty() && index + 1 < argc) {
                pcm_output_path = argv[++index];
            } else if (option == "--sound-event-trace-output" &&
                       sound_event_trace_path.empty() && index + 1 < argc) {
                sound_event_trace_path = argv[++index];
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
        sgb_test::Snes65c816TraceCpu cpu(rom, apu,
                                          path_count == 2 ? &spc : nullptr);
        std::unique_ptr<sgb_test::SnesIcdGbSource> icd;
        if (sync_gb) {
            const auto model = std::string_view(argv[3]) == "--sync-gb-sgb2"
                ? gameboy::HardwareModel::sgb2 : gameboy::HardwareModel::sgb;
            icd = std::make_unique<sgb_test::SnesIcdGbSource>(
                argv[4], argv[5], model);
            if (!input_script_path.empty())
                icd->load_input_script(input_script_path);
            icd->set_audible_sound_substitution(audible_sound_probe);
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
            std::uint8_t address{};
            std::uint8_t value{};
        };
        std::vector<SoundTraceEvent> sound_trace;
        gameboy::SnesApuBus pcm_bus;
        sgb_test::SnesDspPcmRenderer pcm(pcm_bus);
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
            gameboy::SnesApuBus* pcm_bus{};
            const sgb_test::SnesIcdGbSource* icd{};
            std::uint64_t next_sample_cycle{};
            std::uint64_t pcm_samples{};
            std::uint64_t pcm_nonzero{};
            std::uint64_t post_sound_nonzero{};
            std::uint64_t post_audible_sound_nonzero{};
            std::uint64_t pcm_hash{14695981039346656037ULL};
            std::vector<sgb_test::SnesDspPcmRenderer::StereoSample>* pcm_export{};
            std::optional<std::uint64_t> first_audible_sample;
            std::vector<SoundTraceEvent>* sound_trace{};
        } dsp_observation;
        std::vector<sgb_test::SnesDspPcmRenderer::StereoSample> pcm_export;
        if (sync_gb) {
            dsp_observation.pcm = &pcm;
            dsp_observation.pcm_bus = &pcm_bus;
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
        for (unsigned i = 0; i < instruction_bound; ++i) {
            const auto result = cpu.step();
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
                        sound_trace.size() < 16384) {
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
                    std::cout << "synchronized DSP write at SPC instruction end cycle "
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
                        if (sync_gb) {
                            pcm_bus = apu;
                            pcm.reset();
                        }
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
                            }, &dsp_observation);
                        cpu.set_spc_step_observer(
                            [](void* context, std::uint64_t cycle,
                               std::uint8_t opcode, unsigned) noexcept {
                                auto& observed = *static_cast<DspObservation*>(context);
                                if (observed.pcm != nullptr) {
                                    if (observed.next_sample_cycle == 0)
                                        observed.next_sample_cycle = cycle + 32;
                                    while (observed.next_sample_cycle <= cycle &&
                                           !observed.unsupported) {
                                        const auto sample = observed.pcm->next_sample();
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
                                        observed.next_sample_cycle += 32;
                                    }
                                }
                                for (unsigned index = 0;
                                     index < observed.pending_ram_count; ++index) {
                                    const auto [address, value] =
                                        observed.pending_ram[index];
                                    if (observed.pcm_bus != nullptr)
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
                                        if (observed.sound_trace != nullptr &&
                                            observed.icd->audible_sound_packets_delivered() != 0 &&
                                            observed.sound_trace->size() < 16384) {
                                            observed.sound_trace->push_back({
                                                'D', 0, cycle, observed.pcm_samples,
                                                observed.address, observed.value});
                                        }
                                        if (observed.pcm != nullptr)
                                            observed.pcm->write_dsp(
                                                observed.address, observed.value);
                                    }
                                }
                                observed.pending = 0;
                            }, &dsp_observation);
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
                            ++completed_blocks;
                            if (requested_blocks != 0 &&
                                completed_blocks == requested_blocks) return 0;
                            if ((upload_boot || driver_probe || synchronized) &&
                                ports[1] == 0) {
                                entry_address = static_cast<std::uint16_t>(
                                    ports[2] | (static_cast<unsigned>(ports[3]) << 8));
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
        if (!pcm_output_path.empty()) {
            if (!dsp_observation.first_audible_sample)
                throw std::runtime_error(
                    "PCM output requested but no audible SOUND packet was delivered");
            write_pcm_wav(pcm_output_path, pcm_export);
            std::cerr << "PCM output=" << pcm_output_path
                      << " rate=" << sgb_test::SnesDspPcmRenderer::sample_rate
                      << " frames=" << pcm_export.size()
                      << " first_audible_delivery_sample="
                      << *dsp_observation.first_audible_sample << '\n';
        }
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
