#!/usr/bin/env python3
"""Locate the first pre-command phase-write difference without fitting clocks."""
import argparse
import json
from pathlib import Path
import sys

from compare_sgb_apu_bus import load


def sequence(data):
    if data["format"] != "gbb-apu-bus-v2" or data.get("phase_writes_from_reset") is not True:
        raise ValueError("requires explicitly marked phase writes from reset")
    events = data["events"]
    command = next((i for i, e in enumerate(events) if e["kind"] == "H"
                    and e["address"] == 0x2140 and e["value"] == 1), None)
    if command is None:
        raise ValueError("capture lacks first music command")
    before = events[:command]
    writes = [e for e in before if e["kind"] == "w" and e["address"] == 0x43]
    if len(writes) < 2:
        raise ValueError("capture lacks initialization and pre-command phase writes")
    control = 0
    enables = []
    for event in before:
        if event["kind"] == "W" and event["address"] == 0xf1:
            if event["value"] & 1 and not control & 1:
                enables.append(event["spc_half_clock"])
            control = event["value"]
    if not enables:
        raise ValueError("capture lacks timer-0 enable edge")
    return writes, enables


def context(data, writes, index):
    event = writes[index]
    previous = writes[index-1] if index else None
    result = {"half_clock": event["spc_half_clock"], "phase": event["value"],
              "previous_phase": previous["value"] if previous else None,
              "next_phase": writes[index+1]["value"] if index+1 < len(writes) else None}
    window = data.get("history_window_half_clocks")
    if previous and window and window[0] <= previous["spc_half_clock"] < event["spc_half_clock"] < window[1]:
        result["timer_reads_between_phase_writes"] = [
            {"half_clock": e["spc_half_clock"], "ticks": e["value"]}
            for e in data["events"] if e["kind"] == "R" and e["address"] == 0xfd
            and previous["spc_half_clock"] < e["spc_half_clock"] <= event["spc_half_clock"]]
        earlier = [e for e in data["events"] if e["kind"] == "R" and e["address"] == 0xfd
                   and window[0] <= e["spc_half_clock"] <= previous["spc_half_clock"]]
        current = result["timer_reads_between_phase_writes"]
        if earlier and len(current) == 1:
            result["default_divider_model"] = timer_model(data, earlier[-1]["spc_half_clock"], current[0]["half_clock"])
    else:
        result["timer_reads_between_phase_writes"] = None
    return result


def default_timer_interval(enable_half, target, previous_read_half, read_half):
    """Analytical default timer-0 divider, not a measured hardware phase."""
    # CONTROL enables stage 2 after the write's bus clock. The first stage-1
    # pulse therefore follows that clock even if the write is on a boundary.
    first_output = (enable_half // 256 + (target or 256)) * 256
    period = (target or 256) * 256
    def count(half):
        return max(0, (half-first_output)//period + 1)
    return {"first_output_half_clock": first_output, "period_half_clocks": period,
            "previous_read_half_since_tick": (previous_read_half-first_output) % period,
            "read_half_since_tick": (read_half-first_output) % period,
            "expected_ticks": (count(read_half)-count(previous_read_half)) & 15}


def timer_model(data, previous_half, read_half):
    # Refuse the simple analytical model if TEST changed the divider/gates or
    # the target changed while enabled. Do not silently assume such effects.
    target, enabled, enable_half = None, False, None
    stable = True
    for event in data["events"]:
        if event["spc_half_clock"] > read_half:
            break
        if event["kind"] != "W":
            continue
        if event["address"] == 0xf0:
            return None
        if event["address"] == 0xfa:
            if enabled and target != event["value"]:
                stable = False
            target = event["value"]
        elif event["address"] == 0xf1:
            new_enabled = bool(event["value"] & 1)
            if new_enabled and not enabled:
                enable_half = event["spc_half_clock"]
                stable = True
            enabled = new_enabled
    if not enabled or not stable or target is None or enable_half is None or previous_half < enable_half:
        return None
    return default_timer_interval(enable_half, target, previous_half, read_half)


def compare(gbb, reference):
    sequences = [sequence(data) for data in (gbb, reference)]
    writes = [s[0] for s in sequences]
    prefix = 0
    for a, b in zip(*writes):
        if a["value"] != b["value"]:
            break
        prefix += 1
    differing_value = prefix < min(map(len, writes))
    return {"phase_address": 0x43, "matching_phase_write_values": prefix,
            "pre_command_write_counts": list(map(len, writes)),
            "timer0_enable_half_clocks": [s[1] for s in sequences],
            "first_differing_value": None if not differing_value else
            {"ordinal_zero_based": prefix, "gbb": context(gbb, writes[0], prefix),
             "reference": context(reference, writes[1], prefix)},
            "apu_half_hz": [d["apu_half_hz"] for d in (gbb, reference)],
            "reference_options": reference.get("reference_options", {})}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--gbb", type=Path, required=True)
    parser.add_argument("--reference", type=Path, required=True)
    args = parser.parse_args()
    print(json.dumps(compare(load(args.gbb, "gbb"), load(args.reference, "reference")), indent=2))
    print("Ordinal write values only: no time offset, rate fit, or hardware-accuracy verdict.")
    print("A first transient difference need not be the origin of every later phase difference.")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (ValueError, KeyError, OSError) as error:
        print(f"Phase-origin comparison failed: {error}", file=sys.stderr)
        sys.exit(1)
