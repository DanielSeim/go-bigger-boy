#!/usr/bin/env python3
"""Combine CTest and link-harness diagnostics into one CI report.

The report is deliberately independent of the emulator and uses only the
Python standard library so it can run after a failed test step on every
desktop runner.
"""

from __future__ import annotations

import argparse
import json
import xml.etree.ElementTree as ET
from pathlib import Path
from typing import Any


def _number(value: str | None, default: float = 0.0) -> float:
    try:
        return float(value or default)
    except ValueError:
        return default


def _text(element: ET.Element | None, limit: int = 2400) -> str:
    if element is None:
        return ""
    value = "".join(element.itertext()).strip()
    return value[:limit]


def parse_junit(paths: list[Path]) -> dict[str, Any]:
    tests: list[dict[str, Any]] = []
    missing: list[str] = []
    for path in paths:
        if not path.exists():
            missing.append(str(path))
            continue
        try:
            root = ET.parse(path).getroot()
        except (ET.ParseError, OSError) as error:
            tests.append({
                "name": str(path),
                "status": "error",
                "duration_seconds": 0.0,
                "detail": f"could not parse JUnit XML: {error}",
            })
            continue
        for case in root.iter("testcase"):
            failure = case.find("failure")
            if failure is None:
                failure = case.find("error")
            skipped = case.find("skipped")
            if failure is not None:
                status = "failed"
                detail = _text(failure)
            elif skipped is not None:
                status = "skipped"
                detail = _text(skipped)
            else:
                status = "passed"
                detail = ""
            name = case.get("name", "unnamed test")
            classname = case.get("classname")
            if classname:
                name = f"{classname}::{name}"
            tests.append({
                "name": name,
                "status": status,
                "duration_seconds": _number(case.get("time")),
                "detail": detail,
            })

    counts = {status: sum(test["status"] == status for test in tests)
              for status in ("passed", "failed", "skipped", "error")}
    duration = sum(test["duration_seconds"] for test in tests)
    return {
        "files": [str(path) for path in paths if path.exists()],
        "missing_files": missing,
        "counts": counts,
        "duration_seconds": round(duration, 3),
        "tests": tests,
    }


def parse_key_value_report(path: Path) -> dict[str, str]:
    values: dict[str, str] = {}
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except OSError as error:
        return {"_error": str(error)}
    for line in lines:
        if "=" in line:
            key, value = line.split("=", 1)
            values[key] = value
    return values


def parse_link_diagnostics(directory: Path | None) -> dict[str, Any]:
    if directory is None or not directory.exists():
        return {"directory": str(directory) if directory else None,
                "reports": [], "failure_files": [], "trace_files": []}

    reports = []
    for path in sorted(directory.rglob("*.report")):
        values = parse_key_value_report(path)
        reports.append({
            "path": str(path.relative_to(directory)),
            "fault_plan": values.get("fault_plan", ""),
            "fault_events_applied": values.get("fault_events_applied", ""),
            "host_transfers_completed": values.get("host_transfers_completed", ""),
            "join_transfers_completed": values.get("join_transfers_completed", ""),
            "values": values,
        })
    failure_files = []
    for path in sorted(directory.rglob("failure.txt")):
        try:
            detail = path.read_text(encoding="utf-8")[:4000]
        except OSError as error:
            detail = str(error)
        failure_files.append({
            "path": str(path.relative_to(directory)),
            "detail": detail,
        })
    trace_files = [str(path.relative_to(directory))
                   for path in sorted(directory.rglob("*.trace"))]
    return {
        "directory": str(directory),
        "reports": reports,
        "failure_files": failure_files,
        "trace_files": trace_files,
    }


def overall_status(ctest: dict[str, Any], link: dict[str, Any]) -> str:
    counts = ctest["counts"]
    if counts["failed"] or counts["error"] or link["failure_files"]:
        return "FAIL"
    if ctest["missing_files"] and not ctest["tests"]:
        return "INCOMPLETE"
    if not ctest["tests"] and not link["reports"]:
        return "INCOMPLETE"
    return "PASS"


def markdown_report(report: dict[str, Any]) -> str:
    ctest = report["ctest"]
    link = report["link_diagnostics"]
    counts = ctest["counts"]
    lines = [
        "# Go Bigger Boy CI regression report",
        "",
        f"**Status:** `{report['status']}`",
        "",
        "## CTest accuracy and contract results",
        "",
        (f"Passed: **{counts['passed']}**, failed: **{counts['failed']}**, "
         f"skipped: **{counts['skipped']}**, errors: **{counts['error']}**; "
         f"reported test time: **{ctest['duration_seconds']:.3f}s**."),
        "",
    ]
    if ctest["missing_files"]:
        lines += ["Missing JUnit files:", ""]
        lines += [f"- `{path}`" for path in ctest["missing_files"]]
        lines.append("")
    slowest = sorted(ctest["tests"],
                     key=lambda test: test["duration_seconds"], reverse=True)[:10]
    if slowest:
        lines += ["### Slowest tests", "", "| Test | Status | Time |",
                  "|---|---|---:|"]
        for test in slowest:
            lines.append(f"| `{test['name']}` | `{test['status']}` | "
                         f"{test['duration_seconds']:.3f}s |")
        lines.append("")
    failed = [test for test in ctest["tests"]
              if test["status"] in {"failed", "error"}]
    if failed:
        lines += ["### Failed tests", "", "| Test | Time | Detail |", "|---|---:|---|"]
        for test in failed:
            detail = test["detail"].replace("|", "/").replace("\n", " ")
            lines.append(f"| `{test['name']}` | {test['duration_seconds']:.3f}s | "
                         f"{detail[:240]} |")
        lines.append("")
    elif ctest["tests"]:
        lines += ["No CTest failures were reported.", ""]

    lines += ["## Link-cable diagnostics", ""]
    if link["reports"]:
        lines += ["| Report | Plan | Faults | Host transfers | Join transfers |",
                  "|---|---|---:|---:|---:|"]
        for item in link["reports"]:
            lines.append(
                f"| `{item['path']}` | `{item['fault_plan']}` | "
                f"{item['fault_events_applied'] or '-'} | "
                f"{item['host_transfers_completed'] or '-'} | "
                f"{item['join_transfers_completed'] or '-'} |"
            )
        lines.append("")
    else:
        lines += [
            "No preserved link report was produced. Successful link tests clean "
            "up their temporary diagnostics; failure reports are preserved below.",
            "",
        ]
    if link["failure_files"]:
        lines += ["### Link failure details", ""]
        for failure in link["failure_files"]:
            lines += [f"#### `{failure['path']}`", "", "```text",
                      failure["detail"].rstrip(), "```", ""]
    if link["trace_files"]:
        lines += ["Preserved traces:", ""]
        lines += [f"- `{path}`" for path in link["trace_files"]]
        lines.append("")
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--ctest-junit", action="append", type=Path, default=[])
    parser.add_argument("--link-diagnostics", type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--json-output", required=True, type=Path)
    args = parser.parse_args()

    ctest = parse_junit(args.ctest_junit)
    link = parse_link_diagnostics(args.link_diagnostics)
    report = {
        "status": overall_status(ctest, link),
        "ctest": ctest,
        "link_diagnostics": link,
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.json_output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(markdown_report(report), encoding="utf-8")
    args.json_output.write_text(json.dumps(report, indent=2) + "\n",
                                encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
