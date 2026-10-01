#!/usr/bin/env python3
"""Compare GBB PCM with both output stages of a local SNES-only reference.

The native WAV and libretro WAV must belong to one instrumented capture.
Correlation is diagnostic, not proof of hardware-exact audio.
"""

import argparse
import hashlib
import json
from pathlib import Path
import sys

from compare_sgb_title_audio import (correlation, read_stereo_wav,
                                     resample_mono, rms_buckets,
                                     waveform_alignment)


def compare_stages(gbb_path: Path, output_path: Path, native_path: Path,
                   timeline_path: Path, gbb_event_sample: int,
                   event_index: int, offsets: tuple[float, ...] = (0.1, 0.5, 1.0),
                   window_seconds: float = 0.4) -> str:
    timeline = json.loads(timeline_path.read_text(encoding="utf-8"))
    if not isinstance(timeline, dict) or timeline.get("format") != \
            "gbb-libretro-audio-timeline-v1" or \
            timeline.get("audio_source") != "snes-only":
        raise ValueError("expected an SNES-only reference timeline")
    native_meta = timeline.get("native_dsp")
    if not isinstance(native_meta, dict):
        raise ValueError("timeline has no native DSP capture")
    gbb_rate, gbb_pcm = read_stereo_wav(gbb_path)
    output_rate, output_pcm = read_stereo_wav(output_path)
    native_rate, native_pcm = read_stereo_wav(native_path)
    if gbb_rate != 32000 or output_rate != 48000:
        raise ValueError("expected 32 kHz GBB and 48 kHz libretro output")
    for name, rate, pcm, meta in (
            ("output", output_rate, output_pcm, timeline),
            ("native", native_rate, native_pcm, native_meta)):
        if meta.get("sample_rate") != rate or \
                meta.get("sample_count") != len(pcm) // 2 or \
                meta.get("pcm_sha256") != hashlib.sha256(pcm.tobytes()).hexdigest():
            raise ValueError(f"{name} PCM does not match the timeline")
    if native_rate != 32040:
        raise ValueError("unexpected native reference DSP rate")
    events = timeline.get("sgb_sound_events")
    runs = timeline.get("run_samples")
    if not isinstance(events, list) or not 0 <= event_index < len(events) or \
            not isinstance(runs, list):
        raise ValueError("reference SOUND event is absent")
    event = events[event_index]
    index = event.get("run_index") if isinstance(event, dict) else None
    if type(index) is not int or not 0 <= index < len(runs) or \
            [event.get("sample_start"), event.get("sample_end")] != runs[index] or \
            not 0 <= event["sample_start"] < event["sample_end"] <= len(output_pcm) // 2:
        raise ValueError("reference SOUND event is not bound to its output run")
    if not 0 <= gbb_event_sample < len(gbb_pcm) // 2 or \
            not 0 < window_seconds <= 2 or not offsets or \
            any(not 0 <= offset <= 5 for offset in offsets):
        raise ValueError("invalid GBB event or waveform windows")
    gbb_time = gbb_event_sample / gbb_rate
    ref_time = (event["sample_start"] + event["sample_end"]) / (2 * output_rate)
    lines = [f"GBB event {gbb_time:.6f}s; reference SOUND run midpoint "
             f"{ref_time:.6f}s (uncertain within one run)."]
    for name, rate, pcm in (("libretro", output_rate, output_pcm),
                            ("native DSP", native_rate, native_pcm)):
        # Both reference stages start with the same emulation run. Their
        # stream endpoints may differ by a few milliseconds; the bounded lag
        # search absorbs that without shifting the event metadata itself.
        envelope = correlation(rms_buckets(gbb_pcm, gbb_rate, gbb_time, 2.0),
                               rms_buckets(pcm, rate, ref_time, 2.0))
        lines.append(f"{name}: 25ms envelope={envelope:+.5f}")
        for offset in offsets:
            ours = resample_mono(gbb_pcm, gbb_rate,
                                 gbb_time + offset, window_seconds)
            reference = resample_mono(pcm, rate,
                                      ref_time + (offset - 0.05) * 32000 / native_rate,
                                      window_seconds + 0.1,
                                      time_scale=32000 / native_rate)
            lag, score = waveform_alignment(ours, reference, 400)
            lines.append(f"  {offset:.1f}s: correlation={score:+.5f}, "
                         f"lag={lag / 8000:+.5f}s")
    lines.append("Continuous 32000/32040 clock adjustment from the SOUND "
                 "anchor is exploratory; this is not a sample-exact or "
                 "hardware-fidelity test.")
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--gbb", required=True, type=Path)
    parser.add_argument("--reference-output", required=True, type=Path)
    parser.add_argument("--reference-native", required=True, type=Path)
    parser.add_argument("--reference-timeline", required=True, type=Path)
    parser.add_argument("--gbb-event-sample", required=True, type=int)
    parser.add_argument("--reference-sound-event-index", required=True, type=int)
    args = parser.parse_args()
    try:
        print(compare_stages(args.gbb, args.reference_output,
                             args.reference_native, args.reference_timeline,
                             args.gbb_event_sample,
                             args.reference_sound_event_index))
        return 0
    except (OSError, ValueError) as error:
        print(f"stage comparison failed: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
