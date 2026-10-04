#!/usr/bin/env python3
"""Run local Gambatte ly0 fixtures; never copy their ROMs into reports/repo.

The test-only probe observes A on entry to the source-defined result routine
at 7000. Expected bytes come from the hardware-test filenames. This runner is
limited to lycint152 fixtures, not a general ROM completion detector.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys


def fingerprint(path):
    return {"name": path.name, "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--probe", type=Path, required=True)
    parser.add_argument("--rom-directory", type=Path, required=True)
    parser.add_argument("--model", choices=("dmg", "cgb-c"), default="dmg")
    parser.add_argument("--dmg-boot", action="store_true")
    parser.add_argument("--output", type=Path, help="new report, refuses overwrite")
    args = parser.parse_args()
    try:
        if args.dmg_boot and args.model != "dmg":
            raise ValueError("replacement boot requires DMG")
        if args.output and args.output.exists():
            raise ValueError("output already exists")
        # Execute the exact file we fingerprint, never a same-named PATH tool.
        args.probe = args.probe.resolve(strict=True)
        tag = "dmg08" if args.model == "dmg" else "cgb04c"
        roms = sorted(args.rom_directory.glob(f"lycint152_*{tag}*_out*.gbc"))
        if not roms:
            raise ValueError("no supported ly0 fixtures found")
        probe_id = fingerprint(args.probe)
        tool_id = fingerprint(Path(__file__))
        report = {"format": "gbb-stat-ly0-v1", "probe": probe_id, "analysis_tool": tool_id,
                  "model": args.model, "replacement_boot": args.dmg_boot, "cases": []}
        for rom in roms:
            expected = re.search(r"_out([0-9A-F]{2})$", rom.stem)
            if expected is None:
                raise ValueError(f"unsupported result name: {rom.name}")
            before = fingerprint(rom)
            command = [str(args.probe), str(rom), args.model]
            if args.dmg_boot:
                command.append("--dmg-boot")
            result = subprocess.run(command, capture_output=True, text=True, timeout=60)
            if result.returncode:
                raise ValueError(f"probe failed for {rom.name}: {result.stderr.strip()}")
            snapshot = json.loads(result.stdout)
            if (type(snapshot.get("schema")) is not int or snapshot["schema"] != 1 or
                    type(snapshot.get("result")) is not int or not 0 <= snapshot["result"] <= 255 or
                    type(snapshot.get("cycles")) is not int or not 0 < snapshot["cycles"] <= 20_000_000):
                raise ValueError("invalid probe result")
            if before != fingerprint(rom) or probe_id != fingerprint(args.probe) or tool_id != fingerprint(Path(__file__)):
                raise ValueError("input/tool changed during validation")
            value = int(expected.group(1), 16)
            report["cases"].append({"rom": before, "expected": value, "actual": snapshot["result"],
                                    "status": "pass" if value == snapshot["result"] else "fail"})
        report["passed"] = sum(row["status"] == "pass" for row in report["cases"])
        report["status"] = "pass" if report["passed"] == len(roms) else "fail"
        encoded = json.dumps(report, indent=2) + "\n"
        if args.output:
            with args.output.open("x", encoding="utf-8") as stream:
                stream.write(encoded)
        else:
            print(encoded, end="")
        print(f"STAT ly0 {args.model}: {report['passed']}/{len(roms)} passed", file=sys.stderr)
        return 0 if report["status"] == "pass" else 1
    except (OSError, ValueError, KeyError, TypeError, subprocess.TimeoutExpired) as error:
        print(f"STAT validation error: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
