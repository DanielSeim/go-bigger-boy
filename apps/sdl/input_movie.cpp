#include "input_movie.hpp"

#ifndef __ANDROID__

#include <algorithm>
#include <array>
#include <fstream>
#include <stdexcept>

namespace gbb::sdl {

void InputMovie::start_recording(gameboy::Emulator& emulator) {
    for (const auto button : movie_buttons) emulator.set_button(button, false);
    pressed_.fill(false);
    start_state_ = emulator.save_state();
    fingerprint_ = emulator.rom_fingerprint();
    origin_cycles_ = emulator.cpu().total_cycles();
    events_.clear();
    next_event_ = 0;
    mode_ = Mode::recording;
}

void InputMovie::stop_and_save(const std::filesystem::path& path,
                               gameboy::Emulator& emulator) {
    if (!recording()) return;
    release_all(emulator);
    mode_ = Mode::idle;
    save_file(path);
}

void InputMovie::save_frame_inputs(
    gameboy::Emulator& emulator, const std::filesystem::path& path,
    const std::uint64_t fingerprint,
    const std::vector<std::uint8_t>& start_state,
    const std::vector<std::uint8_t>& frame_masks) {
    if (start_state.empty() || frame_masks.empty()) {
        throw std::runtime_error("The TAS timeline has no frames.");
    }
    fingerprint_ = fingerprint;
    start_state_ = start_state;
    events_.clear();
    const auto restore_state = emulator.save_state();
    try {
        emulator.load_state(start_state_);
        const auto origin = emulator.cpu().total_cycles();
        std::uint8_t previous = 0;
        for (const auto current : frame_masks) {
            const auto changed = static_cast<std::uint8_t>(previous ^ current);
            const auto elapsed = emulator.cpu().total_cycles() - origin;
            for (std::size_t button = 0; button < movie_buttons.size();
                 ++button) {
                const auto bit = static_cast<std::uint8_t>(1U << button);
                if ((changed & bit) == 0) continue;
                const auto pressed = (current & bit) != 0;
                events_.push_back({elapsed, static_cast<std::uint8_t>(button),
                                   pressed});
                emulator.set_button(movie_buttons[button], pressed);
            }
            if (emulator.frame_ready()) emulator.consume_frame();
            unsigned cycles = 0;
            const auto lcd_enabled = (emulator.bus().read8(0xFF40) & 0x80U) != 0;
            const auto disabled_lcd_budget =
                emulator.bus().double_speed() ? 140448U : 70224U;
            const auto budget = lcd_enabled ? 280896U : disabled_lcd_budget;
            while (cycles < budget && !emulator.frame_ready()) {
                cycles += emulator.step();
            }
            if (emulator.frame_ready()) emulator.consume_frame();
            previous = current;
        }
        const auto elapsed = emulator.cpu().total_cycles() - origin;
        for (std::size_t button = 0; button < movie_buttons.size(); ++button) {
            const auto bit = static_cast<std::uint8_t>(1U << button);
            if ((previous & bit) != 0) {
                events_.push_back({elapsed, static_cast<std::uint8_t>(button),
                                   false});
            }
        }
        emulator.load_state(restore_state);
    } catch (...) {
        emulator.load_state(restore_state);
        throw;
    }
    mode_ = Mode::idle;
    next_event_ = 0;
    save_file(path);
}

void InputMovie::start_replay(gameboy::Emulator& emulator,
                              const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("No input recording is available yet.");
    std::array<char, 8> magic{};
    input.read(magic.data(), static_cast<std::streamsize>(magic.size()));
    constexpr std::array<char, 8> expected{'G', 'B', 'B', 'M', 'O', 'V', '1', '\0'};
    if (magic != expected) throw std::runtime_error("Invalid input recording file.");
    const auto fingerprint = read<std::uint64_t>(input);
    const auto state_size = read<std::uint32_t>(input);
    const auto event_count = read<std::uint32_t>(input);
    if (fingerprint != emulator.rom_fingerprint()) {
        throw std::runtime_error("This recording belongs to a different ROM.");
    }
    if (state_size == 0 || state_size > 2U * 1024U * 1024U ||
        event_count > 1000000U) {
        throw std::runtime_error("Input recording is unreasonably large.");
    }
    start_state_.resize(state_size);
    input.read(reinterpret_cast<char*>(start_state_.data()),
               static_cast<std::streamsize>(start_state_.size()));
    events_.clear();
    events_.reserve(event_count);
    for (std::uint32_t index = 0; index < event_count; ++index) {
        Event event;
        event.cycle = read<std::uint64_t>(input);
        event.button = read<std::uint8_t>(input);
        event.pressed = read<std::uint8_t>(input) != 0;
        if (event.button >= movie_buttons.size() ||
            (!events_.empty() && event.cycle < events_.back().cycle)) {
            throw std::runtime_error("Input recording contains invalid events.");
        }
        events_.push_back(event);
    }
    if (!input) throw std::runtime_error("Input recording is truncated.");
    emulator.load_state(start_state_);
    pressed_.fill(false);
    fingerprint_ = fingerprint;
    origin_cycles_ = emulator.cpu().total_cycles();
    next_event_ = 0;
    mode_ = Mode::replaying;
}

void InputMovie::stop(gameboy::Emulator* emulator) noexcept {
    if (emulator != nullptr) {
        for (const auto button : movie_buttons) emulator->set_button(button, false);
    }
    pressed_.fill(false);
    mode_ = Mode::idle;
    next_event_ = 0;
}

void InputMovie::set_button(gameboy::Emulator& emulator,
                            const gameboy::Button button, const bool pressed) {
    if (replaying()) return;
    const auto found = std::find(movie_buttons.begin(), movie_buttons.end(), button);
    if (found == movie_buttons.end()) return;
    const auto index = static_cast<std::size_t>(found - movie_buttons.begin());
    if (pressed_[index] == pressed) return;
    pressed_[index] = pressed;
    emulator.set_button(button, pressed);
    if (recording()) {
        events_.push_back({emulator.cpu().total_cycles() - origin_cycles_,
                           static_cast<std::uint8_t>(index), pressed});
    }
}

void InputMovie::release_all(gameboy::Emulator& emulator) {
    for (const auto button : movie_buttons) set_button(emulator, button, false);
}

bool InputMovie::update_replay(gameboy::Emulator& emulator) {
    if (!replaying()) return false;
    const auto elapsed = emulator.cpu().total_cycles() - origin_cycles_;
    while (next_event_ < events_.size() && events_[next_event_].cycle <= elapsed) {
        const auto& event = events_[next_event_++];
        pressed_[event.button] = event.pressed;
        emulator.set_button(movie_buttons[event.button], event.pressed);
    }
    if (next_event_ == events_.size()) {
        stop(&emulator);
        return true;
    }
    return false;
}

void InputMovie::save_file(const std::filesystem::path& path) const {
    if (path.empty()) return;
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) throw std::runtime_error("Could not save input recording.");
    constexpr std::array<char, 8> magic{'G', 'B', 'B', 'M', 'O', 'V', '1', '\0'};
    output.write(magic.data(), static_cast<std::streamsize>(magic.size()));
    write(output, fingerprint_);
    write(output, static_cast<std::uint32_t>(start_state_.size()));
    write(output, static_cast<std::uint32_t>(events_.size()));
    output.write(reinterpret_cast<const char*>(start_state_.data()),
                 static_cast<std::streamsize>(start_state_.size()));
    for (const auto& event : events_) {
        write(output, event.cycle);
        write(output, event.button);
        write(output, static_cast<std::uint8_t>(event.pressed ? 1 : 0));
    }
    if (!output) throw std::runtime_error("Could not finish input recording.");
}

} // namespace gbb::sdl

#endif // __ANDROID__
