#include "harness_io.hpp"

#include <array>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <sstream>
#include <stdexcept>

namespace gbb::link_harness {

std::vector<std::uint8_t> read_bytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("could not open " + path.string());
    std::vector<std::uint8_t> bytes{
        std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    if (input.bad()) throw std::runtime_error("could not read " + path.string());
    return bytes;
}

std::uint64_t fingerprint(const std::vector<std::uint8_t>& bytes) noexcept {
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
    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path());
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

} // namespace gbb::link_harness
