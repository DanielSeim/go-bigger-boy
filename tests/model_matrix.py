#!/usr/bin/env python3
"""Run deterministic ROM checks for every selectable Game Boy model.

The runner's exit status is deliberately converted into a four-state report:
PASS, EXPECTED_FAIL (the ROM is not defined for that hardware), KNOWN_FAIL
(reviewed emulator limitation), or REGRESSION (an unexplained failure).
"""
from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor
import json
import os
import re
import subprocess
from pathlib import Path
from typing import Dict, List, Optional, Set, Tuple

MODELS = ("dmg0", "dmg", "mgb", "sgb", "sgb2", "cgb0", "cgb-c", "cgb-e")
ALL_MODELS = set(MODELS)
DEFERRED_SUITES = {
    "age-test-roms": "screenshot-driven AGE cases need a visual harness",
    "same-suite": "interactive and revision-specific diagnostics need a suite harness",
}


def expected_models(suite: str, relative: str) -> Optional[Set[str]]:
    """Return models covered by upstream's hardware naming convention."""
    name = relative.lower().replace("\\", "/")
    if suite == "gbmicrotest":
        # GBMicrotest v7.0 documents DMG-CPU-08 (DMG CPU B/C) as its
        # reference target.  `dmg` is the corresponding selectable profile;
        # DMG-0 and MGB are deliberately reported as EXPECTED_FAIL.
        return {"dmg"}
    if suite == "mooneye-wilbertpol":
        # Mooneye's documented group suffixes are authoritative: G is
        # DMG/MGB, S is SGB/SGB2, C is the CGB family, and GS combines G+S.
        # A bare `-C` suffix is the CGB group (not the CGB-C revision).
        if re.search(r"-dmg0(?:[-_.]|$)", name):
            return {"dmg0"}
        if "dmgabcmgb" in name:
            return {"dmg", "mgb"}
        if re.search(r"-dmgabc(?:[-_.]|$)", name):
            return {"dmg"}
        if re.search(r"-cgb0(?:[-_.]|$)", name):
            return {"cgb0"}
        if re.search(r"-cgb(?:[-_.]|$)", name):
            return {"cgb0", "cgb-c", "cgb-e"}
        if re.search(r"-a(?:[-_.]|$)", name):
            # A is the AGB/AGS group, which is outside GBB's model set.
            return set()
        if re.search(r"(?:^|[/_.-])cgb(?:[/_.-]|$)", name):
            return {"cgb0", "cgb-c", "cgb-e"}
        if re.search(r"-c(?:[-_.]|$)", name):
            return {"cgb0", "cgb-c", "cgb-e"}
        if "sgb2" in name:
            return {"sgb2"}
        if re.search(r"(?:^|[/_.-])sgb(?:[/_.-]|$)", name):
            return {"sgb", "sgb2"}
        if re.search(r"(?:^|[/_.-])mgb(?:[/_.-]|$)", name):
            return {"mgb"}
        if re.search(r"-gs(?:[-_.]|$)", name):
            return {"dmg", "mgb", "sgb", "sgb2"}
        if re.search(r"-g(?:[-_.]|$)", name):
            return {"dmg", "mgb"}
        if re.search(r"-s(?:[-_.]|$)", name):
            return {"sgb", "sgb2"}
        return ALL_MODELS
    if suite == "mooneye":
        # Follow the upstream group markers rather than assuming an
        # unsuffixed ROM is DMG-only. The upstream README states that a model
        # restriction is encoded in the filename; unsuffixed tests therefore
        # remain applicable to every profile we expose.
        if re.search(r"-dmg0(?:[-_.]|$)", name):
            return {"dmg0"}
        if "dmgabcmgb" in name:
            return {"dmg", "mgb"}
        if re.search(r"-dmgabc(?:[-_.]|$)", name):
            return {"dmg"}
        if re.search(r"-cgb0(?:[-_.]|$)", name):
            return {"cgb0"}
        if "cgb" in name:
            return {"cgb0", "cgb-c", "cgb-e"}
        if "sgb2" in name:
            return {"sgb2"}
        if re.search(r"(?:^|[/_.-])sgb(?:[/_.-]|$)", name):
            return {"sgb", "sgb2"}
        if "mgb" in name:
            return {"mgb"}
        if re.search(r"-gs(?:[-_.]|$)", name):
            return {"dmg", "mgb", "sgb", "sgb2"}
        if re.search(r"-g(?:[-_.]|$)", name):
            return {"dmg", "mgb"}
        if re.search(r"-s(?:[-_.]|$)", name):
            return {"sgb", "sgb2"}
        if re.search(r"-c(?:[-_.]|$)", name):
            return {"cgb0", "cgb-c", "cgb-e"}
        if re.search(r"-a(?:[-_.]|$)", name):
            return set()
        return ALL_MODELS
    for marker, models in (
        ("-dmg0", {"dmg0"}),
        ("-dmgabc", {"dmg"}),
        ("-mgb", {"mgb"}),
        ("-sgb2", {"sgb2"}),
        ("-sgb", {"sgb", "sgb2"}),
        ("-cgb", {"cgb0", "cgb-c", "cgb-e"}),
        ("-c", {"cgb0", "cgb-c", "cgb-e"}),
        ("-s", {"sgb", "sgb2"}),
        ("-gs", {"dmg", "mgb", "sgb", "sgb2"}),
    ):
        if marker in name:
            return models
    if "dmgabcmgb" in name:
        return {"dmg", "mgb"}
    # Do not infer applicability for an unknown suite. An explicit metadata
    # entry can be added once its upstream hardware contract is reviewed.
    return None


def has_gbmicrotest_result_marker(rom: Path) -> bool:
    """Return whether a GBMicrotest ROM implements the FF82 result ABI."""
    data = rom.read_bytes()
    return b"\xe0\x82" in data or b"\xea\x82\xff" in data


def discover(rom_root: Path) -> List[Tuple[str, Path, str, int]]:
    # Keep this list limited to suites whose completion/result protocol is
    # implemented and verified by gbb_test_runner. AGE is primarily a
    # screenshot suite and SameSuite contains interactive/APU experiments;
    # treating either as a Fibonacci test produces false regressions. They
    # remain documented as deferred until dedicated harnesses exist.
    suites = [
        ("mooneye", rom_root / "mooneye-test-suite", "mooneye", 20_000_000),
        ("gbmicrotest", rom_root / "gbmicrotest", "gbmicrotest", 5_000_000),
        ("mooneye-wilbertpol", rom_root / "mooneye-test-suite-wilbertpol",
         "mooneye-wilbertpol", 100_000_000),
    ]
    cases: List[Tuple[str, Path, str, int]] = []
    # The upstream Mooneye repositories contain helper, manual, and hardware
    # diagnostic ROMs alongside the machine-readable acceptance suites.  The
    # runner's Fibonacci/result-register protocol is only defined for these
    # three directories; including the others turns a non-test timeout into a
    # misleading REGRESSION row.
    machine_readable_roots = {"acceptance", "emulator-only", "misc"}
    for suite, root, protocol, cycles in suites:
        if not root.is_dir():
            continue
        for rom in sorted(root.rglob("*.gb")):
            relative = rom.relative_to(rom_root).as_posix()
            if suite in {"mooneye", "mooneye-wilbertpol"}:
                suite_relative = rom.relative_to(root).as_posix()
                if suite_relative.split("/", 1)[0] not in machine_readable_roots:
                    continue
            if suite == "gbmicrotest" and not has_gbmicrotest_result_marker(rom):
                # v7.0 ships a handful of testbench/helper ROMs which never
                # write FF82, so they cannot be evaluated by this runner.
                continue
            if suite == "gbmicrotest" and rom.stem.lower().startswith("poweron_"):
                # These cases assert reset-before-boot state.  This matrix
                # intentionally starts from the documented post-boot profile;
                # keep them deferred until a boot-ROM harness is available.
                continue
            cases.append((suite, rom, protocol, cycles))
    return cases


def run_case_command(
        task: Tuple[Path, Path, Path, str, str, int, str]
) -> Tuple[str, str, str, bool, str]:
    runner, rom_root, rom, suite, protocol, cycles, model = task
    relative = rom.relative_to(rom_root).as_posix()
    command = [str(runner), str(rom), "--max-cycles", str(cycles),
               "--protocol", protocol, "--model", model]
    try:
        result = subprocess.run(command, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, text=True,
                                timeout=max(30, cycles // 1_000_000 * 8))
        detail = result.stdout.splitlines()[-1] if result.stdout else ""
        # A few diagnostic ROMs leave raw bytes in the runner's combined
        # stream. Keep the artifact valid UTF-8/plain text instead of writing
        # NUL/control characters into Markdown, while preserving useful tabs.
        detail = "".join(char if char.isprintable() or char == "\t" else "?"
                          for char in detail)
        return suite, relative, model, result.returncode == 0, detail
    except subprocess.TimeoutExpired:
        return suite, relative, model, False, "timeout"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--runner", required=True, type=Path)
    parser.add_argument("--rom-root", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--models", nargs="*", default=list(MODELS),
                        choices=MODELS)
    parser.add_argument("--jobs", type=int,
                        default=min(8, os.cpu_count() or 1),
                        help="parallel ROM workers (default: detected CPU count, capped at 8)")
    parser.add_argument("--expectations", type=Path,
                        default=Path(__file__).with_name("model_expectations.json"))
    args = parser.parse_args()
    metadata = json.loads(args.expectations.read_text()) if args.expectations.exists() else {}
    known = {(item["suite"], item["path"], item["model"])
             for item in metadata.get("known_failures", [])}
    if args.jobs < 1:
        parser.error("--jobs must be positive")
    rows: List[Tuple[str, str, str, str, str]] = []
    regressions = 0
    tasks = []
    for suite, rom, protocol, cycles in discover(args.rom_root):
        for model in args.models:
            tasks.append((args.runner, args.rom_root, rom, suite, protocol,
                          cycles, model))
    with ThreadPoolExecutor(max_workers=args.jobs) as executor:
        results = executor.map(run_case_command, tasks)
        for suite, relative, model, passed, detail in results:
            applicable = expected_models(suite, relative)
            if passed:
                status = "PASS"
            elif (suite, relative, model) in known:
                status = "KNOWN_FAIL"
            elif applicable is not None and model not in applicable:
                status = "EXPECTED_FAIL"
            else:
                status = "REGRESSION"
                regressions += 1
            rows.append((suite, relative, model.upper().replace("-", "-"), status, detail))

    args.output.parent.mkdir(parents=True, exist_ok=True)
    counts = {status: sum(row[3] == status for row in rows)
              for status in ("PASS", "EXPECTED_FAIL", "KNOWN_FAIL", "REGRESSION")}
    with args.output.open("w", encoding="utf-8") as output:
        output.write("# Hardware model conformance matrix\n\n")
        output.write("Models: " + ", ".join(args.models) + "\n\n")
        output.write("| Suite / ROM | Model | Status | Detail |\n|---|---|---|---|\n")
        for suite, relative, model, status, detail in rows:
            output.write(f"| `{suite}/{relative}` | `{model}` | **{status}** | "
                         f"{detail.replace('|', '/')[:160]} |\n")
        output.write("\n## Summary\n\n")
        output.write(" ".join(f"{key}={value}" for key, value in counts.items()) + "\n")
        output.write("\n## Deferred suites\n\n")
        for suite, reason in DEFERRED_SUITES.items():
            output.write(f"* `{suite}`: {reason}.\n")
        output.write("\n## Compact results\n\n")
        grouped: Dict[Tuple[str, str], List[Tuple[str, str]]] = {}
        for suite, relative, model, status, _ in rows:
            grouped.setdefault((suite, relative), []).append((model, status))
        for (suite, relative), outcomes in grouped.items():
            output.write(f"`{suite}/{relative}` " + " ".join(
                f"{model} -> {status}" for model, status in outcomes) + "\n")
    print(json.dumps(counts, sort_keys=True))
    return 1 if regressions else 0


if __name__ == "__main__":
    raise SystemExit(main())
