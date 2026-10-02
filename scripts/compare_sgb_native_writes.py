#!/usr/bin/env python3
"""Separate accepted DSP register data differences from native-cycle timing."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import sys
import wave

from compare_sgb_sound_ram import read_traces


def native_writes(stream, reference=False):
    writes = [row for row in stream if row["kind"] == "D"]
    key = next((row for row in writes if row["address"] == 0x4c and row["value"]), None)
    if key is None:
        raise ValueError("missing nonzero KON")
    if reference:
        def position(row):
            phase = row.get("clock64")
            if type(phase) is not int or not 0 <= phase < 64:
                raise ValueError("reference write lacks a valid native DSP phase")
            # Output is accepted at phase 27, leaving phase 28. Count the
            # clocks since that boundary, including the 31->0 rollover.
            return row["sample"] * 32 + (phase % 32 - 28) % 32
    else:
        def position(row):
            cycle = row.get("cycle")
            if type(cycle) is not int or cycle <= 0:
                raise ValueError("GBB write lacks an exact SPC cycle")
            return cycle
        phase = next((row for row in stream if row["kind"] == "Q" and
                      row["sample"] == key["sample"] and row["value"] == key["value"]), None)
        if phase is None or phase["address"] != position(key) % 64:
            raise ValueError("GBB write phase does not agree with its SPC clock")
    anchor = position(key)
    result = []
    previous = None
    for row in writes:
        clock = position(row)
        if previous is not None and clock < previous:
            raise ValueError("DSP write clocks reverse")
        previous = clock
        if anchor <= clock < anchor + 27200 * 32:
            result.append((clock - anchor, row["address"], row["value"]))
    return result


def compare(gbb, reference):
    left, right = native_writes(gbb), native_writes(reference, True)
    keys = [next(row for row in stream if row["kind"] == "D" and
                 row["address"] == 0x4c and row["value"]) for stream in (gbb, reference)]
    if keys[0]["value"] != keys[1]["value"]:
        raise ValueError("first KON masks differ; cannot compare unrelated voice starts")
    lines = ["Accepted register writes relative to first KON, in native DSP clocks.",
             f"First KON mask ${keys[0]['value']:02x}; phases GBB={keys[0]['cycle'] % 64}, "
             f"reference={keys[1]['clock64']}. Different phases are not fitted away.",
             "No fitted phase/rate offset. This records acceptance, not per-voice DSP consumption."]
    for address in (0x0c, 0x1c) + tuple(v * 16 + f for v in range(8) for f in (2, 3)):
        a = [(c, v) for c, r, v in left if r == address]
        b = [(c, v) for c, r, v in right if r == address]
        if not a and not b:
            continue
        mismatch = next(((i, x, y) for i, (x, y) in enumerate(zip(a, b)) if x != y), None)
        if mismatch:
            i, x, y = mismatch
            reason = "timing only" if x[1] == y[1] else "different values"
            lines.append(f"${address:02x} first difference at write {i}: "
                         f"GBB cycle {x[0]} value ${x[1]:02x}; reference cycle {y[0]} "
                         f"value ${y[1]:02x}; {reason}; reference-GBB={y[0]-x[0]:+d} clocks.")
        else:
            lines.append(f"${address:02x}: {min(len(a), len(b))} accepted writes match exactly.")
        if len(a) != len(b):
            lines.append(f"${address:02x}: bounded-window counts differ ({len(a)}/{len(b)}); no missing writes are assumed equal.")
    lines.append("Driver/timer/host timing can change accepted writes; do not infer a DSP synthesis bug from timing-only differences.")
    return "\n".join(lines)


def pcm_difference(gbb_path, reference_path, gbb, reference, metadata):
    anchors = [next(row["sample"] for row in stream if row["kind"] == "D" and
                    row["address"] == 0x4c and row["value"])
               for stream in (gbb, reference)]
    windows = []
    for index, (path, anchor, rate) in enumerate(zip(
            (gbb_path, reference_path), anchors, (32000, 32040))):
        with wave.open(str(path), "rb") as source:
            if source.getnchannels() != 2 or source.getsampwidth() != 2 or \
                    source.getframerate() != rate or source.getnframes() < anchor + 4096:
                raise ValueError("native PCM format or comparison window is invalid")
            if index:
                pcm = source.readframes(source.getnframes())
                if not isinstance(metadata, dict) or metadata.get("sample_rate") != rate or \
                        metadata.get("sample_count") != source.getnframes() or \
                        len(pcm) != source.getnframes() * 4 or \
                        metadata.get("pcm_sha256") != hashlib.sha256(pcm).hexdigest():
                    raise ValueError("native reference PCM is not bound to its timeline")
            source.setpos(anchor)
            window = source.readframes(4096)
            if len(window) != 4096 * 4:
                raise ValueError("native PCM comparison window is truncated")
            windows.append(list(struct.iter_unpack("<hh", window)))
    mismatch = next(((i, a, b) for i, (a, b) in enumerate(zip(*windows)) if a != b), None)
    if mismatch is None:
        return "First 4096 post-KON native outputs match exactly; no resampling or phase fitting."
    i, a, b = mismatch
    return (f"First unequal post-KON native PCM buffer index {i} (output {i+1} after KON): "
            f"GBB {a}, reference {b}. No resampling or phase fitting; "
            "different accepted writes can explain output differences.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--gbb-events", type=Path, required=True)
    parser.add_argument("--reference-timeline", type=Path, required=True)
    parser.add_argument("--gbb-pcm", type=Path)
    parser.add_argument("--reference-native-pcm", type=Path)
    args = parser.parse_args()
    try:
        if (args.gbb_pcm is None) != (args.reference_native_pcm is None):
            raise ValueError("both native PCM paths are required")
        streams = read_traces(args.gbb_events, args.reference_timeline)
        print(compare(*streams))
        if args.gbb_pcm:
            metadata = json.loads(args.reference_timeline.read_text()).get("native_dsp")
            print(pcm_difference(args.gbb_pcm, args.reference_native_pcm, *streams, metadata))
        return 0
    except (ValueError, OSError, KeyError, TypeError, wave.Error, struct.error) as error:
        print(f"native write comparison failed: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
