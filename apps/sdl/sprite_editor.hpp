#pragma once

#ifndef __ANDROID__

#include <SDL3/SDL.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>

namespace gameboy {
class Emulator;
}

namespace gbb::sdl {

class SpriteEditorImplementation;

class SpriteEditor final {
public:
    struct IpsExportResult {
        std::size_t exported{};
        std::size_t unresolved{};
    };

    SpriteEditor();
    ~SpriteEditor();
    SpriteEditor(const SpriteEditor&) = delete;
    SpriteEditor& operator=(const SpriteEditor&) = delete;

    [[nodiscard]] bool visible() const noexcept;
    [[nodiscard]] bool take_save_patch_request() noexcept;
    [[nodiscard]] bool take_load_patch_request() noexcept;
    [[nodiscard]] bool take_export_ips_request() noexcept;
    [[nodiscard]] bool has_unsaved_changes(const gameboy::Emulator& emulator) const;
    void mark_saved(const gameboy::Emulator& emulator);
    void open(SDL_Window* parent, const gameboy::Emulator& emulator);
    void close() noexcept;
    void reset_session() noexcept;
    bool handle_event(const SDL_Event& event, gameboy::Emulator* emulator);
    void present(const gameboy::Emulator* emulator);
    void save_patch(const gameboy::Emulator& emulator,
                    const std::filesystem::path& path) const;
    void load_patch(gameboy::Emulator& emulator,
                    const std::filesystem::path& path);
    [[nodiscard]] IpsExportResult export_ips(
        const gameboy::Emulator& emulator,
        const std::filesystem::path& rom_path,
        const std::filesystem::path& output_path) const;

private:
    std::unique_ptr<SpriteEditorImplementation> implementation_;
};

} // namespace gbb::sdl

#endif
