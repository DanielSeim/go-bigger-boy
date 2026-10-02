#!/usr/bin/env python3
"""Compare SGB SPC RAM writes and DSP register state near the first audio split.

This is a development-only emulator-to-emulator diagnostic. The RAM observer
records accepted SPC writes, not DSP echo writeback, DMA, or every RAM read.
"""

import argparse
from collections import Counter
import csv
import hashlib
import json
from pathlib import Path
import sys


def read_traces(gbb_path: Path, timeline_path: Path):
    with gbb_path.open(newline="", encoding="utf-8") as source:
        reader = csv.DictReader(source)
        if reader.fieldnames != ["kind", "master_clock", "spc_cycle",
                                 "pcm_sample", "address", "value"]:
            raise ValueError("invalid GBB trace header")
        gbb = [{"kind": row["kind"], "sample": int(row["pcm_sample"]),
                "address": int(row["address"]), "value": int(row["value"])}
               for row in reader]
    timeline = json.loads(timeline_path.read_text(encoding="utf-8"))
    if not isinstance(timeline, dict) or timeline.get("format") != \
            "gbb-libretro-audio-timeline-v1" or \
            timeline.get("audio_source") != "snes-only":
        raise ValueError("expected SNES-only reference timeline")
    events = timeline.get("post_audible_sound_writes")
    if not isinstance(events, list):
        raise ValueError("reference write trace is absent")
    reference = [{"kind": {"dsp": "D", "ram": "R", "host": "H",
                           "state": "V"}[row["kind"]],
                  "sample": row.get("dsp_sample"),
                  "address": row["address"], "value": row["value"]}
                 for row in events]
    for stream in (gbb, reference):
        for row in stream:
            if row["kind"] not in ("P", "D", "R", "H", "V") or \
                    (row["kind"] in ("D", "R", "V") and
                     (type(row["sample"]) is not int or row["sample"] < 0)) or \
                    not 0 <= row["address"] <= (65535 if row["kind"] == "R" else
                                                   24 if row["kind"] == "V" else 127) or \
                    not 0 <= row["value"] <= (65535 if row["kind"] == "V" else 255):
                raise ValueError("invalid sound trace event")
    return gbb, reference


def keyon(stream):
    rows = [row for row in stream if row["kind"] == "D" and
            row["address"] == 0x4c and row["value"] != 0]
    if not rows:
        raise ValueError("nonzero DSP KON write is missing")
    return rows[0]["sample"], rows[0]["value"]


def register_image(stream, anchor: int, rate: int, offset: float):
    # Only compare registers written after the audible packet in both traces.
    until = anchor + round(offset * rate)
    result = {}
    for row in stream:
        if row["kind"] == "D" and row["sample"] <= until:
            result[row["address"]] = row["value"]
    return result


def compare(gbb_path: Path, reference_timeline: Path,
            gbb_ram: Path | None = None,
            reference_ram: Path | None = None) -> str:
    if (gbb_ram is None) != (reference_ram is None):
        raise ValueError("both APU RAM snapshots are required")
    gbb, reference = read_traces(gbb_path, reference_timeline)
    gbb_anchor, gbb_value = keyon(gbb)
    ref_anchor, ref_value = keyon(reference)
    if gbb_value != ref_value:
        raise ValueError("first nonzero KON values differ")
    ours = [row for row in gbb if row["kind"] == "R"]
    theirs = [row for row in reference if row["kind"] == "R"]
    if not ours or not theirs:
        raise ValueError("both traces need RAM writes in the comparison window")
    lines = [f"First KON ${gbb_value:02x}: GBB PCM sample {gbb_anchor}; "
             f"reference native DSP sample {ref_anchor}.",
             f"Accepted SPC RAM writes at +0.70..+0.85s: "
             f"GBB {len(ours)}, reference {len(theirs)}. "
             "DSP echo writeback is not included."]
    prefix = 0
    for left, right in zip(ours, theirs):
        if (left["address"], left["value"]) != \
                (right["address"], right["value"]):
            break
        prefix += 1
    lines.append(f"RAM address/value ordered prefix: {prefix} writes.")
    if prefix < min(len(ours), len(theirs)):
        left, right = ours[prefix], theirs[prefix]
        lines.append(f"First difference: GBB ${left['address']:04x}=${left['value']:02x} "
                     f"at +{(left['sample']-gbb_anchor)/32000:.6f}s; "
                     f"reference ${right['address']:04x}=${right['value']:02x} "
                     f"at +{(right['sample']-ref_anchor)/32040:.6f}s.")
    if Counter((row["address"], row["value"]) for row in ours) == \
            Counter((row["address"], row["value"]) for row in theirs):
        lines.append("RAM address/value multisets agree; ordering/timing may differ.")
    else:
        lines.append("RAM address/value multisets differ.")
    for name, stream in (("GBB", ours), ("reference", theirs)):
        sample_writes = sum(0x4db0 <= row["address"] < 0xeef0
                            for row in stream)
        lines.append(f"{name} writes into uploaded sample range "
                     f"$4db0..$eeef in window: {sample_writes}.")
    high_ours = Counter((row["address"], row["value"]) for row in ours
                        if row["address"] >= 0x100)
    high_theirs = Counter((row["address"], row["value"]) for row in theirs
                          if row["address"] >= 0x100)
    lines.append(f"RAM writes at $0100 and above: GBB {sum(high_ours.values())}, "
                 f"reference {sum(high_theirs.values())}; "
                 f"address/value multisets {'agree' if high_ours == high_theirs else 'differ'}.")
    for offset in (0.70, 0.75, 0.80, 0.85):
        left = register_image(gbb, gbb_anchor, 32000, offset)
        right = register_image(reference, ref_anchor, 32040, offset)
        known = left.keys() & right.keys()
        different = [(address, left[address], right[address]) for address in
                     sorted(known) if left[address] != right[address]]
        shown = ", ".join(f"${address:02x}:${a:02x}/${b:02x}"
                          for address, a, b in different[:8]) or "none"
        lines.append(f"+{offset:.2f}s DSP write-derived registers: "
                     f"{len(known)} common, {len(different)} differ; "
                     f"first differences (GBB/reference): {shown}.")
    lines.append("Register images exclude unknown pre-packet state and internal "
                 "voice/echo pipeline state. Different RAM writes do not by "
                 "themselves identify a synthesizer bug.")
    for checkpoint in (0, 1, 2):
        snapshots = []
        for stream in (gbb, reference):
            rows = [row for row in stream if row["kind"] == "V"]
            if len(rows) != 75:
                raise ValueError("expected three complete 25-field DSP state snapshots")
            selected = rows[checkpoint * 25:(checkpoint + 1) * 25]
            if [row["address"] for row in selected] != list(range(25)) or \
                    len({row["sample"] for row in selected}) != 1:
                raise ValueError("malformed DSP state snapshot")
            snapshots.append([row["value"] for row in selected])
        left, right = snapshots
        changed_env = [voice for voice in range(8)
                       if left[voice * 3] != right[voice * 3]]
        label = ("first KON +0.75s", "first KON +0.82s",
                 "second KON +0.02s")[checkpoint]
        lines.append(f"Internal DSP checkpoint {label}: "
                     f"envelope differs on voices {changed_env or 'none'}; "
                     f"echo offsets GBB={left[24]} reference={right[24]}.")
        changed_brr = [voice for voice in range(8)
                       if left[voice * 3 + 1] != right[voice * 3 + 1]]
        changed_phase = [voice for voice in range(8)
                         if left[voice * 3 + 2] != right[voice * 3 + 2]]
        active = [voice for voice in range(8)
                  if left[voice * 3] != 0 or right[voice * 3] != 0]
        lines.append(f"  BRR addresses differ on voices {changed_brr or 'none'}; "
                     f"interpolation positions differ on voices "
                     f"{changed_phase or 'none'}; "
                     f"active voices {active or 'none'}.")
        for voice in list(dict.fromkeys(active + changed_env +
                                        changed_brr + changed_phase))[:4]:
            lines.append(f"  voice {voice}: ENV {left[voice*3]}/{right[voice*3]}, "
                         f"BRR address ${left[voice*3+1]:04x}/"
                         f"${right[voice*3+1]:04x}, interpolation position "
                         f"{left[voice*3+2]}/{right[voice*3+2]} "
                         f"(difference {left[voice*3+2]-right[voice*3+2]:+d}).")
    if gbb_ram is not None:
        ours_ram = gbb_ram.read_bytes()
        theirs_ram = reference_ram.read_bytes()
        timeline = json.loads(reference_timeline.read_text(encoding="utf-8"))
        meta = timeline.get("apu_ram")
        if len(ours_ram) != 65536 or len(theirs_ram) != 65536 or \
                not isinstance(meta, dict) or meta.get("size") != 65536 or \
                meta.get("sha256") != hashlib.sha256(theirs_ram).hexdigest():
            raise ValueError("APU RAM snapshots are invalid or not bound to timeline")
        # This is the pinned title's large sample upload destination. It is
        # below the $ef00 echo buffer start seen in this replay.
        first, last = 0x4db0, 0xeef0
        differences = [address for address in range(first, last)
                       if ours_ram[address] != theirs_ram[address]]
        lines.append(f"Final uploaded sample RAM ${first:04x}..${last-1:04x}: "
                     f"{len(differences)} differing bytes of {last-first}; "
                     + (f"first at ${differences[0]:04x} "
                        f"(${ours_ram[differences[0]]:02x}/"
                        f"${theirs_ram[differences[0]]:02x})."
                        if differences else "byte-identical."))
        lines.append("Snapshots were taken at different run endpoints; equality "
                     "in the pinned upload range does not establish equality "
                     "of transient DSP/echo RAM at the audio split.")
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--gbb-events", required=True, type=Path)
    parser.add_argument("--reference-timeline", required=True, type=Path)
    parser.add_argument("--gbb-ram", type=Path)
    parser.add_argument("--reference-ram", type=Path)
    args = parser.parse_args()
    try:
        print(compare(args.gbb_events, args.reference_timeline,
                      args.gbb_ram, args.reference_ram))
        return 0
    except (OSError, ValueError, KeyError, TypeError) as error:
        print(f"sound RAM comparison failed: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
