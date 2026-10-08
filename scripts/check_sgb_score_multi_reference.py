#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Native bounded multi-event alignment and owned three-onset private checks."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile
from build_sgb_score_multi import build
from build_sgb_multi_fixture import CASES, bank, build as fixture_build
from check_sgb_phrase_reference import observe as phrase_observe, run as phrase_run
from schedule_sgb_score import schedule

PITCHES = ((1068, 1132), (1132, 1068), (2140, 2140))


def native(probe, data, tempo=96):
    with tempfile.TemporaryDirectory(prefix='gbb-score-multi-') as directory:
        program, stream = Path(directory)/'program.bin', Path(directory)/'bank.bin'
        program.write_bytes(build())
        stream.write_bytes(data)
        result = subprocess.run([str(probe), str(program), str(stream), str(tempo)],
                                capture_output=True, text=True, timeout=60)
        if result.returncode or len(result.stdout) > 16384:
            raise ValueError('native multi-event probe failed')
        report = json.loads(result.stdout)
    validate(report)
    return report


def validate(report, *, schema='gbb-spc-score-multi-v1', max_events=16, max_ticks=1016, min_events=4, initial_pair=True):
    if (not isinstance(report, dict) or report.get('schema') != schema
            or report.get('qualification') is not False or report.get('playback') is not False
            or report.get('reset_equal') is not True or report.get('restore_equal') is not True
            or type(report.get('status')) is not int or report['status'] not in (2, 0xE1, 0xE2)
            or type(report.get('end_tick')) is not int or not 0 <= report['end_tick'] <= max_ticks
            or not isinstance(report.get('events'), list)):
        raise ValueError('invalid multi-event report')
    events = report['events']
    if report['status'] != 2:
        if events or report['end_tick']:
            raise ValueError('rejected bank emitted events')
        return
    if not min_events <= len(events) <= max_events:
        raise ValueError('multi-event count outside bound')
    last_tick, last_half = -1, -1
    for event in events:
        if not isinstance(event, dict) or any(type(event.get(key)) is not int for key in
                ('tick', 'opcode', 'duration', 'channel', 'half_cycle')):
            raise ValueError('invalid multi-event fields')
        if (event['channel'] not in (2, 3) or not 1 <= event['duration'] <= 127
                or not (0x80 <= event['opcode'] < 0xC8 or event['opcode'] == 0xC9)
                or not last_tick <= event['tick'] < report['end_tick']
                or not last_half < event['half_cycle'] <= 30_000_000):
            raise ValueError('invalid multi-event order/bounds')
        last_tick, last_half = event['tick'], event['half_cycle']
    if initial_pair and [(event['tick'], event['channel']) for event in events[:2]] != [(0, 2), (0, 3)]:
        raise ValueError('missing initial track pair')


def align(report, data, *, schema='gbb-spc-score-multi-v1', max_events=16, max_ticks=1016, min_events=4, initial_pair=True, inherit_timing=False):
    validate(report, schema=schema, max_events=max_events, max_ticks=max_ticks, min_events=min_events, initial_pair=initial_pair)
    expected = schedule(data, int.from_bytes(data[:2], 'little'), inherit_timing=inherit_timing)
    timed = [event for event in expected['events'] if event['kind'] in ('note', 'rest')]
    if report['status'] != 2 or report['end_tick'] != expected['ticks'] or len(timed) != len(report['events']):
        raise ValueError('native/symbolic multi-event timeline differs')
    for actual, event in zip(report['events'], timed):
        opcode = 0xC9 if event['kind'] == 'rest' else 0x80+event['note']
        if any(actual[key] != value for key, value in (('tick', event['tick']), ('channel', event['channel']),
                                                      ('opcode', opcode), ('duration', event['duration']))):
            raise ValueError('native/symbolic multi-event differs')


def observe(source):
    return phrase_observe(source, PITCHES)


def contract(result, case):
    values = result['onset_intervals_spc_cycles']
    bounds = ((40000, 46000), (40000, 46000) if case != 'both-long' else (84000, 90000))
    if len(values) != 2 or any(not low <= value <= high for value, (low, high) in zip(values, bounds)):
        raise ValueError('multi-event reference interval outside fixture contract')


def run(trace, firmware_dir, model, case):
    result = phrase_run(trace, firmware_dir, model, case,
                        fixture_builder=fixture_build, observer=observe, contract=contract)
    result.pop('first_pattern_durations')
    result['first_track_total_durations'] = [8+duration for duration in CASES[case]]
    return result


def intervals(report):
    pairs = []
    for event in report['events']:
        if event['channel'] == 2:
            pairs.append(event['half_cycle'])
    if len(pairs) != 3:
        raise ValueError('fixture needs three channel-2 onsets')
    return [(b-a)/2 for a, b in zip(pairs, pairs[1:])]


def compare(candidates, references):
    if not isinstance(candidates, dict) or set(candidates) != set(CASES) or len(references) != 6:
        raise ValueError('require three native cases and six reference runs')
    for case, report in candidates.items():
        align(report, bank(case))
    seen, comparisons = set(), []
    expected = [{'mask': 12, 'voices': [{'voice': voice+2, 'srcn': 2, 'pitch': pitch}
                 for voice, pitch in enumerate(pitches)]} for pitches in PITCHES]
    for reference in references:
        if not isinstance(reference, dict):
            raise ValueError('invalid multi-event reference')
        key = (reference.get('model'), reference.get('case'))
        if key in seen or key[0] not in ('sgb', 'sgb2') or key[1] not in CASES:
            raise ValueError('duplicate or unknown multi-event reference')
        seen.add(key)
        actual, original = intervals(candidates[key[1]]), reference.get('onset_intervals_spc_cycles')
        if (reference.get('keyons') != expected or not isinstance(original, list) or len(original) != 2
                or any(type(value) is not int for value in original)):
            raise ValueError('invalid multi-event reference onsets')
        contract(reference, key[1])
        if any(abs(a-b) > 4096 for a, b in zip(actual, original)):
            raise ValueError('native multi-event interval outside timer allowance')
        comparisons.append({'model': key[0], 'case': key[1], 'native_intervals_spc_cycles': actual,
                            'reference_intervals_spc_cycles': original})
    for case in CASES:
        first, second = [item['reference_intervals_spc_cycles'] for item in comparisons if item['case'] == case]
        if any(abs(a-b) > 2048 for a, b in zip(first, second)):
            raise ValueError('two-model multi-event interval differs')
    return comparisons


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--probe', type=Path, required=True)
    parser.add_argument('--trace', type=Path, required=True)
    parser.add_argument('--firmware-dir', type=Path, required=True)
    args = parser.parse_args()
    try:
        candidates = {case: native(args.probe.resolve(), bank(case)) for case in CASES}
        references = [run(args.trace.resolve(), args.firmware_dir.resolve(), model, case)
                      for case in CASES for model in ('sgb', 'sgb2')]
        comparisons = compare(candidates, references)
    except (OSError, ValueError, subprocess.TimeoutExpired) as error:
        parser.error(str(error))
    print(json.dumps({'schema': 'gbb-score-multi-reference-v1', 'qualification': False, 'playback': False,
                      'program_sha256': hashlib.sha256(build()).hexdigest(), 'native': candidates,
                      'reference': references, 'comparisons': comparisons}, indent=2))


if __name__ == '__main__':
    main()
