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
std::uint64_t hash(const Host& host,unsigned begin,unsigned length) {
    std::uint64_t value=14695981039346656037ULL;
    for (unsigned i=0;i<length;++i) value=(value^host.debug_spc_ram_byte(begin+i))*1099511628211ULL;
    return value;
}
struct Edge {
    unsigned stage{},route{};
    std::uint64_t frame{},clock{};
    bool operator==(const Edge& other) const {
        return std::tie(stage,route,frame,clock)==std::tie(other.stage,other.route,other.frame,other.clock);
    }
};
struct Result {
    std::vector<std::uint8_t> pcm,state;
    std::vector<Edge> edges;
    ScoreAdsrObserver envelopes;
    std::vector<std::array<std::uint64_t,7>> banks;
    std::vector<std::array<std::uint64_t,3>> clears;
    std::vector<std::uint64_t> onset_frames,off_frames,zero_frames;
    std::vector<unsigned> off_envs,off_endx;
    std::uint64_t gb_samples{},native_samples{},clipped{},clocks{},sounds{};
    unsigned version{},env2{},env3{};
    bool operator==(const Result& other) const {
        return std::tie(pcm,state,edges,off_envs,off_endx,banks,clears,onset_frames,off_frames,zero_frames,gb_samples,native_samples,clipped,clocks,sounds,version,env2,env3)==
            std::tie(other.pcm,other.state,other.edges,other.off_envs,other.off_endx,other.banks,other.clears,other.onset_frames,other.off_frames,other.zero_frames,other.gb_samples,other.native_samples,other.clipped,other.clocks,other.sounds,other.version,other.env2,other.env3) && envelopes==other.envelopes;
    }
};
struct Restores { unsigned count{},pending{},edges{},releases{},zeros{},banks{},clears{},queued_onsets{},queued_offs{},queued_banks{},queued_clears{}; };
Result run(Host& host,Restores* restores=nullptr,unsigned voice=2) {
    Result result;
    unsigned last_stage=0,last_checkpoint=0,last_envelope=0;
    bool previous_clear=false;
    std::uint64_t since_edge=~std::uint64_t{0};
    while (host.cpu().timing().clocks()<100000000) {
        require(host.step(),"host fault");
        const auto& bus=host.icd().emulator().bus();
        const unsigned stage=bus.read8(0xff80);
        const auto frames=result.pcm.size()/4;
        bool edge=false;
        if (stage && stage!=last_stage) {
            require(stage==last_stage+1 && stage<=5,"transition stage order");
            result.edges.push_back({stage,bus.read8(0xff25),frames,host.cpu().timing().clocks()});
            last_stage=stage; since_edge=frames; edge=true;
        }
        result.envelopes.capture(host,host.cpu().debug_wram_byte(0x26));
        while (result.onset_frames.size()<result.envelopes.notes.size()) {
            result.onset_frames.push_back(frames); result.off_frames.push_back(0); result.zero_frames.push_back(0);
            result.off_envs.push_back(0); result.off_endx.push_back(0);
        }
        bool release=false,zero=false,publication=false,clearing=false;
        const auto phase=host.debug_spc_ram_byte(0x0504);
        const auto generation=host.cpu().debug_wram_byte(0x26);
        if (phase==0xa4 && !previous_clear) {
            require(result.clears.size()<2,"clear count");
            for (unsigned address=0x2b00;address<0x3300;++address)
                require(!host.debug_spc_ram_byte(address),"old score survived clearing");
            for (unsigned address=0x5000;address<0x50c0;++address)
                require(!host.debug_spc_ram_byte(address),"old samples survived clearing");
            require(!host.debug_spc_ram_byte(0xd2) && !host.debug_spc_ram_byte(0xdb) && !host.debug_spc_ram_byte(0xdc),"old readiness survived clearing");
            result.clears.push_back({generation,frames,host.cpu().timing().clocks()});clearing=true;
        }
        previous_clear=phase==0xa4;
        if (phase==0xa5 && generation>=1 && generation<=2 && result.banks.size()<generation) {
            require(generation==result.banks.size()+1,"bank publication order");
            result.banks.push_back({generation,frames,host.cpu().timing().clocks(),
                hash(host,0x2b00,2048),hash(host,0x5000,192),host.debug_spc_ram_byte(0x5018),host.debug_spc_ram_byte(0x5019)});
            publication=true;
        }
        for (unsigned i=0;i<result.envelopes.notes.size();++i) {
            const auto& note=result.envelopes.notes[i];
            if (note.off_half && !result.off_frames[i]) { result.off_frames[i]=frames; result.off_envs[i]=host.debug_dsp_register(note.voice*16+8); result.off_endx[i]=host.debug_dsp_register(0x7c)&(1U<<note.voice); release=true; }
            if (note.zero_half && !result.zero_frames[i]) { result.zero_frames[i]=frames; zero=true; }
            // A completed release belongs to its old DSP epoch. Do not keep
            // sampling it through the subsequent upload's DMA/ownership gap.
            if (note.zero_half && result.envelopes.active[note.voice-2]==i)
                result.envelopes.active[note.voice-2]=32;
        }
        const auto offset=last_stage ? frames-since_edge : ~std::uint64_t{0};
        const unsigned checkpoint=unsigned(frames/512);
        bool queued_event=false;
        if (host.pending_samples()) {
            for (const auto& bank:result.banks) if (frames-bank[1]<=32) queued_event=true;
            for (const auto& clear:result.clears) if (frames-clear[1]<=32) queued_event=true;
        }
        for (unsigned i=0;i<result.onset_frames.size();++i)
            if (host.pending_samples() && (frames-result.onset_frames[i]<=32 ||
                (result.off_frames[i] && frames-result.off_frames[i]<=32))) queued_event=true;
        // Save before draining, including queued output, at and around edges.
        if (restores && (edge || release || zero || publication || clearing || queued_event || checkpoint!=last_checkpoint ||
            result.envelopes.checkpoints!=last_envelope ||
            (host.pending_samples() && (offset==1 || offset==2 || offset==3 || offset==4 ||
             offset==8 || offset==16 || offset==32 || offset==64 || offset==128)))) {
            require(++restores->count<=1536,"restore bound");
            if (host.pending_samples()) ++restores->pending;
            if (edge) restores->edges |= 1U<<(stage-1);
            for (unsigned i=0;i<result.off_frames.size();++i) {
                if (host.pending_samples()) {
                    if (frames-result.onset_frames[i]<=32) restores->queued_onsets |= 1U<<i;
                    if (result.off_frames[i] && frames-result.off_frames[i]<=32) restores->queued_offs |= 1U<<i;
                }
                if (result.off_frames[i]==frames) restores->releases |= 1U<<i;
                if (result.zero_frames[i]==frames) restores->zeros |= 1U<<i;
            }
            if (publication) restores->banks |= 1U<<(result.banks.size()-1);
            if (clearing) restores->clears |= 1U<<(result.clears.size()-1);
            if (host.pending_samples()) {
                for (unsigned i=0;i<result.banks.size();++i) if (frames-result.banks[i][1]<=32) restores->queued_banks |= 1U<<i;
                for (unsigned i=0;i<result.clears.size();++i) if (frames-result.clears[i][1]<=32) restores->queued_clears |= 1U<<i;
            }
            const auto state=host.save_state();
            require(host.load_state(state) && host.save_state()==state,"exact queued-output restore");
        }
        last_checkpoint=checkpoint; last_envelope=result.envelopes.checkpoints;
        Host::StereoSample sample;
        while (host.pop_sample(sample)) {
            require(result.pcm.size()<1000000,"PCM bound");
            for (const auto value:{sample.left,sample.right}) {
                result.pcm.push_back(std::uint16_t(value)&255);
                result.pcm.push_back(std::uint16_t(value)>>8);
            }
        }
    }
    require(host.sample_rate()==48000 && result.edges.size()==5,"output rate or missing stage");
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
    require(result.clipped==0 && result.gb_samples && result.sounds==4 && result.version==0xda &&
            !result.env2 && !result.env3 && !host.cpu().debug_wram_byte(0x27),"incomplete lifecycle");
    require(result.envelopes.notes.size()==2,"expected old and replacement onsets");
    for (const auto& note:result.envelopes.notes) {
        if (!(note.off_half>note.on_half && note.zero_half>=note.off_half && !note.setup_changes && !note.missed))
            std::cerr << "note " << note.voice << " source " << note.source << " on " << note.on_half << " off " << note.off_half
                      << " zero " << note.zero_half << " changes " << note.setup_changes << " missed " << note.missed << '\n';
        require(note.off_half>note.on_half && note.zero_half>=note.off_half && !note.setup_changes && !note.missed,
                "missing release or unstable held tone");
    }
    return result;
}
}
int main(int argc,char** argv) {
    try {
        require(argc==8,"ROM GAME MODEL MODE PCM REPORT VOICE");
        const unsigned voice=argc==8 ? (std::string(argv[7])=="2" ? 2 : std::string(argv[7])=="3" ? 3 : 0) : 0;
        require(voice,"bank-replacement voice");
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
        const auto result=run(normal,nullptr,voice);
        Restores restores;
        require(result==run(restored,&restores,voice),"whole state/PCM/edge restore parity");
        normal.reset(); require(result==run(normal,nullptr,voice),"cold reset parity");
        require(restores.pending && restores.edges==31 && restores.releases==3 && restores.zeros==3 && restores.queued_onsets==3 && restores.queued_offs==3 && restores.banks==3 && restores.clears==3 && restores.queued_banks==3 && restores.queued_clears==3,
                "missing queued-output or release/zero restore coverage");
        require(result.banks.size()==2 && result.clears.size()==2 && restored.cpu().debug_wram_byte(0x26)==2,"missing bank replacement");
        require(result.envelopes.notes[0].voice==voice && result.envelopes.notes[1].voice==voice &&
                result.envelopes.notes[0].source==2 && result.envelopes.notes[1].source==3,"replacement did not remap source");
        std::ofstream pcm(argv[5],std::ios::binary),report(argv[6]);
        require(bool(pcm) && bool(report),"output files");
        pcm.write(reinterpret_cast<const char*>(result.pcm.data()),result.pcm.size());
        report << "{\"schema\":\"gbb-sgb-bank-replace-audio-v1\",\"qualification\":false,\"playback\":false,"
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
        report << ",\"restore_banks\":" << restores.banks << ",\"restore_clears\":" << restores.clears
               << ",\"queued_banks\":" << restores.queued_banks << ",\"queued_clears\":" << restores.queued_clears << ",\"banks\":[";
        for (unsigned i=0;i<result.banks.size();++i) {
            report << (i ? "," : "") << '[';
            for (unsigned j=0;j<7;++j) report << (j ? "," : "") << result.banks[i][j];
            report << ']';
        }
        report << "],\"clears\":[";
        for (unsigned i=0;i<result.clears.size();++i) {
            const auto& c=result.clears[i];
            report << (i ? "," : "") << '[' << c[0] << ',' << c[1] << ',' << c[2] << ']';
        }
        report << ']';
        report << '}';
        require(bool(pcm) && bool(report),"write failed");
    } catch (const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
