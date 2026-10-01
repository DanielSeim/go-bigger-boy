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
            native_dsp_output: Path | None = None
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
    if timeline_output is not None and timeline_output.exists():
        raise FileExistsError(timeline_output)
    if native_dsp_output is not None and native_dsp_output.exists():
        raise FileExistsError(native_dsp_output)
    if native_dsp_output is not None and not require_snes_only_probe:
        raise ValueError("native DSP capture requires the SNES-only probe")
    if require_snes_only_probe and timeline_output is None:
        raise ValueError("the SNES-only probe requires a timeline output")
    if (snapshot_frame is None) != (snapshot_output is None):
        raise ValueError("snapshot frame and output must be supplied together")
    if snapshot_frame is not None and not 1 <= snapshot_frame <= frames:
        raise ValueError("snapshot frame must be within the capture")
    if snapshot_output is not None and snapshot_output.exists():
        raise FileExistsError(snapshot_output)
    targets = [path.resolve() for path in
               (output, snapshot_output, timeline_output, native_dsp_output)
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
    if require_snes_only_probe:
        probe_symbols = ("gbb_reference_sgb_probe_version",
                         "gbb_reference_sgb_sound_count",
                         "gbb_reference_sgb_sound_packet",
                         "gbb_reference_sgb_sound_position",
                         "gbb_reference_sgb_dsp_sample_count",
                         "gbb_reference_sgb_dsp_copy",
                         "gbb_reference_sgb_event_count",
                         "gbb_reference_sgb_event_copy")
        if any(not hasattr(core, symbol) for symbol in probe_symbols):
            raise RuntimeError("core is not the instrumented SNES-only reference build")
        core.gbb_reference_sgb_probe_version.restype = C.c_uint
        if core.gbb_reference_sgb_probe_version() != 5:
            raise RuntimeError("unsupported SNES-only reference probe version")
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
        core.gbb_reference_sgb_event_count.restype = C.c_uint
        core.gbb_reference_sgb_event_copy.argtypes = [C.c_uint,
                                                        C.POINTER(C.c_uint)]
        core.gbb_reference_sgb_event_copy.restype = C.c_uint
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
            scheduled = schedule_input(events, av.timing.fps, input_offset,
                                       input_offset_changes)
            next_event = 0
            next_sound_event = 0
            next_trace_event = 0
            sound_trace = []
            for frame in range(frames):
                current_run = frame
                if next_event < len(scheduled) and frame == scheduled[next_event][0]:
                    held_buttons = scheduled[next_event][1]
                    next_event += 1
                sample_start = len(audio) // 4
                core.retro_run()
                if require_snes_only_probe:
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
                    if trace_count < next_trace_event or trace_count > 4096:
                        raise RuntimeError("reference sound write trace count is invalid")
                    for event_index in range(next_trace_event, trace_count):
                        entry = (C.c_uint * 6)()
                        if core.gbb_reference_sgb_event_copy(event_index, entry) != 1 or \
                                entry[0] not in (1, 2) or \
                                entry[1] > (3 if entry[0] == 1 else 127) or \
                                entry[2] > 255 or \
                                (entry[0] == 1 and (entry[4] >= 262 or entry[5] >= 1364)):
                            raise RuntimeError("reference sound write trace is invalid")
                        sound_trace.append({"kind": "host" if entry[0] == 1 else "dsp",
                                            "address": int(entry[1]),
                                            "value": int(entry[2]),
                                            "dsp_sample": int(entry[3]) if entry[0] == 2
                                            else None,
                                            "cpu_vcounter": int(entry[4]),
                                            "cpu_hcounter": int(entry[5]),
                                            "run_index": frame})
                    next_trace_event = trace_count
                if timeline_output is not None:
                    run_samples.append([sample_start, len(audio) // 4])
            if events and next_event == 0:
                raise RuntimeError("capture ended before the first scripted input event")
            if events and (input_queries == 0 or pressed_queries == 0):
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
                            "audio_source": "snes-only" if require_snes_only_probe
                            else "mixed",
                            "sgb_sound_events": sound_events}
                if require_snes_only_probe:
                    timeline["post_audible_sound_writes"] = sound_trace
                    timeline["post_audible_sound_writes_limit_reached"] = len(sound_trace) == 4096
                if native_dsp is not None:
                    timeline["native_dsp"] = {
                        "sample_rate": 32040, "sample_count": native_count,
                        "pcm_sha256": hashlib.sha256(native_dsp).hexdigest()}
                timeline_output.parent.mkdir(parents=True, exist_ok=True)
                with timeline_output.open("x", encoding="utf-8") as timeline_file:
                    json.dump(timeline, timeline_file, separators=(",", ":"))
                    timeline_file.write("\n")
            if events:
                print(f"Applied {next_event}/{len(events)} scripted input events; "
                      f"core queried {input_queries} joypad states "
                      f"({pressed_queries} pressed results); "
                      f"video rate {av.timing.fps:.6f} Hz, "
                      f"GB-to-reference frame offset {input_offset}, "
                      f"changes {input_offset_changes or []}")
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
    parser.add_argument("--require-snes-only-probe", action="store_true",
                        help="require the local instrumented core; log host-consumed SOUND packets")
    parser.add_argument("--native-dsp-output", type=Path,
                        help="capture pre-resampler SNES DSP PCM (instrumented core only)")
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
                              args.native_dsp_output)
    except (OSError, RuntimeError, ValueError) as error:
        parser.error(str(error))
    print(f"Captured {count} stereo frames at {rate} Hz to {args.output}")


if __name__ == "__main__":
    main()
