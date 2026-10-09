// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "sgb_score_adsr_observer.hpp"

// Actual 48-kHz output windows; the native DSP counter is a different clock.
struct HostSamplePitchObserver {
    static constexpr unsigned frames=3072;
    struct Window {
        unsigned voice{}, source{}, pitch{};
        std::uint64_t on_sample{}, start_half{}, end_half{};
        std::vector<std::int16_t> pcm, left;
        bool operator==(const Window& other) const {
            return std::tie(voice,source,pitch,on_sample,start_half,end_half,pcm,left)==
                std::tie(other.voice,other.source,other.pitch,other.on_sample,other.start_half,other.end_half,other.pcm,other.left);
        }
    };
    unsigned rate{};
    std::uint64_t gb_samples{}, clipped_samples{};
    std::vector<Window> windows;
    bool operator==(const HostSamplePitchObserver& other) const {
        return rate==other.rate && gb_samples==other.gb_samples && clipped_samples==other.clipped_samples && windows==other.windows;
    }
    void sample(std::uint64_t index, const gameboy::SgbHost::StereoSample& value, std::uint64_t half) {
        if (windows.empty() || windows.back().pcm.size()==frames) return;
        auto& window=windows.back();
        if (index!=window.on_sample+window.pcm.size()+1 || half<window.end_half)
            throw std::runtime_error("discontinuous combined pitch PCM");
        window.pcm.push_back(value.right);
        window.left.push_back(value.left);
        window.end_half=half;
    }
    void sync(const ScoreAdsrObserver& envelopes, std::uint64_t output, std::uint64_t half) {
        while (windows.size()<envelopes.notes.size()) {
            if (windows.size()>=13 || (!windows.empty() && windows.back().pcm.size()!=frames))
                throw std::runtime_error("combined pitch note/window bound");
            const auto& note=envelopes.notes[windows.size()];
            windows.push_back({note.voice,note.source,note.pitch,output,half,half,{},{}});
        }
    }
    unsigned checkpoint() const {
        unsigned count=0;
        for (const auto& window:windows) count+=unsigned(window.pcm.size());
        return count/512;
    }
    void validate(const ScoreAdsrObserver& envelopes) const {
        if (clipped_samples || !gb_samples || rate!=48000 || windows.size()!=13 || envelopes.notes.size()!=13)
            throw std::runtime_error("incomplete combined pitch octave");
        for (unsigned i=0;i<13;++i) {
            const auto& note=envelopes.notes[i];
            const auto& window=windows[i];
            if (window.pcm.size()!=frames || window.left.size()!=frames || !note.off_half ||
                window.start_half<note.on_half || window.end_half>=note.off_half ||
                window.end_half<=window.start_half || note.setup_changes || note.missed)
                throw std::runtime_error("combined pitch window crosses gate or changed setup");
        }
    }
    void emit() const {
        std::cout << ",\"acoustic\":{\"sample_rate_hz\":" << rate
                  << ",\"gb_samples_captured\":" << gb_samples
                  << ",\"clipped_samples\":" << clipped_samples
                  << ",\"frames_per_window\":" << frames << ",\"windows\":[";
        for (unsigned i=0;i<windows.size();++i) {
            const auto& window=windows[i];
            std::cout << (i ? "," : "") << "{\"voice\":" << window.voice
                << ",\"slot\":" << window.source << ",\"pitch\":" << window.pitch
                << ",\"on_sample\":" << window.on_sample
                << ",\"start_half\":" << window.start_half << ",\"end_half\":" << window.end_half;
            for (unsigned channel=0;channel<2;++channel) {
                const auto& values=channel ? window.left : window.pcm;
                std::cout << (channel ? ",\"left\":[" : ",\"pcm\":[");
                for (unsigned j=0;j<values.size();++j) std::cout << (j ? "," : "") << values[j];
                std::cout << ']';
            }
            std::cout << '}';
        }
        std::cout << "]}";
    }
};
