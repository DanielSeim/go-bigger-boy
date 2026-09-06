#include "audio_output.hpp"

#include "gameboy/apu.hpp"
#include "gbb/audio.hpp"
#include "gbb/frontend_logging.hpp"

#include <SDL3/SDL.h>

#include <stdexcept>
#include <string>

namespace gbb::sdl {

namespace {

// Keep enough audio for a short packet burst or frame-pacing overrun, without
// adding a noticeable startup delay. At 48 kHz stereo this is 7680 bytes.
constexpr std::size_t playback_prebuffer_ms = 40;

} // namespace

AudioOutput::AudioOutput() {
    if (SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        const SDL_AudioSpec audio_spec{
            SDL_AUDIO_S16, 2, static_cast<int>(gameboy::Apu::sample_rate)};
        stream_ = SDL_OpenAudioDeviceStream(
            SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &audio_spec, nullptr, nullptr);
        // Leave the stream paused until submit() has queued a small cushion.
        // Starting the device immediately lets it consume the first frame
        // while the emulation thread is still producing the next one.
    }
    if (stream_ == nullptr) {
        gbb::log_frontend_warning(
            std::string("Audio output is unavailable: ") + SDL_GetError());
    }
}

AudioOutput::~AudioOutput() {
    close();
}

void AudioOutput::close() noexcept {
    if (stream_ != nullptr) {
        SDL_DestroyAudioStream(stream_);
        stream_ = nullptr;
    }
    playback_started_ = false;
}

void AudioOutput::clear() noexcept {
    if (stream_ != nullptr) {
        static_cast<void>(SDL_PauseAudioStreamDevice(stream_));
        static_cast<void>(SDL_ClearAudioStream(stream_));
    }
    // State loads, ROM changes, and debugger operations can introduce a
    // scheduling gap. Re-prime the stream instead of exposing that gap as a
    // click or a short burst of repeated samples.
    playback_started_ = false;
}

int AudioOutput::queued_bytes() const noexcept {
    return stream_ == nullptr ? -1 : SDL_GetAudioStreamQueued(stream_);
}

void AudioOutput::submit(gbb::EmulatorCore* core,
                         const bool fast_forward,
                         const unsigned fast_forward_factor) {
    if (core == nullptr) return;
    auto samples = core->take_audio_samples();
    if (fast_forward && fast_forward_factor > 1 && !samples.empty()) {
        samples = gbb::downsample_audio_box(samples, 2, fast_forward_factor);
    }
    if (stream_ == nullptr || samples.empty()) return;

    const auto& descriptor = core->descriptor();
    const auto maximum_queued_bytes =
        gbb::audio_queue_bytes(descriptor.audio_sample_rate,
                               descriptor.audio_channels, 200);
    if (queued_bytes() > static_cast<int>(maximum_queued_bytes)) {
        // A debugger pause, window drag, suspended mobile activity, or a link
        // wait with no newly generated samples can leave stale audio behind.
        // Recover latency rather than playing an old buffer seconds after its
        // corresponding frame.
        clear();
    }
    if (!SDL_PutAudioStreamData(
            stream_, samples.data(),
            static_cast<int>(samples.size() * sizeof(samples.front())))) {
        throw std::runtime_error(std::string{"Could not queue audio samples: "} +
                                 SDL_GetError());
    }
    if (!playback_started_) {
        const auto prebuffer_bytes = gbb::audio_queue_bytes(
            gameboy::Apu::sample_rate, 2, playback_prebuffer_ms);
        if (queued_bytes() < static_cast<int>(prebuffer_bytes)) return;
        if (!SDL_ResumeAudioStreamDevice(stream_)) {
            gbb::log_frontend_warning(
                std::string{"Audio playback is unavailable: "} +
                SDL_GetError());
            close();
            return;
        }
        playback_started_ = true;
    }
}

} // namespace gbb::sdl
