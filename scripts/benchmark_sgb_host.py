#!/usr/bin/env python3
"""Serial private-firmware playback benchmark with exact PCM and headroom gates.

Use a new output directory: reports are public-safe, but WAVs remain private.
Do not run concurrently with compilation, other benchmarks or snapshot tests.
"""
import argparse
import hashlib
import itertools
import json
from pathlib import Path
import subprocess
import sys


def annotate_report(path, power_profile):
    """Record caller-declared context, never infer or normalize measured speed."""
    data = json.loads(path.read_text())
    data["benchmark_context"] = {
        "power_profile_label": power_profile,
        "power_profile_source": "caller-declared" if power_profile else "unspecified",
    }
    path.write_text(json.dumps(data, indent=2) + "\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--runner", type=Path, required=True)
    parser.add_argument("--roms", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--repeat", type=int, default=1,
                        help="serial complete four-profile repeats; every capture must pass (1-20)")
    parser.add_argument("--power-profile", type=str,
                        help="caller-declared power profile, recorded in each report; does not change settings or thresholds")
    args = parser.parse_args()
    if not 1 <= args.repeat <= 20:
        parser.error("repeat must be between 1 and 20")
    root = Path(__file__).resolve().parents[1]
    runner, roms = args.runner.resolve(), args.roms.resolve()
    game = roms / "Donkey Kong (JU) (V1.1) [S][!].gb"
    initial_save = game.with_suffix(".sav")
    profiles = (
        ("sgb1", False, 32000, "553c8992a2de7dbc86eac6de3132000299f2fcb7e73fbe8ef0d03f9c8ec6856f", 6717456860480117116),
        ("sgb1", True, 48000, "53d94251ed69f1edbd8875088b6670fdf97633c9474fde8c18a45081faacdd06", 7214238551188074392),
        ("sgb2", False, 32000, "d430fa49f199df87fc20b6465cba0fff0e89daf6b446a9a07159b90b188e4bc9", 3340490035328936425),
        ("sgb2", True, 44100, "8854bc396db0a99828d2e34fefba362498d2cab21039b5bb0e2bbb795fdc3071", 1311356025276262707),
    )
    required = (runner, game, initial_save, roms / "sgb1.program.rom", roms / "sgb2.program.rom",
                roms / "sgb.boot.rom", roms / "sgb2.boot.rom", roms / "spc700.rom")
    if any(not path.is_file() for path in required):
        parser.error("missing runner or caller-owned firmware/game image")
    if hashlib.sha256(game.read_bytes()).hexdigest() != \
            "b490c89efe718633b07381def66ce0ed58a5075aabe40c6e644baf2b408a76f4":
        parser.error("unexpected private game revision")
    if hashlib.sha256(initial_save.read_bytes()).hexdigest() != \
            "59123d52ecfd6686b62823ed0e534b6d560e1ed1bf102e63489574305b838a34":
        parser.error("unexpected initial save; use the same private baseline input")
    if args.output_dir.exists():
        parser.error("output directory already exists; choose a new path")
    args.output_dir.mkdir(parents=True)
    reports = []
    for round_index, profile in itertools.product(range(args.repeat), profiles):
        model, combined, rate, expected, expected_state = profile
        directory = args.output_dir
        if args.repeat > 1:
            directory /= f"round-{round_index + 1}"
            directory.mkdir(exist_ok=True)
        prefix = directory / f"{model}-{'combined' if combined else 'native'}"
        wav, report = prefix.with_suffix(".wav"), prefix.with_suffix(".json")
        command = [str(runner), model, str(roms / f"{model}.program.rom"),
                   str(roms / "spc700.rom"), str(game),
                   str(roms / ("sgb.boot.rom" if model == "sgb1" else "sgb2.boot.rom")),
                   str(root / "tests/fixtures/sgb/titles/donkey-kong-gameplay.script"),
                   str(wav), str(report), "--calibrate-host"]
        if combined:
            command.extend(("--combined", "--output-hz", str(rate)))
        subprocess.run(command, check=True, cwd=root)
        if hashlib.sha256(wav.read_bytes()).hexdigest() != expected:
            raise RuntimeError(f"{model}: playback changed the complete PCM baseline")
        if json.loads(report.read_text()).get("gb_state_hash") != expected_state:
            raise RuntimeError(f"{model}: playback changed final GB state/framebuffers")
        annotate_report(report, args.power_profile)
        reports.append(str(report))
        print(f"round {round_index + 1}/{args.repeat}: {model}/{'combined' if combined else 'native'}: "
              "exact PCM and GB state PASS", flush=True)
    return subprocess.run([sys.executable, str(root / "scripts/check_sgb_host_performance.py"),
                           *reports], check=False).returncode


if __name__ == "__main__":
    raise SystemExit(main())
