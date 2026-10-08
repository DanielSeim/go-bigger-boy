#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check instrument-2 setup, native envelope trajectories and private onsets."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
from build_sgb_score_envelope import build
from build_sgb_mix_fixture import CASES, bank
from check_sgb_score_mix_reference import validate as mix_validate, align as mix_align, compare as mix_compare, run, SCHEMA as MIX_SCHEMA
from check_sgb_score_polygate_reference import native as gate_native
from check_sgb_score_duet_reference import integer

SCHEMA = 'gbb-spc-score-envelope-v1'
SETUP = [2, 0x8F, 0x6F, 0xB8]


def mix_report(report):
    if not isinstance(report, dict) or report.get('schema') != SCHEMA:
        raise ValueError('invalid envelope report schema')
    return {**report, 'schema': MIX_SCHEMA}


def validate(report, *, max_events=32, max_ticks=2032, pattern_ticks=None):
    mix_validate(mix_report(report), max_events=max_events, max_ticks=max_ticks, pattern_ticks=pattern_ticks)
    envelopes = report.get('envelopes')
    if not isinstance(envelopes, list) or len(envelopes) > max_events:
        raise ValueError('invalid envelope trajectory list')
    if report['status'] != 2:
        if envelopes:
            raise ValueError('rejected bank ran envelopes')
        return
    for env in envelopes:
        if not isinstance(env, dict) or not integer(env.get('voice'), 2, 3) or any(
                not integer(env.get(key), 1, report['completion_half_cycle']) for key in ('on_half_cycle', 'off_half_cycle')):
            raise ValueError('invalid envelope voice/timestamps')
    active, index = {}, 0
    edges = sorted([(e['half_cycle'], True, e) for e in report['keyons']]+
                   [(e['half_cycle'], False, e) for e in report['keyoffs']])
    for half, is_on, edge in edges:
        setup = edge.get('instrument_setup')
        if not isinstance(setup, list) or len(setup) != 2 or any(pair != SETUP or not isinstance(pair, list) or
                any(type(value) is not int for value in pair) for pair in setup):
            raise ValueError('actual source/ADSR/gain differs from measured instrument 2')
        for voice in (2, 3):
            if not edge['mask'] & (1 << voice):
                continue
            if is_on:
                if index >= len(envelopes):
                    raise ValueError('missing envelope trajectory')
                env = envelopes[index]
                index += 1
                if not isinstance(env, dict) or not integer(env.get('voice'), 2, 3) or env['voice'] != voice or not integer(
                        env.get('on_half_cycle'), half, half):
                    raise ValueError('envelope trajectory onset differs')
                if env.get('attack_zero') is not True or not integer(env.get('peak'), 127, 127) or not integer(
                        env.get('decay_min'), 1, 126) or not integer(env.get('off_env'), 1, 127) or not integer(
                        env.get('release_steps'), 0, env['off_env']) or type(env.get('release_zero')) is not bool:
                    raise ValueError('invalid ADSR attack/decay/release observations')
                active[voice] = env
            else:
                env = active.pop(voice, None)
                if env is None or not integer(env.get('off_half_cycle'), half, half):
                    raise ValueError('envelope trajectory release differs')
                next_on = next((e['on_half_cycle'] for e in envelopes if e['voice'] == voice and e['on_half_cycle'] > half),
                               report['completion_half_cycle']+40000)
                # ENVX hides four low envelope bits. Release subtracts eight per
                # sample; allow two DSP frames for KOF/ENVX latch visibility.
                settle_bound = (env['off_env']+1)*128+128
                if edge['cause'] == 1 and not env['release_steps']:
                    raise ValueError('timer-gated envelope did not release')
                if next_on-half >= settle_bound and not env['release_zero']:
                    raise ValueError('uninterrupted envelope did not settle')
    if active or index != len(envelopes):
        raise ValueError('extra or unreleased envelope trajectories')


def align(report, data, *, max_events=32, max_ticks=2032, pattern_ticks=None):
    validate(report, max_events=max_events, max_ticks=max_ticks, pattern_ticks=pattern_ticks)
    mix_align(mix_report(report), data, max_events=max_events, max_ticks=max_ticks, pattern_ticks=pattern_ticks)


def native(probe, data, tempo=96):
    return gate_native(probe, data, tempo, builder=build, validator=validate, output_bound=65536)


def compare(candidates, references):
    if not isinstance(candidates, dict):
        raise ValueError('invalid envelope candidates')
    for report in candidates.values():
        validate(report)
    # The strict mix observer already pins instrument setup at every original
    # onset. This driver additionally observes matching setup on the native side.
    return mix_compare({key: mix_report(value) for key, value in candidates.items()}, references)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--probe', type=Path, required=True)
    parser.add_argument('--trace', type=Path, required=True)
    parser.add_argument('--firmware-dir', type=Path, required=True)
    args = parser.parse_args()
    try:
        candidates = {f'{case}-{art}': native(args.probe.resolve(), bank(art, case)) for case in CASES for art in (127, 63)}
        references = [run(args.trace.resolve(), args.firmware_dir.resolve(), model, art, case)
                      for case in CASES for art in (127, 63) for model in ('sgb', 'sgb2')]
        comparisons = compare(candidates, references)
    except (OSError, ValueError, subprocess.TimeoutExpired) as error:
        parser.error(str(error))
    print(json.dumps({'schema': 'gbb-score-envelope-reference-v1', 'qualification': False, 'playback': False,
                      'program_sha256': hashlib.sha256(build()).hexdigest(), 'instrument_setup': SETUP,
                      'native': candidates, 'reference': references, 'comparisons': comparisons}, indent=2))


if __name__ == '__main__':
    main()
