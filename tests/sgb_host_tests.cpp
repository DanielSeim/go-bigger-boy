#include "gameboy/sgb_host.hpp"
#include <algorithm>
#include <iostream>
#include <limits>

namespace {
using Host=gameboy::SgbHost;
int failures{};
void check(bool value,const char* text) { if(!value) { std::cerr<<"FAIL: "<<text<<'\n'; ++failures; } }
gameboy::SgbHostConfig config(gameboy::HardwareModel model=gameboy::HardwareModel::sgb2) {
    gameboy::SgbHostConfig c;
    c.model=model;
    c.program_rom.resize(0x40000);
    c.program_rom[0x7fd5]=0x20; c.program_rom[0x7ffc]=4; c.program_rom[0x7ffd]=0x81;
    // Release GB, then mutate WRAM forever. No commercial code or firmware.
    const std::uint8_t snes[]{0x78,0xa9,0x81,0x8d,3,0x60,0xe6,0,0x80,0xfc};
    std::copy(std::begin(snes),std::end(snes),c.program_rom.begin()+0x104);
    c.game_rom.resize(32768); c.game_rom[0x146]=3;
    c.game_rom[0x100]=0xc3; c.game_rom[0x101]=0x50; c.game_rom[0x102]=1;
    c.game_rom[0x150]=0xc3; c.game_rom[0x151]=0x50; c.game_rom[0x152]=1;
    // Original minimal boot: LCD on, jump to our cartridge loop. No SGB packets.
    const std::uint8_t boot[]{0x3e,0x80,0xe0,0x26,0x3e,0x77,0xe0,0x24,
        0x3e,0x11,0xe0,0x25,0x3e,0x80,0xe0,0x11,0x3e,0xf3,0xe0,0x12,
        0x3e,0xc0,0xe0,0x13,0x3e,0x87,0xe0,0x14,0x3e,0x91,0xe0,0x40,0xc3,0,1};
    std::copy(std::begin(boot),std::end(boot),c.gb_boot_rom.begin());
    c.spc_ipl[0]=0x2f; c.spc_ipl[1]=0xfe;
    c.input_events={{0,0x80},{1,0},{2,0x10},{3,0}};
    return c;
}
std::vector<Host::StereoSample> drain(Host& h) {
    std::vector<Host::StereoSample> out; Host::StereoSample s;
    while(h.pop_sample(s)) out.push_back(s); return out;
}
bool equal(const std::vector<Host::StereoSample>& a,const std::vector<Host::StereoSample>& b) {
    return a.size()==b.size() && std::equal(a.begin(),a.end(),b.begin(),
        [](auto x,auto y){ return x.left==y.left && x.right==y.right; });
}
void checksum(std::vector<std::uint8_t>& state) {
    std::uint64_t h=14695981039346656037ULL;
    for(std::size_t n=0;n<state.size()-8;++n) { h^=state[n]; h*=1099511628211ULL; }
    for(unsigned n=0;n<8;++n) state[state.size()-8+n]=static_cast<std::uint8_t>(h>>(n*8));
}
void snapshots(bool combined=false, unsigned rate=48000) {
    for(auto model:{gameboy::HardwareModel::sgb,gameboy::HardwareModel::sgb2}) {
        auto cfg=config(model); cfg.combined_audio=combined; cfg.output_hz=rate;
        Host h(cfg), other(cfg);
        const auto cold=h.save_state();
        check(h.load_state(cold),"cold host restores");
        for(unsigned boundary=0;boundary<32;++boundary) {
            const auto state=h.save_state();
            for(unsigned n=0;n<37;++n) check(h.step(),"synthetic instruction supported");
            const auto pcm=drain(h);
            const auto final=h.save_state();
            check(h.load_state(state),"same-instance whole-host restore");
            for(unsigned n=0;n<37;++n) check(h.step(),"restored instruction supported");
            check(equal(pcm,drain(h)) && h.save_state()==final,"whole-host continuation byte exact");
            check(other.load_state(state),"cross-instance whole-host restore");
            for(unsigned n=0;n<37;++n) check(other.step(),"cross-instance instruction supported");
            check(equal(pcm,drain(other)) && other.save_state()==final,"destination callbacks target restored owner");
        }
        const auto live=h.save_state();
        for(auto length:{std::size_t(0),std::size_t(8),live.size()-1}) {
            auto bad=live; bad.resize(length);
            check(!h.load_state(bad) && h.save_state()==live,"truncated restore is atomic");
        }
        auto bad=live; bad[8]=255;
        check(!h.load_state(bad) && h.save_state()==live,"unknown version rejected atomically");
        bad=live; bad[30]^=1;
        check(!h.load_state(bad) && h.save_state()==live,"corrupt payload rejected atomically");
        bad=live; bad.push_back(0);
        check(!h.load_state(bad) && h.save_state()==live,"trailing state rejected atomically");
        // Valid outer checksum, invalid inner ring index: exercise validation,
        // not merely the accidental-corruption guard.
        bad=live;
        constexpr auto head_offset=17+Host::buffer_capacity*16;
        bad[head_offset]=0; bad[head_offset+1]=0x40;
        checksum(bad);
        check(!h.load_state(bad) && h.save_state()==live,"invalid ring index rejected atomically after parsing");
        if(combined) {
            std::array<std::uint8_t,24> marker{};
            for(unsigned n=0;n<8;++n) {
                marker[n]=static_cast<std::uint8_t>(std::uint64_t(rate)>>(n*8));
                marker[8+n]=marker[16+n]=static_cast<std::uint8_t>(std::uint64_t(16384)>>(n*8));
            }
            const auto at=std::search(live.begin(),live.end(),marker.begin(),marker.end());
            check(at!=live.end(),"combined state contains explicit rate/gain header");
            if(at!=live.end()) {
                const auto offset=static_cast<std::size_t>(at-live.begin());
                bad=live; bad[offset]^=1; checksum(bad);
                check(!h.load_state(bad) && h.save_state()==live,"inner rate disagreement rejected with valid checksum");
                bad=live;
                const auto stream_head=offset+24+gameboy::SgbAudioMixer::capacity*24;
                bad[stream_head]=0; bad[stream_head+1]=0x40; checksum(bad);
                check(!h.load_state(bad) && h.save_state()==live,"invalid source queue index rejected with valid checksum");
            }
        }
        auto wrong=cfg; wrong.spc_ipl[63]=1; Host incompatible(std::move(wrong));
        check(!incompatible.load_state(live),"firmware identity bound to state");
        h.reset(); check(h.save_state()==cold,"full reset recreates exact cold state");
    }
}
void maximum_dma(bool combined=false, bool fastest=false) {
    auto cfg=config();
    cfg.combined_audio=combined;
    if(fastest) cfg.apu_clock_hz=1099968;
    std::vector<std::uint8_t> code{0x78,0xa9,0x81,0x8d,3,0x60};
    if(fastest) code[2]=0x80; // Fastest ICD divider /4 and highest supported APU rate.
    for(unsigned ch=0;ch<8;++ch) {
        const std::uint8_t values[]{0,0x18,0,0,0x7e,0,0};
        for(unsigned off=0;off<7;++off)
            code.insert(code.end(),{0xa9,values[off],0x8d,static_cast<std::uint8_t>(ch*16+off),0x43});
    }
    const auto trigger_pc=0x8104+code.size()+5;
    code.insert(code.end(),{0xa9,0xff,0x8d,0x0b,0x42,0x18,0x80,0xfd});
    std::copy(code.begin(),code.end(),cfg.program_rom.begin()+0x104);
    Host h(cfg), scalar(cfg);
    scalar.debug_set_apu_batch_enabled(false);
    scalar.debug_set_spc_idle_tail_cache_enabled(false);
    scalar.debug_set_direct_dsp_clock_enabled(false);
    scalar.debug_set_dsp_phase_dispatch_enabled(false);
    while(h.cpu().registers().pc<trigger_pc) {
        check(h.step() && scalar.step(),"DMA setup supported");
    }
    const auto pending=h.save_state();
    const auto before=h.samples_produced();
    const auto advanced=h.step();
    if(!advanced) std::cerr<<"DMA status="<<static_cast<unsigned>(h.status())<<" address="<<h.fault().address
                          <<" opcode="<<unsigned(h.fault().opcode)<<" pc="<<h.fault().pc<<'\n';
    check(advanced,"all eight maximum PPU DMA channels complete in one reservation");
    check(scalar.step() && scalar.save_state()==h.save_state(),
          "maximum DMA batched/scalar complete state and unread PCM identical");
    const auto samples=h.samples_produced()-before;
    check(combined ? samples>9000 && samples<Host::combined_instruction_reserve :
                     samples>6000 && samples<Host::instruction_reserve,
          "maximum DMA output bounded below reserved headroom");
    const auto pcm=drain(h);
    const auto final=h.save_state();
    check(h.load_state(pending) && h.step(),"pending DMA state restores and runs once");
    check(equal(pcm,drain(h)) && h.save_state()==final,"DMA timing and PCM continuation exact");
}
void backpressure(bool combined=false) {
    auto cfg=config(); cfg.combined_audio=combined;
    Host h(cfg), ref(cfg);
    ref.debug_set_direct_dsp_clock_enabled(false);
    ref.debug_set_dsp_phase_dispatch_enabled(false);
    std::uint64_t steps{};
    while(h.step()) { ++steps; if(steps>1000000) break; }
    check(h.status()==Host::Status::buffer_full && h.pending_samples()>0,"instruction reservation applies bounded backpressure");
    const auto full=h.save_state(); const auto clock=h.cpu().timing().clocks();
    check(!h.step() && h.save_state()==full,"blocked retry repeats no host IO, GB or APU clocks");
    Host restored(cfg); restored.debug_set_direct_dsp_clock_enabled(false);
    restored.debug_set_dsp_phase_dispatch_enabled(false);
    check(restored.load_state(full),"backpressured host restores");
    check(equal(drain(h),drain(restored)),"pending host PCM survives restore");
    for(std::uint64_t n=0;n<steps;++n) { check(ref.step(),"unbuffered reference advances"); (void)drain(ref); }
    check(h.save_state()==ref.save_state(),"draining pressure preserves exact complete state");
    check(h.run_until(clock,100)==0 && h.cpu().timing().clocks()==clock,"past target never rewinds or advances");
    check(h.run_until(clock+100000,0)==0,"zero instruction budget is no-op");
    check(h.run_until(clock+100000,100)==100,"instruction budget is honored");
    check(h.cpu().timing().clocks()>clock && h.apu_half_clocks()>0,"retry advances synchronized processors");
}
void mixer() {
    using Mixer=gameboy::SgbAudioMixer; using Source=Mixer::Source;
    // Fixed source/output rings are large: keep them off Windows' 1 MiB
    // default stack, just as the host's mixer is heap-owned.
    auto a_storage=std::make_unique<Mixer>(44100), b_storage=std::make_unique<Mixer>(44100);
    auto& a=*a_storage; auto& b=*b_storage;
    check(a.push(Source::gb,0,{10000,-10000}) && a.push(Source::snes,0,{20000,-20000}),"two sources accepted");
    check(b.push(Source::gb,0,{10000,-10000}) && b.push(Source::snes,0,{20000,-20000}),"reference sources accepted");
    check(a.push(Source::gb,1000,{-10000,10000}) && b.push(Source::gb,1000,{-10000,10000}),"sample transition accepted");
    check(a.advance_to(100000),"area sampler advances");
    std::vector<Host::StereoSample> reference, partitioned; Host::StereoSample sample;
    while(a.pop_sample(sample)) reference.push_back(sample);
    for(std::uint64_t clock=0;clock<=100000;clock+=137) {
        check(b.advance_to(clock),"partitioned sampler advances");
        while(b.pop_sample(sample)) partitioned.push_back(sample);
    }
    check(b.advance_to(100000),"final partial interval advances");
    while(b.pop_sample(sample)) partitioned.push_back(sample);
    check(equal(reference,partitioned),"44.1kHz area resampling independent of producer/consumer partitions");
    check(reference.size()==100000*44100ULL/Mixer::master_hz && reference[0].left==15000 &&
          reference[0].right==-15000,"rational count and exact DC gain");
    const auto before=1000*44100ULL-2*Mixer::master_hz;
    const auto expected=(15000*before+5000*(Mixer::master_hz-before)+Mixer::master_hz/2)/Mixer::master_hz;
    check(reference[2].left==expected && reference[2].right==-static_cast<int>(expected),
          "fractional transition has analytically exact area and symmetric rounding");
    check(!a.push(Source::gb,1,{}) && !a.advance_to(1),"late source/rewind rejected");
    auto isolated_storage=std::make_unique<Mixer>(32000,32768,0); auto& isolated=*isolated_storage;
    check(isolated.push(Source::gb,0,{12345,-23456}) && isolated.push(Source::snes,0,{-32768,32767}) &&
          isolated.advance_to(1000) && isolated.pop_sample(sample) && sample.left==12345 && sample.right==-23456,
          "muted SNES contribution cannot leak into GB-only output");
    auto snes_storage=std::make_unique<Mixer>(32000,0,32768); auto& snes_only=*snes_storage;
    check(snes_only.push(Source::gb,0,{-32768,32767}) && snes_only.push(Source::snes,0,{12345,-23456}) &&
          snes_only.advance_to(1000) && snes_only.pop_sample(sample) && sample.left==12345 && sample.right==-23456,
          "muted GB contribution cannot leak into SNES-only output");
    auto clipped_storage=std::make_unique<Mixer>(48000,32768,32768); auto& clipped=*clipped_storage;
    check(clipped.push(Source::gb,0,{32767,-32768}) && clipped.push(Source::snes,0,{32767,-32768}),"unity sources accepted");
    check(clipped.advance_to(1000) && clipped.pop_sample(sample) && sample.left==32767 && sample.right==-32768 &&
          clipped.clipped_samples()==2,"wide sum saturates, never wraps");
    auto pressure_storage=std::make_unique<Mixer>(); auto& pressure=*pressure_storage;
    check(pressure.advance_to(Mixer::master_hz/4),"first quarter second fills output");
    const auto produced=pressure.samples_produced();
    check(!pressure.advance_to(Mixer::master_hz) && pressure.samples_produced()==produced,
          "output preflight has no partial side effects");
    while(pressure.pop_sample(sample)) {}
    check(pressure.advance_to(Mixer::master_hz/3),"output retry continues exact timeline");
    auto inputs_storage=std::make_unique<Mixer>(); auto& inputs=*inputs_storage;
    for(std::size_t n=0;n<Mixer::capacity;++n)
        check(inputs.push(Source::gb,1000,{1,1}),"fixed source queue fills without allocation");
    check(!inputs.push(Source::gb,1001,{2,2}) && inputs.advance_to(1000) &&
          inputs.push(Source::gb,1001,{2,2}),"source overflow fails atomically and drain permits retry");
    check(!inputs.push(static_cast<Source>(255),1001,{}) &&
          !inputs.advance_to(std::numeric_limits<std::uint64_t>::max()),"invalid source and overflowing timeline rejected");
    auto reset_storage=std::make_unique<Mixer>(); auto& reset=*reset_storage;
    check(reset.push(Source::gb,0,{10000,10000}) && reset.push(Source::gb,1000,{20000,20000}) &&
          reset.reset_gb_at(500) && reset.advance_to(2000),"reset removes speculative GB data and inserts silence");
    unsigned nonzero{}; while(reset.pop_sample(sample)) { if(sample.left) ++nonzero; }
    check(nonzero==2,"no stale DAC data after reset");
    for(const auto rate:{8000U,32000U,44100U,48000U}) {
        auto drift_storage=std::make_unique<Mixer>(rate); auto& drift=*drift_storage;
        // Drain at deliberately awkward clock boundaries for a full second.
        for(std::uint64_t clock=1;clock<Mixer::master_hz;clock+=7919) {
            check(drift.advance_to(clock),"long rational progression");
            while(drift.pop_sample(sample)) {}
        }
        check(drift.advance_to(Mixer::master_hz) && drift.samples_produced()==rate,"one second has exactly configured sample count");
    }
    for (const auto rate : {8000U, 8001U, 16000U, 32000U, 44100U, 48000U}) {
        auto limits = std::make_unique<Mixer>(rate);
        const auto last_sample = std::numeric_limits<std::uint64_t>::max() / rate;
        const auto last_advance = (std::numeric_limits<std::uint64_t>::max() - Mixer::master_hz) / rate;
        check(limits->push(Source::gb, last_sample, {1,1}) &&
              !limits->push(Source::snes, last_sample + 1, {}) &&
              !limits->reset_gb_at(last_sample + 1) &&
              !limits->advance_to(last_advance + 1),
              "derived mixer limits preserve exact sample/reset/advance overflow boundaries");
        check(limits->samples_produced() == 0 && limits->pending_samples() == 0 &&
              limits->reset_gb_at(0) && limits->advance_to(100000),
              "overflow rejection preserves queues and permits valid reset/advancement");
        unsigned count{};
        while (limits->pop_sample(sample)) {
            check(!sample.left && !sample.right, "overflow rejection cannot leak future source data");
            ++count;
        }
        check(count == 100000ULL * rate / Mixer::master_hz,
              "derived mixer limit keeps exact rational sample count");
    }
    bool rejected=false; try { (void)std::make_unique<Mixer>(48001); } catch(const std::invalid_argument&) { rejected=true; }
    check(rejected,"unbounded rate rejected");
}
void raw_apu_sink() {
    gameboy::Apu vector, sink;
    vector.initialize_post_boot(gameboy::HardwareModel::sgb2);
    sink.initialize_post_boot(gameboy::HardwareModel::sgb2);
    std::vector<Host::StereoSample> captured;
    sink.set_sample_sink([](void* context,std::int16_t l,std::int16_t r) noexcept {
        static_cast<std::vector<Host::StereoSample>*>(context)->push_back({l,r});
    },&captured);
    for(auto* apu:{&vector,&sink}) {
        apu->write_register(0xff11,0x80); apu->write_register(0xff12,0xf3);
        apu->write_register(0xff13,0xc0); apu->write_register(0xff14,0x87);
    }
    for(unsigned n=0;n<1000;++n) { vector.tick(97); sink.tick(97); }
    const auto samples=vector.take_samples();
    check(samples.size()==captured.size()*2 && !captured.empty(),"raw callback has every stereo sample");
    for(std::size_t n=0;n<captured.size();++n)
        check(samples[2*n]==captured[n].left && samples[2*n+1]==captured[n].right,"raw capture leaves GB synthesis intact");
    check(sink.take_samples().empty(),"exclusive sink bypasses frontend/HLE mixing queue");
    sink.set_sample_sink(nullptr); sink.tick(1000);
    check(!sink.take_samples().empty(),"detaching restores ordinary frontend queue");
}
void combined_chunks_and_reset() {
    for(auto model:{gameboy::HardwareModel::sgb,gameboy::HardwareModel::sgb2}) {
        auto cfg=config(model); cfg.combined_audio=true; cfg.output_hz=44100;
        Host a(cfg), b(cfg);
        std::vector<Host::StereoSample> x,y;
        for(unsigned n=0;n<10000;++n) {
            check(a.step() && b.step(),"combined host advances");
            auto values=drain(a); x.insert(x.end(),values.begin(),values.end());
            if(n%257==0) { values=drain(b); y.insert(y.end(),values.begin(),values.end()); }
        }
        auto values=drain(b); y.insert(y.end(),values.begin(),values.end());
        check(equal(x,y) && a.save_state()==b.save_state(),"consumer chunk sizes cannot alter clocking or audio");
        check(a.gb_samples_captured()>0 && std::any_of(x.begin(),x.end(),[](auto s){return s.left || s.right;}),
              "combined host actually captures audible GB channels");
        check(a.sample_rate()==44100 && a.samples_produced()==a.cpu().timing().clocks()*44100/21477273,
              "combined output remains on absolute host timeline");
        auto wrong=cfg; wrong.gb_gain_q15=32768; Host gains(wrong);
        check(!gains.load_state(a.save_state()),"mix gains are part of state identity");
        wrong=cfg; wrong.output_hz=48000; Host rate(wrong);
        check(!rate.load_state(a.save_state()),"output rate is part of state identity");
    }
    // Repeated ICD reset/release while pulse audio is running. Reset semantics
    // and callback ownership must survive snapshots, not just cold startup.
    auto cfg=config(); cfg.combined_audio=true;
    const std::uint8_t code[]{0x78,0xa9,0x81,0x8d,3,0x60,0xa2,0,
        0xe6,0,0xca,0xd0,0xfb,0xa9,1,0x8d,3,0x60,0xa9,0x81,0x8d,3,0x60,0x80,0xed};
    std::copy(std::begin(code),std::end(code),cfg.program_rom.begin()+0x104);
    Host h(cfg);
    for(unsigned n=0;n<4000;++n) {
        check(h.step(),"warm reset/release supported"); (void)drain(h);
        if(n%73==0) check(h.load_state(h.save_state()),"warm reset presentation state restores");
    }
    check(h.icd().control_writes()>5 && h.gb_samples_captured()>0,"warm resets exercised with GB audio");
    auto changing=config(); changing.combined_audio=true;
    const std::uint8_t divider[]{0x78,0xa9,0x81,0x8d,3,0x60,0xa9,0x80,0x8d,3,0x60};
    std::copy(std::begin(divider),std::end(divider),changing.program_rom.begin()+0x104);
    Host unsupported(changing);
    for(unsigned n=0;n<10 && unsupported.step();++n) {}
    check(unsupported.status()==Host::Status::icd_fault && unsupported.icd().missing_address()==0x6003,
          "live divider change stops instead of silently corrupting the audio timeline");
}
void stop_timeline() {
    for(auto model:{gameboy::HardwareModel::sgb,gameboy::HardwareModel::sgb2}) {
        auto cfg=config(model);
        const std::uint8_t game[]{0x3e,0x10,0xe0,0,0x3e,0x10,0xe0,0xff,0x3e,0,0xe0,0x0f,
            0x01,0,1,0x0b,0x78,0xb1,0x20,0xfb,0x10,0,0xc3,0x66,1};
        std::copy(std::begin(game),std::end(game),cfg.game_rom.begin()+0x150);
        gameboy::SgbIcdGbSource source(cfg.game_rom,cfg.gb_boot_rom,model);
        source.set_native_gb_input(true); source.set_native_gb_input(false);
        std::vector<std::uint64_t> times;
        source.set_audio_sink([](void* context,std::uint64_t clock,std::int16_t,std::int16_t) noexcept {
            static_cast<std::vector<std::uint64_t>*>(context)->push_back(clock);
        },nullptr,&times);
        check(source.write(0x6003,0,0x81),"STOP fixture releases GB");
        source.advance_to(60000);
        const auto paused=times.size(); check(paused>0,"STOP follows audible APU clocking");
        source.advance_to(80000);
        check(times.size()==paused,"STOP pauses APU sampling without dropping host time");
        check(source.write(0x6004,80000,0xef),"host A press wakes stopped GB");
        source.advance_to(90000);
        check(times.size()>paused && times[paused]>=80000 && std::is_sorted(times.begin(),times.end()),
              "resumed samples include stopped-clock gap, never timestamps in the past");
        // Native host input cannot wake this fixture, so also exercise snapshots
        // while STOP is held and the host is continuing to emit output.
        cfg.combined_audio=true; Host host(cfg);
        for(unsigned n=0;n<5000;++n) {
            check(host.step(),"host remains live while GB is stopped"); (void)drain(host);
            if(n%211==0) check(host.load_state(host.save_state()),"STOP gap and partial sample state restore");
        }
    }
}
void faults() {
    auto c=config(); c.program_rom[0x104]=0xf8; Host host(std::move(c));
    check(!host.step() && host.status()==Host::Status::host_fault,"unsupported host opcode stops explicitly");
    const auto stopped=host.save_state();
    check(!host.step() && host.save_state()==stopped,"host fault cannot replay accesses");
    check(host.load_state(stopped) && !host.step(),"fault state remains terminal after restore");
    host.reset(); check(host.status()==Host::Status::ready,"reset clears terminal status");
    auto apu=config(); apu.spc_ipl[0]=0xff; Host af(std::move(apu));
    for(unsigned i=0;i<10 && af.step();++i) {}
    check(af.status()==Host::Status::apu_fault,"unsupported SPC instruction is reported separately");
    const auto astop=af.save_state(); check(af.load_state(astop) && !af.step(),"APU fault state restores without replay");
    auto icd=config(); icd.program_rom[0x10a]=0xad; icd.program_rom[0x10b]=8;
    icd.program_rom[0x10c]=0x60; Host cf(std::move(icd)); // LDA $6008 unsupported.
    for(unsigned i=0;i<10 && cf.step();++i) {}
    check(cf.status()==Host::Status::icd_fault,"unknown ICD write fails closed through host");
    bool rejected=false; try { auto wrong=config(); wrong.model=gameboy::HardwareModel::dmg; Host bad(std::move(wrong)); }
    catch(const std::invalid_argument&) { rejected=true; }
    check(rejected,"non-SGB model rejected");
}
void batch_oracle() {
    for(auto model:{gameboy::HardwareModel::sgb,gameboy::HardwareModel::sgb2})
    for(bool combined:{false,true}) for(bool fault:{false,true}) {
        auto cfg=config(model); cfg.combined_audio=combined;
        cfg.output_hz=model==gameboy::HardwareModel::sgb ? 48000 : 44100;
        cfg.apu_clock_hz=1099968;
        if(fault) {
            // Delay the unsupported STOP until several DSP outputs have
            // occurred, exercising failure inside a partially drained batch.
            const std::uint8_t spc[]{0xcd,0xff,0x1d,0xd0,0xfd,0xff};
            std::copy(std::begin(spc),std::end(spc),cfg.spc_ipl.begin());
        }
        Host batch(cfg), scalar(cfg); scalar.debug_set_apu_batch_enabled(false);
        scalar.debug_set_spc_idle_tail_cache_enabled(false);
        scalar.debug_set_direct_dsp_clock_enabled(false);
        scalar.debug_set_dsp_phase_dispatch_enabled(false);
        for(unsigned i=0;i<30000;++i) {
            const auto a=batch.step(), b=scalar.step();
            check(a==b,"batched/scalar terminal boundary identical");
            if(i%997==0 || !a) {
                check(batch.save_state()==scalar.save_state(),
                      "batched/scalar full state and sample timestamps identical");
                check(equal(drain(batch),drain(scalar)),"batched/scalar PCM identical");
            }
            if(!a) { check(fault,"only intentional SPC fault stops oracle"); break; }
        }
        check(!fault || batch.status()==Host::Status::apu_fault,"late SPC fault exercised");
    }
}
void batch_fault_phase_oracle() {
    // A terminal fetch can coincide with any DSP output/readback phase.
    // Reservation must preserve the pre-fault PC/cycle and leave output from
    // the failing half unread, just like the scalar scheduler.
    for (auto model : {gameboy::HardwareModel::sgb, gameboy::HardwareModel::sgb2})
    for (bool combined : {false, true}) for (unsigned delay = 0; delay < 64; ++delay) {
        auto cfg = config(model);
        cfg.combined_audio = combined;
        cfg.output_hz = model == gameboy::HardwareModel::sgb ? 48000 : 44100;
        cfg.apu_clock_hz = 1099968;
        cfg.spc_ipl.fill(0); // NOP: two clocks.
        unsigned end = delay / 2;
        if (delay & 1U) cfg.spc_ipl[end++] = 0xED; // NOTC: three clocks.
        cfg.spc_ipl[end] = 0xFF; // Unsupported STOP, fail closed.
        Host batch(cfg), scalar(cfg);
        scalar.debug_set_apu_batch_enabled(false);
        scalar.debug_set_direct_dsp_clock_enabled(false);
        scalar.debug_set_dsp_phase_dispatch_enabled(false);
        for (unsigned step = 0; step < 1000; ++step) {
            const bool a = batch.step(), b = scalar.step();
            check(a == b, "batch/scalar fault phase terminal boundary identical");
            if (!a) {
                check(batch.status() == Host::Status::apu_fault,
                      "batch fault phase reaches intentional unsupported fetch");
                check(batch.save_state() == scalar.save_state(),
                      "batch fault phase retains exact state and unread PCM");
                check(equal(drain(batch), drain(scalar)),
                      "batch fault phase retains exact emitted PCM");
                break;
            }
            check(step != 999, "batch fault phase does not run past terminal fetch");
        }
    }
}
}
int main() {
    batch_oracle();
    batch_fault_phase_oracle();
    mixer(); raw_apu_sink(); snapshots(); backpressure(); maximum_dma(); faults();
    snapshots(true); snapshots(true,44100); backpressure(true); maximum_dma(true); maximum_dma(true,true);
    combined_chunks_and_reset(); stop_timeline(); return failures?1:0;
}
