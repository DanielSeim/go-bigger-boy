#pragma once

#ifndef __ANDROID__

#include "gameboy/emulator.hpp"
#include "tool_window_support.hpp"
#include "window_event.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace gbb::sdl {

class SpriteEditor {
public:
    struct IpsExportResult {
        std::size_t exported{};
        std::size_t unresolved{};
    };

    SpriteEditor() = default;
    ~SpriteEditor() { close(); }
    SpriteEditor(const SpriteEditor&) = delete;
    SpriteEditor& operator=(const SpriteEditor&) = delete;

    [[nodiscard]] bool visible() const noexcept { return window_ != nullptr; }
    [[nodiscard]] bool take_save_patch_request() noexcept {
        return std::exchange(save_patch_requested_, false);
    }
    [[nodiscard]] bool take_load_patch_request() noexcept {
        return std::exchange(load_patch_requested_, false);
    }
    [[nodiscard]] bool take_export_ips_request() noexcept {
        return std::exchange(export_ips_requested_, false);
    }
    [[nodiscard]] bool has_unsaved_changes(
        const gameboy::Emulator& emulator) const {
        return current_vram(emulator) != saved_snapshot_;
    }
    void mark_saved(const gameboy::Emulator& emulator) {
        saved_snapshot_ = current_vram(emulator);
    }

    void open(SDL_Window* parent, const gameboy::Emulator& emulator) {
        if (visible()) {
            if (baseline_.empty() || fingerprint_ != emulator.rom_fingerprint()) {
                capture_baseline(emulator);
            }
            SDL_RaiseWindow(window_);
            return;
        }
        window_ = SDL_CreateWindow("Go Bigger Boy - Live Sprite Editor",
                                   1000, 780, SDL_WINDOW_RESIZABLE);
        if (window_ == nullptr) throw_sdl_error("Could not create sprite editor window");
        static_cast<void>(SDL_SetWindowMinimumSize(window_, 960, 760));
        renderer_ = SDL_CreateRenderer(window_, nullptr);
        if (renderer_ == nullptr) {
            close();
            throw_sdl_error("Could not create sprite editor renderer");
        }
        if (parent != nullptr) {
            int x = 0;
            int y = 0;
            static_cast<void>(SDL_GetWindowPosition(parent, &x, &y));
            static_cast<void>(SDL_SetWindowPosition(window_, x + 110, y + 110));
        }
        if (baseline_.empty() || fingerprint_ != emulator.rom_fingerprint()) {
            capture_baseline(emulator);
        }
    }

    void close() noexcept {
        if (renderer_ != nullptr) SDL_DestroyRenderer(renderer_);
        if (window_ != nullptr) SDL_DestroyWindow(window_);
        renderer_ = nullptr;
        window_ = nullptr;
        painting_ = false;
        have_undo_ = false;
        save_patch_requested_ = false;
        load_patch_requested_ = false;
        export_ips_requested_ = false;
    }

    void reset_session() noexcept {
        close();
        baseline_.clear();
        saved_snapshot_.clear();
        fingerprint_ = 0;
    }

    bool handle_event(const SDL_Event& event, gameboy::Emulator* emulator) {
        if (!visible()) return false;
        const auto id = SDL_GetWindowID(window_);
        if (gbb::sdl::event_window_id(event) != id) return false;
        if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
            if (emulator == nullptr || !has_unsaved_changes(*emulator) ||
                confirm_discard_changes(window_,
                                        "Discard unsaved sprite changes?")) {
                close();
            }
            return true;
        }
        if (emulator == nullptr) return true;
        if (!emulator->bus().cgb_mode()) bank_ = 0;
        if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat) {
            if (event.key.key == SDLK_ESCAPE || event.key.key == SDLK_F9) {
                if (!has_unsaved_changes(*emulator) ||
                    confirm_discard_changes(window_,
                                            "Discard unsaved sprite changes?")) {
                    close();
                }
            } else if (event.key.key >= SDLK_1 && event.key.key <= SDLK_4) {
                color_ = static_cast<std::uint8_t>(event.key.key - SDLK_1);
            } else if (event.key.key == SDLK_B && emulator->bus().cgb_mode()) {
                bank_ ^= 1U;
                have_undo_ = false;
            } else if (event.key.key == SDLK_DELETE) {
                snapshot(*emulator);
                clear_tile(*emulator);
            } else if (event.key.key == SDLK_Z &&
                       (event.key.mod & SDL_KMOD_CTRL) != 0) {
                undo(*emulator);
            } else if (event.key.key == SDLK_S &&
                       (event.key.mod & SDL_KMOD_CTRL) != 0) {
                save_patch_requested_ = true;
            } else if (event.key.key == SDLK_O &&
                       (event.key.mod & SDL_KMOD_CTRL) != 0) {
                load_patch_requested_ = true;
            } else if (event.key.key == SDLK_E &&
                       (event.key.mod & SDL_KMOD_CTRL) != 0) {
                export_ips_requested_ = true;
            }
            return true;
        }
        if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
            if (select_tile_at(event.button.x, event.button.y)) {
                have_undo_ = false;
                return true;
            }
            if (select_color_at(event.button.x, event.button.y)) return true;
            if (handle_button(event.button.x, event.button.y, *emulator)) {
                return true;
            }
            if (paint_position(event.button.x, event.button.y)) {
                snapshot(*emulator);
                painting_ = true;
                paint(*emulator, event.button.x, event.button.y,
                      event.button.button == SDL_BUTTON_RIGHT ? 0 : color_);
            }
            return true;
        }
        if (event.type == SDL_EVENT_MOUSE_MOTION && painting_) {
            const auto right = (event.motion.state & SDL_BUTTON_RMASK) != 0;
            paint(*emulator, event.motion.x, event.motion.y,
                  right ? 0 : color_);
            return true;
        }
        if (event.type == SDL_EVENT_MOUSE_BUTTON_UP) {
            painting_ = false;
            return true;
        }
        return true;
    }

    void present(const gameboy::Emulator* emulator) {
        if (!visible() || emulator == nullptr) return;
        if (!emulator->bus().cgb_mode()) bank_ = 0;
        static_cast<void>(SDL_SetRenderDrawColor(renderer_, 8, 12, 20, 255));
        static_cast<void>(SDL_RenderClear(renderer_));
        static_cast<void>(SDL_SetRenderDrawColor(renderer_, 69, 207, 238, 255));
        static_cast<void>(SDL_RenderDebugText(renderer_, 24, 18,
                                              "LIVE VRAM SPRITE / TILE EDITOR"));
        static_cast<void>(SDL_SetRenderDrawColor(renderer_, 177, 192, 208, 255));
        static_cast<void>(SDL_RenderDebugText(
            renderer_, 24, 38,
            "SELECT A TILE, THEN PAINT ITS 2-BIT COLOR INDICES"));
        static_cast<void>(SDL_RenderDebugText(
            renderer_, 24, 54,
            "CHANGES ARE LIVE AND MAY BE OVERWRITTEN WHEN THE GAME RESUMES"));

        constexpr float grid_x = 24.0F;
        constexpr float grid_y = 82.0F;
        constexpr float tile_size = 24.0F;
        constexpr std::size_t columns = 16;
        for (std::size_t tile = 0; tile < 384; ++tile) {
            const auto tile_x = grid_x + (tile % columns) * tile_size;
            const auto tile_y = grid_y + (tile / columns) * tile_size;
            for (std::size_t y = 0; y < 8; ++y) {
                const auto low = emulator->bus().debug_read_vram(
                    bank_, static_cast<std::uint16_t>(tile * 16 + y * 2));
                const auto high = emulator->bus().debug_read_vram(
                    bank_, static_cast<std::uint16_t>(tile * 16 + y * 2 + 1));
                for (std::size_t x = 0; x < 8; ++x) {
                    const auto shift = 7U - x;
                    const auto color = static_cast<std::uint8_t>(
                        ((high >> shift) & 1U) << 1U | ((low >> shift) & 1U));
                    set_color(color);
                    const SDL_FRect pixel{tile_x + x * 3.0F, tile_y + y * 3.0F,
                                          3, 3};
                    static_cast<void>(SDL_RenderFillRect(renderer_, &pixel));
                }
            }
            if (tile == selected_tile_) {
                static_cast<void>(SDL_SetRenderDrawColor(renderer_, 69, 207, 238, 255));
                const SDL_FRect selection{tile_x - 2, tile_y - 2,
                                          tile_size + 4, tile_size + 4};
                static_cast<void>(SDL_RenderRect(renderer_, &selection));
            }
        }

        constexpr float editor_x = 500.0F;
        constexpr float editor_y = 100.0F;
        constexpr float pixel_size = 48.0F;
        for (std::size_t y = 0; y < 8; ++y) {
            const auto low = emulator->bus().debug_read_vram(
                bank_, static_cast<std::uint16_t>(selected_tile_ * 16 + y * 2));
            const auto high = emulator->bus().debug_read_vram(
                bank_, static_cast<std::uint16_t>(selected_tile_ * 16 + y * 2 + 1));
            for (std::size_t x = 0; x < 8; ++x) {
                const auto shift = 7U - x;
                const auto color = static_cast<std::uint8_t>(
                    ((high >> shift) & 1U) << 1U | ((low >> shift) & 1U));
                set_color(color);
                const SDL_FRect pixel{editor_x + x * pixel_size,
                                      editor_y + y * pixel_size,
                                      pixel_size, pixel_size};
                static_cast<void>(SDL_RenderFillRect(renderer_, &pixel));
                static_cast<void>(SDL_SetRenderDrawColor(renderer_, 28, 47, 68, 255));
                static_cast<void>(SDL_RenderRect(renderer_, &pixel));
            }
        }
        static_cast<void>(SDL_SetRenderDrawColor(renderer_, 230, 249, 255, 255));
        const auto tile_label = "TILE " + std::to_string(selected_tile_) +
                                "  VRAM $" + hex_address() +
                                "  BANK " + std::to_string(bank_);
        static_cast<void>(SDL_RenderDebugText(renderer_, editor_x, 82,
                                              tile_label.c_str()));
        for (std::uint8_t color = 0; color < 4; ++color) {
            const SDL_FRect swatch{editor_x + color * 80.0F, 520, 56, 56};
            set_color(color);
            static_cast<void>(SDL_RenderFillRect(renderer_, &swatch));
            static_cast<void>(SDL_SetRenderDrawColor(
                renderer_, color == color_ ? 69 : 52,
                color == color_ ? 207 : 75,
                color == color_ ? 238 : 91, 255));
            static_cast<void>(SDL_RenderRect(renderer_, &swatch));
            const auto label = std::to_string(color + 1);
            static_cast<void>(SDL_RenderDebugText(renderer_, swatch.x + 24,
                                                  swatch.y + 62, label.c_str()));
        }
        draw_button({500, 620, 120, 36}, "CTRL+Z UNDO");
        draw_button({636, 620, 120, 36}, "DELETE CLEAR");
        draw_button({772, 620, 120, 36},
                    emulator->bus().cgb_mode()
                        ? (bank_ == 0 ? "B  BANK 0" : "B  BANK 1")
                        : "DMG BANK 0");
        draw_button({500, 670, 120, 36}, "CTRL+S PATCH");
        draw_button({636, 670, 120, 36}, "CTRL+O IMPORT");
        draw_button({772, 670, 120, 36}, "CTRL+E IPS");
        static_cast<void>(SDL_RenderPresent(renderer_));
    }

    void save_patch(const gameboy::Emulator& emulator,
                    const std::filesystem::path& path) const {
        const auto changed = changes(emulator);
        if (changed.empty()) {
            throw std::runtime_error("No sprite changes are available to save.");
        }
        std::filesystem::create_directories(path.parent_path());
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        if (!output) throw std::runtime_error("Could not create sprite patch.");
        constexpr std::array<char, 8> magic{'G', 'B', 'B', 'T', 'I', 'L', 'E', '1'};
        output.write(magic.data(), static_cast<std::streamsize>(magic.size()));
        write_little(output, fingerprint_, 8);
        write_little(output, changed.size(), 4);
        for (const auto& tile : changed) {
            output.put(static_cast<char>(tile.bank));
            write_little(output, tile.index, 2);
            output.write(reinterpret_cast<const char*>(tile.original.data()), 16);
            output.write(reinterpret_cast<const char*>(tile.replacement.data()), 16);
        }
        if (!output) throw std::runtime_error("Could not finish sprite patch.");
    }

    void load_patch(gameboy::Emulator& emulator,
                    const std::filesystem::path& path) {
        std::ifstream input(path, std::ios::binary);
        if (!input) throw std::runtime_error("No sprite patch is available yet.");
        std::array<char, 8> magic{};
        input.read(magic.data(), static_cast<std::streamsize>(magic.size()));
        constexpr std::array<char, 8> expected{'G', 'B', 'B', 'T', 'I', 'L', 'E', '1'};
        if (magic != expected) throw std::runtime_error("Invalid sprite patch file.");
        const auto fingerprint = read_little(input, 8);
        const auto count = read_little(input, 4);
        if (fingerprint != emulator.rom_fingerprint()) {
            throw std::runtime_error("This sprite patch belongs to a different ROM.");
        }
        if (count == 0 || count > 768) {
            throw std::runtime_error("Sprite patch has an invalid tile count.");
        }
        for (std::uint64_t record = 0; record < count; ++record) {
            const auto bank = static_cast<std::uint8_t>(input.get());
            const auto tile = read_little(input, 2);
            std::array<std::uint8_t, 16> original{};
            std::array<std::uint8_t, 16> replacement{};
            input.read(reinterpret_cast<char*>(original.data()), 16);
            input.read(reinterpret_cast<char*>(replacement.data()), 16);
            if (!input || bank > 1 || tile >= 384 ||
                (bank != 0 && !emulator.bus().cgb_mode())) {
                throw std::runtime_error("Sprite patch contains an invalid tile.");
            }
            for (std::size_t index = 0; index < replacement.size(); ++index) {
                emulator.bus().debug_write_vram(
                    bank, static_cast<std::uint16_t>(tile * 16 + index),
                    replacement[index]);
            }
        }
    }

    [[nodiscard]] IpsExportResult export_ips(
        const gameboy::Emulator& emulator, const std::filesystem::path& rom_path,
        const std::filesystem::path& output_path) const {
        std::ifstream rom_input(rom_path, std::ios::binary);
        if (!rom_input) throw std::runtime_error("Could not read the current ROM.");
        std::vector<std::uint8_t> rom{
            std::istreambuf_iterator<char>(rom_input),
            std::istreambuf_iterator<char>()};
        struct MappedTile {
            std::size_t offset{};
            std::array<std::uint8_t, 16> replacement{};
        };
        std::vector<MappedTile> mapped;
        IpsExportResult result;
        for (const auto& tile : changes(emulator)) {
            std::optional<std::size_t> match;
            for (std::size_t offset = 0; offset + tile.original.size() <= rom.size();
                 ++offset) {
                if (!std::equal(tile.original.begin(), tile.original.end(),
                                rom.begin() + static_cast<std::ptrdiff_t>(offset))) {
                    continue;
                }
                if (match) {
                    match.reset();
                    break;
                }
                match = offset;
            }
            if (!match || *match > 0xFFFFFFU ||
                (*match < 0x150U && *match + tile.original.size() > 0x14DU) ||
                std::any_of(mapped.begin(), mapped.end(),
                            [&](const MappedTile& existing) {
                                return existing.offset == *match;
                            })) {
                ++result.unresolved;
                continue;
            }
            mapped.push_back({*match, tile.replacement});
            ++result.exported;
        }
        if (mapped.empty()) {
            throw std::runtime_error(
                "None of the edited tiles map uniquely to uncompressed ROM data.");
        }
        auto patched_rom = rom;
        for (const auto& tile : mapped) {
            std::copy(tile.replacement.begin(), tile.replacement.end(),
                      patched_rom.begin() +
                          static_cast<std::ptrdiff_t>(tile.offset));
        }
        std::array<std::uint8_t, 3> checksums{};
        if (patched_rom.size() > 0x14FU) {
            std::uint8_t header = 0;
            for (std::size_t offset = 0x134; offset <= 0x14C; ++offset) {
                header = static_cast<std::uint8_t>(
                    header - patched_rom[offset] - 1U);
            }
            patched_rom[0x14D] = header;
            std::uint16_t global = 0;
            for (std::size_t offset = 0; offset < patched_rom.size(); ++offset) {
                if (offset == 0x14E || offset == 0x14F) continue;
                global = static_cast<std::uint16_t>(global + patched_rom[offset]);
            }
            checksums = {header, static_cast<std::uint8_t>(global >> 8U),
                         static_cast<std::uint8_t>(global & 0xFFU)};
        }
        std::filesystem::create_directories(output_path.parent_path());
        std::ofstream output(output_path, std::ios::binary | std::ios::trunc);
        if (!output) throw std::runtime_error("Could not create IPS patch.");
        output.write("PATCH", 5);
        for (const auto& tile : mapped) {
            write_big(output, tile.offset, 3);
            write_big(output, tile.replacement.size(), 2);
            output.write(reinterpret_cast<const char*>(tile.replacement.data()),
                         static_cast<std::streamsize>(tile.replacement.size()));
        }
        if (patched_rom.size() > 0x14FU) {
            write_big(output, 0x14DU, 3);
            write_big(output, checksums.size(), 2);
            output.write(reinterpret_cast<const char*>(checksums.data()),
                         static_cast<std::streamsize>(checksums.size()));
        }
        output.write("EOF", 3);
        if (!output) throw std::runtime_error("Could not finish IPS patch.");
        return result;
    }

  private:
    [[noreturn]] static void throw_sdl_error(const char* action) {
        throw std::runtime_error(std::string(action) + ": " + SDL_GetError());
    }

    struct TileChange {
        std::uint8_t bank{};
        std::uint16_t index{};
        std::array<std::uint8_t, 16> original{};
        std::array<std::uint8_t, 16> replacement{};
    };

    static constexpr float grid_x = 24.0F;
    static constexpr float grid_y = 82.0F;
    static constexpr float tile_size = 24.0F;
    static constexpr float editor_x = 500.0F;
    static constexpr float editor_y = 100.0F;
    static constexpr float pixel_size = 48.0F;

    [[nodiscard]] bool select_tile_at(const float x, const float y) {
        if (x < grid_x || x >= grid_x + 16 * tile_size || y < grid_y ||
            y >= grid_y + 24 * tile_size) {
            return false;
        }
        const auto column = static_cast<std::size_t>((x - grid_x) / tile_size);
        const auto row = static_cast<std::size_t>((y - grid_y) / tile_size);
        selected_tile_ = row * 16 + column;
        return true;
    }

    [[nodiscard]] bool select_color_at(const float x, const float y) {
        if (y < 520 || y >= 576 || x < editor_x || x >= editor_x + 320) {
            return false;
        }
        const auto color = static_cast<std::size_t>((x - editor_x) / 80);
        if (color >= 4) return false;
        color_ = static_cast<std::uint8_t>(color);
        return true;
    }

    [[nodiscard]] static bool paint_position(const float x, const float y) {
        return x >= editor_x && x < editor_x + 8 * pixel_size &&
               y >= editor_y && y < editor_y + 8 * pixel_size;
    }

    void paint(gameboy::Emulator& emulator, const float mouse_x,
               const float mouse_y, const std::uint8_t color) {
        if (!paint_position(mouse_x, mouse_y)) return;
        const auto x = static_cast<unsigned>((mouse_x - editor_x) / pixel_size);
        const auto y = static_cast<unsigned>((mouse_y - editor_y) / pixel_size);
        const auto offset = static_cast<std::uint16_t>(selected_tile_ * 16 + y * 2);
        auto low = emulator.bus().debug_read_vram(bank_, offset);
        auto high = emulator.bus().debug_read_vram(bank_, offset + 1);
        const auto mask = static_cast<std::uint8_t>(1U << (7U - x));
        low = static_cast<std::uint8_t>((low & ~mask) |
                                       ((color & 1U) != 0 ? mask : 0));
        high = static_cast<std::uint8_t>((high & ~mask) |
                                        ((color & 2U) != 0 ? mask : 0));
        emulator.bus().debug_write_vram(bank_, offset, low);
        emulator.bus().debug_write_vram(bank_, offset + 1, high);
    }

    void snapshot(const gameboy::Emulator& emulator) {
        for (std::size_t index = 0; index < undo_tile_.size(); ++index) {
            undo_tile_[index] = emulator.bus().debug_read_vram(
                bank_, static_cast<std::uint16_t>(selected_tile_ * 16 + index));
        }
        undo_index_ = selected_tile_;
        undo_bank_ = bank_;
        have_undo_ = true;
    }

    void undo(gameboy::Emulator& emulator) {
        if (!have_undo_) return;
        for (std::size_t index = 0; index < undo_tile_.size(); ++index) {
            emulator.bus().debug_write_vram(
                undo_bank_, static_cast<std::uint16_t>(undo_index_ * 16 + index),
                undo_tile_[index]);
        }
        selected_tile_ = undo_index_;
        bank_ = undo_bank_;
        have_undo_ = false;
    }

    void clear_tile(gameboy::Emulator& emulator) {
        for (std::size_t index = 0; index < 16; ++index) {
            emulator.bus().debug_write_vram(
                bank_, static_cast<std::uint16_t>(selected_tile_ * 16 + index), 0);
        }
    }

    [[nodiscard]] bool handle_button(const float x, const float y,
                                     gameboy::Emulator& emulator) {
        if (y >= 670 && y <= 706) {
            if (x >= 500 && x <= 620) save_patch_requested_ = true;
            else if (x >= 636 && x <= 756) load_patch_requested_ = true;
            else if (x >= 772 && x <= 892) export_ips_requested_ = true;
            else return false;
            return true;
        }
        if (y < 620 || y > 656) return false;
        if (x >= 500 && x <= 620) {
            undo(emulator);
            return true;
        }
        if (x >= 636 && x <= 756) {
            snapshot(emulator);
            clear_tile(emulator);
            return true;
        }
        if (x >= 772 && x <= 892 && emulator.bus().cgb_mode()) {
            bank_ ^= 1U;
            have_undo_ = false;
            return true;
        }
        return false;
    }

    [[nodiscard]] static std::vector<std::uint8_t> current_vram(
        const gameboy::Emulator& emulator) {
        const auto banks = emulator.bus().cgb_mode() ? 2U : 1U;
        std::vector<std::uint8_t> snapshot(banks * 0x1800U);
        for (std::size_t bank = 0; bank < banks; ++bank) {
            for (std::size_t offset = 0; offset < 0x1800; ++offset) {
                snapshot[bank * 0x1800U + offset] =
                    emulator.bus().debug_read_vram(
                        static_cast<std::uint8_t>(bank),
                        static_cast<std::uint16_t>(offset));
            }
        }
        return snapshot;
    }

    void capture_baseline(const gameboy::Emulator& emulator) {
        const auto banks = emulator.bus().cgb_mode() ? 2U : 1U;
        baseline_.resize(banks * 0x1800U);
        for (std::size_t bank = 0; bank < banks; ++bank) {
            for (std::size_t offset = 0; offset < 0x1800; ++offset) {
                baseline_[bank * 0x1800U + offset] =
                    emulator.bus().debug_read_vram(
                        static_cast<std::uint8_t>(bank),
                        static_cast<std::uint16_t>(offset));
            }
        }
        fingerprint_ = emulator.rom_fingerprint();
        saved_snapshot_ = baseline_;
        have_undo_ = false;
    }

    [[nodiscard]] std::vector<TileChange> changes(
        const gameboy::Emulator& emulator) const {
        std::vector<TileChange> changed;
        const auto banks = emulator.bus().cgb_mode() ? 2U : 1U;
        if (baseline_.size() != banks * 0x1800U ||
            fingerprint_ != emulator.rom_fingerprint()) {
            return changed;
        }
        for (std::size_t bank = 0; bank < banks; ++bank) {
            for (std::size_t tile = 0; tile < 384; ++tile) {
                TileChange change;
                change.bank = static_cast<std::uint8_t>(bank);
                change.index = static_cast<std::uint16_t>(tile);
                for (std::size_t byte = 0; byte < 16; ++byte) {
                    change.original[byte] =
                        baseline_[bank * 0x1800U + tile * 16 + byte];
                    change.replacement[byte] = emulator.bus().debug_read_vram(
                        static_cast<std::uint8_t>(bank),
                        static_cast<std::uint16_t>(tile * 16 + byte));
                }
                if (change.original != change.replacement) {
                    changed.push_back(change);
                }
            }
        }
        return changed;
    }

    static void write_little(std::ostream& output, std::uint64_t value,
                             const unsigned bytes) {
        for (unsigned index = 0; index < bytes; ++index) {
            output.put(static_cast<char>((value >> (index * 8U)) & 0xFFU));
        }
    }

    [[nodiscard]] static std::uint64_t read_little(std::istream& input,
                                                   const unsigned bytes) {
        std::uint64_t value = 0;
        for (unsigned index = 0; index < bytes; ++index) {
            const auto byte = input.get();
            if (byte == std::char_traits<char>::eof()) return 0;
            value |= static_cast<std::uint64_t>(
                         static_cast<std::uint8_t>(byte))
                     << (index * 8U);
        }
        return value;
    }

    static void write_big(std::ostream& output, const std::uint64_t value,
                          const unsigned bytes) {
        for (unsigned index = 0; index < bytes; ++index) {
            const auto shift = (bytes - index - 1U) * 8U;
            output.put(static_cast<char>((value >> shift) & 0xFFU));
        }
    }

    void set_color(const std::uint8_t color) {
        constexpr std::array<std::array<std::uint8_t, 3>, 4> colors{{
            {{238, 249, 255}}, {{156, 192, 207}},
            {{69, 112, 133}}, {{8, 12, 20}},
        }};
        const auto& rgb = colors[color & 3U];
        static_cast<void>(SDL_SetRenderDrawColor(renderer_, rgb[0], rgb[1],
                                                 rgb[2], 255));
    }

    void draw_button(const SDL_FRect& rect, const char* label) {
        draw_tool_button_background(renderer_, window_, rect);
        static_cast<void>(SDL_RenderDebugText(renderer_, rect.x + 9,
                                              rect.y + 14, label));
    }

    [[nodiscard]] std::string hex_address() const {
        std::ostringstream output;
        output << std::uppercase << std::hex << std::setfill('0') << std::setw(4)
               << (0x8000U + selected_tile_ * 16U);
        return output.str();
    }

    SDL_Window* window_{};
    SDL_Renderer* renderer_{};
    std::size_t selected_tile_{};
    std::uint8_t bank_{};
    std::uint8_t color_{};
    std::array<std::uint8_t, 16> undo_tile_{};
    std::size_t undo_index_{};
    std::uint8_t undo_bank_{};
    bool have_undo_{};
    bool painting_{};
    std::uint64_t fingerprint_{};
    std::vector<std::uint8_t> baseline_;
    std::vector<std::uint8_t> saved_snapshot_;
    bool save_patch_requested_{};
    bool load_patch_requested_{};
    bool export_ips_requested_{};
};

} // namespace gbb::sdl

#endif
