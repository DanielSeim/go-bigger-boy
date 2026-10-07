#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Validate owned two-voice audio; compare only onset masks, pitches and intervals."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile
from build_sgb_score_duet import build
from build_sgb_phrase_fixture import CASES
from check_sgb_phrase_reference import run
from check_sgb_score_phrase_reference import fixture, align as phrase_align
from check_sgb_score_pair_reference import validate as pair_validate, compare as pair_compare

PITCH = {0x98: 1068, 0x99: 1132, 0xA4: 2140}


def pair_report(report):
    if not isinstance(report, dict) or report.get('schema') != 'gbb-spc-score-duet-v1':
        raise ValueError('invalid duet schema')
    return {**report, 'schema': 'gbb-spc-score-pair-v1'}


def integer(value, low, high):
    return type(value) is int and low <= value <= high


def validate(report):
    pair_validate(pair_report(report))
    if not integer(report.get('tempo'), 0, 255) or not integer(report.get('completion_half_cycle'), 1, 30_000_000):
        raise ValueError('invalid duet tempo/completion')
    for name in ('keyons', 'keyoffs'):
        values = report.get(name)
        if not isinstance(values, list) or len(values) > 2:
            raise ValueError('invalid duet edges')
        for edge in values:
            if (not isinstance(edge, dict) or not integer(edge.get('half_cycle'), 1, report['completion_half_cycle'])
                    or not integer(edge.get('tick'), 0, report['end_tick'])
                    or not integer(edge.get('mask'), 4, 12) or edge['mask'] not in (4, 8, 12)
                    or not isinstance(edge.get('pitches'), list) or len(edge['pitches']) != 2
                    or any(not integer(pitch, 0, 0x3FFF) for pitch in edge['pitches'])):
                raise ValueError('invalid duet edge fields')
    pcm, steady = report.get('pcm'), report.get('second_tail_pcm')
    if not isinstance(pcm, dict) or not isinstance(steady, dict):
        raise ValueError('missing duet PCM metadata')
    if (not integer(pcm.get('frames'), 1, 500000) or not integer(pcm.get('fnv1a64'), 0, 2**64-1)
            or not integer(pcm.get('peak'), 0, 32768)
            or not integer(pcm.get('quiet_tail_frames'), 64, pcm['frames'])
            or not integer(steady.get('frames'), 0, pcm['frames'])):
        raise ValueError('invalid duet PCM bounds')
    for key in ('left_nonzero_frames', 'right_nonzero_frames'):
        if not integer(pcm.get(key), 0, pcm['frames']) or not integer(steady.get(key), 0, steady['frames']):
            raise ValueError('invalid duet channel counts')
    if not integer(pcm.get('nonzero_frames'), max(pcm['left_nonzero_frames'], pcm['right_nonzero_frames']),
                   min(pcm['frames'], pcm['left_nonzero_frames']+pcm['right_nonzero_frames'])):
        raise ValueError('invalid duet nonzero count')
    if report['status'] != 2:
        if report['keyons'] or report['keyoffs'] or pcm['nonzero_frames'] or pcm['peak'] or steady['frames']:
            raise ValueError('rejected duet rendered audio')
        return
    if report['tempo'] not in (96, 192):
        raise ValueError('accepted unsupported duet tempo')
    events = report['events']
    for event in events:
        if event['duration'] < 2 or event['opcode'] not in (*PITCH, 0xC9):
            raise ValueError('accepted unsupported duet event')
    sounding = [pattern for pattern in range(2)
                if any(event['opcode'] != 0xC9 for event in events[pattern*2:pattern*2+2])]
    if len(report['keyons']) != len(sounding) or len(report['keyoffs']) != len(sounding):
        raise ValueError('duet onset/release count differs')
    for index, pattern in enumerate(sounding):
        on, off = report['keyons'][index], report['keyoffs'][index]
        pair = events[pattern*2:pattern*2+2]
        mask = sum(1 << event['channel'] for event in pair if event['opcode'] != 0xC9)
        boundary_tick = events[2]['tick'] if pattern == 0 else report['end_tick']
        boundary_half = events[2]['half_cycle'] if pattern == 0 else report['completion_half_cycle']
        if (on['mask'] != mask or on['tick'] != pair[0]['tick'] or off['mask'] != 12
                or off['tick'] != boundary_tick or not on['half_cycle'] < off['half_cycle']
                or not 0 < on['half_cycle']-pair[1]['half_cycle'] <= 4096
                or not 0 <= boundary_half-off['half_cycle'] <= 4096):
            raise ValueError('duet onset/release timing differs')
        for voice, event in enumerate(pair):
            if event['opcode'] != 0xC9 and on['pitches'][voice] != PITCH[event['opcode']]:
                raise ValueError('duet voice pitch differs')
        if index and report['keyoffs'][index-1]['half_cycle'] >= on['half_cycle']:
            raise ValueError('duet re-key preceded release')
    if bool(pcm['nonzero_frames']) != bool(sounding) or bool(pcm['peak']) != bool(sounding):
        raise ValueError('duet PCM activity differs')
    for channel, key in enumerate(('left_nonzero_frames', 'right_nonzero_frames')):
        if not any(events[pattern*2+channel]['opcode'] != 0xC9 for pattern in range(2)) and pcm[key]:
            raise ValueError('rest-only voice produced audio')
        if steady['frames']:
            if events[2+channel]['opcode'] == 0xC9 and steady[key]:
                raise ValueError('clipped voice remained audible in second pattern')
            if steady['frames'] >= 64 and events[2+channel]['opcode'] != 0xC9 and not steady[key]:
                raise ValueError('active second-pattern voice is silent')


def align(report, bank):
    validate(report)
    phrase_align({**report, 'schema': 'gbb-spc-score-phrase-v1'}, bank)


def native(probe, bank, tempo=96):
    with tempfile.TemporaryDirectory(prefix='gbb-score-duet-') as directory:
        program = Path(directory)/'program.bin'
        stream = Path(directory)/'bank.bin'
        program.write_bytes(build())
        stream.write_bytes(bank)
        result = subprocess.run([str(probe), str(program), str(stream), str(tempo)],
                                capture_output=True, text=True, timeout=60)
        if result.returncode or len(result.stdout) > 16384:
            raise ValueError(f'native duet probe failed: {result.stderr[:1000]}')
        report = json.loads(result.stdout)
    validate(report)
    return report


def compare(candidates, references):
    if not isinstance(candidates, dict) or set(candidates) != set(CASES):
        raise ValueError('require all three native duet cases')
    for case, report in candidates.items():
        align(report, fixture(case))
    result = pair_compare({case: pair_report(report) for case, report in candidates.items()}, references)
    for comparison in result:
        case = comparison['case']
        reference = next(item for item in references if item['case'] == case and item['model'] == comparison['model'])
        observed = candidates[case]['keyons']
        expected = reference.get('keyons')
        if not isinstance(expected, list) or len(expected) != 2:
            raise ValueError('missing reference duet onsets')
        for actual, original in zip(observed, expected):
            if original != {'mask': actual['mask'], 'voices': [
                    {'voice': voice+2, 'srcn': 2, 'pitch': actual['pitches'][voice]} for voice in range(2)]}:
                raise ValueError('native/reference duet mask or pitches differ')
        interval = (observed[1]['half_cycle']-observed[0]['half_cycle'])/2
        if abs(interval-reference['pattern_interval_spc_cycles']) > 4096:
            raise ValueError('duet DSP onset interval outside allowance')
        comparison['native_DSP_pattern_interval_spc_cycles'] = interval
    return result


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
    print(json.dumps({'schema': 'gbb-score-duet-reference-v1', 'qualification': False,
                      'playback': False, 'program_sha256': hashlib.sha256(build()).hexdigest(),
                      'native': candidates, 'reference': references, 'comparisons': comparisons}, indent=2))


if __name__ == '__main__':
    main()
