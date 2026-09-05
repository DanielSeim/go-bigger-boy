#include "audio_output.hpp"

#include "gameboy/apu.hpp"
#include "gbb/audio.hpp"
#include "gbb/frontend_logging.hpp"

#include <SDL3/SDL.h>

#include <stdexcept>
#include <string>

namespace gbb::sdl {

AudioOutput::AudioOutput() {
    if (SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        const SDL_AudioSpec audio_spec{
            SDL_AUDIO_S16, 2, static_cast<int>(gameboy::Apu::sample_rate)};
        stream_ = SDL_OpenAudioDeviceStream(
            SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &audio_spec, nullptr, nullptr);
        if (stream_ != nullptr && !SDL_ResumeAudioStreamDevice(stream_)) {
            SDL_DestroyAudioStream(stream_);
            stream_ = nullptr;
        }
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
}

void AudioOutput::clear() noexcept {
    if (stream_ != nullptr) static_cast<void>(SDL_ClearAudioStream(stream_));
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
}

} // namespace gbb::sdl
