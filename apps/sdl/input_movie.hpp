#pragma once

#ifndef __ANDROID__

#include "gameboy/emulator.hpp"

#include <array>
#include <cstdint>
#include <cstddef>
#include <filesystem>
#include <iosfwd>
#include <vector>

namespace gbb::sdl {

class InputMovie final {
public:
    enum class Mode { idle, recording, replaying };
    static constexpr std::array movie_buttons{
        gameboy::Button::right, gameboy::Button::left, gameboy::Button::up,
        gameboy::Button::down, gameboy::Button::a, gameboy::Button::b,
        gameboy::Button::select, gameboy::Button::start};

    [[nodiscard]] Mode mode() const noexcept { return mode_; }
    [[nodiscard]] bool recording() const noexcept {
        return mode_ == Mode::recording;
    }
    [[nodiscard]] bool replaying() const noexcept {
        return mode_ == Mode::replaying;
    }
    [[nodiscard]] std::size_t event_count() const noexcept {
        return events_.size();
    }

    void start_recording(gameboy::Emulator& emulator);
    void stop_and_save(const std::filesystem::path& path,
                       gameboy::Emulator& emulator);
    void save_frame_inputs(gameboy::Emulator& emulator,
                           const std::filesystem::path& path,
                           std::uint64_t fingerprint,
                           const std::vector<std::uint8_t>& start_state,
                           const std::vector<std::uint8_t>& frame_masks);
    void start_replay(gameboy::Emulator& emulator,
                      const std::filesystem::path& path);
    void stop(gameboy::Emulator* emulator = nullptr) noexcept;
    void set_button(gameboy::Emulator& emulator, gameboy::Button button,
                    bool pressed);
    void release_all(gameboy::Emulator& emulator);
    [[nodiscard]] bool update_replay(gameboy::Emulator& emulator);

private:
    struct Event {
        std::uint64_t cycle{};
        std::uint8_t button{};
        bool pressed{};
    };

    template <typename Value>
    static void write(std::ostream& output, Value value) {
        output.write(reinterpret_cast<const char*>(&value), sizeof(value));
    }

    template <typename Value>
    static Value read(std::istream& input) {
        Value value{};
        input.read(reinterpret_cast<char*>(&value), sizeof(value));
        return value;
    }

    void save_file(const std::filesystem::path& path) const;

    Mode mode_{Mode::idle};
    std::uint64_t fingerprint_{};
    std::uint64_t origin_cycles_{};
    std::vector<std::uint8_t> start_state_;
    std::vector<Event> events_;
    std::array<bool, movie_buttons.size()> pressed_{};
    std::size_t next_event_{};
};

} // namespace gbb::sdl

#endif // __ANDROID__
