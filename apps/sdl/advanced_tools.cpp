#include "advanced_tools.hpp"

#ifndef __ANDROID__

#include "dialogs.hpp"

#include <exception>
#include <string>

namespace gbb::sdl {

void process_advanced_tool_requests(AdvancedToolContext context) {
    auto* const emulator = context.services.debugger();
    auto* const sprite_emulator = context.services.sprite_editor();

    if (emulator != nullptr && context.debugger.take_record_toggle()) {
        try {
            if (context.input_movie.recording()) {
                context.input_movie.stop_and_save(context.movie_path, *emulator);
            } else {
                context.input_movie.stop(emulator);
                context.input_movie.start_recording(*emulator);
                context.rewind_history.clear();
                context.paused = false;
                context.fast_forward = false;
                context.rewind = false;
                context.debugger.run();
            }
        } catch (const std::exception& error) {
            context.input_movie.stop(emulator);
            show_error(context.sdl.window, error.what());
        }
    }
    if (emulator != nullptr && context.debugger.take_replay_request()) {
        try {
            context.input_movie.stop(emulator);
            context.input_movie.start_replay(*emulator, context.movie_path);
            context.rewind_history.clear();
            context.paused = false;
            context.fast_forward = false;
            context.rewind = false;
            context.sdl.audio.clear();
            context.debugger.run();
        } catch (const std::exception& error) {
            context.input_movie.stop(emulator);
            show_error(context.sdl.window, error.what());
        }
    }
    if (emulator != nullptr && context.debugger.take_tas_request()) {
        context.input_movie.stop(emulator);
        context.tas_editor.open(context.sdl.window, *emulator);
        context.rewind_history.clear();
        context.debugger.pause();
    }
    if (sprite_emulator != nullptr &&
        context.debugger.take_sprite_request()) {
        context.input_movie.stop(emulator);
        context.sprite_editor.open(context.sdl.window, *sprite_emulator);
        context.rewind_history.clear();
        context.debugger.pause();
    }
    if (sprite_emulator != nullptr &&
        context.sprite_editor.take_save_patch_request()) {
        try {
            context.sprite_editor.save_patch(*sprite_emulator,
                                             context.sprite_patch_path);
            context.sprite_editor.mark_saved(*sprite_emulator);
            const auto message = "Sprite patch saved to:\n" +
                                 context.sprite_patch_path.string();
            static_cast<void>(SDL_ShowSimpleMessageBox(
                SDL_MESSAGEBOX_INFORMATION, "Sprite patch saved",
                message.c_str(), context.sdl.window));
        } catch (const std::exception& error) {
            show_error(context.sdl.window, error.what());
        }
    }
    if (sprite_emulator != nullptr &&
        context.sprite_editor.take_load_patch_request()) {
        try {
            context.sprite_editor.load_patch(*sprite_emulator,
                                             context.sprite_patch_path);
        } catch (const std::exception& error) {
            show_error(context.sdl.window, error.what());
        }
    }
    if (sprite_emulator != nullptr &&
        context.sprite_editor.take_export_ips_request()) {
        try {
            const auto result = context.sprite_editor.export_ips(
                *sprite_emulator, context.current_rom, context.sprite_ips_path);
            const auto message =
                "IPS patch saved to:\n" + context.sprite_ips_path.string() +
                "\n\nTiles exported: " + std::to_string(result.exported) +
                "\nTiles skipped because their ROM source was missing or "
                "ambiguous: " + std::to_string(result.unresolved);
            static_cast<void>(SDL_ShowSimpleMessageBox(
                SDL_MESSAGEBOX_INFORMATION, "IPS patch exported",
                message.c_str(), context.sdl.window));
        } catch (const std::exception& error) {
            show_error(context.sdl.window, error.what());
        }
    }
    if (context.services.cheats() != nullptr &&
        context.cheat_manager.take_fetch_request()) {
        context.cheat_manager.start_fetch();
    }
    if (context.cheat_manager.poll_fetch()) {
        if (const auto error = context.cheat_manager.take_fetch_error()) {
            show_error(context.sdl.window, *error);
        }
    }
    if (emulator != nullptr && context.tas_editor.take_new_request()) {
        context.input_movie.stop(emulator);
        context.tas_editor.reset_from(*emulator);
        context.rewind_history.clear();
        context.debugger.pause();
    }
    if (emulator != nullptr && context.tas_editor.take_save_request()) {
        try {
            context.input_movie.stop(emulator);
            context.input_movie.save_frame_inputs(
                *emulator, context.movie_path, context.tas_editor.fingerprint(),
                context.tas_editor.start_state(), context.tas_editor.frames());
            static_cast<void>(emulator->take_audio_samples());
            context.sdl.audio.clear();
            context.tas_editor.mark_saved();
        } catch (const std::exception& error) {
            show_error(context.sdl.window, error.what());
        }
    }
    if (emulator != nullptr && context.tas_editor.take_replay_request()) {
        try {
            context.input_movie.stop(emulator);
            context.input_movie.save_frame_inputs(
                *emulator, context.movie_path, context.tas_editor.fingerprint(),
                context.tas_editor.start_state(), context.tas_editor.frames());
            context.input_movie.start_replay(*emulator, context.movie_path);
            static_cast<void>(emulator->take_audio_samples());
            context.rewind_history.clear();
            context.paused = false;
            context.fast_forward = false;
            context.rewind = false;
            context.sdl.audio.clear();
            context.debugger.run();
        } catch (const std::exception& error) {
            context.input_movie.stop(emulator);
            show_error(context.sdl.window, error.what());
        }
    }
}

} // namespace gbb::sdl

#endif // __ANDROID__
