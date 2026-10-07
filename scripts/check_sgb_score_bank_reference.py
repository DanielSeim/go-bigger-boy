#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check page-crossing owned banks, immutable source and two-model DSP evidence."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
from build_sgb_score_bank import build
from build_sgb_bank_fixture import CASES, bank, build as fixture_build
from check_sgb_score_envelope_reference import validate as envelope_validate, align as envelope_align, mix_report, SCHEMA as ENVELOPE_SCHEMA
from check_sgb_score_mix_reference import compare as mix_compare, observe, contract, KEYS
from check_sgb_score_polygate_reference import native as gate_native
from check_sgb_phrase_reference import run as phrase_run

SCHEMA = 'gbb-spc-score-bank-v1'


def envelope_report(report):
    if not isinstance(report, dict) or report.get('schema') != SCHEMA or report.get('source_unmodified') is not True or report.get('cache_guards_equal') is not True:
        raise ValueError('invalid wide-bank schema/source/cache preservation')
    return {**report, 'schema': ENVELOPE_SCHEMA}


def validate(report):
    envelope_validate(envelope_report(report))


def align(report, data):
    envelope_align(envelope_report(report), data)


def native(probe, data, tempo=96):
    return gate_native(probe, data, tempo, builder=build, validator=validate, output_bound=65536)


def compare(candidates, references):
    if not isinstance(candidates, dict) or set(candidates) != set(KEYS):
        raise ValueError('require both wide-bank mix cases/articulations')
    for case in CASES:
        for art in (63, 127):
            align(candidates[f'{case}-{art}'], bank(art, case))
    # All relocated fixtures execute exactly the compact fixtures' grammar and
    # register timeline. The independent scheduler checks the relocated bytes
    # above; the strict original comparison pins the common observable sequence.
    from build_sgb_mix_fixture import bank as compact_bank
    for case in CASES:
        for art in (63, 127):
            envelope_align(envelope_report(candidates[f'{case}-{art}']), compact_bank(art, case))
    return mix_compare({key: mix_report(envelope_report(value)) for key, value in candidates.items()}, references)


def run(trace, firmware_dir, model, articulation, case):
    result = phrase_run(trace, firmware_dir, model, case,
                        fixture_builder=lambda name: fixture_build(articulation, name),
                        observer=lambda source: observe(source, case), contract=contract, pattern_durations=None)
    result.update(articulation=articulation, instruction_limit=8000000)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--probe', type=Path, required=True)
    parser.add_argument('--trace', type=Path, required=True)
    parser.add_argument('--firmware-dir', type=Path, required=True)
    args = parser.parse_args()
    try:
        candidates = {f'{case}-{art}':native(args.probe.resolve(), bank(art,case)) for case in CASES for art in (127,63)}
        references = [run(args.trace.resolve(), args.firmware_dir.resolve(), model, art, case)
                      for case in CASES for art in (127,63) for model in ('sgb','sgb2')]
        comparisons = compare(candidates,references)
    except (OSError,ValueError,subprocess.TimeoutExpired) as error:
        parser.error(str(error))
    print(json.dumps({'schema':'gbb-score-bank-reference-v1','qualification':False,'playback':False,
                      'program_sha256':hashlib.sha256(build()).hexdigest(),'native':candidates,
                      'reference':references,'comparisons':comparisons},indent=2))


if __name__ == '__main__':
    main()
