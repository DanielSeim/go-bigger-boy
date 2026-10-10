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
    std::vector<std::uint64_t> mute,recovered,consumed_tokens;
    std::vector<std::array<std::uint64_t,6>> gaps;
    std::vector<std::array<std::uint64_t,17>> rejects;
    std::vector<std::array<std::uint64_t,7>> banks;
    std::vector<std::array<std::uint64_t,3>> clears;
    std::vector<std::uint64_t> onset_frames,off_frames,zero_frames;
    std::vector<unsigned> off_envs,off_endx;
    std::uint64_t gb_samples{},native_samples{},clipped{},clocks{},sounds{};
    unsigned version{},env2{},env3{};
    bool operator==(const Result& other) const {
        return std::tie(pcm,state,edges,off_envs,off_endx,mute,recovered,consumed_tokens,gaps,rejects,banks,clears,onset_frames,off_frames,zero_frames,gb_samples,native_samples,clipped,clocks,sounds,version,env2,env3)==
            std::tie(other.pcm,other.state,other.edges,other.off_envs,other.off_endx,other.mute,other.recovered,other.consumed_tokens,other.gaps,other.rejects,other.banks,other.clears,other.onset_frames,other.off_frames,other.zero_frames,other.gb_samples,other.native_samples,other.clipped,other.clocks,other.sounds,other.version,other.env2,other.env3) && envelopes==other.envelopes;
    }
};
struct Restores { unsigned count{},pending{},edges{},releases{},zeros{},banks{},clears{},queued_onsets{},queued_offs{},queued_banks{},queued_clears{},mute{},queued_mute{},rejects{},queued_rejects{},recovered{},queued_recovered{}; };
Result run(Host& host,Restores* restores=nullptr,bool active=false,unsigned rejected=0,bool recover=false,bool repeat=false,bool tail=false,bool mixed=false,bool cold=false) {
    // Both repeated and mixed failures use the same second-upload stage layout.
    const unsigned retry_stage=cold ? 4 : repeat ? 8 : 6;
    const unsigned stages=recover ? retry_stage+2 : 5;
    const unsigned failures=repeat ? 5 : 3;
    const unsigned second_generation=mixed ? 2 : 3;
    const unsigned final_generation=cold ? rejected : mixed ? 3 : rejected+1+(repeat ? 1 : 0);
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
            require(stage==last_stage+1 && stage<=stages,"transition stage order");
            result.edges.push_back({stage,bus.read8(0xff25),frames,host.cpu().timing().clocks()});
            last_stage=stage; since_edge=frames; edge=true;
        }
        const auto missed=result.envelopes.notes.empty() ? 0 : result.envelopes.notes[0].missed;
        const auto last_sample=result.envelopes.notes.empty() ? 0 : result.envelopes.notes[0].last_sample;
        result.envelopes.capture(host,host.cpu().debug_wram_byte(0x26));
        if (!result.envelopes.notes.empty() && result.envelopes.notes[0].missed!=missed) {
            const auto& note=result.envelopes.notes[0];
            require(active && stage==3 && !note.off_half && result.gaps.size()<64 &&
                    note.last_sample>last_sample+1 && note.last_sample-last_sample<=512,"unexpected native sampling gap");
            result.gaps.push_back({frames,host.cpu().timing().clocks(),last_sample,note.last_sample,stage,note.off_half});
        }
        while (result.onset_frames.size()<result.envelopes.notes.size()) {
            result.onset_frames.push_back(frames); result.off_frames.push_back(0); result.zero_frames.push_back(0);
            result.off_envs.push_back(0); result.off_endx.push_back(0);
        }
        bool release=false,zero=false,publication=false,clearing=false;
        const auto phase=host.debug_spc_ram_byte(0x0504);
        const auto generation=host.cpu().debug_wram_byte(0x26);
        if (phase==0xa4 && !previous_clear) {
            require(result.clears.size()<(cold ? 2 : recover ? (repeat ? 4 : 3) : 2),"clear count");
            for (unsigned address=0x2b00;address<0x3300;++address)
                require(!host.debug_spc_ram_byte(address),"old score survived clearing");
            for (unsigned address=0x5000;address<0x50c0;++address)
                require(!host.debug_spc_ram_byte(address),"old samples survived clearing");
            require(!host.debug_spc_ram_byte(0xd2) && !host.debug_spc_ram_byte(0xdb) && !host.debug_spc_ram_byte(0xdc),"old readiness survived clearing");
            result.clears.push_back({generation,frames,host.cpu().timing().clocks()});clearing=true;
        }
        previous_clear=phase==0xa4;
        if (phase==0xa5 && generation>=1 && generation<=(recover ? final_generation : 2) &&
                (result.banks.empty() || generation>result.banks.back()[0])) {
            require(result.banks.size()<(cold ? 1 : 2) && generation==(cold ? final_generation : result.banks.empty() ? 1 : recover ? final_generation : 2),"bank publication order");
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
        bool muted=false;
        if (active && !cold && result.mute.empty() && result.envelopes.notes.size()==1 && result.off_frames[0] &&
                host.debug_dsp_register(0x6c)==0xe0) {
            result.mute={frames,host.cpu().timing().clocks(),host.apu_half_clocks(),
                         host.debug_dsp_register(0x5c),host.debug_dsp_register(0x6c),
                         host.debug_dsp_register(result.envelopes.notes[0].voice*16+8)};
            muted=true;
        }
        bool rejection=false;
        if (rejected && !result.rejects.empty() && (!recover || stage<(cold ? 4 : 6) || (repeat && stage==7 && result.rejects.size()==failures))) {
            require(host.cpu().debug_wram_byte(0x56)==1 && host.cpu().debug_wram_byte(0x20)==9 &&
                    generation==(cold ? rejected-1 : repeat && stage==7 ? second_generation : rejected) && phase==0xa4 && !host.debug_spc_ram_byte(0xd1) &&
                    !host.debug_spc_ram_byte(0xd2) && !host.debug_spc_ram_byte(0xd8) &&
                    !host.debug_spc_ram_byte(0xdb) && !host.debug_spc_ram_byte(0xdc) &&
                    host.debug_dsp_register(0x6c)==0xe0 && host.debug_dsp_register(0x5c)==0xff,
                    "rejected playback escaped its blocked state");
        }
        if (rejected && host.cpu().debug_wram_byte(0x56)==1 && host.cpu().debug_wram_byte(0x20)==9 &&
                (result.rejects.empty() || (result.rejects.back()[1]!=host.cpu().debug_wram_byte(0x59) || result.rejects.back()[0]!=host.cpu().debug_wram_byte(0x57)))) {
            require(result.rejects.size()<failures && host.cpu().debug_wram_byte(0x57)==(repeat && stage>=6 ? 2 : 1),"unexpected rejection counters");
            result.rejects.push_back({host.cpu().debug_wram_byte(0x57),host.cpu().debug_wram_byte(0x59),frames,host.cpu().timing().clocks(),
                host.cpu().debug_wram_byte(0x56),host.cpu().debug_wram_byte(0x27),generation,phase,
                host.debug_spc_ram_byte(0xd1),host.debug_spc_ram_byte(0xd2),host.debug_spc_ram_byte(0xd8),
                host.debug_spc_ram_byte(0xdb),host.debug_spc_ram_byte(0xdc),host.debug_dsp_register(0x6c),
                host.debug_dsp_register(0x5c),hash(host,0x2b00,2048),hash(host,0x5000,192)});
            if (tail) {
                const auto token=host.cpu().debug_wram_byte(0x23);
                require(token==3,"one-byte semantic tail lost consumed IPL token");
                result.consumed_tokens.push_back(token);
            }
            rejection=true;
        }
        if (cold && stage<5) require(result.envelopes.notes.empty(),"cold rejection started a native note");
        bool recovery=false;
        if (recover && result.rejects.size()>=3 && !host.cpu().debug_wram_byte(0x56)) {
            require(result.banks.size()==(cold ? 1 : 2) && phase==0xa5 && generation==final_generation,
                    "retry unblocked playback before valid publication");
        }
        if (recover && !result.recovered.empty()) {
            require(!host.cpu().debug_wram_byte(0x56) && !host.cpu().debug_wram_byte(0x27) &&
                    host.cpu().debug_wram_byte(0x57)==(repeat ? 2 : 1) && host.cpu().debug_wram_byte(0x59)==(repeat ? 3 : 2),
                    "admitted recovery lost readiness or suppressed fresh SOUND");
        }
        if (recover && result.recovered.empty() && result.rejects.size()==failures &&
                !host.cpu().debug_wram_byte(0x56) && host.cpu().debug_wram_byte(0x20)==1) {
            require(stage==retry_stage && result.clears.size()==(cold ? 2 : repeat ? 4 : 3) && result.banks.size()==(cold ? 1 : 2),"recovery before complete publication");
            result.recovered={frames,host.cpu().timing().clocks(),generation,
                host.cpu().debug_wram_byte(0x57),host.cpu().debug_wram_byte(0x59),
                host.cpu().debug_wram_byte(0x56),host.cpu().debug_wram_byte(0x20),host.cpu().debug_wram_byte(0x27),phase,
                host.debug_spc_ram_byte(0xd1),host.debug_spc_ram_byte(0xd2),host.debug_spc_ram_byte(0xd8),
                host.debug_spc_ram_byte(0xdb),host.debug_spc_ram_byte(0xdc),host.debug_dsp_register(0x6c),
                host.debug_dsp_register(0x5c),hash(host,0x2b00,2048),hash(host,0x5000,192)};
            recovery=true;
        }
        const auto offset=last_stage ? frames-since_edge : ~std::uint64_t{0};
        const unsigned checkpoint=unsigned(frames/512);
        bool queued_event=host.pending_samples() && ((!result.mute.empty() && frames-result.mute[0]<=32) ||
                (!result.recovered.empty() && frames-result.recovered[0]<=32));
        if (host.pending_samples()) {
            for (const auto& reject:result.rejects) if (frames-reject[2]<=32) queued_event=true;
            for (const auto& bank:result.banks) if (frames-bank[1]<=32) queued_event=true;
            for (const auto& clear:result.clears) if (frames-clear[1]<=32) queued_event=true;
        }
        for (unsigned i=0;i<result.onset_frames.size();++i)
            if (host.pending_samples() && (frames-result.onset_frames[i]<=32 ||
                (result.off_frames[i] && frames-result.off_frames[i]<=32))) queued_event=true;
        // Save before draining, including queued output, at and around edges.
        if (restores && (edge || release || zero || muted || rejection || recovery || publication || clearing || queued_event || checkpoint!=last_checkpoint ||
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
            if (rejection) restores->rejects |= 1U<<(result.rejects.size()-1);
            if (host.pending_samples()) for (unsigned i=0;i<result.rejects.size();++i)
                if (frames-result.rejects[i][2]<=32) restores->queued_rejects |= 1U<<i;
            if (recovery) restores->recovered=1;
            if (host.pending_samples() && !result.recovered.empty() && frames-result.recovered[0]<=32) restores->queued_recovered=1;
            if (muted) restores->mute=1;
            if (host.pending_samples() && !result.mute.empty() && frames-result.mute[0]<=32) restores->queued_mute=1;
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
    require(host.sample_rate()==48000 && result.edges.size()==stages,"output rate or missing stage");
    result.gb_samples=host.gb_samples_captured();
    result.native_samples=host.snes_samples_produced();
    result.clipped=host.clipped_samples();
    result.clocks=host.cpu().timing().clocks();
    result.sounds=host.icd().sound_packets_delivered();
    result.version=host.cpu().debug_wram_byte(0x2b);
    result.env2=host.debug_dsp_register(0x28); result.env3=host.debug_dsp_register(0x38);
    result.state=host.save_state();
    if (result.version!=0xda || result.env2 || result.env3 || ((!rejected || recover) && host.cpu().debug_wram_byte(0x27)))
        std::cerr << "lifecycle " << result.version << ' ' << result.env2 << ' ' << result.env3
                  << " error " << unsigned(host.cpu().debug_wram_byte(0x27))
                  << " notes " << result.envelopes.notes.size() << '\n';
    require(result.clipped==0 && result.gb_samples && result.sounds==(cold ? 4 : recover ? (repeat ? 6 : 5) : active ? 3 : 4) && result.version==0xda &&
            !result.env2 && !result.env3 && ((rejected && !recover) || !host.cpu().debug_wram_byte(0x27)),"incomplete lifecycle");
    require(result.envelopes.notes.size()==(cold || (rejected && !recover) ? 1 : 2),"expected old and replacement onsets");
    for (const auto& note:result.envelopes.notes) {
        const bool sampled=!note.missed || (active && &note==&result.envelopes.notes[0] && note.missed==result.gaps.size());
        if (!(note.off_half>note.on_half && note.zero_half>=note.off_half && !note.setup_changes && sampled))
            std::cerr << "note " << note.voice << " source " << note.source << " on " << note.on_half << " off " << note.off_half
                      << " zero " << note.zero_half << " changes " << note.setup_changes << " missed " << note.missed << '\n';
        require(note.off_half>note.on_half && note.zero_half>=note.off_half && !note.setup_changes && sampled,
                "missing release or unstable held tone");
    }
    return result;
}
}
int main(int argc,char** argv) {
    try {
        require(argc==8 || argc==9 || argc==10,"ROM GAME MODEL MODE PCM REPORT VOICE");
        const unsigned voice=argc>=8 ? (std::string(argv[7])=="2" ? 2 : std::string(argv[7])=="3" ? 3 : 0) : 0;
        require(voice,"bank-replacement voice");
        const bool active=argc>=9;
        const std::string fault=active ? argv[8] : "";
        // The rejection value also names its expected completed handoff count:
        // preflight rejects before handoff 2; semantic rejection follows it.
        const unsigned rejected=fault=="asset-gap" ? 1 : fault=="bad-root" ? 2 : 0;
        require(!active || fault=="active" || rejected,"upload mode");
        const bool recover=argc==10;
        const std::string recovery= recover ? argv[9] : "";
        const bool tail=recovery=="tail" || recovery=="repeat-tail";
        const bool cold=recovery=="cold";
        const bool mixed=recovery=="mixed";
        const bool repeat=recovery=="repeat-tail" || mixed;
        const unsigned stages=cold ? 6 : recover ? (repeat ? 10 : 8) : 5;
        const unsigned final_generation=cold ? rejected : mixed ? 3 : rejected+1+(repeat ? 1 : 0);
        const unsigned failures=repeat ? 5 : 3;
        const unsigned reject_mask=(1U<<failures)-1;
        require(!recover || (rejected && (recovery=="recover" || mixed || cold || (tail && rejected==2))),"recovery mode");
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
        const auto result=run(normal,nullptr,active,rejected,recover,repeat,tail,mixed,cold);
        Restores restores;
        require(result==run(restored,&restores,active,rejected,recover,repeat,tail,mixed,cold),"whole state/PCM/edge restore parity");
        normal.reset(); require(result==run(normal,nullptr,active,rejected,recover,repeat,tail,mixed,cold),"cold reset parity");
        const unsigned note_mask=cold || (rejected && !recover) ? 1 : 3;
        const unsigned clear_mask=cold ? 3 : recover ? (repeat ? 15 : 7) : 3;
        require(restores.pending && restores.edges==((1U<<stages)-1) && restores.releases==note_mask && restores.zeros==note_mask && restores.queued_onsets==note_mask && restores.queued_offs==note_mask && restores.banks==note_mask && restores.clears==clear_mask && restores.queued_banks==note_mask && restores.queued_clears==clear_mask,
                "missing queued-output or release/zero restore coverage");
        require(!active || cold || (result.mute.size()==6 && restores.mute==1 && restores.queued_mute==1),"missing active mute restore coverage");
        require(result.banks.size()==(cold || (rejected && !recover) ? 1 : 2) && result.clears.size()==(cold ? 2 : recover ? (repeat ? 4 : 3) : 2) && restored.cpu().debug_wram_byte(0x26)==(recover ? final_generation : rejected ? rejected : 2),"missing bank replacement");
        require(result.envelopes.notes[0].voice==voice && result.envelopes.notes[0].source==(cold ? 3 : 2) &&
                (cold || (rejected && !recover) || (result.envelopes.notes[1].voice==voice && result.envelopes.notes[1].source==3)),"replacement did not remap source");
        require(!rejected || (result.rejects.size()==failures && restores.rejects==reject_mask && restores.queued_rejects==reject_mask &&
                restored.cpu().debug_wram_byte(0x56)==(recover ? 0 : 1) && restored.cpu().debug_wram_byte(0x57)==(repeat ? 2 : 1) &&
                restored.cpu().debug_wram_byte(0x59)==(repeat ? 3 : 2) && restored.cpu().debug_wram_byte(0x27)==(recover ? 0 : rejected)),
                "missing blocked SOUND/rejection restore coverage");
        require(!rejected || recover || (hash(restored,0x2b00,2048)==result.rejects.back()[15] &&
                hash(restored,0x5000,192)==result.rejects.back()[16]),"rejected bank changed after suppression");
        require(!recover || (result.recovered.size()==18 && restores.recovered==1 && restores.queued_recovered==1),
                "missing admitted recovery/unread-output replay");
        require(!recover || (hash(restored,0x2b00,2048)==result.banks.back()[3] &&
                hash(restored,0x5000,192)==result.banks.back()[4]),"fresh bank changed after recovery");
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
        if (active && !cold) {
            report << ",\"active_upload\":true,\"restore_mute\":" << restores.mute
                   << ",\"queued_mute\":" << restores.queued_mute << ",\"mute\":[";
            for (unsigned i=0;i<result.mute.size();++i) report << (i ? "," : "") << result.mute[i];
            report << "],\"gaps\":[";
            for (unsigned i=0;i<result.gaps.size();++i) {
                report << (i ? "," : "") << '[';
                for (unsigned j=0;j<6;++j) report << (j ? "," : "") << result.gaps[i][j];
                report << ']';
            }
            report << ']';
        }
        if (rejected) {
            report << ",\"rejected_upload\":\"" << fault << "\",\"restore_rejects\":" << restores.rejects
                   << ",\"queued_rejects\":" << restores.queued_rejects << ",\"rejects\":[";
            for (unsigned i=0;i<result.rejects.size();++i) {
                report << (i ? "," : "") << '[';
                for (unsigned j=0;j<17;++j) report << (j ? "," : "") << result.rejects[i][j];
                report << ']';
            }
            report << ']';
        }
        if (tail) {
            require(result.consumed_tokens.size()==failures,"missing consumed-token evidence");
            report << ",\"consumed_tokens\":[";
            for (unsigned i=0;i<result.consumed_tokens.size();++i) report << (i ? "," : "") << result.consumed_tokens[i];
            report << ']';
        }
        if (cold) report << ",\"cold_rejection\":true";
        if (mixed) report << ",\"mixed_rejection\":true";
        if (tail) report << ",\"semantic_tail\":true,\"repeated_rejection\":" << (repeat ? "true" : "false");
        if (recover) {
            report << ",\"recovery_upload\":true,\"restore_recovered\":" << restores.recovered
                   << ",\"queued_recovered\":" << restores.queued_recovered << ",\"recovered\":[";
            for (unsigned i=0;i<result.recovered.size();++i) report << (i ? "," : "") << result.recovered[i];
            report << ']';
        }
        report << '}';
        require(bool(pcm) && bool(report),"write failed");
    } catch (const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
