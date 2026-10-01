#!/usr/bin/env python3
"""Compare bounded post-SOUND host-port and DSP-write traces.

Host timing uses CPU video-counter positions, never the reference DSP-buffer
length: the CPU, SMP, and DSP threads need not be synchronized at a host write.
"""

import argparse
import csv
import json
from pathlib import Path
import sys


def compare(gbb_csv: Path, reference_timeline: Path,
            sound_index: int = 1) -> str:
    with gbb_csv.open(newline="", encoding="utf-8") as source:
        reader = csv.DictReader(source)
        if reader.fieldnames != ["kind", "master_clock", "spc_cycle",
                                 "pcm_sample", "address", "value"]:
            raise ValueError("invalid GBB sound event CSV header")
        gbb = list(reader)
    timeline = json.loads(reference_timeline.read_text(encoding="utf-8"))
    if not isinstance(timeline, dict) or \
            timeline.get("format") != "gbb-libretro-audio-timeline-v1" or \
            timeline.get("audio_source") != "snes-only":
        raise ValueError("expected SNES-only reference timeline")
    packets = timeline.get("sgb_sound_events")
    events = timeline.get("post_audible_sound_writes")
    if not isinstance(packets, list) or not 0 <= sound_index < len(packets) or \
            not isinstance(events, list) or \
            any(not isinstance(row, dict) for row in events):
        raise ValueError("reference packet or post-SOUND writes are missing")
    packet = packets[sound_index]
    if not isinstance(packet, dict) or not isinstance(packet.get("packet"), str) or \
            not packet["packet"].startswith("4100000001") or \
            type(packet.get("run_index")) is not int or \
            type(packet.get("cpu_vcounter")) is not int or \
            type(packet.get("cpu_hcounter")) is not int:
        raise ValueError("expected the title's audible SOUND packet")
    gbb_packets = [row for row in gbb if row["kind"] == "P"]
    gbb_hosts = [row for row in gbb if row["kind"] == "H"]
    gbb_dsp = [row for row in gbb if row["kind"] == "D"]
    ref_hosts = [row for row in events if row.get("kind") == "host"]
    ref_dsp = [row for row in events if row.get("kind") == "dsp"]
    if len(gbb_packets) != 1 or not gbb_hosts or not ref_hosts or \
            not gbb_dsp or not ref_dsp:
        raise ValueError("both traces need a packet, host writes, and DSP writes")
    first_host = ref_hosts[0]
    if first_host.get("run_index") != packet.get("run_index") + 1 or \
            first_host.get("cpu_vcounter") != packet.get("cpu_vcounter") or \
            not all(type(row.get(key)) is int for row, key in (
                (first_host, "cpu_hcounter"), (packet, "cpu_hcounter"))):
        raise ValueError("reference packet-to-host timing exceeds one known video frame")
    # One NTSC field has 262 scanlines. An interlaced field may shorten one
    # line by four clocks, so report this as a 4-clock uncertainty range.
    reference_master = 262 * 1364 + first_host["cpu_hcounter"] - packet["cpu_hcounter"]
    gbb_master = int(gbb_hosts[0]["master_clock"]) - \
        int(gbb_packets[0]["master_clock"])
    if min(reference_master, gbb_master) <= 0:
        raise ValueError("invalid packet-to-host clock ordering")
    host_count = min(len(gbb_hosts), len(ref_hosts))
    for index in range(host_count):
        if (int(gbb_hosts[index]["address"]), int(gbb_hosts[index]["value"])) != \
                (ref_hosts[index]["address"], ref_hosts[index]["value"]):
            host_count = index
            break
    dsp_count = min(len(gbb_dsp), len(ref_dsp))
    first_dsp_difference = None
    for index in range(dsp_count):
        if (int(gbb_dsp[index]["address"]), int(gbb_dsp[index]["value"])) != \
                (ref_dsp[index]["address"], ref_dsp[index]["value"]):
            first_dsp_difference = index
            break
    lines = [f"Packet-to-first-host write: GBB {gbb_master} master clocks; "
             f"reference {reference_master - 4}..{reference_master} clocks "
             "(possible short-field line).",
             f"Host port/value prefix agrees for {host_count} writes "
             f"(GBB {len(gbb_hosts)}, reference {len(ref_hosts)} retained)."]
    if first_dsp_difference is None:
        lines.append(f"DSP register/value prefix agrees for {dsp_count} writes.")
    else:
        index = first_dsp_difference
        ours = gbb_dsp[index]
        theirs = ref_dsp[index]
        lines.append(f"First DSP write-order difference at index {index}: "
                     f"GBB ${int(ours['address']):02x}=${int(ours['value']):02x}, "
                     f"reference ${theirs['address']:02x}=${theirs['value']:02x}. "
                     "This alone does not establish a core bug: event phase and "
                     "periodic polling can change the intervening write count.")
    lines.append("Reference DSP-buffer sample snapshots at host writes are not "
                 "CPU timestamps and are deliberately excluded from this comparison.")
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--gbb-events", required=True, type=Path)
    parser.add_argument("--reference-timeline", required=True, type=Path)
    parser.add_argument("--reference-sound-event-index", type=int, default=1)
    args = parser.parse_args()
    try:
        print(compare(args.gbb_events, args.reference_timeline,
                      args.reference_sound_event_index))
        return 0
    except (OSError, ValueError, KeyError, TypeError) as error:
        print(f"sound trace comparison failed: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
