#!/usr/bin/env python3
"""Capture an independent SGB/SGB2 audio reference from a user-supplied libretro core.

This does not feed audio into GBB: the reference core runs its own GB and SNES.
No firmware, game data, or captured audio is checked into the repository.
"""

import argparse
import ctypes as C
from pathlib import Path
import tempfile
import wave


class GameInfo(C.Structure):
    _fields_ = [("path", C.c_char_p), ("data", C.c_void_p),
                ("size", C.c_size_t), ("meta", C.c_char_p)]


class SubsystemInfo(C.Structure):
    _fields_ = [("desc", C.c_char_p), ("ident", C.c_char_p),
                ("roms", C.c_void_p), ("num_roms", C.c_uint), ("id", C.c_uint)]


class Geometry(C.Structure):
    _fields_ = [("base_width", C.c_uint), ("base_height", C.c_uint),
                ("max_width", C.c_uint), ("max_height", C.c_uint),
                ("aspect_ratio", C.c_float)]


class Timing(C.Structure):
    _fields_ = [("fps", C.c_double), ("sample_rate", C.c_double)]


class AVInfo(C.Structure):
    _fields_ = [("geometry", Geometry), ("timing", Timing)]


Environment = C.CFUNCTYPE(C.c_bool, C.c_uint, C.c_void_p)
Video = C.CFUNCTYPE(None, C.c_void_p, C.c_uint, C.c_uint, C.c_size_t)
Audio = C.CFUNCTYPE(None, C.c_int16, C.c_int16)
AudioBatch = C.CFUNCTYPE(C.c_size_t, C.c_void_p, C.c_size_t)
InputPoll = C.CFUNCTYPE(None)
InputState = C.CFUNCTYPE(C.c_int16, C.c_uint, C.c_uint, C.c_uint, C.c_uint)
# A libretro printf callback is variadic. Ignoring any extra arguments is safe
# on the 64-bit calling convention used by this local diagnostic tool.
Log = C.CFUNCTYPE(None, C.c_int, C.c_char_p)


def capture(core_path: Path, game_path: Path, sgb_path: Path,
            system_dir: Path, frames: int, output: Path) -> tuple[int, int]:
    if C.sizeof(C.c_void_p) != 8:
        raise RuntimeError("the diagnostic host currently requires a 64-bit process")
    for path in (core_path, game_path, sgb_path):
        if not path.is_file():
            raise FileNotFoundError(path)
    if not system_dir.is_dir():
        raise NotADirectoryError(system_dir)
    if not 1 <= frames <= 2000:
        raise ValueError("frames must be between 1 and 2000")
    if output.exists():
        raise FileExistsError(output)
    core = C.CDLL(str(core_path.resolve()))
    required = ("retro_api_version", "retro_set_environment", "retro_set_video_refresh",
                "retro_set_audio_sample", "retro_set_audio_sample_batch",
                "retro_set_input_poll", "retro_set_input_state", "retro_init",
                "retro_deinit", "retro_load_game_special", "retro_unload_game",
                "retro_get_system_av_info", "retro_run")
    for symbol in required:
        if not hasattr(core, symbol):
            raise RuntimeError(f"not a usable libretro core: missing {symbol}")
    core.retro_api_version.restype = C.c_uint
    if core.retro_api_version() != 1:
        raise RuntimeError("unsupported libretro API version")
    core.retro_set_environment.argtypes = [Environment]
    core.retro_set_video_refresh.argtypes = [Video]
    core.retro_set_audio_sample.argtypes = [Audio]
    core.retro_set_audio_sample_batch.argtypes = [AudioBatch]
    core.retro_set_input_poll.argtypes = [InputPoll]
    core.retro_set_input_state.argtypes = [InputState]
    core.retro_load_game_special.argtypes = [C.c_uint, C.POINTER(GameInfo), C.c_size_t]
    core.retro_load_game_special.restype = C.c_bool
    core.retro_get_system_av_info.argtypes = [C.POINTER(AVInfo)]

    audio = bytearray()
    video_frames = 0
    subsystems: dict[str, int] = {}
    system_bytes = C.c_char_p(str(system_dir.resolve()).encode())

    with tempfile.TemporaryDirectory(prefix="gbb-sgb-reference-") as save_dir:
        save_bytes = C.c_char_p(save_dir.encode())

        @Environment
        def environment(command: int, data: int) -> bool:
            if command == 9:  # GET_SYSTEM_DIRECTORY
                C.cast(data, C.POINTER(C.c_void_p))[0] = C.cast(system_bytes, C.c_void_p).value
                return True
            if command == 31:  # GET_SAVE_DIRECTORY; never write near the ROMs
                C.cast(data, C.POINTER(C.c_void_p))[0] = C.cast(save_bytes, C.c_void_p).value
                return True
            if command == 10:  # SET_PIXEL_FORMAT; bsnes requests XRGB8888
                return C.cast(data, C.POINTER(C.c_int))[0] == 1
            if command == 27:  # GET_LOG_INTERFACE
                C.cast(data, C.POINTER(C.c_void_p))[0] = C.cast(logger, C.c_void_p).value
                return True
            if command == 34:  # SET_SUBSYSTEM_INFO
                entries = C.cast(data, C.POINTER(SubsystemInfo))
                for index in range(32):
                    entry = entries[index]
                    if not entry.ident:
                        break
                    subsystems[entry.ident.decode()] = entry.id
                return True
            if command == 17:  # GET_VARIABLE_UPDATE
                C.cast(data, C.POINTER(C.c_bool))[0] = False
                return True
            return False

        @Log
        def logger(_level: int, _format: bytes) -> None:
            pass

        @Video
        def video(data: int, width: int, height: int, _pitch: int) -> None:
            nonlocal video_frames
            if data and width and height:
                video_frames += 1

        @Audio
        def audio_sample(left: int, right: int) -> None:
            audio.extend(int(left).to_bytes(2, "little", signed=True))
            audio.extend(int(right).to_bytes(2, "little", signed=True))

        @AudioBatch
        def audio_batch(samples: int, count: int) -> int:
            audio.extend(C.string_at(samples, count * 4))
            return count

        @InputPoll
        def input_poll() -> None:
            pass

        @InputState
        def input_state(_port: int, _device: int, _index: int, _button: int) -> int:
            return 0

        # Keep all callbacks and directory byte strings alive while the core runs.
        core.retro_set_environment(environment)
        core.retro_set_video_refresh(video)
        core.retro_set_audio_sample(audio_sample)
        core.retro_set_audio_sample_batch(audio_batch)
        core.retro_set_input_poll(input_poll)
        core.retro_set_input_state(input_state)
        core.retro_init()
        loaded = False
        try:
            if "sgb" not in subsystems:
                raise RuntimeError("core did not advertise an SGB subsystem")
            game_bytes = str(game_path.resolve()).encode()
            sgb_bytes = str(sgb_path.resolve()).encode()
            games = (GameInfo * 2)(GameInfo(game_bytes, None, 0, None),
                                   GameInfo(sgb_bytes, None, 0, None))
            loaded = bool(core.retro_load_game_special(subsystems["sgb"], games, 2))
            if not loaded:
                raise RuntimeError("core could not load the SGB subsystem and ROMs")
            av = AVInfo()
            core.retro_get_system_av_info(C.byref(av))
            sample_rate = round(av.timing.sample_rate)
            if sample_rate <= 0 or abs(av.timing.sample_rate - sample_rate) > 0.01:
                raise RuntimeError("core returned an invalid audio rate")
            for _ in range(frames):
                core.retro_run()
            if not video_frames:
                raise RuntimeError("core produced no video frames; content may not be running")
            if not audio:
                raise RuntimeError("core produced no audio callbacks")
            output.parent.mkdir(parents=True, exist_ok=True)
            with output.open("xb") as output_file:
                with wave.open(output_file, "wb") as wav:
                    wav.setnchannels(2)
                    wav.setsampwidth(2)
                    wav.setframerate(sample_rate)
                    wav.writeframes(audio)
            return sample_rate, len(audio) // 4
        finally:
            if loaded:
                core.retro_unload_game()
            core.retro_deinit()


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--core", type=Path, required=True,
                        help="path to a user-installed SGB-capable libretro core")
    parser.add_argument("--game", type=Path, required=True)
    parser.add_argument("--sgb-rom", type=Path, required=True,
                        help="path to your own SGB/SGB2 SNES-side program ROM")
    parser.add_argument("--system-dir", type=Path, required=True,
                        help="directory containing your own GB-side boot ROM, if needed")
    parser.add_argument("--frames", type=int, default=400)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    try:
        rate, count = capture(args.core, args.game, args.sgb_rom,
                              args.system_dir, args.frames, args.output)
    except (OSError, RuntimeError, ValueError) as error:
        parser.error(str(error))
    print(f"Captured {count} stereo frames at {rate} Hz to {args.output}")


if __name__ == "__main__":
    main()
