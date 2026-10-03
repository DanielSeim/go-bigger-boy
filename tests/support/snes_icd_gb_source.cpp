#include "snes_icd_gb_source.hpp"
#include "sgb_input_script.h"
#include <fstream>
#include <stdexcept>
#include <string>
namespace sgb_test {
namespace {
gameboy::DiagnosticBootRom read_boot(const std::filesystem::path& path) {
    gameboy::DiagnosticBootRom boot{};
    std::ifstream file(path, std::ios::binary);
    if (std::filesystem::file_size(path) != boot.size() ||
        !file.read(reinterpret_cast<char*>(boot.data()), boot.size()))
        throw std::runtime_error("GB boot ROM must be exactly 256 bytes");
    return boot;
}
}
SnesIcdGbSource::SnesIcdGbSource(const std::filesystem::path& rom,
                               const std::filesystem::path& boot, gameboy::HardwareModel model)
    : SgbIcdGbSource(gameboy::Cartridge::from_file(rom), read_boot(boot), model) {}
void SnesIcdGbSource::load_input_script(const std::filesystem::path& path) {
    gbb_sgb_input_script script{}; char error[128]{};
    if (!gbb_sgb_input_load(path.string().c_str(), &script, error, sizeof(error)))
        throw std::runtime_error(std::string("input script: ") + error);
    std::vector<InputEvent> events;
    for (std::size_t i=0; i<script.count; ++i)
        events.push_back({script.events[i].frame, script.events[i].mask});
    set_input_events(events);
}
} // namespace sgb_test
