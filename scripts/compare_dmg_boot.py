#!/usr/bin/env python3
"""Execution-only DMG boot comparison; reports never contain ROM/VRAM bytes.

The original firmware is an opaque local input, not a build dependency or a
hardware oracle: it runs on the same GBB core and can expose core inaccuracies.
Exit 0 means the stable ABI matched, NOT cycle-exact firmware equivalence.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys

APU_CLOCK_NAMES = (
    "sequencer_step", "skip_sequencer_event", "sample_accumulator",
    "pulse1_timer", "pulse1_duty_step", "pulse1_length", "pulse1_volume",
    "pulse1_envelope_timer", "pulse2_timer", "pulse2_duty_step", "pulse2_length",
    "pulse2_volume", "pulse2_envelope_timer", "wave_timer", "wave_position",
    "wave_length", "noise_timer", "noise_lfsr",
)
RAM_BASES = {"vram": 0x8000, "wram": 0xC000, "oam": 0xFE00, "hram": 0xFF80}
# Only hardware-defined, stable registers. DIV and undefined object palettes
# remain reported separately. Wave RAM is not a hardware startup contract.
STABLE_IO = tuple(range(0x00, 0x04)) + tuple(range(0x05, 0x30)) + (
    0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x47, 0x4A, 0x4B, 0x50)


def fingerprint(path: Path) -> dict:
    return {"name": path.name, "size": path.stat().st_size,
            "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}


def mismatch_ranges(left, right, base):
    """Inclusive address ranges, with no copied RAM or firmware contents."""
    if len(left) != len(right):
        raise ValueError("snapshot memory lengths differ")
    ranges = []
    for i, (a, b) in enumerate(zip(left, right)):
        if a == b:
            continue
        address = base + i
        if ranges and ranges[-1][1] + 1 == address:
            ranges[-1][1] = address
        else:
            ranges.append([address, address])
    return [{"first": f"{a:04X}", "last": f"{b:04X}", "bytes": b - a + 1}
            for a, b in ranges]


def compare(candidate: dict, reference: dict) -> dict:
    for snapshot in (candidate, reference):
        if snapshot.get("schema") != 1:
            raise ValueError("unsupported boot snapshot schema")
        for stage in ("handoff", "followup"):
            state = snapshot.get(stage, {})
            for key, size in (("io", 128), ("vram", 8192), ("wram", 8192),
                              ("oam", 160), ("hram", 127), ("apu_clocks", 18)):
                if len(state.get(key, ())) != size:
                    raise ValueError(f"invalid {stage} {key} snapshot length")
            if set(state.get("cpu", {})) != {
                    "a", "f", "b", "c", "d", "e", "h", "l", "sp", "pc",
                    "ime", "halted", "stopped"}:
                raise ValueError("invalid CPU snapshot fields")
    a, b = candidate["handoff"], reference["handoff"]
    cpu = {name: {"replacement": value, "reference": b["cpu"][name]}
           for name, value in a["cpu"].items() if value != b["cpu"][name]}
    io = [{"address": f"FF{i:02X}", "replacement": x, "reference": y,
           "stable_contract": i in STABLE_IO}
          for i, (x, y) in enumerate(zip(a["io"], b["io"])) if x != y]
    ram = {}
    for name, base in RAM_BASES.items():
        ranges = mismatch_ranges(a[name], b[name], base)
        ram[name] = {"different_bytes": sum(r["bytes"] for r in ranges),
                     "ranges": ranges,
                     "replacement_sha256": hashlib.sha256(bytes(a[name])).hexdigest(),
                     "reference_sha256": hashlib.sha256(bytes(b[name])).hexdigest()}
    stable = not cpu and not any(x["stable_contract"] for x in io)
    stable = stable and a["ie"] == b["ie"] and not ram["wram"]["different_bytes"]
    stable = stable and not ram["oam"]["different_bytes"]
    # Two stack frames differ; all other HRAM must retain the cold baseline.
    stable = stable and a["hram"][:0x7A] == b["hram"][:0x7A]
    stable = stable and a["hram"][0x7E:] == b["hram"][0x7E:]
    phase = {key: {"replacement": a[key], "reference": b[key]}
             for key in ("cycles", "divider_counter", "ppu_dot", "ppu_mode",
                         "serial_phase", "serial_bits")}
    phase["apu"] = {key: {"replacement": x, "reference": y}
                    for key, x, y in zip(APU_CLOCK_NAMES, a["apu_clocks"], b["apu_clocks"])}
    # Followup is observational, not an assertion of equal CPU/RAM execution:
    # differing boot duration/graphics/phase can legitimately change a title.
    followup = {}
    for name, snap in (("replacement", candidate), ("reference", reference)):
        state = snap["followup"]
        followup[name] = {"executed_cycles": state["cycles"] - snap["handoff"]["cycles"],
                          "cpu": state["cpu"],
                          "framebuffer_fnv64": state["framebuffer_fnv64"],
                          "lcd": {"ly": state["io"][0x44], "stat": state["io"][0x41],
                                  "dot": state["ppu_dot"], "mode": state["ppu_mode"]},
                          "ram_sha256": {key: hashlib.sha256(bytes(state[key])).hexdigest()
                                         for key in RAM_BASES}}
    equivalent = candidate == reference
    return {"stable_contract": "pass" if stable else "fail",
            "exact_snapshot_equivalence": equivalent, "cpu_mismatches": cpu,
            "followup_framebuffer_hash_match":
                candidate["followup"]["framebuffer_fnv64"] == reference["followup"]["framebuffer_fnv64"],
            "io_mismatches": io, "ie": {"replacement": a["ie"], "reference": b["ie"]},
            "ram": ram, "phase": phase, "followup": followup,
            "audio": {"replacement": candidate.get("audio"),
                      "reference": reference.get("audio")},
            "cold_clock_cycles": {"replacement": candidate.get("cold_clock_cycles", 0),
                                  "reference": reference.get("cold_clock_cycles", 0)}}


def run_probe(probe, rom, boot, max_cycles, run_cycles, align_frame=False):
    command = [str(probe), str(rom), "--max-cycles", str(max_cycles),
               "--run-cycles", str(run_cycles)]
    if boot is not None:
        command.extend(("--boot-rom", str(boot)))
    if align_frame:
        command.append("--align-frame")
    result = subprocess.run(command, capture_output=True, text=True, timeout=120)
    if result.returncode:
        raise ValueError(f"probe failed for {rom.name}: {result.stderr.strip()}")
    snapshot = json.loads(result.stdout)
    if snapshot.get("schema") != 1:
        raise ValueError("unsupported boot snapshot schema")
    return snapshot


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--probe", type=Path, required=True)
    parser.add_argument("--reference-boot", type=Path, required=True)
    parser.add_argument("--rom", type=Path, action="append", required=True)
    parser.add_argument("--max-cycles", type=int, default=40_000_000)
    parser.add_argument("--run-cycles", type=int, default=2_000_000)
    parser.add_argument("--align-frame", action="store_true",
                        help="advance followup to the next completed LCD frame (handoff unaffected)")
    parser.add_argument("--output", type=Path, help="new JSON report, refuses overwrite")
    args = parser.parse_args()
    try:
        if not 0 < args.max_cycles <= 1_000_000_000 or not 0 <= args.run_cycles <= 1_000_000_000:
            raise ValueError("invalid cycle budget")
        if args.reference_boot.stat().st_size != 256:
            raise ValueError("reference boot ROM must be exactly 256 bytes")
        if args.output and args.output.exists():
            raise ValueError("output already exists")
        report = {"schema": 1, "reference_boot": fingerprint(args.reference_boot),
                  "probe": fingerprint(args.probe), "max_cycles": args.max_cycles,
                  "run_cycles": args.run_cycles, "titles": [],
                  "align_frame": args.align_frame,
                  "limitations": ["Same-core comparison, not an independent hardware oracle",
                                  "Zero-filled RAM; no adjacent battery save imported",
                                  "Stable ABI pass does not mean cycle or audio equivalence",
                                  "VRAM logo, startup sound, duration and phase deliberately differ",
                                  "Object palettes and wave RAM have undefined hardware power-on contents"]}
        for rom in args.rom:
            cartridge_id = fingerprint(rom)
            replacement = run_probe(args.probe, rom, None, args.max_cycles, args.run_cycles,
                                    args.align_frame)
            reference = run_probe(args.probe, rom, args.reference_boot,
                                  args.max_cycles, args.run_cycles, args.align_frame)
            if fingerprint(rom) != cartridge_id or fingerprint(args.reference_boot) != report["reference_boot"]:
                raise ValueError("input changed during the comparison")
            report["titles"].append({"cartridge": cartridge_id,
                                     **compare(replacement, reference)})
        report["status"] = "pass" if all(t["stable_contract"] == "pass"
                                         for t in report["titles"]) else "fail"
        encoded = json.dumps(report, indent=2) + "\n"
        if args.output:
            with args.output.open("x", encoding="utf-8") as stream:
                stream.write(encoded)
        else:
            print(encoded, end="")
        return 0 if report["status"] == "pass" else 1
    except (ValueError, KeyError, TypeError, OSError, subprocess.TimeoutExpired) as error:
        print(f"DMG boot comparison error: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
