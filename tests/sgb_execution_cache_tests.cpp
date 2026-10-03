#include "gameboy/emulator.hpp"
#include "gameboy/ppu.hpp"
#include <iostream>
#include <memory>
#include <iterator>
#include <limits>
#include "gameboy/sgb_icd_gb_source.hpp"

int main() {
    std::uint32_t random = 0x53474232;
    const auto next = [&]() { random ^= random << 13; random ^= random >> 17; random ^= random << 5; return random; };
    for (unsigned i=0;i<10000;++i) for (unsigned divider:{4U,5U,7U,9U})
    for (auto model:{gameboy::HardwareModel::sgb,gameboy::HardwareModel::sgb2}) {
        const auto clocks=i==0 ? std::numeric_limits<std::uint64_t>::max() :
            (std::uint64_t(next())<<32)|next();
        const auto oscillator=model==gameboy::HardwareModel::sgb2 ? 20971520ULL : 21477273ULL;
        const auto denominator=21477273ULL*divider;
        const auto original=clocks/denominator*oscillator + clocks%denominator*oscillator/denominator;
        if (gameboy::sgb_icd_target_gb_cycles(clocks,divider,model)!=original) {
            std::cerr << "ICD clock conversion changed\n"; return 1;
        }
    }
    for (auto model : gameboy::concrete_hardware_models) {
        std::vector<std::uint8_t> rom(32768);
        rom[0x146] = 3;
        auto cached = std::make_unique<gameboy::Emulator>(gameboy::Cartridge(rom), model);
        auto reference = std::make_unique<gameboy::Emulator>(gameboy::Cartridge(rom), model);
        reference->bus().debug_set_peripheral_batch_enabled(false);
        reference->bus().debug_set_sgb_palette_cache_enabled(false);
        reference->bus().debug_set_apu_channel_batch_enabled(false);
        for (unsigned i = 0; i < 8192; ++i) {
            const auto value = next();
            cached->bus().debug_write_vram(0, i, value);
            reference->bus().debug_write_vram(0, i, value);
        }
        // Variable bus spans, timer/reload edges, LCD startup/SCX/STAT races,
        // OAM DMA and active HBlank DMA. Compare all serialized state as well
        // as presentation PCM: derived caches are intentionally absent.
        constexpr std::uint16_t addresses[]{0xff04,0xff05,0xff06,0xff07,
            0xff40,0xff41,0xff42,0xff43,0xff45,0xff46,0xff47,0xff48,
            0xff49,0xff4a,0xff4b,0xff51,0xff52,0xff53,0xff54,0xff55,
            0xff10,0xff11,0xff12,0xff13,0xff14,0xff16,0xff17,0xff18,
            0xff19,0xff1a,0xff1b,0xff1c,0xff1d,0xff1e,0xff20,0xff21,
            0xff22,0xff23,0xff24,0xff25,0xff26,0xff30,0xff3f};
        for (unsigned i = 0; i < 4096; ++i) {
            const auto address = addresses[next() % std::size(addresses)];
            auto value = static_cast<std::uint8_t>(next());
            if (address == 0xff46 || address == 0xff51) value = 0xc0;
            const auto clocks = 1 + next() % 521;
            for (auto* emulator : {cached.get(), reference.get()}) {
                if (i % 29 == 0) emulator->bus().write8(0xff26, 0x80);
                emulator->bus().write8(address, value);
                emulator->bus().tick(clocks);
            }
            if (cached->take_audio_samples() != reference->take_audio_samples() ||
                (i % 128 == 0 && cached->save_state() != reference->save_state())) {
                std::cerr << "bus batching changed state/PCM, model " << static_cast<unsigned>(model)
                          << " iteration " << i << '\n'; return 1;
            }
        }
    }
    auto cached = std::make_unique<gameboy::Ppu>();
    auto reference = std::make_unique<gameboy::Ppu>();
    reference->debug_set_sgb_palette_cache_enabled(false);
    for (auto* ppu : {cached.get(), reference.get()}) {
        ppu->set_sgb_mode(true);
        (void)ppu->write_register(0xff40, 0x93);
    }
    for (unsigned i = 0; i < 8192; ++i) {
        const auto value = next();
        cached->debug_write_vram(0, i, value); reference->debug_write_vram(0, i, value);
    }
    for (unsigned sprite = 0; sprite < 40; ++sprite) {
        const std::uint8_t bytes[]{
            static_cast<std::uint8_t>(16 + sprite * 13 % 144),
            static_cast<std::uint8_t>(8 + sprite * 17 % 160),
            static_cast<std::uint8_t>(next()),
            static_cast<std::uint8_t>((sprite & 1U ? 0x10U : 0U) |
                                      (sprite & 2U ? 0x80U : 0U))};
        for (unsigned byte = 0; byte < 4; ++byte)
            for (auto* ppu : {cached.get(), reference.get()})
                ppu->debug_write_oam(sprite * 4 + byte, bytes[byte]);
    }
    constexpr unsigned commands[]{0,1,2,3,4,5,6,7,0x0a,0x0b,0x17};
    for (unsigned i = 0; i < 100; ++i) {
        std::array<std::uint8_t,112> packet{};
        for (auto& byte : packet) byte = next();
        packet[0] = static_cast<std::uint8_t>((commands[i % std::size(commands)] << 3) | 1);
        const std::uint8_t palettes[]{static_cast<std::uint8_t>(next()),
            static_cast<std::uint8_t>(next()), static_cast<std::uint8_t>(next())};
        for (auto* ppu : {cached.get(), reference.get()}) {
            for (unsigned palette = 0; palette < 3; ++palette)
                (void)ppu->write_register(0xFF47 + palette, palettes[palette]);
            ppu->apply_sgb_command(packet, i % 7 ? packet.size() : 1);
            (void)ppu->tick(70224);
        }
        if (cached->framebuffer() != reference->framebuffer() ||
            cached->sgb_framebuffer() != reference->sgb_framebuffer()) {
            std::cerr << "SGB palette cache changed pixels at " << i << '\n'; return 1;
        }
        for (unsigned y = 0; y < gameboy::Ppu::screen_height; ++y)
        for (unsigned x = 0; x < gameboy::Ppu::screen_width; ++x)
            if (cached->debug_sgb_source_pixel(x,y) != reference->debug_sgb_source_pixel(x,y)) {
                std::cerr << "SGB composition changed raw transfer source at " << i << '\n';
                return 1;
            }
    }
    return 0;
}
