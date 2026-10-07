#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Compare an isolated owned SPC clock with bounded private note-onset observations."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile

from build_sgb_score_clock import build
from check_sgb_timing_reference import CASES, check_matrix, run

TOLERANCE = 2048


def observe(report):
    if (not isinstance(report, dict) or report.get('schema') != 'gbb-spc-score-clock-v1' or
            report.get('qualification') is not False or report.get('playback') is not False or
            report.get('unsupported_tempos_rejected') is not True):
        raise ValueError('invalid native clock report')
    runs = report.get('runs')
    if not isinstance(runs, list) or len(runs) != 2:
        raise ValueError('incomplete native clock matrix')
    intervals = {}
    for result, tempo in zip(runs, (96, 192)):
        if not isinstance(result, dict) or result.get('tempo') != tempo or any(result.get(key) is not True for key in
                ('reset_equal', 'restore_equal', 'rollover', 'pending_pulses_equal')):
            raise ValueError('native lifecycle checks missing')
        ticks = result.get('tick_half_cycles')
        if (not isinstance(ticks, list) or len(ticks) != 96 or
                any(type(value) is not int or value <= 0 for value in ticks) or
                any(a >= b for a, b in zip(ticks, ticks[1:]))):
            raise ValueError('invalid native score tick timeline')
        intervals[tempo] = [(ticks[b]-ticks[a])/2 for a, b in ((15, 31), (31, 47))]
        low, high = (84000, 90000) if tempo == 96 else (41000, 46000)
        if any(not low <= value <= high for value in intervals[tempo]):
            raise ValueError('native clock outside bounded timing range')
    return intervals


def native(probe):
    image = build()
    with tempfile.TemporaryDirectory(prefix='gbb-score-clock-') as directory:
        program = Path(directory)/'clock.bin'
        program.write_bytes(image)
        completed = subprocess.run([str(probe.resolve()), str(program)], capture_output=True,
                                   text=True, timeout=60)
        if completed.returncode != 0 or len(completed.stdout) > 32768:
            raise ValueError('native clock probe failed or exceeded report bound')
        report = json.loads(completed.stdout)
    return observe(report), hashlib.sha256(image).hexdigest()


def compare(intervals, references):
    check_matrix(references)
    comparisons = []
    for result in references:
        candidate = intervals[result['tempo']]
        differences = [abs(a-b) for a, b in zip(candidate, result['onset_intervals_spc_cycles'])]
        if any(value > TOLERANCE for value in differences):
            raise ValueError('native clock differs beyond one timer pulse')
        comparisons.append({**result, 'native_intervals_spc_cycles': candidate,
                            'absolute_difference_spc_cycles': differences})
    return comparisons


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--probe', type=Path, required=True)
    parser.add_argument('--trace', type=Path, required=True)
    parser.add_argument('--firmware-dir', type=Path, required=True)
    args = parser.parse_args()
    try:
        intervals, digest = native(args.probe)
        references = [run(args.trace.resolve(), args.firmware_dir.resolve(), model, case)
                      for model in ('sgb', 'sgb2') for case in CASES]
        comparisons = compare(intervals, references)
    except (OSError, ValueError, TypeError, KeyError, subprocess.TimeoutExpired) as error:
        parser.exit(1, f'score-clock check failed: {error}\n')
    print(json.dumps({'schema': 'gbb-score-clock-reference-v1', 'qualification': False,
                      'playback': False, 'native_program_sha256': digest,
                      'timer_period_spc_cycles': 2048, 'tolerance_spc_cycles': TOLERANCE,
                      'comparisons': comparisons}, indent=2))


if __name__ == '__main__':
    main()
