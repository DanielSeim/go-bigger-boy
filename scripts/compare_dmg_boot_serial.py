#!/usr/bin/env python3
"""Compare post-handoff serial bus diagnostics, without exporting ROM/save bytes.

Reference firmware is an opaque local input, not an independent hardware oracle.
No cartridge code runs during the host-driven serial checks. Exit 0: matching
handoff/serial contracts; 1: mismatch; 2: invalid input or tool failure.
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import subprocess
import sys

from compare_dmg_boot import compare, fingerprint

IDLES = (0, 1, 4, 59, 60, 119, 511, 512, 4095, 8192)


def validate(snapshot):
    """Check every bit against the protocol, not just against another run."""
    if snapshot.get("schema") != 1 or snapshot.get("cold_clock_cycles") != 0:
        raise ValueError("unsupported snapshot or nonzero cold clock offset")
    serial = snapshot["serial_check"]
    if serial.get("schema") != 1:
        raise ValueError("unsupported serial diagnostics schema")
    phase = snapshot["handoff"]["serial_phase"]
    if type(phase) is not int or not 0 <= phase < 512:
        raise ValueError("invalid handoff serial phase")
    failures = []
    handoff = snapshot["handoff"]
    if handoff["serial_bits"] != 0 or handoff["io"][1:3] != [0, 0x7E]:
        failures.append("handoff is not an idle, cleared DMG serial port")
    internal = serial["internal"]
    if len(internal) != len(IDLES) or tuple(row["idle"] for row in internal) != IDLES:
        raise ValueError("missing or duplicate internal serial cases")
    for row in internal:
        expected_phase = (phase + row["idle"]) % 512
        edges = [[512 - expected_phase + bit * 512,
                  ((0xA5 << (bit + 1)) | ((1 << (bit + 1)) - 1)) & 255,
                  0x7F if bit == 7 else 0xFF, 0xE8 if bit == 7 else 0xE0]
                 for bit in range(8)]
        if row["phase"] != expected_phase or row["edges"] != edges:
            failures.append(f"internal edges/interrupt/data at idle={row['idle']}")
        for key in ("restore_exact", "output_once", "abort_clean", "rearm_edge"):
            if row[key] != 1:
                failures.append(f"internal {key} at idle={row['idle']}")
    external = serial["external"]
    edges = [[int(bool(0x3C & (0x80 >> bit))),
              ((0x3C << (bit + 1)) | (0xA5 >> (7 - bit))) & 255,
              0x7E if bit == 7 else 0xFE, 0xE8 if bit == 7 else 0xE0]
             for bit in range(8)]
    if external["edges"] != edges:
        failures.append("external outgoing/incoming bits or interrupt")
    for key in ("waits", "restore_exact", "output_once"):
        if external[key] != 1:
            failures.append(f"external {key}")
    linked = serial["linked"]
    if len(linked) != 4 or [(r["boot_master"], r["late"]) for r in linked] != [
            (1, 0), (1, 4096), (0, 0), (0, 4096)]:
        raise ValueError("missing or duplicate linked cases")
    for row in linked:
        edges = [[(1 if row["late"] else 512) + bit * 512,
                  ((0xA5 << (bit + 1)) | (0x3C >> (7 - bit))) & 255,
                  ((0x3C << (bit + 1)) | (0xA5 >> (7 - bit))) & 255,
                  0x7F if bit == 7 else 0xFF, 0x7E if bit == 7 else 0xFE,
                  0xE8 if bit == 7 else 0xE0, 0xE8 if bit == 7 else 0xE0]
                 for bit in range(8)]
        if row["edges"] != edges or row["held"] != 1 or row["completion_once"] != 1:
            failures.append(f"linked protocol master={row['boot_master']} late={row['late']}")
    return failures


def comparison(candidate, reference):
    failures = {"replacement": validate(candidate), "reference": validate(reference)}
    # Equal-looking broken traces must never pass: each run has its own oracle.
    matching = candidate["serial_check"] == reference["serial_check"]
    stable = compare(candidate, reference)["stable_contract"]
    return {"status": "pass" if matching and stable == "pass" and
            not any(failures.values()) else "fail", "stable_boot_contract": stable,
            "protocol_failures": failures, "serial_checks_match": matching,
            "handoff_phase": {"replacement": candidate["handoff"]["serial_phase"],
                              "reference": reference["handoff"]["serial_phase"]},
            "replacement": candidate["serial_check"], "reference": reference["serial_check"]}


def run(probe, rom, boot, max_cycles):
    command = [str(probe), str(rom), "--max-cycles", str(max_cycles),
               "--run-cycles", "0", "--serial-check"]
    if boot is not None:
        command.extend(("--boot-rom", str(boot)))
    result = subprocess.run(command, capture_output=True, text=True, timeout=120)
    if result.returncode:
        raise ValueError(f"probe failed: {result.stderr.strip()}")
    return json.loads(result.stdout)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--probe", type=Path, required=True)
    parser.add_argument("--reference-boot", type=Path, required=True)
    parser.add_argument("--rom", type=Path, action="append", required=True)
    parser.add_argument("--max-cycles", type=int, default=40_000_000)
    parser.add_argument("--output", type=Path, help="new report, refuses overwrite")
    args = parser.parse_args()
    try:
        if not 0 < args.max_cycles <= 1_000_000_000:
            raise ValueError("invalid cycle budget")
        if args.reference_boot.stat().st_size != 256:
            raise ValueError("reference boot ROM must be exactly 256 bytes")
        if args.output and args.output.exists():
            raise ValueError("output already exists")
        provenance = {"probe": fingerprint(args.probe),
                      "reference_boot": fingerprint(args.reference_boot),
                      "analysis_tool": fingerprint(Path(__file__)),
                      "boot_contract_tool": fingerprint(Path(__file__).with_name("compare_dmg_boot.py"))}
        report = {"format": "gbb-dmg-boot-serial-v1", **provenance,
                  "max_cycles": args.max_cycles, "cold_clock_cycles": 0,
                  "limitations": ["Same-core comparison, not physical hardware equivalence",
                                  "Host bus writes/ticks, not cartridge instruction timings",
                                  "Attached cable retains its existing peer-ready phase policy",
                                  "No battery saves imported or persisted"], "titles": []}
        for rom in args.rom:
            cartridge = fingerprint(rom)
            candidate = run(args.probe, rom, None, args.max_cycles)
            reference = run(args.probe, rom, args.reference_boot, args.max_cycles)
            if fingerprint(rom) != cartridge or provenance != {
                    "probe": fingerprint(args.probe),
                    "reference_boot": fingerprint(args.reference_boot),
                    "analysis_tool": fingerprint(Path(__file__)),
                    "boot_contract_tool": fingerprint(Path(__file__).with_name("compare_dmg_boot.py"))}:
                raise ValueError("input or tool changed during comparison")
            report["titles"].append({"cartridge": cartridge, **comparison(candidate, reference)})
        report["status"] = "pass" if all(t["status"] == "pass" for t in report["titles"]) else "fail"
        encoded = json.dumps(report, indent=2) + "\n"
        if args.output:
            with args.output.open("x", encoding="utf-8") as stream:
                stream.write(encoded)
        else:
            print(encoded, end="")
        print(f"DMG serial contracts: {report['status']} ({len(report['titles'])} titles)", file=sys.stderr)
        return 0 if report["status"] == "pass" else 1
    except (ValueError, KeyError, TypeError, OSError, subprocess.TimeoutExpired) as error:
        print(f"DMG serial comparison error: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
