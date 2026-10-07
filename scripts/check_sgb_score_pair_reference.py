#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Compare isolated native pair transitions with owned-score private observations."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile
from build_sgb_score_pair import build
from build_sgb_phrase_fixture import CASES, score_payload
from check_sgb_phrase_reference import run
from schedule_sgb_score import schedule


def fixture(case):
    a, b = CASES[case]
    return bytes((a, 0x98, b, 0x99, 16, 0xA4, 16, 0xA4))


def native(probe, pair, tempo=96):
    with tempfile.TemporaryDirectory(prefix='gbb-score-pair-') as directory:
        program = Path(directory)/'program.bin'
        stream = Path(directory)/'pair.bin'
        program.write_bytes(build())
        stream.write_bytes(pair)
        result = subprocess.run([str(probe), str(program), str(stream), str(tempo)],
                                capture_output=True, text=True, timeout=60)
        if result.returncode or len(result.stdout) > 16384:
            raise ValueError(f'native pair probe failed: {result.stderr[:1000]}')
        report = json.loads(result.stdout)
    validate(report)
    return report


def validate(report):
    if (not isinstance(report, dict) or report.get('schema') != 'gbb-spc-score-pair-v1'
            or report.get('qualification') is not False or report.get('playback') is not False
            or report.get('reset_equal') is not True or report.get('restore_equal') is not True
            or type(report.get('status')) is not int or report['status'] not in (2, 0xE1, 0xE2)
            or type(report.get('end_tick')) is not int or not 0 <= report['end_tick'] <= 254
            or not isinstance(report.get('events'), list)):
        raise ValueError('invalid native pair report')
    events = report['events']
    if report['status'] != 2:
        if events or report['end_tick']:
            raise ValueError('rejected pair emitted events')
        return
    if len(events) != 4:
        raise ValueError('pair must emit exactly four events')
    previous = -1
    for index, event in enumerate(events):
        if not isinstance(event, dict) or any(type(event.get(key)) is not int for key in
                ('tick', 'opcode', 'duration', 'channel', 'half_cycle')):
            raise ValueError('invalid pair event fields')
        if (event['channel'] != 2 + index % 2 or not 1 <= event['duration'] <= 127
                or not (0x80 <= event['opcode'] < 0xC8 or event['opcode'] == 0xC9)
                or not 0 <= event['tick'] < report['end_tick']
                or not previous < event['half_cycle'] <= 30_000_000):
            raise ValueError('invalid pair event bounds or order')
        previous = event['half_cycle']
    if (events[0]['tick'] != 0 or events[1]['tick'] != 0
            or events[2]['tick'] != min(events[0]['duration'], events[1]['duration'])
            or events[3]['tick'] != events[2]['tick']
            or report['end_tick'] != events[2]['tick'] + min(events[2]['duration'], events[3]['duration'])):
        raise ValueError('pair does not follow first-ending-track semantics')


def align(report, score):
    validate(report)
    expected = [event for event in score['events'] if event['kind'] in ('note', 'rest')]
    observed = report['events']
    if report['status'] != 2 or report['end_tick'] != score['ticks'] or len(expected) != len(observed):
        raise ValueError('native pair differs from symbolic score')
    for actual, event in zip(observed, expected):
        opcode = 0xC9 if event['kind'] == 'rest' else 0x80 + event['note']
        if any(actual[key] != value for key, value in (
                ('tick', event['tick']), ('channel', event['channel']),
                ('duration', event['duration']), ('opcode', opcode))):
            raise ValueError('native pair event differs from symbolic score')


def compare(candidates, references):
    if set(candidates) != set(CASES) or len(references) != 6:
        raise ValueError('require all three native cases and six reference runs')
    seen = set()
    comparisons = []
    for reference in references:
        if not isinstance(reference, dict):
            raise ValueError('invalid pair reference')
        key = (reference.get('model'), reference.get('case'))
        if key in seen or key[0] not in ('sgb', 'sgb2') or key[1] not in CASES:
            raise ValueError('duplicate or unknown pair reference')
        seen.add(key)
        report = candidates[key[1]]
        payload = score_payload(key[1])
        bank = payload[4:4 + int.from_bytes(payload[:2], 'little')]
        align(report, schedule(bank, 0x2B10))
        interval = (report['events'][2]['half_cycle'] - report['events'][0]['half_cycle']) / 2
        original = reference.get('pattern_interval_spc_cycles')
        low, high = (170000, 180000) if key[1] == 'both-long' else (84000, 90000)
        if type(original) is not int or not low <= original <= high or abs(interval-original) > 4096:
            raise ValueError('native pattern transition outside timer quantization allowance')
        comparisons.append({'model': key[0], 'case': key[1],
                            'native_pattern_interval_spc_cycles': interval,
                            'reference_pattern_interval_spc_cycles': original})
    for case in CASES:
        intervals = [item['reference_pattern_interval_spc_cycles'] for item in comparisons
                     if item['case'] == case]
        if abs(intervals[0]-intervals[1]) > 2048:
            raise ValueError('two-model reference phrase transition differs')
    return comparisons


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
    print(json.dumps({'schema': 'gbb-score-pair-reference-v1', 'qualification': False,
                      'playback': False, 'program_sha256': hashlib.sha256(build()).hexdigest(),
                      'native': candidates, 'reference': references, 'comparisons': comparisons}, indent=2))


if __name__ == '__main__':
    main()
