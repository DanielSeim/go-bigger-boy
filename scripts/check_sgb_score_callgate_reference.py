#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check gated native finite-call audio against bounded original DSP events."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
from build_sgb_score_callgate import build
from build_sgb_calls_fixture import CASES, bank, build as fixture_build
from check_sgb_score_polygate_reference import validate as gate_validate, align as gate_align
from check_sgb_score_polygate_reference import native as gate_native, native_gates, observe as gate_observe
from check_sgb_score_calls_reference import pitches, observe as calls_observe, contract as calls_contract, compare as calls_compare
from check_sgb_phrase_reference import run as phrase_run
from check_sgb_score_duet_reference import integer

SCHEMA = 'gbb-spc-score-callgate-v1'


def validate(report):
    gate_validate(report, schema=SCHEMA, max_events=32, max_ticks=2032)


def align(report, data):
    gate_align(report, data, schema=SCHEMA, max_events=32, max_ticks=2032)


def native(probe, data, tempo=96):
    return gate_native(probe, data, tempo, builder=build, validator=validate, output_bound=32768)


def observe(source, case):
    # All notes have equal duration 16; each original release must be observed.
    # Missing KOF cannot be inferred from a pattern handoff in these fixtures.
    return gate_observe(source, onset_observer=lambda trace: calls_observe(trace, case),
                        expected_notes=2*len(pitches(case)), handoff_note_count=None)


def contract(result, case):
    calls_contract(result, case)
    notes = result.get('note_gates')
    if not isinstance(notes, list) or len(notes) != 2*len(pitches(case)) or any(
            not isinstance(note, dict) or note.get('voice') != 2+i%2
            or note.get('release_kind') != 'keyoff'
            or not integer(note.get('gate_spc_cycles'), 1, 200000) for i, note in enumerate(notes)):
        raise ValueError('incomplete original finite-call key-off evidence')


def run(trace, firmware_dir, model, case, articulation=127):
    result = phrase_run(trace, firmware_dir, model, case,
                        fixture_builder=lambda name: fixture_build(name, articulation),
                        observer=lambda source: observe(source, case), contract=contract, pattern_durations=None)
    result['articulation'] = articulation
    return result


def compare(candidates, references, articulation=127):
    if not isinstance(candidates, dict) or set(candidates) != set(CASES):
        raise ValueError('require all gated finite-call cases')
    for case, report in candidates.items():
        align(report, bank(case, articulation))
    if not isinstance(references, list) or any(not isinstance(reference, dict)
            or not integer(reference.get('articulation'), 63, 127)
            or reference['articulation'] != articulation for reference in references):
        raise ValueError('finite-call reference articulation differs')
    muted = {case: {**report, 'schema': 'gbb-spc-score-calls-v1'} for case, report in candidates.items()}
    comparisons = calls_compare(muted, references, fixture_bank=lambda case: bank(case, articulation))
    for comparison in comparisons:
        case, model = comparison['case'], comparison['model']
        reference = next(item for item in references if item['case'] == case and item['model'] == model)
        contract(reference, case)
        report = candidates[case]
        notes = native_gates(report)
        original = reference['note_gates']
        if len(notes) != len(original) or any(note['voice'] != other['voice'] or note['cause'] != 1
                for note, other in zip(notes, original)):
            raise ValueError('finite-call native/original release sequence differs')
        differences = [abs(note['gate_spc_cycles']-other['gate_spc_cycles']) for note, other in zip(notes, original)]
        if any(value > 4096 for value in differences):
            raise ValueError('finite-call gate outside existing reference allowance')
        onsets = report['keyons']
        intervals = [(b['half_cycle']-a['half_cycle'])/2 for a, b in zip(onsets, onsets[1:])]
        if len(intervals) != len(reference['onset_intervals_spc_cycles']) or any(
                abs(a-b) > 4096 for a, b in zip(intervals, reference['onset_intervals_spc_cycles'])):
            raise ValueError('finite-call native DSP onset outside existing allowance')
        comparison['native_DSP_intervals_spc_cycles'] = intervals
        comparison['native_gate_spc_cycles'] = [note['gate_spc_cycles'] for note in notes]
        comparison['absolute_gate_difference_spc_cycles'] = differences
    return comparisons


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--probe', type=Path, required=True)
    parser.add_argument('--trace', type=Path, required=True)
    parser.add_argument('--firmware-dir', type=Path, required=True)
    parser.add_argument('--articulation', type=int, choices=(63, 127), default=127)
    args = parser.parse_args()
    try:
        candidates = {case: native(args.probe.resolve(), bank(case, args.articulation)) for case in CASES}
        references = [run(args.trace.resolve(), args.firmware_dir.resolve(), model, case, args.articulation)
                      for case in CASES for model in ('sgb', 'sgb2')]
        comparisons = compare(candidates, references, args.articulation)
    except (OSError, ValueError, subprocess.TimeoutExpired) as error:
        parser.error(str(error))
    print(json.dumps({'schema': 'gbb-score-callgate-reference-v1', 'qualification': False, 'playback': False,
                      'program_sha256': hashlib.sha256(build()).hexdigest(), 'articulation': args.articulation,
                      'native': candidates, 'reference': references, 'comparisons': comparisons}, indent=2))


if __name__ == '__main__':
    main()
