// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "sgb_score_adsr_observer.hpp"

// Diagnostic only: native PCM windows after actual observed KON edges.
// No DSP/core hooks, inferred samples or corrections to the sample timeline.
struct HostSamplePitchObserver {
    static constexpr unsigned frames = 2048;
    struct Window {
        unsigned voice{}, source{}, pitch{};
        std::uint64_t on_sample{};
        std::vector<std::int16_t> pcm;
        bool operator==(const Window& other) const {
            return std::tie(voice,source,pitch,on_sample,pcm)==
                   std::tie(other.voice,other.source,other.pitch,other.on_sample,other.pcm);
        }
    };
    unsigned rate{};
    std::vector<Window> windows;
    bool operator==(const HostSamplePitchObserver& other) const {
        return rate==other.rate && windows==other.windows;
    }
    void sample(std::uint64_t index, const gameboy::SgbHost::StereoSample& value) {
        if (windows.empty() || windows.back().pcm.size()==frames) return;
        auto& window=windows.back();
        if (index!=window.on_sample+window.pcm.size()+1 || value.left!=value.right)
            throw std::runtime_error("discontinuous or asymmetric native pitch PCM");
        window.pcm.push_back(value.left);
    }
    void sync(const ScoreAdsrObserver& envelopes) {
        while (windows.size()<envelopes.notes.size()) {
            if (windows.size()>=13 || (!windows.empty() && windows.back().pcm.size()!=frames))
                throw std::runtime_error("pitch note/window bound");
            const auto& note=envelopes.notes[windows.size()];
            windows.push_back({note.voice,note.source,note.pitch,note.on_sample,{}});
        }
    }
    unsigned checkpoint() const {
        unsigned count=0;
        for (const auto& window:windows) count+=unsigned(window.pcm.size());
        return count/512;
    }
    void validate(const ScoreAdsrObserver& envelopes) const {
        if (rate!=32000 || windows.size()!=13 || envelopes.notes.size()!=13)
            throw std::runtime_error("incomplete native pitch octave");
        for (unsigned i=0;i<13;++i) {
            const auto& note=envelopes.notes[i];
            if (windows[i].pcm.size()!=frames || !note.off_half ||
                note.off_half<=note.on_half+64*(frames+2) || note.setup_changes || note.missed)
                throw std::runtime_error("pitch window crosses gate or changed setup");
        }
    }
    void emit() const {
        std::cout << ",\"acoustic\":{\"sample_rate_hz\":" << rate
                  << ",\"frames_per_window\":" << frames << ",\"windows\":[";
        for (unsigned i=0;i<windows.size();++i) {
            const auto& window=windows[i];
            std::cout << (i ? "," : "") << "{\"voice\":" << window.voice
                << ",\"slot\":" << window.source << ",\"pitch\":" << window.pitch
                << ",\"on_sample\":" << window.on_sample << ",\"pcm\":[";
            for (unsigned j=0;j<window.pcm.size();++j)
                std::cout << (j ? "," : "") << window.pcm[j];
            std::cout << "]}";
        }
        std::cout << "]}";
    }
};
