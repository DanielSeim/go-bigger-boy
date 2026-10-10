// SPDX-License-Identifier: GPL-3.0-or-later
#include "gameboy/sgb_host.hpp"
#include "support/sgb_score_adsr_observer.hpp"
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <tuple>
using Host=gameboy::SgbHost;
namespace {
void require(bool ok,const char* text) { if (!ok) throw std::runtime_error(text); }
std::vector<std::uint8_t> read(const char* path,std::size_t length) {
    std::ifstream file(path,std::ios::binary);
    std::vector<std::uint8_t> bytes(length);
    require(bool(file.read(reinterpret_cast<char*>(bytes.data()),length)) && file.peek()==EOF,"image size");
    return bytes;
}
struct Edge {
    unsigned stage{},route{};
    std::uint64_t frame{},clock{};
    bool operator==(const Edge& other) const {
        return std::tie(stage,route,frame,clock)==std::tie(other.stage,other.route,other.frame,other.clock);
    }
};
struct Completion {
    std::uint64_t end_half{},end_frame{},zero_half{},zero_frame{};
    bool cleared{},active{};
    auto fields() const { return std::tie(end_half,end_frame,zero_half,zero_frame,cleared,active); }
    bool operator==(const Completion& other) const { return fields()==other.fields(); }
};
struct Result {
    std::vector<std::uint8_t> pcm,state;
    std::vector<Edge> edges;
    ScoreAdsrObserver envelopes;
    std::vector<Completion> completions;
    std::vector<std::uint64_t> onset_frames,off_frames,zero_frames;
    std::vector<unsigned> off_envs,off_endx;
    std::uint64_t gb_samples{},native_samples{},clipped{},clocks{},sounds{};
    unsigned version{},env2{},env3{};
    bool operator==(const Result& other) const {
        return std::tie(pcm,state,edges,off_envs,off_endx,completions,onset_frames,off_frames,zero_frames,gb_samples,native_samples,clipped,clocks,sounds,version,env2,env3)==
            std::tie(other.pcm,other.state,other.edges,other.off_envs,other.off_endx,other.completions,other.onset_frames,other.off_frames,other.zero_frames,other.gb_samples,other.native_samples,other.clipped,other.clocks,other.sounds,other.version,other.env2,other.env3) && envelopes==other.envelopes;
    }
};
struct Restores { unsigned count{},pending{},edges{},releases{},zeros{},ends{},natural_zeros{},queued_onsets{},queued_offs{}; };
Result run(Host& host,Restores* restores=nullptr,unsigned held_voice=0) {
    Result result;
    unsigned last_stage=0,last_checkpoint=0,last_envelope=0;
    std::uint64_t since_edge=~std::uint64_t{0};
    while (host.cpu().timing().clocks()<55000000) {
        require(host.step(),"host fault");
        const auto& bus=host.icd().emulator().bus();
        const unsigned stage=bus.read8(0xff80);
        const auto frames=result.pcm.size()/4;
        bool edge=false;
        if (stage && stage!=last_stage) {
            require(stage==last_stage+1 && stage<=2,"transition stage order");
            result.edges.push_back({stage,bus.read8(0xff25),frames,host.cpu().timing().clocks()});
            last_stage=stage; since_edge=frames; edge=true;
        }
        result.envelopes.capture(host);
        while (result.onset_frames.size()<result.envelopes.notes.size()) {
            result.onset_frames.push_back(frames); result.off_frames.push_back(0); result.zero_frames.push_back(0);
            result.completions.push_back({}); result.off_envs.push_back(0); result.off_endx.push_back(0);
        }
        bool release=false,zero=false,natural=false;
        for (unsigned i=0;i<result.envelopes.notes.size();++i) {
            const auto& note=result.envelopes.notes[i];
            if (note.source==5-held_voice && !note.off_half) {
                // Retired notes must not acquire a later retrigger's ENDX.
                auto& completion=result.completions[i];
                const auto env=host.debug_dsp_register(note.voice*16+8);
                const bool end=host.debug_dsp_register(0x7c)&(1U<<note.voice);
                if (!end) completion.cleared=true;
                if (env && completion.cleared) completion.active=true;
                if (end && completion.active && !completion.end_half) {
                    completion.end_half=host.apu_half_clocks(); completion.end_frame=frames; natural=true;
                }
                if (completion.end_half && !env && !completion.zero_half) {
                    require(!note.off_half,"natural completion must precede KOF");
                    completion.zero_half=host.apu_half_clocks(); completion.zero_frame=frames; natural=true;
                }
            }
            if (note.off_half && !result.off_frames[i]) { result.off_frames[i]=frames; result.off_envs[i]=host.debug_dsp_register(note.voice*16+8); result.off_endx[i]=host.debug_dsp_register(0x7c)&(1U<<note.voice); release=true; }
            if (note.zero_half && !result.zero_frames[i]) { result.zero_frames[i]=frames; zero=true; }
        }
        const auto offset=last_stage ? frames-since_edge : ~std::uint64_t{0};
        const unsigned checkpoint=unsigned(frames/512);
        bool queued_event=false;
        for (unsigned i=0;i<result.onset_frames.size();++i)
            if (host.pending_samples() && (frames-result.onset_frames[i]<=32 ||
                (result.off_frames[i] && frames-result.off_frames[i]<=32))) queued_event=true;
        // Save before draining, including queued output, at and around edges.
        if (restores && (edge || release || zero || natural || queued_event || checkpoint!=last_checkpoint ||
            result.envelopes.checkpoints!=last_envelope ||
            (host.pending_samples() && (offset==1 || offset==2 || offset==3 || offset==4 ||
             offset==8 || offset==16 || offset==32 || offset==64 || offset==128)))) {
            require(++restores->count<=1024,"restore bound");
            if (host.pending_samples()) ++restores->pending;
            if (edge) restores->edges |= 1U<<(stage-1);
            for (unsigned i=0;i<result.off_frames.size();++i) {
                if (host.pending_samples()) {
                    if (frames-result.onset_frames[i]<=32) restores->queued_onsets |= 1U<<i;
                    if (result.off_frames[i] && frames-result.off_frames[i]<=32) restores->queued_offs |= 1U<<i;
                }
                if (result.off_frames[i]==frames) restores->releases |= 1U<<i;
                if (result.zero_frames[i]==frames) restores->zeros |= 1U<<i;
                if (result.completions[i].end_frame==frames) restores->ends |= 1U<<i;
                if (result.completions[i].zero_frame==frames) restores->natural_zeros |= 1U<<i;
            }
            const auto state=host.save_state();
            require(host.load_state(state) && host.save_state()==state,"exact queued-output restore");
        }
        last_checkpoint=checkpoint; last_envelope=result.envelopes.checkpoints;
        Host::StereoSample sample;
        while (host.pop_sample(sample)) {
            require(result.pcm.size()<600000,"PCM bound");
            for (const auto value:{sample.left,sample.right}) {
                result.pcm.push_back(std::uint16_t(value)&255);
                result.pcm.push_back(std::uint16_t(value)>>8);
            }
        }
    }
    require(host.sample_rate()==48000 && result.edges.size()==2,"output rate or missing stage");
    result.gb_samples=host.gb_samples_captured();
    result.native_samples=host.snes_samples_produced();
    result.clipped=host.clipped_samples();
    result.clocks=host.cpu().timing().clocks();
    result.sounds=host.icd().sound_packets_delivered();
    result.version=host.cpu().debug_wram_byte(0x2b);
    result.env2=host.debug_dsp_register(0x28); result.env3=host.debug_dsp_register(0x38);
    result.state=host.save_state();
    if (result.version!=0xda || result.env2 || result.env3 || host.cpu().debug_wram_byte(0x27))
        std::cerr << "lifecycle " << result.version << ' ' << result.env2 << ' ' << result.env3
                  << " error " << unsigned(host.cpu().debug_wram_byte(0x27))
                  << " notes " << result.envelopes.notes.size() << '\n';
    require(result.clipped==0 && result.gb_samples && result.sounds==2 && result.version==0xda &&
            !result.env2 && !result.env3 && !host.cpu().debug_wram_byte(0x27),"incomplete lifecycle");
    require(result.envelopes.notes.size()==4,"expected held voice and three instrument selections");
    for (const auto& note:result.envelopes.notes)
        require(note.off_half>note.on_half && note.zero_half>=note.off_half && !note.setup_changes && !note.missed,
                "missing release or unstable held tone");
    return result;
}
}
int main(int argc,char** argv) {
    try {
        require(argc==8,"ROM GAME MODEL MODE PCM REPORT HELD_VOICE");
        const unsigned held_voice=argc==8 ? (std::string(argv[7])=="2" ? 2 : std::string(argv[7])=="3" ? 3 : 0) : 0;
        require(held_voice,"instrument-change held voice");
        gameboy::SgbHostConfig config;
        config.program_rom=read(argv[1],262144); config.game_rom=read(argv[2],32768);
        const std::string model=argv[3],mode=argv[4];
        require(model=="sgb" || model=="sgb2","model");
        require(mode=="combined" || mode=="scalar","mode");
        config.model=model=="sgb" ? gameboy::HardwareModel::sgb : gameboy::HardwareModel::sgb2;
        config.combined_audio=true;
        config.gb_boot_rom[0]=0xc3; config.gb_boot_rom[2]=1;
        Host normal(config),restored(config);
        if (mode=="scalar") { normal.debug_set_apu_batch_enabled(false); restored.debug_set_apu_batch_enabled(false); }
        const auto result=run(normal,nullptr,held_voice);
        Restores restores;
        require(result==run(restored,&restores,held_voice),"whole state/PCM/edge restore parity");
        normal.reset(); require(result==run(normal,nullptr,held_voice),"cold reset parity");
        require(restores.pending && restores.edges==3 && restores.releases==15 && restores.zeros==15 && restores.queued_onsets==15 && restores.queued_offs==15,
                "missing queued-output or release/zero restore coverage");
        unsigned transient=4;
        for (unsigned i=0;i<result.envelopes.notes.size();++i) {
            if (result.envelopes.notes[i].source!=5-held_voice) continue;
            require(transient==4,"duplicate transient selection");
            transient=i;
        }
        require(transient<4,"missing transient selection");
        const auto& c=result.completions[transient];
        require(c.end_half && c.zero_half>=c.end_half && c.zero_half<result.envelopes.notes[transient].off_half,
                "selected one-shot did not complete naturally");
        require(restores.ends==(1U<<transient) && restores.natural_zeros==(1U<<transient),"natural completion restore coverage");
        std::ofstream pcm(argv[5],std::ios::binary),report(argv[6]);
        require(bool(pcm) && bool(report),"output files");
        pcm.write(reinterpret_cast<const char*>(result.pcm.data()),result.pcm.size());
        report << "{\"schema\":\"gbb-sgb-instrument-change-audio-v1\",\"qualification\":false,\"playback\":false,"
               << "\"reset_equal\":true,\"restore_equal\":true,\"sample_rate_hz\":48000,\"frames\":" << result.pcm.size()/4
               << ",\"restore_count\":" << restores.count << ",\"pending_restores\":" << restores.pending
               << ",\"restore_edges\":" << restores.edges << ",\"restore_releases\":" << restores.releases
               << ",\"queued_onsets\":" << restores.queued_onsets << ",\"queued_offs\":" << restores.queued_offs
               << ",\"restore_zeros\":" << restores.zeros << ",\"gb_samples\":" << result.gb_samples
               << ",\"native_samples\":" << result.native_samples << ",\"clipped\":" << result.clipped
               << ",\"sounds\":" << result.sounds << ",\"version\":" << result.version << ",\"clocks\":" << result.clocks
               << ",\"edges\":[";
        for (unsigned i=0;i<result.edges.size();++i) {
            const auto& e=result.edges[i];
            report << (i ? "," : "") << '[' << e.stage << ',' << e.route << ',' << e.frame << ',' << e.clock << ']';
        }
        report << "],\"notes\":[";
        for (unsigned i=0;i<result.envelopes.notes.size();++i) {
            const auto& n=result.envelopes.notes[i];
            report << (i ? "," : "") << '[' << n.voice << ',' << n.source << ',' << n.pitch << ',' << n.on_half << ',' << n.off_half << ',' << n.zero_half << ',' << result.onset_frames[i] << ',' << result.off_frames[i] << ',' << result.zero_frames[i] << ',' << result.off_envs[i] << ',' << result.off_endx[i] << ']';
        }
        report << ']';
        if (held_voice) {
            report << ",\"restore_ends\":" << restores.ends << ",\"restore_natural_zeros\":" << restores.natural_zeros
                   << ",\"completions\":[";
            for (unsigned i=0;i<result.completions.size();++i) {
                const auto& c=result.completions[i];
                report << (i ? "," : "") << '[' << c.end_half << ',' << c.end_frame << ',' << c.zero_half << ',' << c.zero_frame << ']';
            }
            report << ']';
        }
        report << '}';
        require(bool(pcm) && bool(report),"write failed");
    } catch (const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
