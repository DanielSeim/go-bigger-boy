#!/usr/bin/env python3
"""Phase-tolerant, same-core cartridge audio comparison after DMG boot.

Only local execution inputs are used. Reports contain metrics/hashes, never
PCM, wave RAM, ROM bytes or firmware instructions. No external Python packages.
"""
from array import array
from contextlib import nullcontext
import argparse
import hashlib
import json
import math
from pathlib import Path
import statistics
import subprocess
import sys
import tempfile
import wave

from compare_dmg_boot import compare as compare_boot, fingerprint

RATE = 48_000
CLOCK = 4_194_304
BUCKET = 2400  # 50 ms; no alignment search or time stretching
FFT_SIZE = 2048
ACTIVE_RMS = 64  # -54 dBFS, safely above the quiet handoff's DC floor
THRESHOLDS = {
    "event_timing_ms": 1.0, "onset_ms": 50.0, "active_duration_ms": 100.0,
    "gap_ms": 50.0, "rms_gain_db": 0.5, "window_gain_p95_db": 2.0,
    "mean_spectral_cosine": 0.98, "mean_centroid_relative_error": 0.03,
    "additional_clipping_fraction": 0.0001, "minimum_active_seconds": 0.5,
}


def fft(values):
    """Small radix-2 FFT of a Hann-windowed, DC-centered PCM window."""
    n = len(values)
    if n != FFT_SIZE:
        raise ValueError("invalid spectrum window size")
    mean = statistics.fmean(values)
    output = [complex((x - mean) * (0.5 - 0.5 * math.cos(2 * math.pi * i / (n - 1))))
              for i, x in enumerate(values)]
    j = 0
    for i in range(1, n):
        bit = n >> 1
        while j & bit:
            j ^= bit
            bit >>= 1
        j ^= bit
        if i < j:
            output[i], output[j] = output[j], output[i]
    size = 2
    while size <= n:
        angle = -2 * math.pi / size
        step = complex(math.cos(angle), math.sin(angle))
        for start in range(0, n, size):
            weight = 1 + 0j
            for k in range(size // 2):
                a = output[start + k]
                b = weight * output[start + k + size // 2]
                output[start + k], output[start + k + size // 2] = a + b, a - b
                weight *= step
        size *= 2
    return [x.real * x.real + x.imag * x.imag for x in output[:n // 2 + 1]]


def spectrum(values):
    powers = fft(values)
    bands = [0.0] * 40
    energy = moment = 0.0
    for index, power in enumerate(powers[1:], 1):
        frequency = index * RATE / FFT_SIZE
        if frequency < 40:
            continue
        band = min(39, max(0, int(math.log(frequency / 40) / math.log(600) * 40)))
        bands[band] += power
        energy += power
        moment += power * frequency
    return bands, moment / energy if energy else 0


def analyze(pcm):
    if len(pcm) % 2 or len(pcm) < 2 * BUCKET:
        raise ValueError("capture must contain frame-aligned stereo PCM and at least 50 ms")
    windows = []
    for start in range(0, len(pcm) - 2 * BUCKET + 1, 2 * BUCKET):
        channels = [pcm[start + channel:start + 2 * BUCKET:2] for channel in (0, 1)]
        powers = [statistics.fmean(x * x for x in c) - statistics.fmean(c) ** 2
                  for c in channels]
        rms = math.sqrt(max(0, sum(powers) / 2))
        # Analyze both channels separately; mono mixing would hide anti-phase stereo.
        bands = [0.0] * 40
        moment = energy = 0.0
        if rms >= ACTIVE_RMS:
            for c in channels:
                b, centroid = spectrum(c[:FFT_SIZE])
                amount = sum(b)
                moment += centroid * amount
                energy += amount
                bands = [x + y for x, y in zip(bands, b)]
        windows.append({"rms": rms, "active": rms >= ACTIVE_RMS,
                        "channel_power": powers,
                        "bands": bands, "centroid": moment / energy if energy else 0})
    active = [i for i, w in enumerate(windows) if w["active"]]
    gaps = []
    if active:
        for left, right in zip(active, active[1:]):
            if right > left + 1:
                gaps.append({"start_ms": (left + 1) * 50, "duration_ms": (right - left - 1) * 50})
    summary = {
        "frames": len(pcm) // 2, "duration_seconds": len(pcm) / (2 * RATE),
        "peak": max(abs(x) for x in pcm),
        "rms_ac": math.sqrt(statistics.fmean(w["rms"] ** 2 for w in windows)),
        "channel_rms_ac": [math.sqrt(max(0, statistics.fmean(w["channel_power"][c] for w in windows)))
                           for c in (0, 1)],
        "clipping_fraction": sum(abs(x) >= 32760 for x in pcm) / len(pcm),
        "onset_ms": active[0] * 50 if active else None,
        "active_duration_ms": len(active) * 50,
        "last_active_ms": (active[-1] + 1) * 50 if active else None,
        "longest_gap_ms": max((g["duration_ms"] for g in gaps), default=0),
        "gaps": gaps,
    }
    return summary, windows


def percentile(values, fraction):
    return sorted(values)[min(len(values) - 1, int((len(values) - 1) * fraction))]


def compare_audio(left, right, left_events, right_events):
    a, aw = analyze(left)
    b, bw = analyze(right)
    if abs(len(left) - len(right)) > 2:
        raise ValueError("capture durations differ by more than one stereo sample frame")
    commands_equal = [e[1:] for e in left_events] == [e[1:] for e in right_events]
    event_delta = max((abs(x[0] - y[0]) for x, y in zip(left_events, right_events)), default=0)
    gain = abs(20 * math.log10(max(a["rms_ac"], 1e-12) / max(b["rms_ac"], 1e-12)))
    cosines, centroids, window_gains = [], [], []
    active_disagreements = 0
    for x, y in zip(aw, bw):
        active_disagreements += x["active"] != y["active"]
        if not (x["active"] and y["active"]):
            continue
        window_gains.append(abs(20 * math.log10(x["rms"] / y["rms"])))
        norm = math.sqrt(sum(v * v for v in x["bands"]) * sum(v * v for v in y["bands"]))
        if not any(x["bands"]) and not any(y["bands"]):
            # Activity only in the bucket's untransformed tail is not spectral
            # disagreement, nor useful spectral coverage.
            continue
        cosines.append(sum(v * w for v, w in zip(x["bands"], y["bands"])) / norm if norm else 0)
        centroids.append(abs(x["centroid"] - y["centroid"]) / max(y["centroid"], 1e-12))
    covered = min(a["active_duration_ms"], b["active_duration_ms"], len(cosines) * 50) >= \
        THRESHOLDS["minimum_active_seconds"] * 1000
    metrics = {
        "register_commands_equal": commands_equal, "max_event_timing_delta_ms": event_delta * 1000 / CLOCK,
        "compared_spectral_windows": len(cosines),
        "rms_gain_delta_db": gain, "window_gain_p95_db": percentile(window_gains, .95) if window_gains else None,
        "mean_spectral_cosine": statistics.fmean(cosines) if cosines else None,
        "mean_centroid_relative_error": statistics.fmean(centroids) if centroids else None,
        "active_window_disagreement_ms": active_disagreements * 50,
        "onset_delta_ms": abs(a["onset_ms"] - b["onset_ms"]) if a["onset_ms"] is not None and b["onset_ms"] is not None else None,
        "active_duration_delta_ms": abs(a["active_duration_ms"] - b["active_duration_ms"]),
        "longest_gap_delta_ms": abs(a["longest_gap_ms"] - b["longest_gap_ms"]),
        "additional_clipping_fraction": max(0, a["clipping_fraction"] - b["clipping_fraction"]),
        "channel_gain_delta_db": [abs(20 * math.log10(max(x, 1e-12) / max(y, 1e-12)))
                                  if max(x, y) >= ACTIVE_RMS else 0.0
                                  for x, y in zip(a["channel_rms_ac"], b["channel_rms_ac"])],
    }
    failures = []
    if not commands_equal:
        failures.append("sound_register_commands")
    if max(metrics["channel_gain_delta_db"]) > THRESHOLDS["rms_gain_db"]:
        failures.append("channel_gain_delta_db")
    for metric, limit in (("max_event_timing_delta_ms", "event_timing_ms"),
                          ("rms_gain_delta_db", "rms_gain_db"),
                          ("window_gain_p95_db", "window_gain_p95_db"),
                          ("mean_centroid_relative_error", "mean_centroid_relative_error"),
                          ("onset_delta_ms", "onset_ms"),
                          ("active_duration_delta_ms", "active_duration_ms"),
                          ("active_window_disagreement_ms", "active_duration_ms"),
                          ("longest_gap_delta_ms", "gap_ms"),
                          ("additional_clipping_fraction", "additional_clipping_fraction")):
        if metrics[metric] is not None and metrics[metric] > THRESHOLDS[limit]:
            failures.append(metric)
    if cosines and metrics["mean_spectral_cosine"] < THRESHOLDS["mean_spectral_cosine"]:
        failures.append("mean_spectral_cosine")
    return {"status": "fail" if failures else ("pass" if covered else "incomplete"),
            "failures": failures, "coverage_sufficient": covered,
            "replacement": a, "reference": b, "metrics": metrics}


def read_pcm(path, snapshot):
    payload = path.read_bytes()
    pcm = array("h")
    if len(payload) % 4:
        raise ValueError("PCM file is not stereo-frame aligned")
    pcm.frombytes(payload)
    if sys.byteorder != "little":
        pcm.byteswap()
    if len(pcm) != snapshot["audio"]["followup"]["samples"]:
        raise ValueError("PCM sample count disagrees with probe metadata")
    digest = 14695981039346656037
    for byte in payload:
        digest = ((digest ^ byte) * 1099511628211) & ((1 << 64) - 1)
    if digest != snapshot["audio"]["followup"]["pcm_fnv64"]:
        raise ValueError("PCM hash disagrees with probe metadata")
    return pcm


def capture(probe, rom, boot, cycles, presses, directory):
    command = [str(probe), str(rom), "--run-cycles", str(cycles),
               "--audio-directory", str(directory)]
    if boot is not None:
        command += ["--boot-rom", str(boot)]
    for press in presses:
        command += ["--press-start-cycle", str(press)]
    result = subprocess.run(command, capture_output=True, text=True, timeout=180)
    if result.returncode:
        raise ValueError(f"audio probe failed: {result.stderr.strip()}")
    snapshot = json.loads(result.stdout)
    if snapshot.get("schema") != 1 or snapshot.get("cold_clock_cycles") != 0:
        raise ValueError("invalid audio capture baseline")
    elapsed = snapshot["followup"]["cycles"] - snapshot["handoff"]["cycles"]
    if not cycles <= elapsed <= cycles + 24:
        raise ValueError("capture instruction budget disagrees with requested duration")
    previous = 0
    for event in snapshot["apu_writes"]:
        if not isinstance(event, list) or len(event) != 3 or any(type(x) is not int for x in event) or \
                not previous <= event[0] <= elapsed or not 0xFF10 <= event[1] <= 0xFF3F or not 0 <= event[2] <= 255:
            raise ValueError("invalid APU register timeline")
        previous = event[0]
    pcm_path = directory / "followup.pcm"
    return snapshot, read_pcm(pcm_path, snapshot), fingerprint(pcm_path)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--probe", type=Path, required=True)
    parser.add_argument("--reference-boot", type=Path, required=True)
    parser.add_argument("--rom", type=Path, action="append", required=True)
    parser.add_argument("--seconds", type=int, default=30)
    parser.add_argument("--press-start-second", type=int, action="append", default=[])
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--capture-directory", type=Path,
                        help="optional fresh directory for private PCM/WAV audition; never commit these captures")
    args = parser.parse_args()
    try:
        if not 1 <= args.seconds <= 120 or any(not 0 <= n < args.seconds for n in args.press_start_second):
            raise ValueError("duration must be 1..120 seconds and inputs inside the capture")
        if args.output.exists():
            raise ValueError("output already exists")
        if args.capture_directory and args.capture_directory.exists():
            raise ValueError("capture directory already exists")
        report = {"format": "gbb-dmg-boot-audio-v1", "probe": fingerprint(args.probe),
                  "analysis_tool": fingerprint(Path(__file__)),
                  "reference_boot": fingerprint(args.reference_boot), "sample_rate": RATE,
                  "seconds": args.seconds, "start_press_seconds": args.press_start_second,
                  "thresholds": THRESHOLDS, "titles": [],
                  "limitations": ["Same-core comparison, not independent hardware validation",
                                  "No battery saves; deterministic cold baseline unchanged",
                                  "Phase-tolerant metrics do not prove sample-exact equivalence",
                                  "50 ms windows bound onset/gap resolution; shorter defects may escape",
                                  "Register commands validate programmed pitch, not each audible voice",
                                  "PCM and wave RAM are private local data, never report contents"]}
        if args.capture_directory:
            args.capture_directory.mkdir()
            manager = nullcontext(str(args.capture_directory))
        else:
            manager = tempfile.TemporaryDirectory(prefix="gbb-dmg-audio-")
        with manager as temporary:
            for index, rom in enumerate(args.rom):
                identity = fingerprint(rom)
                root = Path(temporary)
                a, apcm, aid = capture(args.probe, rom, None, args.seconds * CLOCK,
                                       [n * CLOCK for n in args.press_start_second], root / f"{index}-replacement")
                b, bpcm, bid = capture(args.probe, rom, args.reference_boot, args.seconds * CLOCK,
                                       [n * CLOCK for n in args.press_start_second], root / f"{index}-reference")
                result = compare_audio(apcm, bpcm, a["apu_writes"], b["apu_writes"])
                boot = compare_boot(a, b)
                if boot["stable_contract"] != "pass":
                    result["failures"].append("boot_contract")
                    result["status"] = "fail"
                result.update({"cartridge": identity, "pcm": {"replacement": aid, "reference": bid},
                               "event_counts": {"replacement": len(a["apu_writes"]), "reference": len(b["apu_writes"])},
                               "start_input_events": {"replacement": a["start_input_events"], "reference": b["start_input_events"]},
                               "stable_boot_contract": boot["stable_contract"]})
                if args.capture_directory:
                    for kind, metadata in (("replacement", aid), ("reference", bid)):
                        directory = root / f"{index}-{kind}"
                        with (directory / "followup.wav").open("xb") as stream:
                            with wave.open(stream, "wb") as wav:
                                wav.setnchannels(2)
                                wav.setsampwidth(2)
                                wav.setframerate(RATE)
                                wav.writeframes((directory / "followup.pcm").read_bytes())
                        with (directory / "provenance.json").open("x", encoding="utf-8") as manifest:
                            json.dump({"format": "gbb-dmg-audio-capture-v1", "cartridge": identity,
                                       "pcm": metadata, "sample_rate": RATE, "boot_kind": kind,
                                       "probe": report["probe"], "reference_boot": report["reference_boot"]},
                                      manifest, indent=2)
                report["titles"].append(result)
                if fingerprint(rom) != identity:
                    raise ValueError("cartridge changed during capture")
        if fingerprint(args.probe) != report["probe"] or fingerprint(args.reference_boot) != report["reference_boot"]:
            raise ValueError("probe or firmware changed during capture")
        if fingerprint(Path(__file__)) != report["analysis_tool"]:
            raise ValueError("analysis tool changed during capture")
        statuses = [t["status"] for t in report["titles"]]
        report["status"] = "fail" if "fail" in statuses else ("incomplete" if "incomplete" in statuses else "pass")
        with args.output.open("x", encoding="utf-8") as output:
            json.dump(report, output, indent=2, allow_nan=False)
            output.write("\n")
        print(f"DMG cartridge audio: {report['status']} ({len(statuses)} titles)")
        return {"pass": 0, "fail": 1, "incomplete": 3}[report["status"]]
    except (ValueError, KeyError, TypeError, OSError, wave.Error, subprocess.TimeoutExpired) as error:
        print(f"DMG audio comparison error: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
