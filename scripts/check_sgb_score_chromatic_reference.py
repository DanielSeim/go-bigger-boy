#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check measured chromatic pitches, gates and calls against both original models."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
from build_sgb_score_chromatic import build
from build_sgb_chromatic_fixture import CASES, bank
from check_sgb_chromatic_reference import PITCHES, run, contract, check_models
from check_sgb_score_polygate_reference import validate as gate_validate, align as gate_align, native as gate_native, native_gates
from check_sgb_score_duet_reference import integer

SCHEMA = 'gbb-spc-score-chromatic-v1'
PITCH = {0x80+note: value for note, value in PITCHES.items()}
KEYS = tuple(f'{case}-{art}' for case in CASES for art in (127, 63))


def validate(report):
    gate_validate(report, schema=SCHEMA, max_events=32, max_ticks=2032, pitch_table=PITCH)


def align(report, data):
    gate_align(report, data, schema=SCHEMA, max_events=32, max_ticks=2032, pitch_table=PITCH)


def native(probe, data, tempo=96):
    return gate_native(probe, data, tempo, builder=build, validator=validate, output_bound=32768)


def compare(candidates, references):
    if not isinstance(candidates, dict) or set(candidates) != set(KEYS):
        raise ValueError('require chromatic and call cases with both articulations')
    if not isinstance(references, list) or len(references) != 8:
        raise ValueError('require both models for every chromatic case')
    seen, comparisons = set(), []
    for reference in references:
        if not isinstance(reference, dict):
            raise ValueError('invalid chromatic reference')
        case, art, model = reference.get('case'), reference.get('articulation'), reference.get('model')
        if case not in CASES or not integer(art, 63, 127) or art not in (63, 127) or model not in ('sgb', 'sgb2') or (case, art, model) in seen:
            raise ValueError('invalid or duplicate chromatic reference case')
        seen.add((case, art, model))
        contract(reference, case)
        report = candidates[f'{case}-{art}']
        align(report, bank(art, case))
        actual = native_gates(report)
        original = reference['note_gates']
        if len(actual) != len(original) or any(note['cause'] != 1 or note['voice'] != other['voice']
                for note, other in zip(actual, original)):
            raise ValueError('chromatic native/reference release sequence differs')
        differences = [abs(note['gate_spc_cycles']-other['gate_spc_cycles']) for note, other in zip(actual, original)]
        if any(value > 4096 for value in differences):
            raise ValueError('chromatic gate outside existing reference allowance')
        expected_pairs = [[PITCHES[note]]*2 for note in CASES[case]]
        ons = report['keyons']
        if [edge['pitches'] for edge in ons] != expected_pairs or any(edge['mask'] != 12 for edge in ons):
            raise ValueError('native chromatic onset pitches/count/masks differ')
        intervals = [(b['half_cycle']-a['half_cycle'])/2 for a, b in zip(ons, ons[1:])]
        original_intervals = reference['onset_intervals_spc_cycles']
        if len(intervals) != len(original_intervals) or any(abs(a-b) > 4096 for a, b in zip(intervals, original_intervals)):
            raise ValueError('chromatic DSP onset outside existing reference allowance')
        comparisons.append({'case': case, 'articulation': art, 'model': model,
                            'native_DSP_intervals_spc_cycles': intervals,
                            'reference_DSP_intervals_spc_cycles': original_intervals,
                            'absolute_gate_difference_spc_cycles': differences})
    for case in CASES:
        for art in (127, 63):
            check_models([reference for reference in references if reference['case'] == case and reference['articulation'] == art])
    return comparisons


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--probe', type=Path, required=True)
    parser.add_argument('--trace', type=Path, required=True)
    parser.add_argument('--firmware-dir', type=Path, required=True)
    args = parser.parse_args()
    try:
        candidates = {f'{case}-{art}': native(args.probe.resolve(), bank(art, case))
                      for case in CASES for art in (127, 63)}
        references = [run(args.trace.resolve(), args.firmware_dir.resolve(), model, art, case)
                      for case in CASES for art in (127, 63) for model in ('sgb', 'sgb2')]
        comparisons = compare(candidates, references)
    except (OSError, ValueError, subprocess.TimeoutExpired) as error:
        parser.error(str(error))
    print(json.dumps({'schema': 'gbb-score-chromatic-reference-v1', 'qualification': False, 'playback': False,
                      'program_sha256': hashlib.sha256(build()).hexdigest(), 'pitch_table': PITCHES,
                      'native': candidates, 'reference': references, 'comparisons': comparisons}, indent=2))


if __name__ == '__main__':
    main()
