#include "gameboy/hardware_model.hpp"
#include "gameboy/memory_bus.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

int failures = 0;

void check(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

std::vector<std::uint8_t> test_rom() {
    std::vector<std::uint8_t> rom(0x8000, 0);
    constexpr std::string_view title = "APU WAVEFORM";
    std::copy(title.begin(), title.end(), rom.begin() + 0x134);
    return rom;
}

std::uint64_t waveform_signature(const std::vector<std::int16_t>& samples) noexcept {
    auto hash = UINT64_C(1469598103934665603);
    for (const auto sample : samples) {
        const auto bits = static_cast<std::uint16_t>(sample / 64);
        hash ^= static_cast<std::uint8_t>(bits);
        hash *= UINT64_C(1099511628211);
        hash ^= static_cast<std::uint8_t>(bits >> 8);
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

struct Reference {
    bool valid_format{};
    std::string name;
    std::string model;
    unsigned sample_rate{};
    unsigned channels{};
    unsigned quantization{};
    std::size_t sample_count{};
    std::int16_t max_abs_error{};
    std::int16_t rms_error{};
    std::vector<std::int16_t> samples;
};

std::optional<Reference> load_reference(const std::filesystem::path& path) {
    std::ifstream input(path);
    if (!input) return std::nullopt;
    Reference reference;
    std::string line;
    bool data = false;
    while (std::getline(input, line)) {
        if (line.empty() || line.front() == '#') continue;
        if (line == "data=") { data = true; continue; }
        if (data) {
            std::istringstream values(line);
            int sample = 0;
            while (values >> sample) {
                if (sample < std::numeric_limits<std::int16_t>::min() ||
                    sample > std::numeric_limits<std::int16_t>::max()) return std::nullopt;
                reference.samples.push_back(static_cast<std::int16_t>(sample));
            }
            continue;
        }
        const auto separator = line.find('=');
        if (separator == std::string::npos) return std::nullopt;
        const auto key = line.substr(0, separator);
        const auto value = line.substr(separator + 1);
        try {
            if (key == "name") reference.name = value;
            else if (key == "model") reference.model = value;
            else if (key == "format") reference.valid_format = value == "gbb-audio-waveform-v1";
            else if (key == "sample_rate") reference.sample_rate = static_cast<unsigned>(std::stoul(value));
            else if (key == "channels") reference.channels = static_cast<unsigned>(std::stoul(value));
            else if (key == "quantization") reference.quantization = static_cast<unsigned>(std::stoul(value));
            else if (key == "samples") reference.sample_count = static_cast<std::size_t>(std::stoull(value));
            else if (key == "max_abs_error") reference.max_abs_error = static_cast<std::int16_t>(std::stoi(value));
            else if (key == "rms_error") reference.rms_error = static_cast<std::int16_t>(std::stoi(value));
        } catch (const std::exception&) { return std::nullopt; }
    }
    if (!reference.valid_format || reference.sample_rate == 0 || reference.channels == 0 ||
        reference.quantization == 0 || reference.sample_count == 0 ||
        reference.samples.size() != reference.sample_count) return std::nullopt;
    return reference;
}

void compare_reference(const std::filesystem::path& path, const std::string_view name,
                       const std::string_view model,
                       const std::vector<std::int16_t>& rendered) {
    const auto reference = load_reference(path);
    check(reference.has_value(), "audio reference " + std::string{name} + " has a valid format");
    if (!reference) return;
    check(reference->name == name && reference->model == model && reference->sample_rate == 48000 &&
              reference->channels == 2 && reference->quantization == 64 &&
              rendered.size() == reference->samples.size(),
          "audio reference " + std::string{name} + " matches the 48 kHz stereo render shape");
    if (rendered.size() != reference->samples.size()) return;
    std::int32_t max_error = 0;
    long double squared_error = 0;
    std::size_t first_mismatch = rendered.size();
    for (std::size_t index = 0; index < rendered.size(); ++index) {
        const auto error = static_cast<std::int32_t>(rendered[index] / 64) -
                           static_cast<std::int32_t>(reference->samples[index]);
        max_error = std::max(max_error, std::abs(error));
        squared_error += static_cast<long double>(error) * error;
        if (first_mismatch == rendered.size() && error != 0) first_mismatch = index;
    }
    const auto rms = static_cast<std::int32_t>(std::sqrt(
        squared_error / static_cast<long double>(rendered.size())));
    const auto within_tolerance = max_error <= reference->max_abs_error && rms <= reference->rms_error;
    std::string detail = "audio reference " + std::string{name} + " stays within waveform tolerance";
    if (!within_tolerance) {
        detail += " (max=" + std::to_string(max_error) + ", rms=" + std::to_string(rms);
        if (first_mismatch != rendered.size()) detail += ", first sample=" + std::to_string(first_mismatch);
        detail += ')';
    }
    check(within_tolerance, detail);
}

template <typename Configure>
std::vector<std::int16_t> render(const gameboy::HardwareModel model,
                                 Configure configure) {
    gameboy::MemoryBus bus{gameboy::Cartridge{test_rom()}};
    bus.initialize_post_boot(model);
    bus.write8(0xFF24, 0x77);
    configure(bus);
    bus.tick(8192);
    return bus.take_audio_samples();
}

bool write_reference(const std::filesystem::path& path,
                     const std::string_view name,
                     const std::string_view model,
                     const std::vector<std::int16_t>& samples) {
    std::ofstream output(path);
    if (!output) return false;
    output << "# GBB audio waveform reference v1\n"
           << "format=gbb-audio-waveform-v1\n"
           << "name=" << name << '\n'
           << "model=" << model << '\n'
           << "sample_rate=48000\n"
           << "channels=2\n"
           << "quantization=64\n"
           << "samples=" << samples.size() << '\n'
           << "max_abs_error=0\n"
           << "rms_error=0\n"
           << "data=\n";
    for (std::size_t index = 0; index < samples.size(); ++index) {
        output << samples[index] / 64;
        if (index + 1 == samples.size() || (index + 1) % 16 == 0)
            output << '\n';
        else
            output << ' ';
    }
    return static_cast<bool>(output);
}

void test_waveforms(const std::filesystem::path& executable_directory) {
    const auto pulse_setup = [](gameboy::MemoryBus& bus) {
        bus.write8(0xFF25, 0x11); bus.write8(0xFF11, 0x80);
        bus.write8(0xFF12, 0xF0); bus.write8(0xFF13, 0xE8); bus.write8(0xFF14, 0x87);
    };
    const auto wave_setup = [](gameboy::MemoryBus& bus) {
        bus.write8(0xFF25, 0x44);
        for (unsigned index = 0; index < 16; ++index)
            bus.write8(static_cast<std::uint16_t>(0xFF30 + index), static_cast<std::uint8_t>(0xF0 - index * 7));
        bus.write8(0xFF1A, 0x80); bus.write8(0xFF1C, 0x20);
        bus.write8(0xFF1D, 0x00); bus.write8(0xFF1E, 0x87);
    };
    const auto noise_setup = [](gameboy::MemoryBus& bus) {
        bus.write8(0xFF25, 0x88); bus.write8(0xFF21, 0xF0);
        bus.write8(0xFF22, 0x08); bus.write8(0xFF23, 0x80);
    };
    const auto pulse = render(gameboy::HardwareModel::dmg, pulse_setup);
    const auto wave = render(gameboy::HardwareModel::dmg, wave_setup);
    const auto noise = render(gameboy::HardwareModel::dmg, noise_setup);
    const auto cgb_pulse = render(gameboy::HardwareModel::cgb, pulse_setup);
    const auto cgb_wave = render(gameboy::HardwareModel::cgb, wave_setup);
    const auto cgb_noise = render(gameboy::HardwareModel::cgb, noise_setup);
    check(pulse.size() == 186 && wave.size() == 186 && noise.size() == 186,
          "waveform regression fixtures produce a deterministic PCM window");
    check(waveform_signature(pulse) == UINT64_C(0x55e406d59a987d8b), "pulse waveform regression signature");
    check(waveform_signature(wave) == UINT64_C(0x5b853b4bcc531d67), "wave waveform regression signature");
    check(waveform_signature(noise) == UINT64_C(0x224d45db0d6b5c33), "noise waveform regression signature");
    struct Fixture { std::string_view name, model; const std::vector<std::int16_t>* samples; };
    const std::array<Fixture, 6> fixtures{{
        {"pulse", "dmg", &pulse}, {"wave", "dmg", &wave}, {"noise", "dmg", &noise},
        {"pulse", "cgb", &cgb_pulse}, {"wave", "cgb", &cgb_wave}, {"noise", "cgb", &cgb_noise}}};
    std::filesystem::path reference_directory;
    if (const auto* value = std::getenv("GBB_AUDIO_REFERENCE_DIR"); value && *value)
        reference_directory = value;
    else if (std::filesystem::exists(executable_directory / "audio-fixtures"))
        reference_directory = executable_directory / "audio-fixtures";
    else reference_directory = "tests/fixtures/audio";
    std::filesystem::path capture_directory;
    if (const auto* value = std::getenv("GBB_AUDIO_REFERENCE_CAPTURE_DIR");
        value != nullptr && *value != '\0') {
        capture_directory = value;
    }
    if (!capture_directory.empty()) {
        std::error_code error;
        std::filesystem::create_directories(capture_directory, error);
        check(!error, "audio reference capture directory is writable");
        if (!error) {
            for (const auto& fixture : fixtures) {
                check(write_reference(
                          capture_directory /
                              (std::string{fixture.model} + "-" +
                               std::string{fixture.name} + ".txt"),
                          fixture.name, fixture.model, *fixture.samples),
                      "audio reference capture writes " +
                          std::string{fixture.model} + " " +
                          std::string{fixture.name});
            }
        }
    }
    for (const auto& fixture : fixtures)
        compare_reference(reference_directory / (std::string{fixture.model} + "-" + std::string{fixture.name} + ".txt"),
                          fixture.name, fixture.model, *fixture.samples);
}

} // namespace

int main(int argc, char** argv) {
    std::error_code error;
    const auto executable_directory = argc > 0
        ? std::filesystem::absolute(argv[0], error).parent_path() : std::filesystem::path{};
    test_waveforms(error ? std::filesystem::path{} : executable_directory);
    return failures == 0 ? 0 : 1;
}
