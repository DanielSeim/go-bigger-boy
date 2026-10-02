#!/usr/bin/env python3
"""Capture an independent SGB/SGB2 audio reference from a user-supplied libretro core.

This does not feed audio into GBB: the reference core runs its own GB and SNES.
No firmware, game data, or captured audio is checked into the repository.
"""

import argparse
import ctypes as C
import hashlib
import json
from pathlib import Path
import sys
import tempfile
import wave


BUTTON_IDS = {"right": 7, "left": 6, "up": 4, "down": 5,
              "a": 8, "b": 0, "select": 2, "start": 3}
GB_FRAME_RATE = 4194304 / 70224
# GET_VARIABLE returns borrowed C strings. Keep option buffers alive after
# the Python callback returns, until the core has consumed them.
ENTROPY_VALUES = {"None": b"None", "Low": b"Low", "High": b"High"}


def native_button_mask(libretro_mask: int) -> int:
    """Convert libretro joypad IDs to the GBB/SGB eight-bit held state."""
    return sum(((libretro_mask >> BUTTON_IDS[name]) & 1) << bit
               for bit, name in enumerate(("right", "left", "up", "down",
                                           "a", "b", "select", "start")))


def configure_frame_input(core, events: list[tuple[int, int]], enabled: bool) -> None:
    """Reset optional probe state even when reusing a core in legacy mode."""
    names = ("gbb_reference_sgb_frame_input_version", "gbb_reference_sgb_frame_input_enable",
             "gbb_reference_sgb_frame_input_add", "gbb_reference_sgb_frame_input_applied")
    if enabled and (any(not hasattr(core, name) for name in names) or
                    core.gbb_reference_sgb_frame_input_version() != 1):
        raise RuntimeError("reference lacks the version-1 native GB input probe")
    if not hasattr(core, names[1]):
        return
    setter = core.gbb_reference_sgb_frame_input_enable
    setter.argtypes, setter.restype = [C.c_uint], C.c_uint
    if setter(int(enabled)) != 1:
        raise RuntimeError("reference rejected native GB input configuration")
    if enabled:
        add = core.gbb_reference_sgb_frame_input_add
        add.argtypes, add.restype = [C.c_uint64, C.c_uint], C.c_uint
        core.gbb_reference_sgb_frame_input_applied.restype = C.c_uint
        for frame, mask in events:
            if add(frame, native_button_mask(mask)) != 1:
                raise RuntimeError("reference rejected native GB input event")


class RetroVariable(C.Structure):
    _fields_ = [("key", C.c_char_p), ("value", C.c_char_p)]


def diagnostic_option(key: bytes, snes_only: bool, reference_entropy: str | None = None) -> bytes | None:
    # A batched DSP can expose state several phases after a DAC sample. Use
    # single-clock scheduling for the instrumented reference, explicitly.
    if snes_only and key == b"bsnes_entropy" and reference_entropy is not None:
        return ENTROPY_VALUES[reference_entropy]
    return b"OFF" if snes_only and key == b"bsnes_dsp_fast" else None


def decode_video_row(row: bytes, pixel_format: int) -> bytes:
    """Convert one native-endian libretro row to packed RGB888."""
    bytes_per_pixel = 4 if pixel_format == 1 else 2
    if pixel_format not in (0, 1, 2) or len(row) % bytes_per_pixel:
        raise ValueError("invalid libretro video row or pixel format")
    rgb = bytearray()
    for offset in range(0, len(row), bytes_per_pixel):
        pixel = int.from_bytes(row[offset:offset + bytes_per_pixel], sys.byteorder)
        if pixel_format == 1:  # XRGB8888
            rgb.extend(((pixel >> 16) & 255, (pixel >> 8) & 255, pixel & 255))
        elif pixel_format == 2:  # RGB565
            rgb.extend(((((pixel >> 11) & 31) * 255 + 15) // 31,
                        (((pixel >> 5) & 63) * 255 + 31) // 63,
                        ((pixel & 31) * 255 + 15) // 31))
        else:  # 0RGB1555 is the libretro default when no format is requested.
            rgb.extend(((((pixel >> 10) & 31) * 255 + 15) // 31,
                        (((pixel >> 5) & 31) * 255 + 15) // 31,
                        ((pixel & 31) * 255 + 15) // 31))
    return bytes(rgb)


def load_input_script(path: Path) -> list[tuple[int, int]]:
    """Read the repository's complete-held-state GBB SGB input v1 format."""
    lines = path.read_text(encoding="utf-8").splitlines()
    if not lines or lines[0] != "GBB SGB input v1":
        raise ValueError("expected GBB SGB input v1 header")
    events: list[tuple[int, int]] = []
    for number, line in enumerate(lines[1:], 2):
        content = line.split("#", 1)[0].strip()
        if not content:
            continue
        fields = content.split()
        if len(fields) != 2 or not fields[0].isascii() or not fields[0].isdecimal():
            raise ValueError(f"input script line {number}: expected frame and buttons")
        frame = int(fields[0])
        if frame > 100000 or (events and frame <= events[-1][0]):
            raise ValueError(f"input script line {number}: frames must increase and fit limit")
        names = [] if fields[1] == "none" else fields[1].split("+")
        if any(name not in BUTTON_IDS for name in names) or len(set(names)) != len(names):
            raise ValueError(f"input script line {number}: invalid button list")
        mask = sum(1 << BUTTON_IDS[name] for name in names)
        events.append((frame, mask))
        if len(events) > 1024:
            raise ValueError("input script exceeds 1024 events")
    return events


def schedule_input(events: list[tuple[int, int]], reference_fps: float,
                   offset: int,
                   offset_changes: list[tuple[int, int]] | None = None
                   ) -> list[tuple[int, int]]:
    """Map GB frame boundaries to zero-based libretro run-call indices."""
    if not 30 <= reference_fps <= 120:
        raise ValueError("reference core returned an implausible video rate")
    changes = offset_changes or []
    if any(frame <= 0 or (index and frame <= changes[index - 1][0])
           for index, (frame, _) in enumerate(changes)):
        raise ValueError("offset changes must use increasing positive GB frames")
    scheduled = []
    next_change = 0
    for frame, mask in events:
        while next_change < len(changes) and frame >= changes[next_change][0]:
            offset = changes[next_change][1]
            next_change += 1
        scheduled.append((round(frame * reference_fps / GB_FRAME_RATE) + offset,
                          mask))
    if any(frame < 0 or (index and frame <= scheduled[index - 1][0])
           for index, (frame, _) in enumerate(scheduled)):
        raise ValueError("mapped input frames must increase and be nonnegative")
    return scheduled


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
            system_dir: Path, frames: int, output: Path,
            input_script: Path | None = None,
            input_offset: int = 0,
            input_offset_changes: list[tuple[int, int]] | None = None,
            snapshot_frame: int | None = None,
            snapshot_output: Path | None = None,
            snapshot_series: tuple[int, int, Path] | None = None,
            timeline_output: Path | None = None,
            require_snes_only_probe: bool = False,
            native_dsp_output: Path | None = None,
            apu_ram_output: Path | None = None,
            apu_bus_output: Path | None = None,
            native_cycle_checkpoints: bool = False,
            timer_poll_trace: bool = False,
            reference_entropy: str | None = None,
            history_window_half: tuple[int, int] | None = None,
            boot_timeline_output: Path | None = None,
            native_gb_input: bool = False
            ) -> tuple[int, int]:
    if C.sizeof(C.c_void_p) != 8:
        raise RuntimeError("the diagnostic host currently requires a 64-bit process")
    for path in (core_path, game_path, sgb_path):
        if not path.is_file():
            raise FileNotFoundError(path)
    if not system_dir.is_dir():
        raise NotADirectoryError(system_dir)
    if not 1 <= frames <= 10000:
        raise ValueError("frames must be between 1 and 10000")
    if output.exists():
        raise FileExistsError(output)
    if native_gb_input and (not require_snes_only_probe or boot_timeline_output is None or
                            input_script is None or input_offset != 0 or input_offset_changes):
        raise ValueError("native GB input requires a script and SNES-only boot probe, without frame offsets")
    if timeline_output is not None and timeline_output.exists():
        raise FileExistsError(timeline_output)
    if native_dsp_output is not None and native_dsp_output.exists():
        raise FileExistsError(native_dsp_output)
    if apu_ram_output is not None and apu_ram_output.exists():
        raise FileExistsError(apu_ram_output)
    if apu_bus_output is not None and apu_bus_output.exists():
        raise FileExistsError(apu_bus_output)
    if boot_timeline_output is not None and boot_timeline_output.exists():
        raise FileExistsError(boot_timeline_output)
    if boot_timeline_output is not None and (not require_snes_only_probe or apu_bus_output is None):
        raise ValueError("boot timeline requires the SNES-only probe and APU bus output")
    if apu_bus_output is not None and not require_snes_only_probe:
        raise ValueError("APU bus capture requires the SNES-only probe")
    if native_cycle_checkpoints and not require_snes_only_probe:
        raise ValueError("native-cycle checkpoints require the SNES-only probe")
    if timer_poll_trace and (not require_snes_only_probe or apu_bus_output is None):
        raise ValueError("timer polling requires the SNES-only probe and APU bus output")
    if reference_entropy is not None and (not require_snes_only_probe or reference_entropy not in ("None", "Low", "High")):
        raise ValueError("reference entropy requires the SNES-only probe and None, Low or High")
    if history_window_half is not None and (not timer_poll_trace or len(history_window_half) != 2 or
            any(type(v) is not int or not 0 <= v < 2**64 for v in history_window_half) or
            not 0 < history_window_half[1] - history_window_half[0] <= 200000):
        raise ValueError("history window requires timer polling and 1..200000 valid half clocks")
    if native_dsp_output is not None and not require_snes_only_probe:
        raise ValueError("native DSP capture requires the SNES-only probe")
    if require_snes_only_probe and timeline_output is None:
        raise ValueError("the SNES-only probe requires a timeline output")
    if apu_ram_output is not None and not require_snes_only_probe:
        raise ValueError("APU RAM output requires the SNES-only probe")
    if (snapshot_frame is None) != (snapshot_output is None):
        raise ValueError("snapshot frame and output must be supplied together")
    if snapshot_frame is not None and not 1 <= snapshot_frame <= frames:
        raise ValueError("snapshot frame must be within the capture")
    if snapshot_output is not None and snapshot_output.exists():
        raise FileExistsError(snapshot_output)
    targets = [path.resolve() for path in
               (output, snapshot_output, timeline_output, native_dsp_output,
                apu_ram_output, apu_bus_output, boot_timeline_output)
               if path is not None]
    if len(targets) != len(set(targets)):
        raise ValueError("WAV, native DSP, snapshot, and timeline paths must differ")
    if snapshot_series is not None:
        first, last, directory = snapshot_series
        if not 1 <= first <= last <= frames or last - first > 200:
            raise ValueError("snapshot series must fit capture and span at most 201 frames")
        if directory.exists() and any(directory.iterdir()):
            raise FileExistsError("snapshot series directory must be empty")
    events = load_input_script(input_script) if input_script is not None else []
    core = C.CDLL(str(core_path.resolve()))
    phase_writes_from_reset = False
    if require_snes_only_probe:
        probe_symbols = ("gbb_reference_sgb_probe_version",
                         "gbb_reference_sgb_sound_count",
                         "gbb_reference_sgb_sound_packet",
                         "gbb_reference_sgb_sound_position",
                         "gbb_reference_sgb_dsp_sample_count",
                         "gbb_reference_sgb_dsp_copy",
                         "gbb_reference_sgb_ram_copy",
                         "gbb_reference_sgb_event_count",
                         "gbb_reference_sgb_event_copy")
        if any(not hasattr(core, symbol) for symbol in probe_symbols):
            raise RuntimeError("core is not the instrumented SNES-only reference build")
        core.gbb_reference_sgb_probe_version.restype = C.c_uint
        if core.gbb_reference_sgb_probe_version() != 7:
            raise RuntimeError("unsupported SNES-only reference probe version")
        if native_cycle_checkpoints and not hasattr(core, "gbb_reference_sgb_native_checkpoints"):
            raise RuntimeError("core lacks the native-cycle checkpoint probe")
        if hasattr(core, "gbb_reference_sgb_native_checkpoints"):
            core.gbb_reference_sgb_native_checkpoints.argtypes = [C.c_uint]
            core.gbb_reference_sgb_native_checkpoints.restype = C.c_uint
            if core.gbb_reference_sgb_native_checkpoints(int(native_cycle_checkpoints)) != 1:
                raise RuntimeError("native-cycle checkpoint probe rejected the request")
        core.gbb_reference_sgb_sound_count.restype = C.c_uint
        core.gbb_reference_sgb_sound_packet.argtypes = [C.c_uint,
                                                         C.POINTER(C.c_uint8)]
        core.gbb_reference_sgb_sound_packet.restype = C.c_uint
        core.gbb_reference_sgb_sound_position.argtypes = [
            C.c_uint, C.POINTER(C.c_uint), C.POINTER(C.c_uint)]
        core.gbb_reference_sgb_sound_position.restype = C.c_uint
        core.gbb_reference_sgb_dsp_sample_count.restype = C.c_uint
        core.gbb_reference_sgb_dsp_copy.argtypes = [
            C.c_uint, C.c_uint, C.POINTER(C.c_int16)]
        core.gbb_reference_sgb_dsp_copy.restype = C.c_uint
        core.gbb_reference_sgb_ram_copy.argtypes = [C.POINTER(C.c_uint8), C.c_uint]
        core.gbb_reference_sgb_ram_copy.restype = C.c_uint
        core.gbb_reference_sgb_event_count.restype = C.c_uint
        core.gbb_reference_sgb_event_copy.argtypes = [C.c_uint,
                                                        C.POINTER(C.c_uint)]
        core.gbb_reference_sgb_event_copy.restype = C.c_uint
        if apu_bus_output is not None:
            symbols = ("gbb_reference_sgb_apu_bus_version", "gbb_reference_sgb_apu_bus_count",
                       "gbb_reference_sgb_apu_bus_copy", "gbb_reference_sgb_apu_bus_frequencies")
            if any(not hasattr(core, name) for name in symbols) or \
                    core.gbb_reference_sgb_apu_bus_version() not in (1, 2, 3):
                raise RuntimeError("core lacks a supported APU bus probe")
            if timer_poll_trace and (core.gbb_reference_sgb_apu_bus_version() not in (2, 3) or
                                    not hasattr(core, "gbb_reference_sgb_timer_trace")):
                raise RuntimeError("core lacks the version-2 timer polling probe")
            phase_writes_from_reset = core.gbb_reference_sgb_apu_bus_version() == 3
            core.gbb_reference_sgb_apu_bus_count.restype = C.c_uint
            core.gbb_reference_sgb_apu_bus_copy.argtypes = [C.c_uint, C.POINTER(C.c_uint64)]
            core.gbb_reference_sgb_apu_bus_copy.restype = C.c_uint
            core.gbb_reference_sgb_apu_bus_frequencies.argtypes = [C.POINTER(C.c_uint64)]
            core.gbb_reference_sgb_apu_bus_frequencies.restype = C.c_uint
        if hasattr(core, "gbb_reference_sgb_timer_trace"):
            core.gbb_reference_sgb_timer_trace.argtypes = [C.c_uint]
            core.gbb_reference_sgb_timer_trace.restype = C.c_uint
            if core.gbb_reference_sgb_timer_trace(int(timer_poll_trace)) != 1:
                raise RuntimeError("timer polling probe rejected the request")
        if history_window_half is not None and not hasattr(core, "gbb_reference_sgb_history_window"):
            raise RuntimeError("reference lacks the bounded history window probe")
        if hasattr(core, "gbb_reference_sgb_history_window"):
            core.gbb_reference_sgb_history_window.argtypes = [C.c_uint64, C.c_uint64]
            core.gbb_reference_sgb_history_window.restype = C.c_uint
            if core.gbb_reference_sgb_history_window(*(history_window_half or (0, 0))) != 1:
                raise RuntimeError("reference rejected the history window")
        boot_symbols = ("gbb_reference_sgb_boot_version", "gbb_reference_sgb_boot_enable",
                        "gbb_reference_sgb_boot_count", "gbb_reference_sgb_boot_copy", "gbb_reference_sgb_boot_input")
        if boot_timeline_output is not None and (any(not hasattr(core, name) for name in boot_symbols) or
                                                core.gbb_reference_sgb_boot_version() not in (1, 2)):
            raise RuntimeError("reference lacks a supported boot timeline probe (version 1 or 2)")
        if hasattr(core, "gbb_reference_sgb_boot_enable"):
            core.gbb_reference_sgb_boot_enable.argtypes = [C.c_uint]
            core.gbb_reference_sgb_boot_enable.restype = C.c_uint
            if core.gbb_reference_sgb_boot_enable(int(boot_timeline_output is not None)) != 1:
                raise RuntimeError("reference rejected the boot timeline")
        if boot_timeline_output is not None:
            core.gbb_reference_sgb_boot_input.argtypes = [C.c_uint]
            core.gbb_reference_sgb_boot_input.restype = C.c_uint
            core.gbb_reference_sgb_boot_count.restype = C.c_uint
            core.gbb_reference_sgb_boot_copy.argtypes = [C.c_uint, C.POINTER(C.c_uint64)]
            core.gbb_reference_sgb_boot_copy.restype = C.c_uint
    configure_frame_input(core, events, native_gb_input)
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
    held_buttons = 0
    input_queries = 0
    pressed_queries = 0
    snapshot: tuple[int, int, bytes] | None = None
    snapshots: dict[int, tuple[int, int, bytes]] = {}
    frame_callbacks: list[dict[str, int]] = []
    run_samples: list[list[int]] = []
    snapshot_hashes: dict[str, str] = {}
    sound_events: list[dict[str, object]] = []
    current_run = -1
    video_pixel_format = 0  # RETRO_PIXEL_FORMAT_0RGB1555 is the API default.
    subsystems: dict[str, int] = {}
    system_bytes = C.c_char_p(str(system_dir.resolve()).encode())
    configured_options: dict[str, str] = {}

    with tempfile.TemporaryDirectory(prefix="gbb-sgb-reference-") as save_dir:
        save_bytes = C.c_char_p(save_dir.encode())

        @Environment
        def environment(command: int, data: int) -> bool:
            nonlocal video_pixel_format
            if command == 9:  # GET_SYSTEM_DIRECTORY
                C.cast(data, C.POINTER(C.c_void_p))[0] = C.cast(system_bytes, C.c_void_p).value
                return True
            if command == 31:  # GET_SAVE_DIRECTORY; never write near the ROMs
                C.cast(data, C.POINTER(C.c_void_p))[0] = C.cast(save_bytes, C.c_void_p).value
                return True
            if command == 10:  # SET_PIXEL_FORMAT
                requested = C.cast(data, C.POINTER(C.c_int))[0]
                if requested not in (0, 1, 2):
                    return False
                video_pixel_format = requested
                return True
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
            if command == 15:  # GET_VARIABLE
                variable = C.cast(data, C.POINTER(RetroVariable)).contents
                value = diagnostic_option(variable.key, require_snes_only_probe, reference_entropy)
                if value is None:
                    return False
                variable.value = value
                configured_options[variable.key.decode()] = value.decode()
                return True
            return False

        @Log
        def logger(_level: int, _format: bytes) -> None:
            pass

        @Video
        def video(data: int, width: int, height: int, pitch: int) -> None:
            nonlocal video_frames, snapshot
            if data and width and height:
                video_frames += 1
                if timeline_output is not None:
                    frame_callbacks.append({"video_frame": video_frames,
                                            "run_index": current_run,
                                            "sample_at_callback": len(audio) // 4})
                in_series = (snapshot_series is not None and
                             snapshot_series[0] <= video_frames <= snapshot_series[1])
                if video_frames == snapshot_frame or in_series:
                    pixels = bytearray()
                    bytes_per_pixel = 4 if video_pixel_format == 1 else 2
                    for row in range(height):
                        row_bytes = C.string_at(data + row * pitch,
                                                width * bytes_per_pixel)
                        pixels.extend(decode_video_row(row_bytes,
                                                       video_pixel_format))
                    captured = (width, height, bytes(pixels))
                    if video_frames == snapshot_frame:
                        snapshot = captured
                    if in_series:
                        snapshots[video_frames] = captured

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
        def input_state(port: int, device: int, index: int, button: int) -> int:
            nonlocal input_queries, pressed_queries
            if port != 0 or device != 1 or index != 0:
                return 0
            input_queries += 1
            if button == 256:  # RETRO_DEVICE_ID_JOYPAD_MASK
                pressed_queries += held_buttons != 0
                return held_buttons
            pressed = int(0 <= button < 16 and (held_buttons & (1 << button)) != 0)
            pressed_queries += pressed
            return pressed

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
            scheduled = [] if native_gb_input else schedule_input(events, av.timing.fps, input_offset,
                                                                  input_offset_changes)
            next_event = 0
            next_sound_event = 0
            next_trace_event = 0
            sound_trace = []
            for frame in range(frames):
                current_run = frame
                if next_event < len(scheduled) and frame == scheduled[next_event][0]:
                    held_buttons = scheduled[next_event][1]
                    if boot_timeline_output is not None and core.gbb_reference_sgb_boot_input(native_button_mask(held_buttons)) != 1:
                        raise RuntimeError("reference rejected input timeline marker")
                    next_event += 1
                sample_start = len(audio) // 4
                core.retro_run()
                if require_snes_only_probe:
                    if configured_options.get("bsnes_dsp_fast") != "OFF":
                        raise RuntimeError("reference did not request single-clock DSP configuration")
                    if reference_entropy is not None and configured_options.get("bsnes_entropy") != reference_entropy:
                        raise RuntimeError("reference did not request the selected power-on entropy")
                    sound_count = core.gbb_reference_sgb_sound_count()
                    if sound_count < next_sound_event or sound_count > 1024:
                        raise RuntimeError("reference SOUND probe count reset or overflowed")
                    for event_index in range(next_sound_event, sound_count):
                        packet = (C.c_uint8 * 16)()
                        if core.gbb_reference_sgb_sound_packet(event_index, packet) != 1:
                            raise RuntimeError("reference SOUND probe lost a packet")
                        vertical = C.c_uint()
                        horizontal = C.c_uint()
                        if core.gbb_reference_sgb_sound_position(
                                event_index, C.byref(vertical),
                                C.byref(horizontal)) != 1 or \
                                vertical.value >= 262 or horizontal.value >= 1364:
                            raise RuntimeError("reference SOUND probe has invalid CPU position")
                        sound_events.append({"run_index": frame,
                                             "video_frame_after_run": video_frames,
                                             "sample_start": sample_start,
                                             "sample_end": len(audio) // 4,
                                             "cpu_vcounter": vertical.value,
                                             "cpu_hcounter": horizontal.value,
                                             "packet": bytes(packet).hex()})
                    next_sound_event = sound_count
                    trace_count = core.gbb_reference_sgb_event_count()
                    if trace_count < next_trace_event or trace_count > 32768:
                        raise RuntimeError("reference sound write trace count is invalid")
                    for event_index in range(next_trace_event, trace_count):
                        entry = (C.c_uint * 6)()
                        if core.gbb_reference_sgb_event_copy(event_index, entry) != 1 or \
                                entry[0] not in (1, 2, 3, 4) or \
                                entry[1] > (3 if entry[0] == 1 else
                                            127 if entry[0] == 2 else
                                            24 if entry[0] == 4 else 65535) or \
                                entry[2] > (65535 if entry[0] == 4 else 255) or \
                                (entry[0] == 1 and (entry[4] >= 262 or entry[5] >= 1364)) or \
                                (entry[0] == 2 and entry[5] >= 64) or \
                                (entry[0] == 4 and entry[5] != 28):
                            raise RuntimeError("reference sound write trace is invalid")
                        sound_trace.append({"kind": {1: "host", 2: "dsp", 3: "ram",
                                                      4: "state"}[entry[0]],
                                            "address": int(entry[1]),
                                            "value": int(entry[2]),
                                            "dsp_sample": int(entry[3]) if entry[0] in (2, 3, 4)
                                            else None,
                                            "cpu_vcounter": int(entry[4]),
                                            "cpu_hcounter": int(entry[5]),
                                            "run_index": frame})
                        if entry[0] == 4:
                            sound_trace[-1]["dsp_phase"] = int(entry[5])
                        if entry[0] == 2:
                            sound_trace[-1]["dsp_clock64"] = int(entry[5])
                    next_trace_event = trace_count
                if timeline_output is not None:
                    run_samples.append([sample_start, len(audio) // 4])
            if native_gb_input and core.gbb_reference_sgb_frame_input_applied() != len(events):
                raise RuntimeError("native GB input replay ended before all scripted events were applied")
            if events and not native_gb_input and next_event == 0:
                raise RuntimeError("capture ended before the first scripted input event")
            if events and not native_gb_input and (input_queries == 0 or pressed_queries == 0):
                raise RuntimeError("reference core did not poll scripted controller presses")
            if not video_frames:
                raise RuntimeError("core produced no video frames; content may not be running")
            if not audio:
                raise RuntimeError("core produced no audio callbacks")
            native_dsp = None
            native_count = None
            if native_dsp_output is not None:
                native_count = core.gbb_reference_sgb_dsp_sample_count()
                if not 0 < native_count <= 4000000:
                    raise RuntimeError("reference native DSP probe overflowed")
                if abs(native_count / 32040 - (len(audio) // 4) / sample_rate) > 0.1:
                    raise RuntimeError("native DSP and libretro output durations disagree")
                samples = (C.c_int16 * (native_count * 2))()
                if core.gbb_reference_sgb_dsp_copy(0, native_count, samples) != 1:
                    raise RuntimeError("reference native DSP probe lost samples")
                native_dsp = C.string_at(samples, native_count * 4)
            output.parent.mkdir(parents=True, exist_ok=True)
            with output.open("xb") as output_file:
                with wave.open(output_file, "wb") as wav:
                    wav.setnchannels(2)
                    wav.setsampwidth(2)
                    wav.setframerate(sample_rate)
                    wav.writeframes(audio)
            if native_dsp_output is not None:
                native_dsp_output.parent.mkdir(parents=True, exist_ok=True)
                with native_dsp_output.open("xb") as native_file:
                    with wave.open(native_file, "wb") as wav:
                        wav.setnchannels(2)
                        wav.setsampwidth(2)
                        wav.setframerate(32040)
                        wav.writeframes(native_dsp)
            ram_bytes = None
            if apu_ram_output is not None:
                ram = (C.c_uint8 * 65536)()
                if core.gbb_reference_sgb_ram_copy(ram, 65536) != 1:
                    raise RuntimeError("reference APU RAM probe failed")
                ram_bytes = bytes(ram)
                apu_ram_output.parent.mkdir(parents=True, exist_ok=True)
                with apu_ram_output.open("xb") as ram_file:
                    ram_file.write(ram_bytes)
            if snapshot_output is not None:
                if snapshot is None:
                    raise RuntimeError("core did not produce the requested video frame")
                snapshot_output.parent.mkdir(parents=True, exist_ok=True)
                width, height, pixels = snapshot
                image = f"P6\n{width} {height}\n255\n".encode() + pixels
                with snapshot_output.open("xb") as image_file:
                    image_file.write(image)
                snapshot_hashes[str(snapshot_frame)] = hashlib.sha256(image).hexdigest()
            if snapshot_series is not None:
                first, last, directory = snapshot_series
                if len(snapshots) != last - first + 1:
                    raise RuntimeError("core did not produce every requested series frame")
                directory.mkdir(parents=True, exist_ok=True)
                for frame, (width, height, pixels) in snapshots.items():
                    image = f"P6\n{width} {height}\n255\n".encode() + pixels
                    with (directory / f"reference-frame-{frame}.ppm").open("xb") as image_file:
                        image_file.write(image)
                    snapshot_hashes[str(frame)] = hashlib.sha256(image).hexdigest()
            if timeline_output is not None:
                timeline = {"format": "gbb-libretro-audio-timeline-v1",
                            "core_sha256": hashlib.sha256(core_path.read_bytes()).hexdigest(),
                            "game_sha256": hashlib.sha256(game_path.read_bytes()).hexdigest(),
                            "sgb_rom_sha256": hashlib.sha256(sgb_path.read_bytes()).hexdigest(),
                            "sample_rate": sample_rate,
                            "sample_count": len(audio) // 4,
                            "pcm_sha256": hashlib.sha256(audio).hexdigest(),
                            "video_fps": av.timing.fps,
                            "run_samples": run_samples,
                            "video_callbacks": frame_callbacks,
                            "snapshot_sha256": snapshot_hashes,
                            "scheduled_input": scheduled,
                            "input_mode": "gb-lcd-frame-held-v1" if native_gb_input else "legacy-libretro-run-v1",
                            "native_gb_input_events": [(f, native_button_mask(m)) for f, m in events] if native_gb_input else [],
                            "audio_source": "snes-only" if require_snes_only_probe
                            else "mixed",
                            "sgb_sound_events": sound_events}
                if require_snes_only_probe:
                    timeline["post_audible_sound_writes"] = sound_trace
                    timeline["native_cycle_checkpoints"] = native_cycle_checkpoints
                    timeline["reference_options"] = configured_options
                    timeline["post_audible_sound_writes_limit_reached"] = len(sound_trace) == 32768
                if native_dsp is not None:
                    timeline["native_dsp"] = {
                        "sample_rate": 32040, "sample_count": native_count,
                        "pcm_sha256": hashlib.sha256(native_dsp).hexdigest()}
                if ram_bytes is not None:
                    timeline["apu_ram"] = {
                        "size": len(ram_bytes),
                        "sha256": hashlib.sha256(ram_bytes).hexdigest()}
                timeline_output.parent.mkdir(parents=True, exist_ok=True)
                with timeline_output.open("x", encoding="utf-8") as timeline_file:
                    json.dump(timeline, timeline_file, separators=(",", ":"))
                    timeline_file.write("\n")
            if boot_timeline_output is not None:
                count = core.gbb_reference_sgb_boot_count()
                if not 0 < count <= 128:
                    raise RuntimeError("reference boot timeline is empty or overflowed")
                boot_events = []
                for index in range(count):
                    entry = (C.c_uint64 * 7)()
                    if core.gbb_reference_sgb_boot_copy(index, entry) != 1 or entry[0] not in map(ord, "CBUSEPTAINR") or entry[6] != 0:
                        raise RuntimeError("reference boot timeline lost or corrupted an event")
                    boot_events.append(dict(zip(
                        ("kind", "master_clock_snapshot", "spc_half_clock_snapshot", "value", "count", "digest_fnv64"),
                        [chr(entry[0])] + list(entry)[1:6])))
                frequencies = (C.c_uint64 * 2)()
                if core.gbb_reference_sgb_apu_bus_frequencies(frequencies) != 1:
                    raise RuntimeError("reference boot timeline lacks clock frequencies")
                boot_timeline_output.parent.mkdir(parents=True, exist_ok=True)
                with boot_timeline_output.open("x", encoding="utf-8") as boot_file:
                    json.dump({"format": "gbb-sgb-boot-timeline-v1", "source": "reference",
                               "master_hz": int(frequencies[0]), "apu_half_hz": int(frequencies[1]),
                               "reference_options": configured_options,
                               "core_sha256": hashlib.sha256(core_path.read_bytes()).hexdigest(),
                               "input_mode": "gb-lcd-frame-held-v1" if native_gb_input else "legacy-libretro-run-v1",
                               "events": boot_events}, boot_file, indent=2)
                    boot_file.write("\n")
            if apu_bus_output is not None:
                count = core.gbb_reference_sgb_apu_bus_count()
                frequencies = (C.c_uint64 * 2)()
                if not 0 < count < 262144 or \
                        core.gbb_reference_sgb_apu_bus_frequencies(frequencies) != 1 or \
                        not all(frequencies):
                    raise RuntimeError("reference APU bus probe is empty, overflowed or invalid")
                bus_events = []
                for index in range(count):
                    entry = (C.c_uint64 * 7)()
                    if core.gbb_reference_sgb_apu_bus_copy(index, entry) != 1 or \
                            entry[0] not in map(ord, "hHRWKrw" if timer_poll_trace else "hHRWK") or entry[5] > 255 or entry[6] >= 64:
                        raise RuntimeError("reference APU bus probe lost or corrupted an event")
                    bus_events.append(dict(zip(
                        ("kind", "master_clock", "spc_half_clock", "pcm_sample",
                         "address", "value", "dsp_clock64"),
                        [chr(entry[0])] + list(entry)[1:])))
                apu_bus_output.parent.mkdir(parents=True, exist_ok=True)
                with apu_bus_output.open("x", encoding="utf-8") as bus_file:
                    json.dump({"format": "gbb-apu-bus-v2" if timer_poll_trace else "gbb-apu-bus-v1",
                               "source": "reference",
                               **({"phase_writes_from_reset": phase_writes_from_reset,
                                   "reference_options": configured_options} if timer_poll_trace else {}),
                               **({"history_window_half_clocks": list(history_window_half)} if history_window_half else {}),
                               "master_hz": int(frequencies[0]),
                               "apu_half_hz": int(frequencies[1]),
                               "core_sha256": hashlib.sha256(core_path.read_bytes()).hexdigest(),
                               "events": bus_events}, bus_file, separators=(",", ":"))
                    bus_file.write("\n")
            if events:
                applied = core.gbb_reference_sgb_frame_input_applied() if native_gb_input else next_event
                print(f"Applied {applied}/{len(events)} scripted input events; "
                      f"core queried {input_queries} joypad states "
                      f"({pressed_queries} pressed results); "
                      f"video rate {av.timing.fps:.6f} Hz, "
                      f"GB-to-reference frame offset {input_offset}, "
                      f"changes {input_offset_changes or []}; native GB input {native_gb_input}")
            if require_snes_only_probe:
                print(f"SNES-only reference: {len(sound_events)} host-consumed SOUND packets")
            if snapshot_output is not None or snapshot_series is not None:
                print(f"Reference video pixel format: {video_pixel_format} "
                      "(0=0RGB1555, 1=XRGB8888, 2=RGB565)")
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
    parser.add_argument("--input-script", type=Path,
                        help="GBB SGB input v1 script; mapped from GB to SNES frames")
    parser.add_argument("--input-offset-frames", type=int, default=0,
                        help="signed reference-frame adjustment after rate conversion")
    parser.add_argument("--input-offset-change", action="append", default=[],
                        metavar="GB_FRAME:OFFSET",
                        help="change the signed reference-frame offset at a GB frame")
    parser.add_argument("--snapshot-frame", type=int,
                        help="optional 1-based libretro video frame to save")
    parser.add_argument("--snapshot-output", type=Path,
                        help="PPM output for the requested video frame")
    parser.add_argument("--snapshot-series", nargs=3, metavar=("FIRST", "LAST", "DIR"),
                        help="save up to 201 consecutive video frames as local PPM files")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--timeline-output", type=Path,
                        help="JSON mapping callbacks, sound events, and runs to PCM samples")
    parser.add_argument("--apu-bus-output", type=Path,
                        help="bounded host/SPC bus timeline (additional local reference patch required)")
    parser.add_argument("--native-cycle-checkpoints", action="store_true",
                        help="equal native output counts, not wall-time checkpoints (local probe required)")
    parser.add_argument("--timer-poll-trace", action="store_true",
                        help="include timer configuration and read-to-clear polling in APU bus capture")
    parser.add_argument("--reference-entropy", choices=("None", "Low", "High"),
                        help="explicit diagnostic reference power-on state; default leaves core unchanged")
    parser.add_argument("--apu-history-window-half", type=int, nargs=2, metavar=("START", "END"),
                        help="bounded pre-command bus history in native half clocks (local probe required)")
    parser.add_argument("--boot-timeline-output", type=Path,
                        help="bounded boot/control/upload timeline (additional local probe required)")
    parser.add_argument("--native-gb-input", action="store_true",
                        help="hold scripted player-1 input at observed GB LCD frames; forbids frame offsets")
    parser.add_argument("--require-snes-only-probe", action="store_true",
                        help="require the local instrumented core; log host-consumed SOUND packets")
    parser.add_argument("--native-dsp-output", type=Path,
                        help="capture pre-resampler SNES DSP PCM (instrumented core only)")
    parser.add_argument("--apu-ram-output", type=Path,
                        help="capture final physical APU RAM (instrumented core only)")
    args = parser.parse_args()
    try:
        changes = []
        for item in args.input_offset_change:
            frame_text, separator, offset_text = item.partition(":")
            if not separator:
                raise ValueError("offset change must be GB_FRAME:OFFSET")
            changes.append((int(frame_text), int(offset_text)))
        series = None
        if args.snapshot_series is not None:
            first, last, directory = args.snapshot_series
            series = (int(first), int(last), Path(directory))
        rate, count = capture(args.core, args.game, args.sgb_rom,
                              args.system_dir, args.frames, args.output,
                              args.input_script, args.input_offset_frames, changes,
                              args.snapshot_frame, args.snapshot_output, series,
                              args.timeline_output, args.require_snes_only_probe,
                              args.native_dsp_output, args.apu_ram_output, args.apu_bus_output,
                              args.native_cycle_checkpoints, args.timer_poll_trace, args.reference_entropy,
                              tuple(args.apu_history_window_half) if args.apu_history_window_half else None,
                              args.boot_timeline_output, args.native_gb_input)
    except (OSError, RuntimeError, ValueError) as error:
        parser.error(str(error))
    print(f"Captured {count} stereo frames at {rate} Hz to {args.output}")


if __name__ == "__main__":
    main()
