#!/usr/bin/env python3
"""Exercise the real link harness executable over its TCP loopback path."""

from __future__ import annotations

import pathlib
import subprocess
import sys
import tempfile


class TcpUnavailable(RuntimeError):
    """Raised when the host environment prevents loopback TCP setup."""


def make_rom(path: pathlib.Path) -> None:
    """Create a tiny valid MBC1+battery ROM that starts a serial transfer."""
    rom = bytearray(0x8000)
    rom[0x134 : 0x134 + len(b"GBB TCP TEST")] = b"GBB TCP TEST"
    rom[0x147] = 0x03  # MBC1 + battery-backed RAM
    rom[0x148] = 0x00  # 32 KiB ROM
    rom[0x149] = 0x02  # 8 KiB RAM

    # Both guests start the same transfer. The endpoint arbitration policy
    # selects the host as the clock owner, which mirrors the production path.
    program = bytes(
        (
            0x3E,
            0xA5,  # LD A,$A5
            0xEA,
            0x01,
            0xFF,  # LD (FF01),A
            0x3E,
            0x81,  # LD A,$81 (start transfer, internal clock)
            0xEA,
            0x02,
            0xFF,  # LD (FF02),A
            0x18,
            0xFE,  # JR back to the transfer-start instruction
        )
    )
    rom[0x100 : 0x100 + len(program)] = program
    path.write_bytes(rom)


def parse_report(path: pathlib.Path) -> dict[str, str]:
    values: dict[str, str] = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        if "=" not in line:
            continue
        key, value = line.split("=", 1)
        values[key] = value
    return values


def run_harness(executable: pathlib.Path, arguments: list[str]) -> None:
    result = subprocess.run(
        [str(executable), *arguments],
        text=True,
        capture_output=True,
        timeout=30,
        check=False,
    )
    if result.returncode != 0:
        if "could not listen for TCP link harness" in result.stderr:
            raise TcpUnavailable(result.stderr)
        raise AssertionError(
            "gbb_link_harness failed with exit code "
            f"{result.returncode}\nstdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        )


def check_run(report_path: pathlib.Path, trace_path: pathlib.Path, plan: str) -> None:
    report = parse_report(report_path)
    if report.get("fault_plan") != plan:
        raise AssertionError(f"expected fault_plan={plan!r}, got {report.get('fault_plan')!r}")
    if report.get("fault_events_applied") != "1":
        raise AssertionError(
            "expected exactly one injected fault, got "
            f"{report.get('fault_events_applied')!r}"
        )
    trace = trace_path.read_text(encoding="utf-8")
    if "event=fault_injected" not in trace:
        raise AssertionError("harness trace does not contain fault_injected")
    if "action=drop" not in trace or "packet=byte" not in trace:
        raise AssertionError("harness trace does not identify the dropped byte")


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} GBB_LINK_HARNESS", file=sys.stderr)
        return 2

    executable = pathlib.Path(sys.argv[1]).resolve()
    try:
        with tempfile.TemporaryDirectory(prefix="gbb-real-link-") as directory:
            root = pathlib.Path(directory)
            rom = root / "tcp-test.gb"
            save1 = root / "player1.sav"
            save2 = root / "player2.sav"
            make_rom(rom)
            save1.write_bytes(bytes(0x2000))
            save2.write_bytes(bytes(0x2000))

            original_trace = root / "original.trace"
            original_report = root / "original.report"
            common = [
                "--rom",
                str(rom),
                "--save1",
                str(save1),
                "--save2",
                str(save2),
                "--transport",
                "tcp",
                "--frames",
                "8",
            ]
            run_harness(
                executable,
                [
                    *common,
                    "--fault",
                    "drop",
                    "--trace",
                    str(original_trace),
                    "--report",
                    str(original_report),
                ],
            )
            check_run(original_report, original_trace, "drop")

            replay_trace = root / "replay.trace"
            replay_report = root / "replay.report"
            run_harness(
                executable,
                [
                    *common,
                    "--fault-replay",
                    str(original_trace),
                    "--trace",
                    str(replay_trace),
                    "--report",
                    str(replay_report),
                ],
            )
            check_run(replay_report, replay_trace, "replay")
    except TcpUnavailable as error:
        print(f"SKIP: TCP loopback is unavailable in this environment: {error}")
        return 77

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
