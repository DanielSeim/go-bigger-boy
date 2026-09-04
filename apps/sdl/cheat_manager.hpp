#pragma once

#ifndef __ANDROID__

#include "gameboy/emulator.hpp"
#include "update_checker.hpp"
#include "tool_window_support.hpp"
#include "window_event.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <future>
#include <iomanip>
#include <iterator>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace gbb::sdl {

class CheatManager {
public:
    ~CheatManager() {
        if (fetch_future_.valid()) fetch_future_.wait();
        close();
    }
    CheatManager(const CheatManager&) = delete;
    CheatManager& operator=(const CheatManager&) = delete;
    CheatManager() = default;

    void load(const std::filesystem::path& preference_directory,
              const gameboy::RomMetadata& metadata) {
        close();
        metadata_ = metadata;
        preference_directory_ = preference_directory;
        directory_ = preference_directory / "cheats";
        std::ostringstream name;
        name << std::hex << std::setfill('0') << std::setw(16)
             << metadata.fingerprint;
        path_ = directory_ / (name.str() + ".cht");
        cheats_.clear();
        archive_attempted_ = false;
        fetch_error_.reset();
        std::ifstream input(path_, std::ios::binary);
        if (input) {
            const std::string text{std::istreambuf_iterator<char>{input}, {}};
            cheats_ = gameboy::parse_libretro_cheats(text, false);
        }
        selected_ = cheats_.empty() ? 0 : std::min(selected_, cheats_.size() - 1);
        scroll_ = 0;
        status_ = cheats_.empty() ? "No cheats loaded. Fetch or add one manually."
                                  : std::to_string(cheats_.size()) +
                                        " cheats loaded for this ROM.";
    }

    void apply(gameboy::Emulator& emulator) const {
        gameboy::apply_gameshark_cheats(cheats_, emulator.bus());
    }

    [[nodiscard]] bool visible() const noexcept { return window_ != nullptr; }
    [[nodiscard]] bool fetching() const noexcept { return fetch_in_progress_; }
    [[nodiscard]] bool take_fetch_request() noexcept {
        return std::exchange(fetch_requested_, false);
    }

    void start_fetch() {
        if (fetch_in_progress_) return;
        fetch_progress_.completed_bytes.store(0);
        fetch_progress_.total_bytes.store(0);
        fetch_progress_.cancel_requested.store(false);
        fetch_in_progress_ = true;
        status_ = "Fetching the matching Libretro archive in the background "
                  "(close to cancel)...";
        fetch_future_ = std::async(std::launch::async, [this] {
            return download_archive_text();
        });
    }

    [[nodiscard]] bool poll_fetch() {
        if (!fetch_in_progress_ || !fetch_future_.valid()) return false;
        if (fetch_future_.wait_for(std::chrono::seconds(0)) !=
            std::future_status::ready) {
            return false;
        }
        try {
            auto text = fetch_future_.get();
            import_archive(text);
        } catch (const std::exception& error) {
            status_ = "No exact archive match. Manual codes are still available.";
            fetch_error_ = error.what();
        }
        fetch_in_progress_ = false;
        return true;
    }

    [[nodiscard]] std::optional<std::string> take_fetch_error() {
        return std::exchange(fetch_error_, std::nullopt);
    }

    void open(SDL_Window* parent) {
        if (visible()) {
            static_cast<void>(SDL_RaiseWindow(window_));
            return;
        }
        window_ = SDL_CreateWindow("Go Bigger Boy - GameShark Cheats", 760, 660,
                                   SDL_WINDOW_RESIZABLE);
        if (window_ == nullptr) throw_sdl_error("Could not create cheat manager window");
        static_cast<void>(SDL_SetWindowMinimumSize(window_, 680, 560));
        renderer_ = SDL_CreateRenderer(window_, nullptr);
        if (renderer_ == nullptr) {
            close();
            throw_sdl_error("Could not create cheat manager renderer");
        }
        if (parent != nullptr) {
            int x = 0;
            int y = 0;
            static_cast<void>(SDL_GetWindowPosition(parent, &x, &y));
            static_cast<void>(SDL_SetWindowPosition(window_, x + 56, y + 56));
        }
        if (cheats_.empty() && !archive_attempted_) {
            archive_attempted_ = true;
            fetch_requested_ = true;
            status_ = "Looking up this ROM in the Libretro cheat archive...";
        }
    }

    void close() noexcept {
        fetch_progress_.cancel_requested.store(true);
        if (fetch_future_.valid()) fetch_future_.wait();
        fetch_in_progress_ = false;
        stop_editing();
        if (renderer_ != nullptr) SDL_DestroyRenderer(renderer_);
        if (window_ != nullptr) SDL_DestroyWindow(window_);
        renderer_ = nullptr;
        window_ = nullptr;
    }

    bool handle_event(const SDL_Event& event) {
        if (!visible()) return false;
        const auto id = SDL_GetWindowID(window_);
        if (gbb::sdl::event_window_id(event) != id) return false;

        if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
            close();
        } else if (event.type == SDL_EVENT_TEXT_INPUT && editing_ != Field::none) {
            auto& target = editing_ == Field::description ? description_ : code_;
            target += event.text.text;
            if (target.size() > 160) target.resize(160);
        } else if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat) {
            if (editing_ != Field::none) {
                auto& target = editing_ == Field::description ? description_ : code_;
                if (event.key.key == SDLK_ESCAPE) stop_editing();
                else if (event.key.key == SDLK_BACKSPACE && !target.empty()) {
                    target.pop_back();
                } else if (event.key.key == SDLK_TAB) {
                    begin_edit(editing_ == Field::description ? Field::code
                                                              : Field::description);
                } else if (event.key.key == SDLK_RETURN ||
                           event.key.key == SDLK_KP_ENTER) {
                    add_manual();
                }
            } else if (event.key.key == SDLK_ESCAPE) {
                close();
            } else if (event.key.key == SDLK_UP && !cheats_.empty()) {
                if (selected_ > 0) --selected_;
                reveal_selected();
            } else if (event.key.key == SDLK_DOWN && !cheats_.empty()) {
                if (selected_ + 1 < cheats_.size()) ++selected_;
                reveal_selected();
            } else if (event.key.key == SDLK_SPACE && !cheats_.empty()) {
                cheats_[selected_].enabled = !cheats_[selected_].enabled;
                save();
            } else if (event.key.key == SDLK_DELETE && !cheats_.empty()) {
                erase_selected();
            }
        } else if (event.type == SDL_EVENT_MOUSE_WHEEL) {
            if (event.wheel.y > 0 && scroll_ > 0) --scroll_;
            if (event.wheel.y < 0 && scroll_ + visible_rows_ < cheats_.size()) {
                ++scroll_;
            }
        } else if (event.type == SDL_EVENT_MOUSE_BUTTON_UP &&
                   event.button.button == SDL_BUTTON_LEFT) {
            int width = 0;
            int height = 0;
            static_cast<void>(SDL_GetWindowSize(window_, &width, &height));
            const auto x = event.button.x;
            const auto y = event.button.y;
            const auto list_bottom = static_cast<float>(height - 182);
            if (y >= 92 && y < list_bottom) {
                const auto row = static_cast<std::size_t>((y - 92) / 30);
                const auto index = scroll_ + row;
                if (index < cheats_.size()) {
                    selected_ = index;
                    if (x < 60) {
                        cheats_[index].enabled = !cheats_[index].enabled;
                        save();
                    }
                }
            } else if (y >= height - 150 && y <= height - 120) {
                if (x >= 24 && x < width / 2 - 8) begin_edit(Field::description);
                else if (x >= width / 2 + 8 && x <= width - 24) {
                    begin_edit(Field::code);
                }
            } else if (y >= height - 100 && y <= height - 64) {
                if (x >= 24 && x <= 178 && !fetch_in_progress_) {
                    fetch_requested_ = true;
                }
                else if (x >= 194 && x <= 334) add_manual();
                else if (x >= 350 && x <= 490) erase_selected();
                else if (x >= width - 144 && x <= width - 24) close();
            }
        }
        return true;
    }

    void fetch_archive() {
        auto text = download_archive_text();
        import_archive(text);
    }

  private:
    [[noreturn]] static void throw_sdl_error(const char* action) {
        throw std::runtime_error(std::string(action) + ": " + SDL_GetError());
    }

    std::string download_archive_text() {
        if (directory_.empty()) throw std::runtime_error("No ROM is active");
        archive_attempted_ = true;
        std::filesystem::create_directories(directory_);
        const auto remote = directory_ / "archive-download.cht";
        const auto system = std::string{gameboy::cover_system_name(metadata_.platform)};
        const auto canonical_name = resolve_canonical_name(system);
        const auto url = "https://raw.githubusercontent.com/libretro/"
                         "libretro-database/master/cht/" +
                         url_component(system) + "/" +
                         url_component(canonical_name + ".cht");
        std::string error;
        if (!gbb_desktop::download_public_file(
                url, remote, 4 * 1024 * 1024, error, &fetch_progress_)) {
            throw std::runtime_error(
                "No exact Libretro cheat archive match was found for:\n" +
                canonical_name +
                "\n\nUse a No-Intro named ROM or add a code manually.\n" + error);
        }
        std::string text;
        {
            std::ifstream input(remote, std::ios::binary);
            if (!input) {
                throw std::runtime_error(
                    "Could not open the downloaded cheat archive");
            }
            text.assign(std::istreambuf_iterator<char>{input}, {});
            if (input.bad()) {
                throw std::runtime_error(
                    "Could not read the downloaded cheat archive");
            }
        }
        std::error_code cleanup_error;
        std::filesystem::remove(remote, cleanup_error);
        return text;
    }

    void import_archive(const std::string& text) {
        auto imported = gameboy::parse_libretro_cheats(text, true);
        if (imported.empty()) {
            status_ = "The archive has no supported type-01 codes.";
            throw std::runtime_error(
                "The archive file contains no supported Game Boy type-01 codes");
        }
        std::size_t added = 0;
        for (auto& cheat : imported) {
            const auto duplicate = std::any_of(
                cheats_.begin(), cheats_.end(), [&](const auto& existing) {
                    return existing.code == cheat.code;
                });
            if (!duplicate) {
                cheats_.push_back(std::move(cheat));
                ++added;
            }
        }
        save();
        status_ = std::to_string(added) + " archive cheats added (" +
                  std::to_string(cheats_.size()) + " total).";
    }

public:
    void present() {
        if (!visible()) return;
        int width = 0;
        int height = 0;
        static_cast<void>(SDL_GetWindowSize(window_, &width, &height));
        visible_rows_ = std::max<std::size_t>(1, (height - 274) / 30);
        if (scroll_ + visible_rows_ > cheats_.size() &&
            cheats_.size() > visible_rows_) {
            scroll_ = cheats_.size() - visible_rows_;
        }
        static_cast<void>(SDL_SetRenderDrawColor(renderer_, 8, 12, 20, 255));
        static_cast<void>(SDL_RenderClear(renderer_));
        text(24, 20, "GAMESHARK CHEATS / " + metadata_.title, 69, 207, 238);
        text(24, 42, "ROM-aware source: Libretro Database (CC BY-SA 4.0)",
             177, 192, 208);
        auto status = status_;
        if (fetch_in_progress_) {
            const auto total = fetch_progress_.total_bytes.load();
            const auto completed = fetch_progress_.completed_bytes.load();
            if (total > 0) {
                const auto percent = std::min<std::uintmax_t>(
                    100, completed * 100 / total);
                status += " " + std::to_string(percent) + "%";
            }
        }
        text(24, 64, status, 177, 192, 208);
        text(26, 82, "ON", 238, 249, 255);
        text(62, 82, "DESCRIPTION", 238, 249, 255);
        text(static_cast<float>(width - 210), 82, "SOURCE", 238, 249, 255);
        for (std::size_t row = 0; row < visible_rows_; ++row) {
            const auto index = scroll_ + row;
            if (index >= cheats_.size()) break;
            const auto y = 92.0F + static_cast<float>(row) * 30.0F;
            const SDL_FRect background{20, y, static_cast<float>(width - 40), 27};
            const auto selected = index == selected_;
            static_cast<void>(SDL_SetRenderDrawColor(
                renderer_, selected ? 28 : 14, selected ? 47 : 26,
                selected ? 68 : 38, 255));
            static_cast<void>(SDL_RenderFillRect(renderer_, &background));
            text(29, y + 9, cheats_[index].enabled ? "[X]" : "[ ]",
                 cheats_[index].enabled ? 69 : 177,
                 cheats_[index].enabled ? 207 : 192,
                 cheats_[index].enabled ? 238 : 208);
            text(62, y + 9, shortened(cheats_[index].description, 64),
                 238, 249, 255);
            text(static_cast<float>(width - 210), y + 9,
                 cheats_[index].from_archive ? "LIBRETRO" : "MANUAL",
                 177, 192, 208);
        }
        const auto field_y = static_cast<float>(height - 150);
        field({24, field_y, static_cast<float>(width / 2 - 32), 30},
              description_.empty() ? "Description" : description_,
              editing_ == Field::description);
        field({static_cast<float>(width / 2 + 8), field_y,
               static_cast<float>(width / 2 - 32), 30},
              code_.empty() ? "Code: 01VVLLHH[+...]" : code_,
              editing_ == Field::code);
        const auto button_y = static_cast<float>(height - 100);
        button({24, button_y, 154, 36},
               fetch_in_progress_ ? "FETCHING..." : "FETCH FOR ROM");
        button({194, button_y, 140, 36}, "ADD MANUAL");
        button({350, button_y, 140, 36}, "DELETE");
        button({static_cast<float>(width - 144), button_y, 120, 36}, "CLOSE");
        text(24, static_cast<float>(height - 42),
             "Click [ ] to toggle. Up/Down select, Space toggles, Delete removes.",
             177, 192, 208);
        static_cast<void>(SDL_RenderPresent(renderer_));
    }

private:
    enum class Field { none, description, code };

    static std::string url_component(const std::string& value) {
        std::ostringstream output;
        output << std::uppercase << std::hex;
        for (const auto raw : value) {
            const auto character = static_cast<unsigned char>(raw);
            if (std::isalnum(character) || character == '-' || character == '_' ||
                character == '.' || character == '~') {
                output << static_cast<char>(character);
            } else {
                output << '%' << std::setw(2) << std::setfill('0')
                       << static_cast<unsigned>(character);
            }
        }
        return output.str();
    }

    static std::string shortened(const std::string& value,
                                 const std::size_t maximum) {
        if (value.size() <= maximum) return value;
        return value.substr(0, maximum - 3) + "...";
    }

    static std::string quoted_value(const std::string& line) {
        const auto first = line.find('"');
        const auto last = line.rfind('"');
        return first == std::string::npos || last <= first
                   ? std::string{}
                   : line.substr(first + 1, last - first - 1);
    }

    std::string resolve_canonical_name(const std::string& system) const {
        auto filename = system;
        for (auto& character : filename) {
            if (!std::isalnum(static_cast<unsigned char>(character))) character = '-';
        }
        const auto database = preference_directory_ / "metadata" /
                              (filename + ".dat");
        if (!std::filesystem::is_regular_file(database)) {
            std::string ignored_error;
            const auto url =
                "https://raw.githubusercontent.com/libretro/libretro-database/"
                "master/metadat/no-intro/" + url_component(system + ".dat");
            static_cast<void>(gbb_desktop::download_public_file(
                url, database, 3 * 1024 * 1024, ignored_error));
        }
        std::ifstream input(database);
        std::string line;
        std::string name;
        while (std::getline(input, line)) {
            const auto first = line.find_first_not_of(" \t\r\n");
            if (first == std::string::npos) continue;
            line.erase(0, first);
            if (line == "game (") {
                name.clear();
            } else if (line.rfind("name \"", 0) == 0 && name.empty()) {
                name = quoted_value(line);
            } else if (line.rfind("rom (", 0) == 0 && !name.empty()) {
                const auto marker = line.find(" crc ");
                if (marker == std::string::npos || marker + 13 > line.size()) {
                    continue;
                }
                std::uint32_t crc{};
                std::istringstream value(line.substr(marker + 5, 8));
                if ((value >> std::hex >> crc) && crc == metadata_.crc32) {
                    return name;
                }
            }
        }
        return metadata_.cover_name;
    }

    void text(const float x, const float y, const std::string& value,
              const std::uint8_t r, const std::uint8_t g,
              const std::uint8_t b) const {
        static_cast<void>(SDL_SetRenderDrawColor(renderer_, r, g, b, 255));
        static_cast<void>(SDL_RenderDebugText(renderer_, x, y, value.c_str()));
    }

    void button(const SDL_FRect& rect, const char* label) const {
        draw_tool_button_background(renderer_, window_, rect);
        text(rect.x + 10, rect.y + 14, label, 238, 249, 255);
    }

    void field(const SDL_FRect& rect, const std::string& value,
               const bool active) const {
        static_cast<void>(SDL_SetRenderDrawColor(renderer_, 14, 26, 38, 255));
        static_cast<void>(SDL_RenderFillRect(renderer_, &rect));
        static_cast<void>(SDL_SetRenderDrawColor(renderer_, active ? 69 : 69,
                                                 active ? 207 : 112,
                                                 active ? 238 : 133, 255));
        static_cast<void>(SDL_RenderRect(renderer_, &rect));
        text(rect.x + 8, rect.y + 11, shortened(value, 42), 238, 249, 255);
    }

    void begin_edit(const Field field) {
        editing_ = field;
        static_cast<void>(SDL_StartTextInput(window_));
    }

    void stop_editing() noexcept {
        if (window_ != nullptr && editing_ != Field::none) SDL_StopTextInput(window_);
        editing_ = Field::none;
    }

    void add_manual() {
        try {
            auto writes = gameboy::parse_gameshark_code(code_);
            auto description = description_.empty() ? code_ : description_;
            cheats_.push_back({std::move(description), code_, true, false,
                               std::move(writes)});
            selected_ = cheats_.size() - 1;
            reveal_selected();
            description_.clear();
            code_.clear();
            stop_editing();
            save();
            status_ = "Manual cheat added and enabled.";
        } catch (const std::exception& error) {
            status_ = std::string{"Invalid code: "} + error.what();
        }
    }

    void erase_selected() {
        if (cheats_.empty()) return;
        cheats_.erase(cheats_.begin() + static_cast<std::ptrdiff_t>(selected_));
        if (selected_ >= cheats_.size() && selected_ > 0) --selected_;
        reveal_selected();
        save();
        status_ = "Cheat removed.";
    }

    void reveal_selected() {
        if (selected_ < scroll_) scroll_ = selected_;
        if (selected_ >= scroll_ + visible_rows_) {
            scroll_ = selected_ - visible_rows_ + 1;
        }
    }

    void save() const {
        if (path_.empty()) return;
        std::filesystem::create_directories(path_.parent_path());
        std::ofstream output(path_, std::ios::binary | std::ios::trunc);
        if (!output) throw std::runtime_error("Could not save the ROM cheat list");
        output << gameboy::serialize_libretro_cheats(cheats_);
    }

    SDL_Window* window_{};
    SDL_Renderer* renderer_{};
    gameboy::RomMetadata metadata_{};
    std::filesystem::path preference_directory_;
    std::filesystem::path directory_;
    std::filesystem::path path_;
    std::vector<gameboy::GameSharkCheat> cheats_;
    std::size_t selected_{};
    std::size_t scroll_{};
    std::size_t visible_rows_{10};
    Field editing_{Field::none};
    std::string description_;
    std::string code_;
    std::string status_;
    bool fetch_requested_{};
    bool archive_attempted_{};
    std::future<std::string> fetch_future_;
    bool fetch_in_progress_{};
    std::optional<std::string> fetch_error_;
    gbb_desktop::DownloadProgress fetch_progress_;
};

} // namespace gbb::sdl

#endif
