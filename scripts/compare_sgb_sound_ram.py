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
                "address": int(row["address"]), "value": int(row["value"]),
                "clocked": int(row["spc_cycle"]) > 0,
                "cycle": int(row["spc_cycle"])}
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
                  "address": row["address"], "value": row["value"],
                  "phase": row.get("dsp_phase"),
                  "clock64": row.get("dsp_clock64")}
                 for row in events]
    for stream in (gbb, reference):
        for row in stream:
            if row["kind"] not in ("P", "D", "R", "H", "V", "Q") or \
                    (row["kind"] in ("D", "R", "V", "Q") and
                     (type(row["sample"]) is not int or row["sample"] < 0)) or \
                    not 0 <= row["address"] <= (65535 if row["kind"] == "R" else
                                                   24 if row["kind"] == "V" else
                                                   63 if row["kind"] == "Q" else 127) or \
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
            reference_ram: Path | None = None,
            require_cycle_checkpoints: bool = False,
            equal_native_checkpoints: bool = False) -> str:
    if (gbb_ram is None) != (reference_ram is None):
        raise ValueError("both APU RAM snapshots are required")
    gbb, reference = read_traces(gbb_path, reference_timeline)
    native_capture = json.loads(reference_timeline.read_text()).get("native_cycle_checkpoints") is True
    if equal_native_checkpoints and not native_capture:
        raise ValueError("equal-native comparison requires explicitly captured native checkpoints")
    if native_capture and not equal_native_checkpoints:
        raise ValueError("native checkpoint capture requires equal-native comparison, not wall-time labels")
    if equal_native_checkpoints:
        require_cycle_checkpoints = True
    gbb_anchor, gbb_value = keyon(gbb)
    ref_anchor, ref_value = keyon(reference)
    if gbb_value != ref_value:
        raise ValueError("first nonzero KON values differ")
    key_phases = [row for row in gbb if row["kind"] == "Q"]
    gbb_keys = [row for row in gbb if row["kind"] == "D" and
                row["address"] == 0x4c and row["value"] != 0]
    ref_keys = [row for row in reference if row["kind"] == "D" and
                row["address"] == 0x4c and row["value"] != 0]
    ours = [row for row in gbb if row["kind"] == "R"]
    theirs = [row for row in reference if row["kind"] == "R"]
    if not ours or not theirs:
        raise ValueError("both traces need RAM writes in the comparison window")
    lines = [f"First KON ${gbb_value:02x}: GBB PCM sample {gbb_anchor}; "
             f"reference native DSP sample {ref_anchor}.",
             f"Accepted SPC RAM writes at +0.70..+0.85s: "
             f"GBB {len(ours)}, reference {len(theirs)}. "
             "DSP echo writeback is not included."]
    if len(key_phases) >= 2 and len(ref_keys) >= 2 and \
            all(type(row["clock64"]) is int and 0 <= row["clock64"] < 64
                for row in ref_keys[:2]):
        for index in range(2):
            if len(gbb_keys) <= index or \
                    (key_phases[index]["sample"], key_phases[index]["value"]) != \
                    (gbb_keys[index]["sample"], gbb_keys[index]["value"]):
                raise ValueError("key-write phase event does not match KON write")
            ours_phase = key_phases[index]["address"]
            ref_phase = ref_keys[index]["clock64"]
            lines.append(f"KON {index+1} write clock in 64-cycle key-poll period: "
                         f"GBB={ours_phase}, reference={ref_phase}; "
                         f"{'same' if ours_phase == ref_phase else 'different'} write phase.")
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
        right = register_image(reference, ref_anchor, 32000 if equal_native_checkpoints else 32040, offset)
        known = left.keys() & right.keys()
        different = [(address, left[address], right[address]) for address in
                     sorted(known) if left[address] != right[address]]
        shown = ", ".join(f"${address:02x}:${a:02x}/${b:02x}"
                          for address, a, b in different[:8]) or "none"
        label = f"+{round(offset*32000)} native outputs" if equal_native_checkpoints else f"+{offset:.2f}s"
        lines.append(f"{label} DSP write-derived registers: "
                     f"{len(known)} common, {len(different)} differ; "
                     f"first differences (GBB/reference): {shown}.")
    lines.append("Register images exclude unknown pre-packet state and internal "
                 "voice/echo pipeline state. Different RAM writes do not by "
                 "themselves identify a synthesizer bug.")
    for checkpoint in (0, 1, 2):
        snapshots = []
        elapsed = []
        for stream_index, stream in enumerate((gbb, reference)):
            rows = [row for row in stream if row["kind"] == "V"]
            if len(rows) != 75:
                raise ValueError("expected three complete 25-field DSP state snapshots")
            selected = rows[checkpoint * 25:(checkpoint + 1) * 25]
            if [row["address"] for row in selected] != list(range(25)) or \
                    len({row["sample"] for row in selected}) != 1:
                raise ValueError("malformed DSP state snapshot")
            if equal_native_checkpoints and checkpoint < 2 and \
                    selected[0]["sample"] - keyon(stream)[0] != (24000, 26240)[checkpoint]:
                raise ValueError("native checkpoint is not at the exact requested output count")
            snapshots.append([row["value"] for row in selected])
            if require_cycle_checkpoints and (
                    (stream_index == 0 and not all(row["clocked"] for row in selected)) or
                    (stream_index == 1 and not all(row["phase"] == 28 for row in selected))):
                raise ValueError("cycle comparison requires checkpoints immediately after DSP phase 27")
            if checkpoint == 2:
                keys = [row for row in stream if row["kind"] == "D" and
                        row["address"] == 0x4c and row["value"] != 0]
                if len(keys) < 2:
                    raise ValueError("second nonzero KON is missing")
                if stream_index == 0:
                    second_mask = keys[1]["value"]
                elif second_mask != keys[1]["value"]:
                    raise ValueError("second KON values differ")
                elapsed.append(selected[0]["sample"] - keys[1]["sample"])
        if checkpoint == 2 and elapsed != [640, 640]:
            raise ValueError(f"second-KON checkpoints need exactly 640 native samples on both sides; got {elapsed}")
        left, right = snapshots
        changed_env = [voice for voice in range(8)
                       if left[voice * 3] != right[voice * 3]]
        label = (("first KON +24000 native outputs", "first KON +26240 native outputs")
                 if equal_native_checkpoints else ("first KON +0.75s", "first KON +0.82s")) + (
                 "second KON +640 native samples",)
        label = label[checkpoint]
        if checkpoint == 2:
            lines.append("Second-KON elapsed outputs: GBB=640, reference=640. "
                         "Equal output counts do not establish equal write/poll phase.")
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
    parser.add_argument("--require-cycle-checkpoints", action="store_true")
    parser.add_argument("--equal-native-checkpoints", action="store_true")
    args = parser.parse_args()
    try:
        print(compare(args.gbb_events, args.reference_timeline,
                      args.gbb_ram, args.reference_ram, args.require_cycle_checkpoints,
                      args.equal_native_checkpoints))
        return 0
    except (OSError, ValueError, KeyError, TypeError) as error:
        print(f"sound RAM comparison failed: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
