#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Measure bounded original duration/tempo gate timing from owned fixtures."""
import argparse
import json
from pathlib import Path
import subprocess
from build_sgb_gate_timing_fixture import CASES, build
from check_sgb_timing_reference import run_fixture

TOLERANCE = 2048
# Pinned bounded observations, not interpolation rules between these cases.
GATE_BOUNDS = {
    'control': (72000, 76000), 'half-duration': (28000, 33000),
    'long-duration': (116000, 121000), 'half-duration-short': (18000, 23000),
    'long-duration-short': (73000, 79000), 'middle-tempo': (52000, 58000),
    'middle-tempo-short': (32000, 37000), 'double-tempo-short': (20000, 25000),
}


def check_matrix(results):
    expected = {(model, case) for model in ('sgb', 'sgb2') for case in CASES}
    if (not isinstance(results, list) or len(results) != len(expected) or
            any(not isinstance(r, dict) for r in results) or
            {(r.get('model'), r.get('case')) for r in results} != expected):
        raise ValueError('incomplete gate timing matrix')
    for result in results:
        if tuple(result.get(key) for key in ('tempo', 'articulation', 'duration')) != CASES[result['case']]:
            raise ValueError('timing parameters differ from owned fixture')
        for field, size in (('onset_intervals_spc_cycles', 2), ('gate_spc_cycles', 3)):
            values = result.get(field)
            if (not isinstance(values, list) or len(values) != size or
                    any(type(value) is not int or value <= 0 for value in values)):
                raise ValueError('invalid gate timing observations')
        intervals, gates = result['onset_intervals_spc_cycles'], result['gate_spc_cycles']
        low, high = GATE_BOUNDS[result['case']]
        if any(not low <= value <= high for value in gates):
            raise ValueError('gate timing outside the observed case bounds')
        if any(gate >= interval for gate, interval in zip(gates, intervals)):
            raise ValueError('gate overlaps the following note')
        if any(not 0.2 <= gate/(sum(intervals)/2) <= 0.98 for gate in gates):
            raise ValueError('note gate outside bounded duration range')
    summaries = []
    for model in ('sgb', 'sgb2'):
        cases = {r['case']: r for r in results if r['model'] == model}
        control = cases['control']['onset_intervals_spc_cycles']
        if any(not 84000 <= value <= 90000 for value in control):
            raise ValueError('control onset timing outside the prior observed range')
        bounds = {'half-duration': (0.45, 0.55), 'long-duration': (1.45, 1.55),
                  'half-duration-short': (0.45, 0.55), 'long-duration-short': (1.45, 1.55),
                  'middle-tempo': (0.72, 0.78), 'middle-tempo-short': (0.72, 0.78),
                  'double-tempo-short': (0.47, 0.53)}
        for case, (low, high) in bounds.items():
            ratios = [a/b for a, b in zip(cases[case]['onset_intervals_spc_cycles'], control)]
            if any(not low <= value <= high for value in ratios):
                raise ValueError('duration/tempo onset relationship differs from the fixture hypothesis')
            summaries.append({'model': model, 'case': case, 'onset_ratios_to_control': ratios,
                              'gate_ratios_to_control': [a/b for a, b in zip(
                                  cases[case]['gate_spc_cycles'], cases['control']['gate_spc_cycles'])]})
        for normal, short in (('half-duration', 'half-duration-short'),
                              ('long-duration', 'long-duration-short'),
                              ('middle-tempo', 'middle-tempo-short')):
            a, b = cases[normal], cases[short]
            if any(abs(x-y) > TOLERANCE for x, y in zip(a['onset_intervals_spc_cycles'], b['onset_intervals_spc_cycles'])):
                raise ValueError('articulation changed paired onset timing beyond one pulse')
            if any(not 0 < short_gate < gate for gate, short_gate in zip(a['gate_spc_cycles'], b['gate_spc_cycles'])):
                raise ValueError('short articulation failed to shorten every paired note gate')
    for case in CASES:
        a, b = [next(r for r in results if r['model'] == model and r['case'] == case)
                for model in ('sgb', 'sgb2')]
        for field in ('onset_intervals_spc_cycles', 'gate_spc_cycles'):
            if any(abs(x-y) > TOLERANCE for x, y in zip(a[field], b[field])):
                raise ValueError('two-model gate timing differs beyond one pulse')
    return summaries


def run(trace, firmware_directory, model, case):
    if case not in CASES:
        raise ValueError('unknown gate timing reference case')
    return run_fixture(trace, firmware_directory, model, case, build(case), *CASES[case])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--trace', type=Path, required=True)
    parser.add_argument('--firmware-dir', type=Path, required=True)
    args = parser.parse_args()
    try:
        results = [run(args.trace.resolve(), args.firmware_dir.resolve(), model, case)
                   for case in CASES for model in ('sgb', 'sgb2')]
        summaries = check_matrix(results)
    except (OSError, ValueError, TypeError, KeyError, subprocess.TimeoutExpired) as error:
        parser.exit(1, f'gate timing reference failed: {error}\n')
    print(json.dumps({'schema': 'gbb-gate-timing-reference-v1', 'qualification': False, 'playback': False,
                      'evidence': 'original_DSP_onsets_and_keyoffs_with_owned_parameter_fixtures',
                      'tolerance_spc_cycles': TOLERANCE, 'runs': results, 'comparisons': summaries}, indent=2))


if __name__ == '__main__':
    main()
