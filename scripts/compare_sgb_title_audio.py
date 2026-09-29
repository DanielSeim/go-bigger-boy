#!/usr/bin/env python3
"""Compare a real SGB title event against independent, mixed reference audio.

This is a timing/envelope diagnostic, not a sample-exact accuracy test: the
libretro reference includes GB audio and may have different boot/input phases.
"""

import argparse
from array import array
from collections import Counter
import hashlib
import json
import math
from pathlib import Path
import statistics
import sys
import wave

from capture_sgb_libretro_audio import GB_FRAME_RATE
from align_sgb_frames import read_sgb_image, VIEWPORT, WIDTH


def scene_agreement(gbb_scene: Path, reference_scene: Path) -> float:
    """Check a recognizable GB viewport, allowing small palette differences."""
    ours = read_sgb_image(gbb_scene)
    theirs = read_sgb_image(reference_scene)
    x0, y0, width, height = VIEWPORT
    pixels_ours = []
    pixels_theirs = []
    for y in range(y0, y0 + height):
        for x in range(x0, x0 + width):
            offset = (y * WIDTH + x) * 3
            pixels_ours.append(ours[offset:offset + 3])
            pixels_theirs.append(theirs[offset:offset + 3])
    # A blank/loading frame can trivially match an unrelated blank frame.
    for path, pixels in ((gbb_scene, pixels_ours),
                         (reference_scene, pixels_theirs)):
        if len(set(pixels)) < 4 or Counter(pixels).most_common(1)[0][1] > len(pixels) * 0.9:
            raise ValueError(f"{path}: scene checkpoint is too uniform")
    matching = sum(all(abs(a - b) <= 16 for a, b in zip(left, right))
                   for left, right in zip(pixels_ours, pixels_theirs))
    return matching / len(pixels_ours)


def read_stereo_wav(path: Path) -> tuple[int, array]:
    with wave.open(str(path), "rb") as source:
        if source.getnchannels() != 2 or source.getsampwidth() != 2 or \
                source.getcomptype() != "NONE":
            raise ValueError(f"{path}: expected uncompressed stereo 16-bit PCM")
        rate = source.getframerate()
        values = array("h")
        values.frombytes(source.readframes(source.getnframes()))
    if sys.byteorder != "little":
        values.byteswap()
    return rate, values


def rms_buckets(pcm: array, rate: int, start_seconds: float,
                seconds: float, bucket_seconds: float = 0.025) -> list[float]:
    bucket_frames = max(1, round(rate * bucket_seconds))
    start = round(start_seconds * rate)
    count = round(seconds / bucket_seconds)
    if start < 0 or start + count * bucket_frames > len(pcm) // 2:
        raise ValueError("requested comparison window falls outside a WAV file")
    result = []
    for bucket in range(count):
        left = (start + bucket * bucket_frames) * 2
        right = left + bucket_frames * 2
        energy = sum(int(value) * int(value) for value in pcm[left:right])
        result.append(math.sqrt(energy / (bucket_frames * 2)) / 32768)
    return result


def correlation(a: list[float], b: list[float]) -> float:
    if len(a) != len(b) or len(a) < 2:
        raise ValueError("correlation needs equal nontrivial series")
    mean_a = statistics.fmean(a)
    mean_b = statistics.fmean(b)
    centered_a = [value - mean_a for value in a]
    centered_b = [value - mean_b for value in b]
    power_a = sum(value * value for value in centered_a)
    power_b = sum(value * value for value in centered_b)
    if power_a == 0 or power_b == 0:
        return 0.0
    return sum(x * y for x, y in zip(centered_a, centered_b)) / \
        math.sqrt(power_a * power_b)


def best_envelope_alignment(ours: list[float], reference: list[float],
                            center_bucket: int, radius_buckets: int
                            ) -> tuple[int, float]:
    best = (center_bucket, -2.0)
    for start in range(max(0, center_bucket - radius_buckets),
                       min(len(reference) - len(ours),
                           center_bucket + radius_buckets) + 1):
        candidate = correlation(ours, reference[start:start + len(ours)])
        if candidate > best[1]:
            best = (start, candidate)
    if best[1] == -2.0:
        raise ValueError("no reference window within the search range")
    return best


def describe(values: list[float]) -> tuple[float, float, float]:
    return (math.sqrt(statistics.fmean(value * value for value in values)),
            max(values), sum(value > 0.001 for value in values) / len(values))


def resample_mono(pcm: array, source_rate: int, start_seconds: float,
                  seconds: float, output_rate: int = 8000) -> list[float]:
    count = round(seconds * output_rate)
    if start_seconds < 0 or (start_seconds + seconds) * source_rate >= len(pcm) // 2:
        raise ValueError("waveform window falls outside a WAV file")
    values = []
    for index in range(count):
        position = (start_seconds + index / output_rate) * source_rate
        source = int(position)
        fraction = position - source
        first = (int(pcm[source * 2]) + int(pcm[source * 2 + 1])) / 2
        second = (int(pcm[(source + 1) * 2]) + int(pcm[(source + 1) * 2 + 1])) / 2
        values.append((first + fraction * (second - first)) / 32768)
    return values


def waveform_alignment(ours: list[float], reference: list[float],
                       radius_samples: int) -> tuple[int, float]:
    if len(reference) < len(ours) + 2 * radius_samples:
        raise ValueError("waveform search window is too short")
    best = (0, 0.0)
    for shift in range(-radius_samples, radius_samples + 1, 8):
        start = radius_samples + shift
        score = correlation(ours, reference[start:start + len(ours)])
        if abs(score) > abs(best[1]):
            best = (shift, score)
    return best


def reference_frame_window(timeline_path: Path, video_frame: int,
                           rate: int, pcm: array,
                           scene_path: Path | None = None) -> tuple[int, int]:
    """Bound a video callback by samples emitted during its retro_run call.

    The reference core may batch audio on either side of the video callback;
    this is a frame-sized uncertainty interval, not an exact sound onset.
    """
    timeline = json.loads(timeline_path.read_text(encoding="utf-8"))
    sample_count = len(pcm) // 2
    if not isinstance(timeline, dict) or timeline.get("format") != \
            "gbb-libretro-audio-timeline-v1":
        raise ValueError("invalid libretro audio timeline format")
    if timeline.get("sample_rate") != rate or timeline.get("sample_count") != sample_count:
        raise ValueError("timeline does not match reference WAV rate and length")
    if timeline.get("pcm_sha256") != hashlib.sha256(pcm.tobytes()).hexdigest():
        raise ValueError("timeline does not match reference WAV PCM content")
    if scene_path is not None:
        snapshots = timeline.get("snapshot_sha256")
        if not isinstance(snapshots, dict) or \
                snapshots.get(str(video_frame)) != \
                hashlib.sha256(scene_path.read_bytes()).hexdigest():
            raise ValueError("reference scene does not match timeline snapshot")
    runs = timeline.get("run_samples")
    callbacks = timeline.get("video_callbacks")
    if not isinstance(runs, list) or not runs or not isinstance(callbacks, list):
        raise ValueError("timeline has no run or video records")
    previous = 0
    for run in runs:
        if not isinstance(run, list) or len(run) != 2 or \
                any(type(value) is not int for value in run) or \
                run[0] != previous or run[1] < run[0] or run[1] > sample_count:
            raise ValueError("timeline has noncontiguous or invalid PCM run ranges")
        previous = run[1]
    if previous != sample_count:
        raise ValueError("timeline does not cover the complete reference WAV")
    selected = None
    last_frame = 0
    for entry in callbacks:
        if not isinstance(entry, dict):
            raise ValueError("timeline contains invalid video callback")
        frame = entry.get("video_frame")
        index = entry.get("run_index")
        sample = entry.get("sample_at_callback")
        if type(frame) is not int or frame <= last_frame or \
                type(index) is not int or not 0 <= index < len(runs) or \
                type(sample) is not int or not runs[index][0] <= sample <= runs[index][1]:
            raise ValueError("timeline contains invalid video callback mapping")
        last_frame = frame
        if frame == video_frame:
            selected = tuple(runs[index])
    if selected is None:
        raise ValueError(f"video frame {video_frame} is absent from reference timeline")
    return selected


def compare(gbb_path: Path, reference_path: Path, gbb_event_sample: int,
            event_gb_frame: int, search_seconds: float,
            window_seconds: float, reference_video_fps: float = 60.098812,
            reference_frame_offset: int = 0,
            gbb_scene: Path | None = None,
            reference_scene: Path | None = None,
            reference_timeline: Path | None = None,
            reference_scene_frame: int | None = None) -> str:
    if gbb_scene is None or reference_scene is None:
        raise ValueError("both scene checkpoint images are required before audio comparison")
    if reference_scene_frame is not None and reference_timeline is None:
        raise ValueError("reference scene frame requires a reference timeline")
    scene_score = scene_agreement(gbb_scene, reference_scene)
    if scene_score < 0.65:
        raise ValueError(f"scene checkpoint mismatch: {scene_score:.1%} of GB viewport "
                         "pixels agree within 16/channel (minimum 65%)")
    gbb_rate, gbb_pcm = read_stereo_wav(gbb_path)
    reference_rate, reference_pcm = read_stereo_wav(reference_path)
    if gbb_event_sample < 0 or gbb_event_sample >= len(gbb_pcm) // 2:
        raise ValueError("GBB event sample is outside the GBB WAV")
    if event_gb_frame < 0 or search_seconds < 0 or window_seconds <= 0:
        raise ValueError("frame and window arguments must be nonnegative")
    bucket = 0.025
    gbb_time = gbb_event_sample / gbb_rate
    if not 30 <= reference_video_fps <= 120:
        raise ValueError("reference video rate is implausible")
    event_reference_frame = round(event_gb_frame * reference_video_fps /
                                  GB_FRAME_RATE) + reference_frame_offset
    if reference_timeline is not None:
        if reference_scene_frame is None:
            raise ValueError("reference scene frame is required with a timeline")
        if reference_scene_frame != event_reference_frame:
            raise ValueError("reference scene frame differs from predicted event frame")
        first_sample, last_sample = reference_frame_window(
            reference_timeline, reference_scene_frame, reference_rate,
            reference_pcm, reference_scene)
        expected_reference_time = (first_sample + last_sample) / (2 * reference_rate)
        anchor_description = (f"callback audio range [{first_sample}, {last_sample}] "
                              f"({first_sample / reference_rate:.3f}-"
                              f"{last_sample / reference_rate:.3f}s), "
                              f"midpoint {expected_reference_time:.3f}s")
    else:
        expected_reference_time = event_reference_frame / reference_video_fps
        anchor_description = f"frame-based estimate {expected_reference_time:.3f}s"
    ours = rms_buckets(gbb_pcm, gbb_rate, gbb_time, window_seconds, bucket)
    reference_start = max(0.0, expected_reference_time - search_seconds)
    reference_span = window_seconds + 2 * search_seconds
    reference = rms_buckets(reference_pcm, reference_rate, reference_start,
                            reference_span, bucket)
    center = round((expected_reference_time - reference_start) / bucket)
    radius = round(search_seconds / bucket)
    nominal = rms_buckets(reference_pcm, reference_rate,
                          expected_reference_time, window_seconds, bucket)
    nominal_score = correlation(ours, nominal)
    start, score = best_envelope_alignment(ours, reference, center, radius)
    aligned_time = reference_start + start * bucket
    segment_buckets = min(len(ours), round(1.0 / bucket))
    segment_start = max(range(len(ours) - segment_buckets + 1),
                        key=lambda index: sum(ours[index:index + segment_buckets]))
    segment_offset = segment_start * bucket
    radius_samples = 400  # +/-50ms at 8kHz after coarse envelope alignment.
    gbb_wave = resample_mono(gbb_pcm, gbb_rate,
                             gbb_time + segment_offset,
                             segment_buckets * bucket)
    reference_wave = resample_mono(reference_pcm, reference_rate,
                                   aligned_time + segment_offset - 0.05,
                                   segment_buckets * bucket + 0.1)
    waveform_shift, waveform_score = waveform_alignment(
        gbb_wave, reference_wave, radius_samples)
    gbb_rms, gbb_peak, gbb_active = describe(ours)
    ref_rms, ref_peak, ref_active = describe(reference[start:start + len(ours)])
    return (f"Same-scene checkpoint: {scene_score:.1%} of GB viewport pixels "
            "agree within 16/channel (minimum 65%); this does not prove "
            "audio phase alignment.\n"
            f"GBB: {gbb_rate} Hz, event sample {gbb_event_sample} "
            f"({gbb_time:.3f}s in trace WAV)\n"
            f"Reference: {reference_rate} Hz, {anchor_description} "
            f"(video frame {event_reference_frame}); "
            f"best envelope start "
            f"{aligned_time:.3f}s (shift {aligned_time - expected_reference_time:+.3f}s)\n"
            f"{window_seconds:.2f}s post-event window: "
            f"GBB RMS={gbb_rms:.4f} peak-bucket={gbb_peak:.4f} "
            f"active={gbb_active:.1%}; "
            f"reference RMS={ref_rms:.4f} peak-bucket={ref_peak:.4f} "
            f"active={ref_active:.1%}\n"
            f"25ms RMS-envelope correlation at nominal frame={nominal_score:.3f}; "
            f"best within search range={score:.3f}\n"
            f"Best 8kHz mono waveform correlation={waveform_score:.3f} "
            f"within +/-50ms (lag {waveform_shift / 8000:+.4f}s)\n"
            "Interpretation: this does not establish waveform fidelity. "
            "The reference mixes GB and SNES audio, and independent runs "
            "may differ before scripted input. Inspect game phase and "
            "audio stems before using this as an accuracy gate.")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--gbb", type=Path, required=True)
    parser.add_argument("--reference", type=Path, required=True)
    parser.add_argument("--gbb-scene", type=Path, required=True,
                        help="256x224 image captured at the GBB event frame")
    parser.add_argument("--reference-scene", type=Path, required=True,
                        help="256x224 image captured at the predicted reference event frame")
    parser.add_argument("--gbb-event-sample", type=int, required=True)
    parser.add_argument("--event-gb-frame", type=int, default=2472)
    parser.add_argument("--reference-video-fps", type=float, default=60.098812)
    parser.add_argument("--reference-frame-offset", type=int, default=0,
                        help="offset effective at the event GB frame")
    parser.add_argument("--reference-timeline", type=Path,
                        help="JSON timeline from the same reference WAV capture")
    parser.add_argument("--reference-scene-frame", type=int,
                        help="1-based reference video frame saved as --reference-scene")
    parser.add_argument("--search-seconds", type=float, default=0.0,
                        help="optional exploratory envelope search around the nominal frame")
    parser.add_argument("--window-seconds", type=float, default=3.0)
    args = parser.parse_args()
    try:
        print(compare(args.gbb, args.reference, args.gbb_event_sample,
                      args.event_gb_frame, args.search_seconds,
                      args.window_seconds, args.reference_video_fps,
                      args.reference_frame_offset, args.gbb_scene,
                      args.reference_scene, args.reference_timeline,
                      args.reference_scene_frame))
        return 0
    except (OSError, ValueError, wave.Error) as error:
        print(f"audio comparison failed: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
