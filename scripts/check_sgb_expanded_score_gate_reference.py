#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Validate all measured native gate profiles against original SGB timing."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

from build_sgb_gate_timing_fixture import CASES as EXPANDED_CASES
from build_sgb_score_gate import build
from check_sgb_gate_timing_reference import check_matrix as check_expanded, run as run_expanded
from check_sgb_score_gate_reference import PULSES, TOLERANCE, align, native
from check_sgb_timing_reference import check_matrix as check_legacy, run as run_legacy

CASES = {**EXPANDED_CASES, 'double-tempo': (192, 127, 16), 'short-gate': (96, 63, 16)}


def fixture(case):
    if case not in CASES:
        raise ValueError('unknown expanded native gate fixture')
    _, articulation, duration = CASES[case]
    # A warmup rest of the tested duration avoids claiming initial-note phase.
    return bytes((duration, articulation, 0xC9, 0x98, 0x99, 0xA4, 8, 0xC9, 0))


def compare(candidates, references):
    if (not isinstance(references, list) or len(references) != 20 or
            any(not isinstance(row, dict) for row in references) or
            {(row.get('model'), row.get('case')) for row in references} != {
                (model, case) for model in ('sgb', 'sgb2') for case in CASES}):
        raise ValueError('incomplete expanded native reference matrix')
    check_expanded([row for row in references if row['case'] in EXPANDED_CASES])
    legacy = [{**row, 'case': 'baseline' if row['case'] == 'control' else row['case']}
              for row in references if row['case'] in ('control', 'double-tempo', 'short-gate')]
    check_legacy(legacy)
    if set(candidates) != set(CASES):
        raise ValueError('incomplete expanded native timing matrix')
    comparisons = []
    for result in references:
        report = candidates[result['case']]
        align(report, fixture(result['case']))
        if (report['tempo'] != result['tempo'] or
                tuple(result.get(key) for key in ('tempo', 'articulation', 'duration')) != CASES[result['case']]):
            raise ValueError('native/reference profile parameters differ')
        keyons = report['keyons']
        intervals = [(b['half_cycle']-a['half_cycle'])/2 for a, b in zip(keyons, keyons[1:])]
        gates = [(off-on['half_cycle'])/2 for on, off in zip(keyons, report['keyoff_half_cycles'])]
        differences = {}
        for name, observed, expected in (('onset', intervals, result['onset_intervals_spc_cycles']),
                                        ('gate', gates, result['gate_spc_cycles'])):
            differences[name] = [abs(a-b) for a, b in zip(observed, expected)]
            if len(observed) != len(expected) or any(value > TOLERANCE for value in differences[name]):
                raise ValueError(f"{result['model']}/{result['case']}: native {name} differs beyond one timer pulse")
        comparisons.append({**result, 'native_onset_intervals_spc_cycles': intervals,
                            'native_gate_spc_cycles': gates, 'absolute_difference_spc_cycles': differences})
    return comparisons


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--probe', type=Path, required=True)
    parser.add_argument('--trace', type=Path, required=True)
    parser.add_argument('--firmware-dir', type=Path, required=True)
    args = parser.parse_args()
    try:
        candidates = {case: native(args.probe, fixture(case), CASES[case][0]) for case in CASES}
        references = [(run_expanded if case in EXPANDED_CASES else run_legacy)(
                        args.trace.resolve(), args.firmware_dir.resolve(), model, case)
                      for case in CASES for model in ('sgb', 'sgb2')]
        comparisons = compare(candidates, references)
    except (OSError, ValueError, TypeError, KeyError, subprocess.TimeoutExpired) as error:
        parser.exit(1, f'expanded native gate check failed: {error}\n')
    print(json.dumps({'schema': 'gbb-expanded-score-gate-reference-v1', 'qualification': False, 'playback': False,
                      'evidence': 'calibrated_native_DSP_onsets_and_keyoffs_with_owned_PCM',
                      'native_program_sha256': hashlib.sha256(build()).hexdigest(),
                      'tolerance_spc_cycles': TOLERANCE,
                      'profiles': [{'tempo': tempo, 'articulation': art, 'duration': duration, 'timer_pulses': pulses}
                                   for (tempo, art, duration), pulses in PULSES.items()],
                      'owned_pcm': {case: report['pcm'] for case, report in candidates.items()},
                      'comparisons': comparisons}, indent=2))


if __name__ == '__main__':
    main()
