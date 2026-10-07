#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Validate bounded calibrated native gates against private SGB timing observations."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile

from build_sgb_score_gate import build
from check_sgb_score_render_reference import validate as validate_render, track_report, align_track
from check_sgb_timing_reference import CASES, check_matrix, run

PULSES = {(96, 127): 36, (192, 127): 18, (96, 63): 23}
TOLERANCE = 2048


def validate(report):
    if not isinstance(report, dict) or report.get('schema') != 'gbb-spc-score-gate-v1':
        raise ValueError('invalid native gate schema')
    normalized = {**report, 'schema': 'gbb-spc-score-render-v1'}
    validate_render(normalized, articulations=(127, 63))
    tempo = report.get('tempo')
    if type(tempo) is not int or not 0 <= tempo <= 255 or (report['status'] == 2 and tempo not in (96, 192)):
        raise ValueError('invalid native gate tempo')
    keyoffs = report.get('keyoff_half_cycles')
    if not isinstance(keyoffs, list) or len(keyoffs) != len(report['keyons']):
        raise ValueError('native gate key-off count differs')
    notes = [event for event in report['events'] if event['opcode'] != 0xC9]
    for note, keyon, off in zip(notes, report['keyons'], keyoffs):
        profile = (tempo, note['articulation'])
        following = next((event['half_cycle'] for event in report['events'] if event['tick'] > note['tick']),
                         30_000_000)
        if (type(off) is not int or note['duration'] != 16 or profile not in PULSES or
                not keyon['half_cycle'] < off < following):
            raise ValueError('invalid native gate profile or ordering')
        gate = (off-keyon['half_cycle'])/2
        # Polling and note setup occur after a timer pulse; allow their bounded latency.
        if not PULSES[profile]*2048-1024 <= gate <= PULSES[profile]*2048+256:
            raise ValueError('native gate countdown differs from the declared pulse profile')
    return report


def align(report, track):
    validate(report)
    align_track(track_report(report), track)


def native(probe, track, tempo):
    with tempfile.TemporaryDirectory(prefix='gbb-score-gate-') as directory:
        program, stream = [Path(directory)/name for name in ('program.bin', 'track.bin')]
        program.write_bytes(build())
        stream.write_bytes(track)
        completed = subprocess.run([str(probe.resolve()), str(program), str(stream), str(tempo)],
                                   capture_output=True, text=True, timeout=60)
        if completed.returncode != 0 or len(completed.stdout) > 32768:
            raise ValueError('native gate probe failed or exceeded report bound')
        return validate(json.loads(completed.stdout))


def fixture(case):
    if case not in CASES:
        raise ValueError('unknown gate fixture')
    articulation = CASES[case][1]
    return bytes((16, articulation, 0xC9, 0x98, 0x99, 0xA4, 8, 0xC9, 0))


def compare(candidates, references):
    check_matrix(references)
    if set(candidates) != set(CASES):
        raise ValueError('incomplete native articulation matrix')
    comparisons = []
    for result in references:
        report = candidates[result['case']]
        align(report, fixture(result['case']))
        if report['tempo'] != result['tempo']:
            raise ValueError('native/reference tempos differ')
        keyons = report['keyons']
        intervals = [(b['half_cycle']-a['half_cycle'])/2 for a, b in zip(keyons, keyons[1:])]
        gates = [(off-on['half_cycle'])/2 for on, off in zip(keyons, report['keyoff_half_cycles'])]
        differences = {}
        for name, observed, expected in (('onset', intervals, result['onset_intervals_spc_cycles']),
                                        ('gate', gates, result['gate_spc_cycles'])):
            differences[name] = [abs(a-b) for a, b in zip(observed, expected)]
            if len(observed) != len(expected) or any(value > TOLERANCE for value in differences[name]):
                raise ValueError(f'native {name} differs beyond one timer pulse')
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
        references = [run(args.trace.resolve(), args.firmware_dir.resolve(), model, case)
                      for model in ('sgb', 'sgb2') for case in CASES]
        comparisons = compare(candidates, references)
    except (OSError, ValueError, TypeError, KeyError, subprocess.TimeoutExpired) as error:
        parser.exit(1, f'native gate check failed: {error}\n')
    print(json.dumps({'schema': 'gbb-score-gate-reference-v1', 'qualification': False, 'playback': False,
                      'evidence': 'calibrated_duration16_native_DSP_onsets_and_keyoffs',
                      'native_program_sha256': hashlib.sha256(build()).hexdigest(),
                      'tolerance_spc_cycles': TOLERANCE,
                      'profiles': [{'tempo': tempo, 'articulation': art, 'timer_pulses': pulses}
                                   for (tempo, art), pulses in PULSES.items()],
                      'owned_pcm': {case: report['pcm'] for case, report in candidates.items()},
                      'comparisons': comparisons}, indent=2))


if __name__ == '__main__':
    main()
