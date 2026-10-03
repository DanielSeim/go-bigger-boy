#pragma once
#include "gameboy/sgb_icd_gb_source.hpp"
namespace sgb_test {
using gameboy::sgb_icd_target_gb_cycles;
class SnesIcdGbSource final : public gameboy::SgbIcdGbSource {
public:
    SnesIcdGbSource(const std::filesystem::path& rom, const std::filesystem::path& boot,
                    gameboy::HardwareModel model);
    void load_input_script(const std::filesystem::path& path);
};
} // namespace sgb_test
