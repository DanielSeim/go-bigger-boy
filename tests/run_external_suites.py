#!/usr/bin/env python3
"""Run the metadata-driven AGE and SameSuite research suites.

The external bundles deliberately remain outside the repository.  This runner
discovers their ROMs from the pinned bundle layout and keeps suite-specific
completion, model, and screenshot rules in one place.  Research failures are
reported but do not fail a release unless a manifest entry is promoted to the
``required`` gate.
"""

from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass
import json
import os
from pathlib import Path
import re
import subprocess
import sys
from typing import Iterable, List, Optional, Sequence, Tuple


ROOT = Path(__file__).resolve().parents[1]

AGE_REFERENCE_SUFFIXES = (
    ("nocgb-ncmBCE", "ncmBCE"), ("ds-cgbBCE", "cgbBCE"),
    ("dmgC", "dmgC"), ("ncmBCE", "ncmBCE"), ("ncmBC", "ncmBC"),
    ("ncmE", "ncmE"), ("cgbBCE", "cgbBCE"), ("cgbBC", "cgbBC"),
    ("cgbE", "cgbE"),
)


@dataclass(frozen=True)
class Case:
    suite: str
    case_id: str
    rom: Optional[Path]
    model: Optional[str]
    reference: Optional[Path]
    compatibility_colors: bool
    kind: str
    max_cycles: int
    gate: str
    status: str = "pending"
    detail: str = ""


def safe_id(value: str) -> str:
    return re.sub(r"[^A-Za-z0-9_.-]+", "_", value).strip("_")


def unique_models(models: Iterable[str]) -> List[str]:
    return list(dict.fromkeys(models))


def age_models(label: str) -> Tuple[List[str], bool]:
    """Map AGE's verified device suffixes to GBB's available profiles.

    AGE distinguishes CGB-B/C/E, while GBB currently exposes CGB-0/C/E.
    CGB-B-only coverage is therefore reported through the closest supported
    CGB-C/E profile instead of being mislabeled as a complete revision match.
    """
    name = label.casefold()
    models: List[str] = []
    compatibility = "ncm" in name
    if "dmgc" in name:
        models.append("dmg")
    if "ncmbce" in name:
        models.extend(("cgb-c", "cgb-e"))
    elif "ncmbc" in name:
        models.append("cgb-c")
    elif "ncme" in name:
        models.append("cgb-e")
    elif "cgbbce" in name:
        models.extend(("cgb-c", "cgb-e"))
    elif "cgbbc" in name:
        models.append("cgb-c")
    elif "cgbe" in name:
        models.append("cgb-e")
    return unique_models(models), compatibility


def age_cases(root: Path, config: dict) -> List[Case]:
    cases: List[Case] = []
    visual_roms = set()
    for reference in sorted(root.rglob("*.png")):
        target = None
        suffix = None
        for candidate, model_suffix in AGE_REFERENCE_SUFFIXES:
            marker = f"-{candidate}"
            if reference.stem.endswith(marker):
                model_marker = f"-{model_suffix}"
                target = reference.with_name(
                    f"{reference.stem[:-len(model_marker)]}.gb")
                suffix = model_suffix
                break
        if target is None or suffix is None:
            continue
        if not target.is_file():
            relative = reference.relative_to(root).as_posix()
            cases.append(Case(
                "age-test-roms", safe_id(relative), None, None, reference,
                False, "visual", config["max_cycles"], config["gate"],
                "unsupported", "reference has no matching ROM variant"))
            continue
        visual_roms.add(target)
        relative = target.relative_to(root).as_posix()
        models, compatibility = age_models(suffix)
        if not models:
            cases.append(Case(
                "age-test-roms", safe_id(f"{relative}-{suffix}"), target,
                None, reference, compatibility, "visual", config["max_cycles"],
                config["gate"], "unsupported",
                "reference suffix has no supported profile"))
            continue
        for model in models:
            case_id = safe_id(f"{relative}-{suffix}-{model}")
            cases.append(Case(
                "age-test-roms", case_id, target, model, reference,
                compatibility, "visual", config["max_cycles"],
                config["gate"]))

    for rom in sorted(root.rglob("*.gb")):
        if rom in visual_roms:
            continue
        relative = rom.relative_to(root).as_posix()
        models, compatibility = age_models(rom.stem)
        if not models:
            cases.append(Case(
                "age-test-roms", safe_id(relative), rom, None, None,
                compatibility, "machine", config["max_cycles"], config["gate"],
                "unsupported", "ROM suffix has no supported profile"))
            continue
        for model in models:
            cases.append(Case(
                "age-test-roms", safe_id(f"{relative}-{model}"), rom, model,
                None, compatibility, "machine", config["max_cycles"],
                config["gate"]))
    return cases


def samesuite_apu_models(name: str) -> List[str]:
    lower = name.casefold()
    if lower in {"div_write_trigger.gb", "div_write_trigger_10.gb"}:
        return ["dmg"]
    if "cgb0b" in lower or "cgb0bc" in lower:
        return ["cgb0", "cgb-c"]
    if "cgbbce" in lower:
        return ["cgb-c", "cgb-e"]
    if "cgbde" in lower or "cgbe" in lower:
        return ["cgb-e"]
    return ["cgb-e"]


def samesuite_cases(root: Path, config: dict) -> List[Case]:
    cases: List[Case] = []
    for rom in sorted(root.rglob("*.gb")):
        relative = rom.relative_to(root).as_posix()
        category = relative.split("/", 1)[0].casefold()
        if category == "sgb":
            # MLT_REQ probes expose diagnostic information rather than the
            # Fibonacci pass/fail ABI. Keep them visible without pretending
            # that an informational result is a regression.
            cases.append(Case(
                "same-suite", safe_id(relative), rom, None, None, False,
                "informational", config["max_cycles"], config["gate"],
                "info", "SGB command probe is informational-only"))
            continue
        if category == "apu":
            models = samesuite_apu_models(rom.name)
        elif category in {"dma", "interrupt", "ppu"}:
            # The upstream bundle does not document revision applicability for
            # non-APU cases. CGB-E is the broadest represented profile and the
            # result remains in the research lane until verified.
            models = ["cgb-e"]
        else:
            cases.append(Case(
                "same-suite", safe_id(relative), rom, None, None, False,
                "informational", config["max_cycles"], config["gate"],
                "info", "unclassified SameSuite sub-suite"))
            continue
        for model in models:
            cases.append(Case(
                "same-suite", safe_id(f"{relative}-{model}"), rom, model,
                None, False, "machine", config["max_cycles"], config["gate"]))
    return cases


def discover_cases(
        rom_root: Path, same_suite_root: Optional[Path], manifest: dict,
        selected: Sequence[str]) -> List[Case]:
    cases: List[Case] = []
    for suite in selected:
        config = manifest["suites"][suite]
        if suite == "age-test-roms":
            root = rom_root / config["root"]
            if root.is_dir():
                cases.extend(age_cases(root, config))
        elif suite == "same-suite":
            root = same_suite_root or (rom_root / config["root"])
            if root.is_dir():
                cases.extend(samesuite_cases(root, config))
    return cases


def run_case(case: Case, runner: Path, output_dir: Path) -> Case:
    if case.status != "pending":
        return case
    if case.rom is None or case.model is None:
        return case
    timeout = max(30, case.max_cycles // 1_000_000 * 8)
    if case.kind == "visual":
        actual = output_dir / f"{case.case_id}.ppm"
        command = [
            sys.executable, str(ROOT / "tests" / "compare_frame.py"),
            "--runner", str(runner), "--rom", str(case.rom),
            "--reference", str(case.reference), "--actual", str(actual),
            "--model", case.model, "--max-cycles", str(case.max_cycles),
            "--frame-on-ld-bb",
        ]
        if case.compatibility_colors:
            command.append("--dmg-compatibility-colors")
    else:
        command = [
            str(runner), str(case.rom), "--max-cycles", str(case.max_cycles),
            "--protocol", "mooneye", "--model", case.model,
        ]
    try:
        result = subprocess.run(
            command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            text=True, timeout=timeout,
        )
    except subprocess.TimeoutExpired:
        return Case(**{**case.__dict__, "status": "timeout", "detail": "timeout"})
    lines = result.stdout.splitlines() if result.stdout else []
    detail = lines[-1] if lines else f"exit code {result.returncode}"
    status = "pass" if result.returncode == 0 else "fail"
    return Case(**{**case.__dict__, "status": status, "detail": detail})


def write_report(path: Path, cases: Sequence[Case], missing: Sequence[str]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    counts = {key: sum(case.status == key for case in cases)
              for key in ("pass", "fail", "timeout", "unsupported", "info")}
    with path.open("w", encoding="utf-8") as output:
        output.write("# External Game Boy suite report\n\n")
        output.write("This is a metadata-driven research report. Research failures "
                     "do not fail the release gate.\n\n")
        if missing:
            output.write("## Missing suites\n\n")
            for suite in missing:
                output.write(f"* `{suite}`\n")
            output.write("\n")
        output.write("## Summary\n\n")
        output.write(" ".join(f"{key.upper()}={value}"
                              for key, value in counts.items()) + "\n\n")
        output.write("| Suite | Case | Model | Kind | Gate | Status | Detail |\n")
        output.write("|---|---|---|---|---|---|---|\n")
        for case in cases:
            output.write(
                f"| `{case.suite}` | `{case.case_id}` | "
                f"`{case.model or '-'.upper()}` | `{case.kind}` | "
                f"`{case.gate}` | **{case.status.upper()}** | "
                f"{case.detail.replace('|', '/')[:180]} |\n")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--runner", type=Path, required=True)
    parser.add_argument("--rom-root", type=Path, default=Path("."))
    parser.add_argument("--same-suite-root", type=Path)
    parser.add_argument("--manifest", type=Path,
                        default=Path(__file__).with_name("external_suite_manifest.json"))
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--suite", action="append",
                        choices=("age-test-roms", "same-suite"))
    parser.add_argument("--jobs", type=int,
                        default=min(8, os.cpu_count() or 1))
    parser.add_argument("--list", action="store_true")
    parser.add_argument("--fail-on-required", action="store_true")
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error("--jobs must be positive")
    manifest = json.loads(args.manifest.read_text(encoding="utf-8"))
    selected = args.suite or list(manifest["suites"])
    cases = discover_cases(args.rom_root, args.same_suite_root, manifest, selected)
    missing = []
    for suite in selected:
        config = manifest["suites"][suite]
        root = (args.same_suite_root if suite == "same-suite" and
                args.same_suite_root else args.rom_root / config["root"])
        if not root.is_dir():
            missing.append(suite)
    if args.list:
        for case in cases:
            print(f"{case.suite}\t{case.case_id}\t{case.model or '-'}\t{case.kind}")
        return 0
    output_dir = args.output.parent / "external-suite-captures"
    with ThreadPoolExecutor(max_workers=args.jobs) as executor:
        cases = list(executor.map(
            lambda case: run_case(case, args.runner, output_dir), cases))
    write_report(args.output, cases, missing)
    counts = {key: sum(case.status == key for case in cases)
              for key in ("pass", "fail", "timeout", "unsupported", "info")}
    print(json.dumps(counts, sort_keys=True))
    if args.fail_on_required and any(
            case.gate == "required" and case.status in {"fail", "timeout"}
            for case in cases):
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
