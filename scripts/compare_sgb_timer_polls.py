#!/usr/bin/env python3
"""Compare timer polls and bounded driver phase state, anchored at first KON."""
import argparse
import json
from pathlib import Path
import sys

from compare_sgb_apu_bus import load


def observations(data, timer=0xfd, phase=0x43):
    if data["format"] != "gbb-apu-bus-v2":
        raise ValueError("requires explicit timer/driver-state v2 capture")
    events = data["events"]
    key = next((e for e in events if e["kind"] == "K" and e["value"]), None)
    if key is None:
        raise ValueError("capture lacks nonzero KON")
    origin = key["spc_half_clock"]
    polls = []
    for index, event in enumerate(events):
        if event["kind"] != "R" or event["address"] != timer or event["spc_half_clock"] < origin:
            continue
        item = {"half_clocks_after_kon": event["spc_half_clock"] - origin,
                "ticks": event["value"]}
        if event["value"]:
            before = None
            for following in events[index+1:]:
                if following["kind"] == "R" and following["address"] == timer:
                    break
                if following["address"] != phase:
                    continue
                if following["kind"] == "r" and before is None:
                    before = following["value"]
                elif following["kind"] == "w" and before is not None:
                    after = following["value"]
                    delta = (after - before) & 255
                    # Only infer an increment when the bounded observation is
                    # unambiguous; do not assume general firmware semantics.
                    item.update(phase_before=before, phase_after=after,
                                wrapped=after < before,
                                inferred_increment=delta // event["value"]
                                if delta % event["value"] == 0 else None)
                    break
        polls.append(item)
    if not polls:
        raise ValueError("capture lacks timer polls after KON")
    pre = [e for e in events if e["kind"] == "w" and e["address"] == phase
           and e["spc_half_clock"] < origin]
    command_index = next((i for i, e in enumerate(events) if e["kind"] == "H"
                          and e["address"] == 0x2140 and e["value"] == 1), None)
    early = next((e for e in events[:command_index] if e["kind"] == "r"
                  and e["address"] == phase), None) if command_index is not None else None
    return {"polls": polls, "last_pre_kon_phase": pre[-1]["value"] if pre else None,
            "first_pre_command_phase_read": None if early is None else
            {"value": early["value"], "half_clocks_after_kon": early["spc_half_clock"] - origin}}


def compare(gbb, reference):
    sides = [observations(data) for data in (gbb, reference)]
    polls = [side["polls"] for side in sides]
    prefix = 0
    for a, b in zip(*polls):
        if (a["half_clocks_after_kon"], a["ticks"]) != (b["half_clocks_after_kon"], b["ticks"]):
            break
        prefix += 1
    if not prefix:
        raise ValueError("no common timer poll prefix; cannot attribute branch divergence")
    phase_differences = [{"gbb": a, "reference": b}
                         for a, b in zip(polls[0][:prefix], polls[1][:prefix])
                         if "phase_before" in a and "phase_before" in b
                         and (a["phase_before"], a["phase_after"]) !=
                         (b["phase_before"], b["phase_after"])]
    # Once poll times diverge, compare positive observations by ordinal too.
    # These are NOT exact timing matches: preserve each timestamp separately.
    positive = [[item for item in side if item["ticks"]] for side in polls]
    positive_pairs = [{"gbb": a, "reference": b} for a, b in zip(*positive)
                      if "phase_before" in a and "phase_before" in b]
    first_difference = (None if prefix == len(polls[0]) == len(polls[1]) else
                        {"gbb": polls[0][prefix] if prefix < len(polls[0]) else None,
                         "reference": polls[1][prefix] if prefix < len(polls[1]) else None})
    return {"matched_timer_poll_prefix": prefix,
            "phase_address": 0x43,
            "last_pre_kon_phase": [side["last_pre_kon_phase"] for side in sides],
            "first_pre_command_phase_read": [side["first_pre_command_phase_read"] for side in sides],
            "same_poll_different_phase": phase_differences,
            "positive_poll_state_pairs": positive_pairs,
            "first_poll_difference": first_difference,
            "apu_half_hz": [data["apu_half_hz"] for data in (gbb, reference)]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--gbb", type=Path, required=True)
    parser.add_argument("--reference", type=Path, required=True)
    args = parser.parse_args()
    result = compare(load(args.gbb, "gbb"), load(args.reference, "reference"))
    print(json.dumps(result, indent=2))
    print("Relative native half clocks, not fitted wall time. Host snapshots are not command latency.")
    print("A differing inherited phase does not establish its earlier boot-time cause or hardware accuracy.")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (ValueError, KeyError, OSError) as error:
        print(f"Timer comparison failed: {error}", file=sys.stderr)
        sys.exit(1)
