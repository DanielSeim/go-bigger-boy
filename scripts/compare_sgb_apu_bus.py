#!/usr/bin/env python3
"""Compare bounded SNES/SPC bus probes without fitting clock offsets or rates."""
import argparse
from collections import Counter
from fractions import Fraction
import json
from pathlib import Path
import sys


def integer(value, maximum, label, minimum=0):
    if type(value) is not int or not minimum <= value <= maximum:
        raise ValueError(f"invalid {label}")
    return value


def load(path, source):
    data = json.loads(path.read_text())
    if not isinstance(data, dict) or data.get("format") not in ("gbb-apu-bus-v1", "gbb-apu-bus-v2") or data.get("source") != source:
        raise ValueError("wrong bus format or source")
    timer_trace = data["format"] == "gbb-apu-bus-v2"
    for key in ("master_hz", "apu_half_hz"):
        integer(data.get(key), 100_000_000, key, 1)
    events = data.get("events")
    if not isinstance(events, list) or not 0 < len(events) < 262144:
        raise ValueError("empty or overflowing bus capture")
    previous = {"master_clock": 0, "spc_half_clock": 0, "pcm_sample": 0}
    for event in events:
        if not isinstance(event, dict) or event.get("kind") not in (
                ("h", "H", "R", "W", "K", "r", "w") if timer_trace else ("h", "H", "R", "W", "K")):
            raise ValueError("invalid bus event kind")
        for key in previous:
            integer(event.get(key), 2**64 - 1, key)
            if event[key] < previous[key]:
                raise ValueError(f"nonmonotonic {key}")
            previous[key] = event[key]
        integer(event.get("value"), 255, "value")
        integer(event.get("address"), 65535, "address")
        integer(event.get("dsp_clock64"), 63, "DSP phase")
        kind, address = event["kind"], event["address"]
        valid = (address < 0xf0 if kind in "rw" else
                 0x2140 <= address <= 0x2143 if kind in "hH" else
                 (0xf4 <= address <= 0xf7 or timer_trace and 0xfd <= address <= 0xff) if kind == "R" else
                 address == 0xf3 if kind == "K" else
                 address in (0xf1, 0xf3) or 0xf4 <= address <= 0xf7 or
                 timer_trace and (address == 0xf0 or 0xfa <= address <= 0xfc))
        if not valid:
            raise ValueError("bus event address inconsistent with kind")
        if kind == "R" and address >= 0xfd and event["value"] > 15:
            raise ValueError("timer counter is not four-bit")
    return data


def summarize(data):
    events = data["events"]
    counts = Counter(e["kind"] for e in events)
    if any(not counts[k] for k in ("H", "h", "R", "K")):
        raise ValueError("capture lacks host accesses, port reads, or key writes")
    reads = [e for e in events if e["kind"] == "R" and 0xf4 <= e["address"] <= 0xf7]
    if not reads:
        raise ValueError("capture lacks SPC input-port reads")
    keys = [e for e in events if e["kind"] == "K" and e["value"]]
    if len(keys) < 2:
        raise ValueError("capture needs two nonzero key writes")
    # Host events contain the scheduler's current SPC snapshot, which may run
    # ahead in the independent coroutine scheduler. This distance is not a
    # hardware command latency. Never subtract unrelated boot-time origins.
    host_index = next((i for i, e in enumerate(events)
                       if e["kind"] == "H" and e["address"] == 0x2140 and e["value"] == 1), None)
    if host_index is None:
        raise ValueError("capture lacks the SOUND command handshake")
    host = events[host_index]
    read = next((e for e in events[host_index+1:]
                 if e["kind"] == "R" and e["address"] == 0xf4 and e["value"] == 1), None)
    if read is None:
        raise ValueError("host command was not observed by SPC")
    halves = read["spc_half_clock"] - host["spc_half_clock"]
    return {"counts": dict(counts), "port_reads": len(reads),
            "midpoint_reads": sum(e["spc_half_clock"] % 2 for e in reads),
            "key_phases": [e["dsp_clock64"] for e in keys[:2]],
            "command_observation_halves": halves,
            "command_observation_spacing_us": float(Fraction(halves * 1_000_000, data["apu_half_hz"])),
            "key_spacing_us": float(Fraction(
                (keys[1]["spc_half_clock"]-keys[0]["spc_half_clock"])*1_000_000,
                data["apu_half_hz"]))}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--gbb", type=Path, required=True)
    parser.add_argument("--reference", type=Path, required=True)
    parser.add_argument("--require-midpoint-reads", action="store_true")
    args = parser.parse_args()
    results = []
    for source, path in (("gbb", args.gbb), ("reference", args.reference)):
        data = load(path, source)
        result = summarize(data)
        results.append(result)
        print(f"{source}: master={data['master_hz']} Hz, SPC half={data['apu_half_hz']} Hz")
        print(json.dumps(result, sort_keys=True))
        if args.require_midpoint_reads and result["midpoint_reads"] != result["port_reads"]:
            raise ValueError(f"{source}: input-port reads are not all sampled at the midpoint")
    print(f"First two KON write phases agree: {results[0]['key_phases'] == results[1]['key_phases']}")
    print("Native rates and boot origins differ. No phase offset was fitted; this is not a hardware-accuracy verdict.")
    print("Host SPC timestamps are scheduler snapshots; command observation spacing is not hardware latency.")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (ValueError, KeyError, OSError) as error:
        print(f"APU bus comparison failed: {error}", file=sys.stderr)
        sys.exit(1)
