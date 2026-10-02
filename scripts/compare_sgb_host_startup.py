#!/usr/bin/env python3
"""Locate instruction/state and clock divergence between bounded startup probes."""
import argparse
import json
from pathlib import Path


def validate(data, source):
    if not isinstance(data, dict) or data.get("format") != "gbb-sgb-host-startup-v1" or data.get("source") != source:
        raise ValueError("wrong host startup format or source")
    if type(data.get("master_hz")) is not int or not 0 < data["master_hz"] < 100000000:
        raise ValueError("invalid host clock frequency")
    rows = data.get("instructions")
    if not isinstance(rows, list) or not 0 < len(rows) <= 131072:
        raise ValueError("empty or oversized host instruction trace")
    limits = (1 << 64, 1 << 24, 65536, 65536, 65536, 65536, 65536, 256, 256, 262, 1364)
    for row in rows:
        if not isinstance(row, list) or len(row) != 11 or any(
                type(x) is not int or not 0 <= x < limit for x, limit in zip(row, limits)):
            raise ValueError("invalid instruction state")
    if any(b[0] <= a[0] for a, b in zip(rows, rows[1:])):
        raise ValueError("non-increasing instruction clock")
    for field in ("ppu_dma_timing", "host_bus_timing"):
        if field in data and type(data[field]) is not bool:
            raise ValueError("invalid host timing mode")
    if "apu_half_hz" in data and (type(data["apu_half_hz"]) is not int or
                                  not 2000000 <= data["apu_half_hz"] <= 2200000):
        raise ValueError("invalid APU oscillator profile")
    dma = data.get("dma_requests", [])
    if not isinstance(dma, list) or len(dma) > 128:
        raise ValueError("invalid startup DMA requests")
    for row in dma:
        if not isinstance(row, list) or len(row) != 7 or any(
                type(x) is not int or not 0 <= x < limit for x, limit in
                zip(row, (1 << 64, 256, 8, 256, 256, 1 << 24, 65536))):
            raise ValueError("invalid DMA request")
    return rows


def compare(gbb, reference):
    a, b = validate(gbb, "gbb"), validate(reference, "reference")
    common = min(len(a), len(b))
    pc_prefix = next((i for i in range(common) if a[i][1] != b[i][1]), common)
    state_first = next((i for i in range(pc_prefix) if a[i][1:9] != b[i][1:9]), None)
    intervals = []
    for i in range(max(0, pc_prefix - 1)):
        x, y = a[i + 1][0] - a[i][0], b[i + 1][0] - b[i][0]
        intervals.append({"ordinal_zero_based": i, "pc": a[i][1],
                          "gbb_master_clocks": x, "reference_master_clocks": y,
                          "reference_minus_gbb_seconds": y / reference["master_hz"] - x / gbb["master_hz"]})
    return {"format": "gbb-sgb-host-startup-comparison-v1",
            "instruction_counts": [len(a), len(b)], "matching_pc_prefix_length": pc_prefix,
            "first_pc_mismatch": None if pc_prefix == common else {
                "ordinal_zero_based": pc_prefix, "gbb": a[pc_prefix], "reference": b[pc_prefix]},
            "first_register_state_mismatch": None if state_first is None else {
                "ordinal_zero_based": state_first, "gbb": a[state_first], "reference": b[state_first]},
            "largest_instruction_interval_deltas": sorted(intervals,
                key=lambda row: abs(row["reference_minus_gbb_seconds"]), reverse=True)[:12],
            "gbb_ppu_dma_timing": gbb.get("ppu_dma_timing", False),
            "gbb_host_bus_timing": gbb.get("host_bus_timing", False),
            "gbb_apu_half_hz": gbb.get("apu_half_hz"),
            "gbb_dma_requests": gbb.get("dma_requests", []),
            "caution": "PC equality is not register or beam equality. Only the common PC prefix is compared; "
                       "clock observations do not establish hardware accuracy or complete PPU DMA data semantics."}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--gbb", type=Path, required=True)
    parser.add_argument("--reference", type=Path, required=True)
    args = parser.parse_args()
    try:
        result = compare(json.loads(args.gbb.read_text()), json.loads(args.reference.read_text()))
    except (OSError, ValueError, TypeError) as error:
        parser.error(str(error))
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
