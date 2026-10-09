// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "gameboy/sgb_host.hpp"
#include <array>
#include <iostream>
#include <stdexcept>
#include <vector>
#include <tuple>

// Read-only instruction-boundary observation of physical DSP register edges
// and phase-27 output samples. No firmware or core scheduler instrumentation.
struct ScoreAdsrObserver {
    static constexpr std::array<unsigned,11> offsets{1,4,8,16,32,64,128,512,2048,4096,8192};
    struct Note {
        unsigned voice{}, source{}, adsr1{}, adsr2{}, gain{}, tuning{}, pitch{};
        unsigned peak{}, peak_offset{}, active_last{}, release_start{255}, drops{}, cadence_bad{}, decay_bad{}, release_bad{}, missed{};
        std::uint64_t on_half{}, off_half{}, zero_half{}, on_sample{}, last_sample{}, last_drop{};
        unsigned previous{}, setup_changes{}, release_samples{};
        std::array<unsigned,8> release_prefix{255,255,255,255,255,255,255,255};
        std::array<unsigned,11> points{255,255,255,255,255,255,255,255,255,255,255};
        auto fields() const {
            return std::tie(voice,source,adsr1,adsr2,gain,tuning,pitch,peak,peak_offset,active_last,
                release_start,drops,cadence_bad,decay_bad,release_bad,missed,on_half,off_half,zero_half,
                on_sample,last_sample,last_drop,previous,setup_changes,release_samples,release_prefix,points);
        }
        bool operator==(const Note& other) const { return fields()==other.fields(); }
    };
    std::vector<Note> notes;
    std::array<unsigned,2> active{32,32};
    unsigned previous_kon{}, previous_kof{}, checkpoints{};
    std::uint64_t previous_sample{};
    bool operator==(const ScoreAdsrObserver& other) const {
        return std::tie(notes,active,previous_kon,previous_kof,checkpoints,previous_sample)==
            std::tie(other.notes,other.active,other.previous_kon,other.previous_kof,other.checkpoints,other.previous_sample);
    }

    void capture(const gameboy::SgbHost& host) {
        const auto sample = host.snes_samples_produced();
        const auto half = host.apu_half_clocks();
        const unsigned kon = host.debug_dsp_register(0x4c), kof = host.debug_dsp_register(0x5c);
        for (unsigned i=0;i<2;++i) {
            const unsigned voice=i+2, bit=1U<<voice;
            if ((kon & bit) && !(previous_kon & bit) && host.debug_spc_ram_byte(0xd2)==2 &&
                host.debug_spc_ram_byte(0xdb)==1 && host.cpu().debug_wram_byte(0x26)==1) {
                if (notes.size()>=32) throw std::runtime_error("ADSR note bound");
                Note note;
                note.voice=voice;
                note.source=host.debug_dsp_register(voice*16+4);
                note.adsr1=host.debug_dsp_register(voice*16+5);
                note.adsr2=host.debug_dsp_register(voice*16+6);
                note.gain=host.debug_dsp_register(voice*16+7);
                note.tuning=host.debug_spc_ram_byte(0xec+i);
                note.pitch=host.debug_dsp_register(voice*16+2)|(unsigned(host.debug_dsp_register(voice*16+3))<<8);
                note.on_half=half;
                note.on_sample=note.last_sample=sample;
                notes.push_back(note);
                active[i]=unsigned(notes.size()-1);
                ++checkpoints;
            }
            if (active[i]==32) continue;
            auto& note=notes[active[i]];
            if ((kof & bit) && !(previous_kof & bit) && !note.off_half) {
                note.off_half=half;
                ++checkpoints;
            }
            if (!note.off_half && (host.debug_dsp_register(voice*16+4)!=note.source ||
                    host.debug_dsp_register(voice*16+5)!=note.adsr1 ||
                    host.debug_dsp_register(voice*16+6)!=note.adsr2 ||
                    host.debug_dsp_register(voice*16+7)!=note.gain ||
                    host.debug_spc_ram_byte(0xec+i)!=note.tuning ||
                    (host.debug_dsp_register(voice*16+2)|(unsigned(host.debug_dsp_register(voice*16+3))<<8))!=note.pitch)) {
                if (!note.setup_changes) ++checkpoints;
                ++note.setup_changes;
            }
            if (sample==previous_sample || sample<=note.on_sample || sample-note.on_sample>12000) continue;
            if (sample!=note.last_sample+1) ++note.missed;
            note.last_sample=sample;
            const unsigned env=host.debug_dsp_register(voice*16+8);
            if (!note.off_half) {
                const bool past_peak=note.peak==127;
                if (env>note.peak) {
                    if (note.peak==127) ++note.decay_bad;
                    note.peak=env;
                    note.peak_offset=unsigned(sample-note.on_sample);
                    if (env==127) ++checkpoints;
                }
                if (past_peak && env>note.previous) ++note.decay_bad;
                note.active_last=env;
                for (unsigned p=0;p<offsets.size();++p)
                    if (sample-note.on_sample==offsets[p]) { note.points[p]=env; ++checkpoints; }
            } else {
                if (note.release_samples<note.release_prefix.size()) note.release_prefix[note.release_samples]=env;
                ++note.release_samples;
                if (note.release_start==255) note.release_start=env;
                else if (env<note.previous) {
                    if (note.previous-env!=1) ++note.release_bad;
                    if (note.last_drop && sample-note.last_drop!=2) ++note.cadence_bad;
                    note.last_drop=sample;
                    ++note.drops;
                }
                if (env>note.previous) ++note.release_bad;
                if (!env && !note.zero_half) { note.zero_half=half; ++checkpoints; }
            }
            note.previous=env;
        }
        previous_kon=kon;
        previous_kof=kof;
        previous_sample=sample;
    }
    void print() const {
        std::cout << ",\"envelope_notes\":[";
        for (unsigned i=0;i<notes.size();++i) {
            const auto& n=notes[i];
            std::cout << (i?",":"") << '[' << n.voice << ',' << n.source << ',' << n.adsr1 << ',' << n.adsr2 << ',' << n.gain
                << ',' << n.tuning << ',' << n.pitch << ',' << n.peak << ',' << n.peak_offset << ',' << n.active_last
                << ',' << n.release_start << ',' << n.drops << ',' << n.cadence_bad << ',' << n.decay_bad << ',' << n.release_bad
                << ',' << n.missed << ',' << n.on_half << ',' << n.off_half << ',' << n.zero_half;
            for (auto point:n.points) std::cout << ',' << point;
            std::cout << ']';
        }
        std::cout << "],\"envelope_setup_changes\":[";
        for (unsigned i=0;i<notes.size();++i) std::cout << (i?",":"") << notes[i].setup_changes;
        std::cout << ']';
        std::cout << ",\"envelope_release_prefix\":[";
        for (unsigned i=0;i<notes.size();++i) {
            std::cout << (i?",":"") << '[';
            for (unsigned p=0;p<notes[i].release_prefix.size();++p) std::cout << (p?",":"") << notes[i].release_prefix[p];
            std::cout << ']';
        }
        std::cout << ']';
    }
};
