#!/usr/bin/env python3
"""Diagnostic native SGB DSP comparison anchored to a matching key-on write.

The reference trace's DSP sample is a probe position, not a hardware timestamp.
This tool never treats a freely searched waveform lag as proof of fidelity.
"""

import argparse
import csv
import hashlib
import json
import math
from pathlib import Path
import sys
import wave

from compare_sgb_title_audio import correlation, read_stereo_wav, resample_mono


def keyon_positions(gbb_events: Path, timeline_path: Path,
                    native_path: Path, gbb_path: Path) -> tuple[int, int]:
    with gbb_events.open(newline="", encoding="utf-8") as source:
        reader = csv.DictReader(source)
        if reader.fieldnames != ["kind", "master_clock", "spc_cycle",
                                 "pcm_sample", "address", "value"]:
            raise ValueError("invalid GBB sound event CSV header")
        gbb = list(reader)
    timeline = json.loads(timeline_path.read_text(encoding="utf-8"))
    if not isinstance(timeline, dict) or timeline.get("format") != \
            "gbb-libretro-audio-timeline-v1" or \
            timeline.get("audio_source") != "snes-only":
        raise ValueError("expected a SNES-only reference timeline")
    native_rate, native_pcm = read_stereo_wav(native_path)
    gbb_rate, gbb_pcm = read_stereo_wav(gbb_path)
    meta = timeline.get("native_dsp")
    if native_rate != 32040 or gbb_rate != 32000 or \
            not isinstance(meta, dict) or meta.get("sample_rate") != native_rate or \
            meta.get("sample_count") != len(native_pcm) // 2 or \
            meta.get("pcm_sha256") != hashlib.sha256(native_pcm.tobytes()).hexdigest():
        raise ValueError("native reference WAV does not match its timeline")
    packet = [row for row in gbb if row["kind"] == "P"]
    if len(packet) != 1:
        raise ValueError("GBB trace must contain exactly one audible SOUND packet marker")
    events = timeline.get("post_audible_sound_writes")
    if not isinstance(events, list) or not events:
        raise ValueError("reference DSP trace absent")
    ours = [row for row in gbb if row["kind"] == "D" and
            int(row["address"]) == 0x4c and int(row["value"]) != 0]
    theirs = [row for row in events if isinstance(row, dict) and
              row.get("kind") == "dsp" and row.get("address") == 0x4c and
              type(row.get("value")) is int and row["value"] != 0]
    if not ours or not theirs or int(ours[0]["value"]) != theirs[0]["value"]:
        raise ValueError("first nonzero KON register writes do not match")
    gbb_sample = int(ours[0]["pcm_sample"])
    ref_sample = theirs[0].get("dsp_sample")
    if type(ref_sample) is not int or not \
            0 <= int(packet[0]["pcm_sample"]) < gbb_sample < len(gbb_pcm) // 2 or \
            not 0 <= ref_sample < len(native_pcm) // 2:
        raise ValueError("key-on sample positions are invalid")
    return gbb_sample, ref_sample


def aligned_window(gbb_pcm, ref_pcm, gbb_rate: int, ref_rate: int,
                   gbb_anchor: int, ref_anchor: int, offset: float,
                   duration: float, shift: int, output_rate: int = 8000):
    """Sample both streams on one GBB-clock grid; shift is in output samples."""
    ours = resample_mono(gbb_pcm, gbb_rate,
                         gbb_anchor / gbb_rate + offset, duration,
                         output_rate=output_rate)
    reference = resample_mono(ref_pcm, ref_rate,
                              (ref_anchor + (offset + shift / output_rate) * gbb_rate)
                              / ref_rate, duration, output_rate=output_rate,
                              time_scale=gbb_rate / ref_rate)
    return ours, reference


def metrics(ours: list[float], reference: list[float]) -> tuple[float, float, float, float]:
    energy = sum(value * value for value in ours)
    other = sum(value * value for value in reference)
    if energy < 1e-8 or other < 1e-8:
        raise ValueError("silent comparison window")
    score = correlation(ours, reference)
    error = math.sqrt(sum((a - b) ** 2 for a, b in zip(ours, reference)) /
                      len(ours))
    level = math.sqrt(energy / len(ours))
    return score, error / level, math.sqrt(other / energy), level


def compare(gbb_path: Path, reference_native: Path, gbb_events: Path,
            reference_timeline: Path,
            offsets: tuple[float, ...] = (0.02,) +
            tuple(index / 10 for index in range(1, 16)),
            window_seconds: float = 0.08, max_lag_ms: float = 15) -> str:
    if not offsets or any(not math.isfinite(x) or x < 0 for x in offsets) or \
            not 0 < window_seconds <= 1 or not 0 <= max_lag_ms <= 50:
        raise ValueError("invalid window or lag search")
    anchor_gbb, anchor_ref = keyon_positions(gbb_events, reference_timeline,
                                             reference_native, gbb_path)
    gbb_rate, gbb_pcm = read_stereo_wav(gbb_path)
    ref_rate, ref_pcm = read_stereo_wav(reference_native)
    radius = round(max_lag_ms * 8)
    # Pick one phase using the first audible window. Later windows retain it.
    first = offsets[0]
    best = None
    for shift in range(-radius, radius + 1):
        try:
            ours, reference = aligned_window(gbb_pcm, ref_pcm, gbb_rate,
                                              ref_rate, anchor_gbb, anchor_ref,
                                              first, window_seconds, shift)
            score, error, level, rms = metrics(ours, reference)
        except ValueError:
            continue
        if best is None or score > best[0]:
            best = (score, shift)
    if best is None:
        raise ValueError("first window is silent or outside the PCM captures")
    fixed_shift = best[1]
    lines = [f"First nonzero DSP KON: GBB sample {anchor_gbb} value from trace; "
             f"reference native sample {anchor_ref} (trace-buffer position).",
             f"Native rates: GBB {gbb_rate} Hz, reference {ref_rate} Hz; "
             f"continuous clock ratio {gbb_rate}/{ref_rate}.",
             f"Initial phase search: +/-{max_lag_ms:g} ms at 8 kHz; "
             f"fixed lag {fixed_shift / 8:+.3f} ms. Later windows do not "
             "re-optimize phase.",
             "Offset from KON | correlation | normalized RMS error | level ratio | GBB RMS"]
    for offset in offsets:
        ours, reference = aligned_window(gbb_pcm, ref_pcm, gbb_rate,
                                          ref_rate, anchor_gbb, anchor_ref,
                                          offset, window_seconds, fixed_shift)
        score, error, level, rms = metrics(ours, reference)
        lines.append(f"{offset:>6.3f}s | {score:+.4f} | {error:.4f} | "
                     f"{level:.4f} | {rms:.5f}")
    lines.append("This is an emulator-to-emulator, 8 kHz mono diagnostic. "
                 "A matching KON write and high correlation do not prove "
                 "sample-exact or hardware-accurate audio.")
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--gbb", required=True, type=Path)
    parser.add_argument("--reference-native", required=True, type=Path)
    parser.add_argument("--gbb-events", required=True, type=Path)
    parser.add_argument("--reference-timeline", required=True, type=Path)
    args = parser.parse_args()
    try:
        print(compare(args.gbb, args.reference_native, args.gbb_events,
                      args.reference_timeline))
        return 0
    except (OSError, ValueError, KeyError, wave.Error) as error:
        print(f"key-on PCM comparison failed: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
