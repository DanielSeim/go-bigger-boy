#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check muted native finite calls against symbolic and bounded original onsets."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile
from build_sgb_score_calls import build
from build_sgb_calls_fixture import CASES, bank, build as fixture_build
from check_sgb_score_multi_reference import validate as multi_validate, align as multi_align
from check_sgb_phrase_reference import observe as phrase_observe, run as phrase_run

SCHEMA = 'gbb-spc-score-calls-v1'


def validate(report):
    multi_validate(report, schema=SCHEMA, max_events=32, max_ticks=2032)


def align(report, data):
    multi_align(report, data, schema=SCHEMA, max_events=32, max_ticks=2032)


def native(probe, data, tempo=96):
    with tempfile.TemporaryDirectory(prefix='gbb-score-calls-') as directory:
        program, stream = Path(directory)/'program.bin', Path(directory)/'bank.bin'
        program.write_bytes(build())
        stream.write_bytes(data)
        result = subprocess.run([str(probe), str(program), str(stream), str(tempo)],
                                capture_output=True, text=True, timeout=60)
        if result.returncode or len(result.stdout) > 16384:
            raise ValueError('native finite-call probe failed')
        report = json.loads(result.stdout)
    validate(report)
    return report


def pitches(case):
    return tuple((pitch, pitch) for pitch in [1068, 1132]*CASES[case]+[2140, 1068, 1132, 2140])


def observe(source, case):
    return phrase_observe(source, pitches(case))


def contract(result, case):
    intervals = result.get('onset_intervals_spc_cycles')
    if not isinstance(intervals, list) or len(intervals) != len(pitches(case))-1 or any(
            type(value) is not int or not 84000 <= value <= 92000 for value in intervals):
        raise ValueError('finite-call reference onset outside fixture contract')


def run(trace, firmware_dir, model, case):
    return phrase_run(trace, firmware_dir, model, case, fixture_builder=fixture_build,
                      observer=lambda source: observe(source, case), contract=contract,
                      pattern_durations=None)


def compare(candidates, references, *, fixture_bank=bank):
    if not isinstance(candidates, dict) or set(candidates) != set(CASES):
        raise ValueError('require all native finite-call cases')
    if not isinstance(references, list) or len(references) != 6:
        raise ValueError('require both original models for each finite-call case')
    pairs = set()
    comparisons = []
    for reference in references:
        if not isinstance(reference, dict):
            raise ValueError('invalid finite-call reference')
        model, case = reference.get('model'), reference.get('case')
        if model not in ('sgb', 'sgb2') or case not in CASES or (model, case) in pairs:
            raise ValueError('invalid or duplicate finite-call reference')
        pairs.add((model, case))
        expected = [{'mask': 12, 'voices': [{'voice': channel, 'srcn': 2, 'pitch': pitch}
                     for channel, pitch in zip((2, 3), pair)]} for pair in pitches(case)]
        if reference.get('keyons') != expected:
            raise ValueError('finite-call original repeat/return sequence differs')
        contract(reference, case)
        report = candidates[case]
        align(report, fixture_bank(case))
        onsets = [event['half_cycle'] for event in report['events'] if event['channel'] == 2]
        intervals = [(b-a)/2 for a, b in zip(onsets, onsets[1:])]
        actual = reference['onset_intervals_spc_cycles']
        if len(intervals) != len(actual) or any(abs(a-b) > 4096 for a, b in zip(intervals, actual)):
            raise ValueError('native/reference finite-call onset outside allowance')
        comparisons.append({'model': model, 'case': case, 'native_event_intervals_spc_cycles': intervals,
                            'reference_DSP_intervals_spc_cycles': actual})
    for case in CASES:
        intervals = [reference['onset_intervals_spc_cycles'] for reference in references if reference['case'] == case]
        if any(abs(a-b) > 2048 for a, b in zip(*intervals)):
            raise ValueError('two-model finite-call reference timing differs')
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
    print(json.dumps({'schema': 'gbb-score-calls-reference-v1', 'qualification': False, 'playback': False,
                      'program_sha256': hashlib.sha256(build()).hexdigest(), 'native': candidates,
                      'reference': references, 'comparisons': comparisons}, indent=2))


if __name__ == '__main__':
    main()
