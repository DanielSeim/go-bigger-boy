#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Compare owned symbolic score schedules with bounded original DSP observations."""
import argparse
import json
from pathlib import Path
import struct
import subprocess

import build_sgb_phrase_fixture as phrase_fixture
import build_sgb_subroutine_fixture as call_fixture
from check_sgb_phrase_reference import run as phrase_run
from check_sgb_subroutine_reference import run as call_run
from schedule_sgb_score import schedule

PITCHES = {24: 1068, 25: 1132, 36: 2140}


def compare(oracle, native, kind):
    groups = []
    ticks = []
    for note in (event for event in oracle['events'] if event['kind'] == 'note'):
        if note['note'] not in PITCHES or note['channel'] not in (2, 3):
            raise ValueError('fixture comparison has an unqualified pitch or channel')
        if not ticks or ticks[-1] != note['tick']:
            groups.append({'mask': 0, 'voices': []})
            ticks.append(note['tick'])
        group = groups[-1]
        bit = 1 << note['channel']
        if group['mask'] & bit:
            raise ValueError('duplicate fixture voice at one symbolic onset')
        group['mask'] |= bit
        group['voices'].append({'voice': note['channel'], 'srcn': 2, 'pitch': PITCHES[note['note']]})
    if kind == 'phrase':
        expected = native['keyons']
        reference = [native['pattern_interval_spc_cycles']]
        high = 90000
    elif kind == 'subroutine':
        expected = [{'mask': 4, 'voices': [{'voice': 2, 'srcn': 2, 'pitch': pitch}]} for pitch in native['pitches']]
        reference = native['onset_intervals_spc_cycles']
        high = 92000
    else:
        raise ValueError('unknown scheduler comparison kind')
    intervals = [b-a for a, b in zip(ticks, ticks[1:])]
    if groups != expected or len(intervals) != len(reference):
        raise ValueError('symbolic notes differ from original DSP key-ons')
    # Normalize to the measured duration-16 ranges, without defining a general
    # tempo conversion or treating tick positions as hardware cycle timestamps.
    if any(tick_delta <= 0 or not 84000 <= cycles*16/tick_delta <= high
           for tick_delta, cycles in zip(intervals, reference)):
        raise ValueError('symbolic onset spacing differs from original timing')
    return {'symbolic_ticks': oracle['ticks'], 'symbolic_onset_ticks': ticks,
            'symbolic_onset_intervals': intervals, 'note_groups': groups,
            'reference_onset_intervals_spc_cycles': reference,
            'patterns': oracle['patterns']}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--trace', type=Path, required=True)
    parser.add_argument('--firmware-dir', type=Path, required=True)
    args = parser.parse_args()
    try:
        results = []
        for kind, fixture, runner in (('phrase', phrase_fixture, phrase_run),
                                      ('subroutine', call_fixture, call_run)):
            for case in fixture.CASES:
                payload = fixture.score_payload(case)
                size, base = struct.unpack_from('<HH', payload)
                if base != 0x2B00:
                    raise ValueError('fixture bank has an unexpected base')
                oracle = schedule(payload[4:4+size], 0x2B10)
                for model in ('sgb', 'sgb2'):
                    native = runner(args.trace.resolve(), args.firmware_dir.resolve(), model, case)
                    pins = {key: value for key, value in native.items() if key.endswith('_sha256')}
                    results.append({'kind': kind, 'case': case, 'model': model, **pins,
                                    **compare(oracle, native, kind)})
    except (OSError, ValueError, subprocess.TimeoutExpired) as error:
        parser.error(str(error))
    print(json.dumps({'schema': 'gbb-sgb-scheduler-reference-v1', 'qualification': False,
                      'playback': False, 'evidence': 'owned_symbolic_schedule_and_original_DSP_writes',
                      'runs': results}, indent=2))


if __name__ == '__main__':
    main()
