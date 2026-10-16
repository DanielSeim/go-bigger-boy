#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Compare pinned CGB-E SameSuite frequency-write observations with SameBoy."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

import samesuite_alignment_reference as reference

PIN = Path(__file__).resolve().parents[1] / 'tests/fixtures/external-reference/samesuite-frequency.json'


def curve(memory, entry):
    if len(memory) not in (8192, 8352):
        raise ValueError('unexpected WRAM snapshot size')
    values = list(memory[:16])
    if not set(values) <= {0, 15, 240, 255}:
        raise ValueError('unexpected PCM nibble or volume')
    return dict(samples=values, observations=[[v >> 4, v & 15] for v in values],
                observation_fields=['immediate_pcm', 'late_pcm'],
                sample_sha256=hashlib.sha256(bytes(values)).hexdigest())


def run_reference(rom_root, runner, adapter, output):
    return reference.run_reference(rom_root, runner, adapter, output, pin_path=PIN,
        curve_reader=curve, schema='gbb-samesuite-frequency-reference-v1')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--rom-root', type=Path, required=True)
    parser.add_argument('--runner', type=Path, required=True)
    parser.add_argument('--sameboy-capture', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    try:
        report = run_reference(args.rom_root.resolve(), args.runner.resolve(),
                               args.sameboy_capture.resolve(), args.output.resolve())
        print(json.dumps(report))
        return 0 if reference.passes(report, PIN) else 1
    except (OSError, ValueError, KeyError, TypeError, subprocess.SubprocessError) as error:
        parser.exit(2, f'frequency reference failed: {error}\n')


if __name__ == '__main__':
    raise SystemExit(main())
