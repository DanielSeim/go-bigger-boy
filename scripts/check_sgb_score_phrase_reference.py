#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Compare native raw-bank phrase scheduling with owned private-reference fixtures."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile
from build_sgb_score_phrase import build
from build_sgb_phrase_fixture import CASES, score_payload
from check_sgb_phrase_reference import run
from check_sgb_score_pair_reference import validate as pair_validate, align as pair_align, compare as pair_compare
from schedule_sgb_score import schedule


def fixture(case):
    payload = score_payload(case)
    return payload[4:4 + int.from_bytes(payload[:2], 'little')]


def pair_report(report):
    if not isinstance(report, dict) or report.get('schema') != 'gbb-spc-score-phrase-v1':
        raise ValueError('invalid native phrase schema')
    return {**report, 'schema': 'gbb-spc-score-pair-v1'}


def validate(report):
    pair_validate(pair_report(report))


def align(report, bank):
    if len(bank) < 2:
        raise ValueError('bank has no root word')
    pair_align(pair_report(report), schedule(bank, int.from_bytes(bank[:2], 'little')))


def native(probe, bank, tempo=96):
    with tempfile.TemporaryDirectory(prefix='gbb-score-phrase-') as directory:
        program = Path(directory)/'program.bin'
        stream = Path(directory)/'bank.bin'
        program.write_bytes(build())
        stream.write_bytes(bank)
        result = subprocess.run([str(probe), str(program), str(stream), str(tempo)],
                                capture_output=True, text=True, timeout=60)
        if result.returncode or len(result.stdout) > 16384:
            raise ValueError(f'native phrase probe failed: {result.stderr[:1000]}')
        report = json.loads(result.stdout)
    validate(report)
    return report


def compare(candidates, references):
    if not isinstance(candidates, dict) or set(candidates) != set(CASES):
        raise ValueError('require all three native phrase cases')
    for case, report in candidates.items():
        align(report, fixture(case))
    return pair_compare({case: pair_report(report) for case, report in candidates.items()}, references)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--probe', type=Path, required=True)
    parser.add_argument('--trace', type=Path, required=True)
    parser.add_argument('--firmware-dir', type=Path, required=True)
    args = parser.parse_args()
    try:
        candidates = {case: native(args.probe.resolve(), fixture(case)) for case in CASES}
        references = [run(args.trace.resolve(), args.firmware_dir.resolve(), model, case)
                      for case in CASES for model in ('sgb', 'sgb2')]
        comparisons = compare(candidates, references)
    except (OSError, ValueError, subprocess.TimeoutExpired) as error:
        parser.error(str(error))
    print(json.dumps({'schema': 'gbb-score-phrase-reference-v1', 'qualification': False,
                      'playback': False, 'program_sha256': hashlib.sha256(build()).hexdigest(),
                      'native': candidates, 'reference': references, 'comparisons': comparisons}, indent=2))


if __name__ == '__main__':
    main()
