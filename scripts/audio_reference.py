#!/usr/bin/env python3
"""Create and analyze external GBB audio waveform references.

The emulator's contract test consumes a deliberately small text format. This
tool converts 48 kHz/16-bit/stereo WAV captures into that format and
aggregates repeated captures into a median reference with measured tolerances.
References can use raw PCM comparison or a normalized per-channel shape
comparison for trusted digital-emulator captures.
SameBoy commonly records at 96 kHz; that path is supported only through an
explicit integer downsample request, so setup errors cannot be hidden by an
implicit resample or channel conversion.
"""

from __future__ import annotations

import argparse
import math
import statistics
import struct
import sys
from typing import NoReturn
import wave
from pathlib import Path


FORMAT = "gbb-audio-waveform-v1"
SAMPLE_RATE = 48_000
CHANNELS = 2
QUANTIZATION = 64
DEFAULT_FRAMES = 93  # 186 interleaved stereo samples, matching the fixture.
SOURCES = ("hardware", "trusted-emulator")
COMPARISONS = ("raw", "normalized")


def normalize_source(value: str) -> str:
    # ``external`` was used by an unpublished early version of this tool.
    return "hardware" if value in ("", "external") else value


def fail(message: str) -> NoReturn:
    raise ValueError(message)


def read_wav(path: Path, expected_rate: int | None = SAMPLE_RATE) -> tuple[list[int], int, int]:
    try:
        with wave.open(str(path), "rb") as source:
            channels = source.getnchannels()
            sample_width = source.getsampwidth()
            sample_rate = source.getframerate()
            frame_count = source.getnframes()
            compression = source.getcomptype()
            payload = source.readframes(frame_count)
    except (OSError, wave.Error) as error:
        fail(f"cannot read WAV {path}: {error}")
    if compression != "NONE":
        fail(f"{path}: compressed WAV input is not supported")
    if channels != CHANNELS:
        fail(f"{path}: expected {CHANNELS} channels, got {channels}")
    if sample_width != 2:
        fail(f"{path}: expected 16-bit PCM, got {sample_width * 8}-bit")
    if expected_rate is not None and sample_rate != expected_rate:
        fail(f"{path}: expected {expected_rate} Hz, got {sample_rate} Hz")
    expected_bytes = frame_count * CHANNELS * sample_width
    if len(payload) != expected_bytes:
        fail(f"{path}: truncated PCM payload")
    values = list(struct.unpack("<%dh" % (len(payload) // 2), payload))
    return values, sample_rate, channels


def _trunc_divide(numerator: int, denominator: int) -> int:
    """Integer division with the C/C++ rule (truncate toward zero)."""
    if numerator < 0:
        return -((-numerator) // denominator)
    return numerator // denominator


def downsample(values: list[int], source_rate: int, target_rate: int = SAMPLE_RATE) -> list[int]:
    """Average an integer number of source frames into each target frame.

    This deterministic box filter is intended for SameBoy's 96 kHz output,
    not as a general-purpose audio resampler. An incomplete trailing source
    group is discarded (capture loops can overshoot by one callback).
    """
    if source_rate == target_rate:
        return values
    if source_rate < target_rate or source_rate % target_rate != 0:
        fail(f"cannot deterministically downsample {source_rate} Hz to {target_rate} Hz")
    ratio = source_rate // target_rate
    if len(values) % CHANNELS != 0:
        fail("stereo PCM payload is not frame-aligned")
    source_frames = len(values) // CHANNELS
    # A run loop can finish just after a callback, leaving one trailing frame
    # (SameBoy commonly produces 187 frames for a requested 186).  Keep only
    # complete source groups; the requested --downsample makes this truncation
    # explicit and deterministic.
    complete_frames = source_frames - source_frames % ratio
    output: list[int] = []
    for frame in range(0, complete_frames, ratio):
        for channel in range(CHANNELS):
            total = sum(values[(frame + offset) * CHANNELS + channel]
                        for offset in range(ratio))
            output.append(_trunc_divide(total, ratio))
    return output


def quantize(values: list[int], start_frame: int, frame_count: int,
             gain: float, dc_offset: float) -> list[int]:
    if not math.isfinite(gain) or gain <= 0:
        fail("gain must be a finite positive number")
    if start_frame < 0 or frame_count <= 0:
        fail("start-frame must be non-negative and frames must be positive")
    start = start_frame * CHANNELS
    end = start + frame_count * CHANNELS
    if end > len(values):
        fail("capture does not contain the requested frame window")
    output: list[int] = []
    for value in values[start:end]:
        # C++ integer division truncates toward zero, unlike Python's //.  Keep
        # the conversion identical to apu_waveform_contract_tests.cpp.
        adjusted = (value - dc_offset) * gain / QUANTIZATION
        output.append(math.trunc(adjusted))
    return output


def write_reference(path: Path, name: str, model: str, samples: list[int],
                    max_abs_error: int, rms_error: int,
                    provenance: str, source: str = "hardware",
                    comparison: str = "raw") -> None:
    if not samples or len(samples) % CHANNELS != 0:
        fail("reference must contain a non-empty interleaved stereo sample list")
    if max_abs_error < 0 or rms_error < 0:
        fail("tolerances must be non-negative")
    if source not in SOURCES:
        fail(f"unsupported reference source: {source}")
    if comparison not in COMPARISONS:
        fail(f"unsupported comparison mode: {comparison}")
    if comparison == "normalized" and source != "trusted-emulator":
        fail("normalized references require source=trusted-emulator")
    if not provenance.strip():
        fail("provenance must not be empty")
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="\n") as output:
        output.write("# GBB audio waveform reference v1\n")
        output.write(f"format={FORMAT}\n")
        output.write(f"name={name}\n")
        output.write(f"model={model}\n")
        output.write(f"source={source}\n")
        output.write(f"comparison={comparison}\n")
        output.write(f"provenance={provenance}\n")
        output.write(f"sample_rate={SAMPLE_RATE}\n")
        output.write(f"channels={CHANNELS}\n")
        output.write(f"quantization={QUANTIZATION}\n")
        output.write(f"samples={len(samples)}\n")
        output.write(f"max_abs_error={max_abs_error}\n")
        output.write(f"rms_error={rms_error}\n")
        output.write("data=\n")
        for index in range(0, len(samples), 16):
            output.write(" ".join(str(value) for value in samples[index:index + 16]))
            output.write("\n")


def parse_reference(path: Path) -> tuple[dict[str, str], list[int]]:
    metadata: dict[str, str] = {}
    samples: list[int] = []
    data = False
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except OSError as error:
        fail(f"cannot read reference {path}: {error}")
    for line in lines:
        if not line or line.startswith("#"):
            continue
        if line == "data=":
            data = True
            continue
        if data:
            try:
                samples.extend(int(value) for value in line.split())
            except ValueError:
                fail(f"{path}: invalid sample value")
            continue
        if "=" not in line:
            fail(f"{path}: malformed metadata line")
        key, value = line.split("=", 1)
        metadata[key] = value
    required = {"format", "name", "model", "sample_rate", "channels",
                "quantization", "samples"}
    if not required.issubset(metadata):
        fail(f"{path}: missing required metadata")
    if metadata["format"] != FORMAT:
        fail(f"{path}: unsupported format")
    if int(metadata["sample_rate"]) != SAMPLE_RATE or int(metadata["channels"]) != CHANNELS:
        fail(f"{path}: reference shape is not 48 kHz stereo")
    if int(metadata["quantization"]) != QUANTIZATION:
        fail(f"{path}: unsupported quantization")
    if int(metadata["samples"]) != len(samples):
        fail(f"{path}: sample count does not match metadata")
    return metadata, samples


def convert(args: argparse.Namespace) -> None:
    values, sample_rate, _ = read_wav(args.input, expected_rate=None)
    if sample_rate != SAMPLE_RATE:
        ratio = sample_rate // SAMPLE_RATE if sample_rate % SAMPLE_RATE == 0 else 0
        if args.downsample <= 1 or ratio != args.downsample:
            ratio_text = str(ratio) if ratio else "an integer ratio"
            fail(f"{args.input}: expected {SAMPLE_RATE} Hz; pass --downsample {ratio_text} "
                 "for an explicit integer conversion")
        values = downsample(values, sample_rate)
    elif args.downsample != 1:
        fail("--downsample is only valid when the input rate differs from 48 kHz")
    if args.source == "trusted-emulator" and args.provenance == "hardware":
        fail("trusted-emulator references require --provenance with the emulator and revision")
    samples = quantize(values, args.start_frame, args.frames, args.gain,
                       args.dc_offset)
    write_reference(args.output, args.name, args.model, samples, args.max_abs_error,
                    args.rms_error, args.provenance, args.source, args.comparison)


def aggregate(args: argparse.Namespace) -> None:
    if len(args.inputs) < 2:
        fail("aggregate requires at least two reference files")
    parsed = [parse_reference(path) for path in args.inputs]
    metadata = parsed[0][0]
    samples = [entry[1] for entry in parsed]
    input_source = normalize_source(metadata.get("source", ""))
    metadata_comparison = metadata.get("comparison", "") or ""
    input_comparison = metadata_comparison or "raw"
    if input_comparison not in COMPARISONS:
        fail(f"unsupported comparison mode: {input_comparison}")
    if any(entry[0].get("name") != metadata.get("name") or
           entry[0].get("model") != metadata.get("model") or
           normalize_source(entry[0].get("source", "")) != input_source or
           (entry[0].get("comparison", "raw") or "raw") != input_comparison or
           len(entry[1]) != len(samples[0]) for entry in parsed[1:]):
        fail("all captures must have the same name, model, source, comparison, and sample count")
    consensus = [int(statistics.median(values)) for values in zip(*samples)]
    max_error = 0
    rms_values: list[float] = []
    for capture in samples:
        errors = [value - reference for value, reference in zip(capture, consensus)]
        max_error = max(max_error, max(abs(error) for error in errors))
        rms_values.append(math.sqrt(sum(error * error for error in errors) /
                                    len(errors)))
    margin = args.margin
    if margin < 0:
        fail("margin must be non-negative")
    suggested_max = max_error + margin
    suggested_rms = math.ceil(max(rms_values) + margin)
    provenance = args.provenance or "aggregate:" + ",".join(path.name for path in args.inputs)
    source = args.source or input_source
    if args.source and input_source != args.source and metadata.get("source", ""):
        fail("--source does not match the source metadata in the input captures")
    if source not in SOURCES:
        fail("aggregate inputs need source=hardware or source=trusted-emulator")
    comparison = args.comparison or input_comparison
    if metadata_comparison and comparison != input_comparison:
        fail("--comparison does not match the comparison metadata in the input captures")
    write_reference(args.output, metadata["name"], metadata["model"], consensus,
                    suggested_max, suggested_rms, provenance, source, comparison)
    print(f"captures={len(samples)} samples={len(consensus)} "
          f"observed_max_abs_error={max_error} "
          f"observed_rms_error={max(rms_values):.3f} "
          f"suggested_max_abs_error={suggested_max} "
          f"suggested_rms_error={suggested_rms}")


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)
    convert_parser = subparsers.add_parser("convert", help="convert a WAV capture")
    convert_parser.add_argument("input", type=Path)
    convert_parser.add_argument("output", type=Path)
    convert_parser.add_argument("--name", required=True)
    convert_parser.add_argument(
        "--model",
        choices=("dmg", "dmg0", "dmg-b", "mgb", "cgb0", "cgb-c", "cgb-e", "cgb"),
        required=True,
        help="hardware or trusted-emulator revision label",
    )
    convert_parser.add_argument("--start-frame", type=int, default=0)
    convert_parser.add_argument("--frames", type=int, default=DEFAULT_FRAMES)
    convert_parser.add_argument("--gain", type=float, default=1.0)
    convert_parser.add_argument("--dc-offset", type=float, default=0.0)
    convert_parser.add_argument("--max-abs-error", type=int, default=0)
    convert_parser.add_argument("--rms-error", type=int, default=0)
    convert_parser.add_argument("--provenance", default="hardware")
    convert_parser.add_argument("--source", choices=SOURCES, default="hardware",
                                help="hardware capture or trusted emulator output")
    convert_parser.add_argument("--comparison", choices=COMPARISONS, default="raw",
                                help="raw PCM or normalized per-channel waveform shape")
    convert_parser.add_argument("--downsample", type=int, default=1,
                                help="explicit source-rate/48 kHz ratio (for example 2 for 96 kHz)")
    convert_parser.set_defaults(function=convert)

    aggregate_parser = subparsers.add_parser(
        "aggregate", help="median-aggregate repeated reference captures")
    aggregate_parser.add_argument("inputs", type=Path, nargs="+")
    aggregate_parser.add_argument("output", type=Path)
    aggregate_parser.add_argument("--margin", type=int, default=1)
    aggregate_parser.add_argument("--provenance")
    aggregate_parser.add_argument("--source", choices=SOURCES,
                                  help="source to record when inputs predate source metadata")
    aggregate_parser.add_argument("--comparison", choices=COMPARISONS,
                                  help="comparison mode to record when inputs predate metadata")
    aggregate_parser.set_defaults(function=aggregate)
    return parser


def main() -> int:
    try:
        args = build_parser().parse_args()
        args.function(args)
    except (ValueError, OSError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
