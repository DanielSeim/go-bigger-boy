#include "gameboy/apu.hpp"
#include "gameboy/hardware_model.hpp"
#include <algorithm>
#include <array>
#include <iostream>
#include <utility>
#include <vector>

namespace {
void record(void* context, std::int16_t l, std::int16_t r) noexcept {
    auto& pcm = *static_cast<std::vector<std::int16_t>*>(context);
    pcm.push_back(l); pcm.push_back(r);
}
}
int main() {
    unsigned failures{};
    for (auto model:gameboy::concrete_hardware_models) {
        gameboy::Apu cached, reference;
        cached.initialize_post_boot(model); reference.initialize_post_boot(model);
        reference.debug_set_mixer_cache_enabled(false);
        reference.debug_set_channel_batch_enabled(false);
        std::vector<std::int16_t> a,b;
        cached.set_sample_sink(record,&a); reference.set_sample_sink(record,&b);
        std::size_t samples{}, audible{};
        std::uint32_t random = 0x145ad03f;
        const auto next = [&]() { random ^= random<<13; random ^= random>>17; random ^= random<<5; return random; };
        // Exercise routing, volume, duty latches, wave writes, length/envelope/
        // sweep edges, triggers, power cycling and presentation mute/unmute.
        for (unsigned step=0;step<6000;++step) {
            const auto address = static_cast<std::uint16_t>(step%5==0 ? 0xff30+(next()%16) : 0xff10+(next()%23));
            const auto value = static_cast<std::uint8_t>(next());
            const auto cycles = 1+next()%257;
            for (auto* apu:{&cached,&reference}) {
                apu->write_register(address,value,(step&1)!=0);
                if (step%29==0) apu->write_register(0xff26,0x80);
                if (step%17==0) apu->clock_frame_sequencer();
                if (step==2000) apu->set_audio_enabled(false);
                if (step==2100) apu->set_audio_enabled(true);
                apu->tick(cycles);
            }
            if (a!=b || cached.pcm12()!=reference.pcm12() || cached.pcm34()!=reference.pcm34() ||
                cached.read_register(0xff26)!=reference.read_register(0xff26)) {
                std::cerr << "cache changed samples/channel state: model " << static_cast<unsigned>(model)
                          << " step " << step << '\n'; ++failures; break;
            }
            a.clear(); b.clear();
        }
        // A deterministic four-channel tail guarantees audible coverage even
        // when the randomized stream happens to spend most of its time muted.
        for (auto* apu:{&cached,&reference}) {
            apu->initialize_post_boot(model);
            apu->set_audio_enabled(true);
            for (unsigned i=0;i<16;++i) apu->write_register(0xff30+i,0x12+i*13);
            for (const auto [address,value]:std::array<std::pair<unsigned,unsigned>,17>{{
                {0xff24,0x77},{0xff25,0xff},
                {0xff11,0x80},{0xff12,0xf3},{0xff13,0xda},{0xff14,0x87},
                {0xff16,0x40},{0xff17,0xb2},{0xff18,0x91},{0xff19,0x86},
                {0xff1a,0x80},{0xff1c,0x20},{0xff1d,0xa3},{0xff1e,0x87},
                {0xff21,0xc2},{0xff22,0x13},{0xff23,0x80}}})
                apu->write_register(address,value);
        }
        for (unsigned step=0;step<1024;++step) {
            for (auto* apu:{&cached,&reference}) {
                if (step%32==0) apu->clock_frame_sequencer();
                apu->tick(256);
            }
            if (a!=b) { std::cerr << "audible cache mismatch\n"; ++failures; break; }
            samples+=a.size();
            audible+=std::count_if(a.begin(),a.end(),[](auto sample){return sample!=0;});
            a.clear(); b.clear();
        }
        if (samples<5000 || audible<1000) { std::cerr << "insufficient audible coverage\n"; ++failures; }
    }
    return failures?1:0;
}
