#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Compare owned native flat-track events with symbolic and bounded private timing evidence."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import tempfile

from build_sgb_score_track import build
from check_sgb_timing_reference import CASES, check_matrix, run
from schedule_sgb_score import schedule

TOLERANCE = 2048


def symbolic(track):
    # An owned one-pattern/channel-2 wrapper feeds the existing independent oracle.
    bank = bytearray(48)
    struct.pack_into('<HH', bank, 0, 0x2B10, 0)
    struct.pack_into('<H', bank, 20, 0x2B30)
    bank.extend(track)
    return schedule(bytes(bank), 0x2B00)


def validate(report):
    if (not isinstance(report, dict) or report.get('schema') != 'gbb-spc-score-track-v1' or
            any(report.get(key) is not False for key in ('qualification', 'playback')) or
            any(report.get(key) is not True for key in ('reset_equal', 'restore_equal')) or
            report.get('status') not in (2, 0xE1, 0xE2) or
            type(report.get('end_tick')) is not int or not 0 <= report['end_tick'] <= 2032):
        raise ValueError('invalid native track report')
    events = report.get('events')
    if not isinstance(events, list) or len(events) > 16:
        raise ValueError('invalid native event count')
    previous_tick, previous_half = -1, 0
    for event in events:
        if not isinstance(event, dict) or any(type(event.get(key)) is not int for key in
                ('tick', 'opcode', 'duration', 'articulation', 'half_cycle')):
            raise ValueError('invalid native event fields')
        if (not previous_tick < event['tick'] < report['end_tick'] or
                not previous_half < event['half_cycle'] <= 30_000_000 or
                not (0x80 <= event['opcode'] < 0xC8 or event['opcode'] == 0xC9) or
                not 1 <= event['duration'] <= 127 or not 0 <= event['articulation'] <= 127):
            raise ValueError('invalid native event range/order')
        previous_tick, previous_half = event['tick'], event['half_cycle']
    if report['status'] != 2:
        if events or report['end_tick'] != 0:
            raise ValueError('rejected track emitted events')
    elif not events or events[0]['tick'] != 0 or any(
            a['tick']+a['duration'] != b['tick'] for a, b in zip(events, events[1:])) or (
            events[-1]['tick']+events[-1]['duration'] != report['end_tick']):
        raise ValueError('native event durations do not close the track')
    return report


def native(probe, track, tempo):
    image = build()
    with tempfile.TemporaryDirectory(prefix='gbb-score-track-') as directory:
        program, stream = [Path(directory)/name for name in ('program.bin', 'track.bin')]
        program.write_bytes(image)
        stream.write_bytes(track)
        completed = subprocess.run([str(probe.resolve()), str(program), str(stream), str(tempo)],
                                   capture_output=True, text=True, timeout=60)
        if completed.returncode != 0 or len(completed.stdout) > 32768:
            raise ValueError('native track probe failed or exceeded report bound')
        report = validate(json.loads(completed.stdout))
    return report


def align(report, track):
    validate(report)
    oracle = symbolic(track)
    expected = [{'tick': event['tick'], 'opcode': (0xC9 if event['kind'] == 'rest' else 0x80+event['note']),
                 'duration': event['duration'], 'articulation': event['articulation']}
                for event in oracle['events'] if event['kind'] in ('note', 'rest')]
    actual = [{key: value for key, value in event.items() if key != 'half_cycle'} for event in report['events']]
    if report['status'] != 2 or oracle['ticks'] != report['end_tick'] or actual != expected:
        raise ValueError('native flat-track timeline differs from symbolic oracle')


def fixture(case):
    # Owned duration-16 warmup rest removes startup phase from interval comparison.
    articulation = CASES[case][1]
    return bytes((16, articulation, 0xC9, 0x98, 0x99, 0xA4, 1, 0xC9, 0))


def compare(candidates, references):
    check_matrix(references)
    if set(candidates) != set(CASES):
        raise ValueError('incomplete native timing matrix')
    comparisons = []
    for result in references:
        report = candidates[result['case']]
        align(report, fixture(result['case']))
        notes = [event for event in report['events'] if event['opcode'] != 0xC9]
        intervals = [(b['half_cycle']-a['half_cycle'])/2 for a, b in zip(notes, notes[1:])]
        differences = [abs(a-b) for a, b in zip(intervals, result['onset_intervals_spc_cycles'])]
        if len(intervals) != 2 or any(value > TOLERANCE for value in differences):
            raise ValueError('native note spacing differs beyond one timer pulse')
        comparisons.append({**result, 'native_note_intervals_spc_cycles': intervals,
                            'absolute_difference_spc_cycles': differences})
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
        parser.exit(1, f'native track check failed: {error}\n')
    print(json.dumps({'schema': 'gbb-score-track-reference-v1', 'qualification': False,
                      'playback': False, 'evidence': 'native_event_log_vs_original_DSP_onsets',
                      'native_program_sha256': hashlib.sha256(build()).hexdigest(),
                      'tolerance_spc_cycles': TOLERANCE, 'comparisons': comparisons}, indent=2))


if __name__ == '__main__':
    main()
