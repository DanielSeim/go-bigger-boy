#!/usr/bin/env python3
"""Compare observed boot landmarks without fitting clocks or hiding input drift."""
import argparse
import json
from pathlib import Path
import sys

KINDS = "IC RAUE TBS PN".replace(" ", "")
FIELDS = ("master_clock_snapshot", "spc_half_clock_snapshot", "value", "count", "digest_fnv64")


def validate(data, source):
    if data.get("format") != "gbb-sgb-boot-timeline-v1" or data.get("source") != source:
        raise ValueError("wrong boot timeline format or source")
    for field in ("master_hz", "apu_half_hz"):
        if type(data.get(field)) is not int or not 0 < data[field] < 100000000:
            raise ValueError("missing or invalid clock frequency")
    events = data.get("events")
    if not isinstance(events, list) or not 0 < len(events) <= 128:
        raise ValueError("empty or oversized boot timeline")
    for e in events:
        if not isinstance(e, dict) or e.get("kind") not in tuple(KINDS):
            raise ValueError("invalid boot event kind")
        if any(type(e.get(f)) is not int or not 0 <= e[f] < 1 << 64 for f in FIELDS):
            raise ValueError("invalid boot event field")
    # Master timestamps may be coroutine snapshots; do not demand joint clock ordering.
    for kind in KINDS:
        rows = [e for e in events if e["kind"] == kind]
        if any(b["master_clock_snapshot"] < a["master_clock_snapshot"] for a, b in zip(rows, rows[1:])):
            raise ValueError("backwards landmark clock")
    return events


def compare(gbb, reference):
    left, right = validate(gbb, "gbb"), validate(reference, "reference")
    landmarks = []
    for kind in KINDS:
        a, b = ([e for e in rows if e["kind"] == kind] for rows in (left, right))
        for ordinal, (x, y) in enumerate(zip(a, b)):
            landmarks.append({"kind": kind, "ordinal_zero_based": ordinal,
                              "gbb": x, "reference": y,
                              "reference_minus_gbb_seconds":
                              y["master_clock_snapshot"] / reference["master_hz"] -
                              x["master_clock_snapshot"] / gbb["master_hz"]})
    uploads = [[(e["value"], e["count"], e["digest_fnv64"]) for e in rows if e["kind"] == "U"]
               for rows in (left, right)]
    inputs = [[(e["count"], e["value"]) for e in rows if e["kind"] == "N"] for rows in (left, right)]
    common = min(map(len, inputs))
    first = next((i for i in range(common) if inputs[0][i] != inputs[1][i]), None)
    sounds = [[e["value"] for e in rows if e["kind"] == "S"] for rows in (left, right)]
    modes = [data.get("input_mode", "unspecified-legacy") for data in (gbb, reference)]
    eligible = (modes == ["gb-lcd-frame-held-v1"] * 2 and
                common > 0 and first is None and len(inputs[0]) == len(inputs[1]) and
                bool(uploads[0]) and uploads[0] == uploads[1] and
                bool(sounds[0]) and sounds[0] == sounds[1])
    return {"format": "gbb-sgb-boot-comparison-v1",
            "input_and_upload_preconditions_met": eligible,
            "input_modes": modes,
            "upload_fingerprints_match": bool(uploads[0]) and uploads[0] == uploads[1],
            "upload_counts": list(map(len, uploads)), "sound_parameters_match": sounds[0] == sounds[1],
            "input_counts": list(map(len, inputs)), "compared_input_prefix_length": common,
            "first_input_mismatch": None if first is None else {
                "ordinal_zero_based": first, "gbb_frame_mask": inputs[0][first],
                "reference_frame_mask": inputs[1][first]},
            "landmarks": landmarks,
            "caution": "Snapshot times and input markers are observations, not hardware latency proof. "
                       "Matching preconditions alone do not establish identical input delivery semantics."}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--gbb", required=True, type=Path)
    parser.add_argument("--reference", required=True, type=Path)
    args = parser.parse_args()
    try:
        result = compare(json.loads(args.gbb.read_text()), json.loads(args.reference.read_text()))
    except (ValueError, OSError, TypeError) as error:
        parser.error(str(error))
    print(json.dumps(result, indent=2))
    return 0 if result["input_and_upload_preconditions_met"] else 2


if __name__ == "__main__":
    sys.exit(main())
