#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check independent owned multi-track audio and bounded three-onset references."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile
from build_sgb_score_poly import build
from build_sgb_multi_fixture import CASES, bank
from check_sgb_score_multi_reference import validate as multi_validate, align as multi_align, compare as multi_compare, run
from check_sgb_score_duet_reference import PITCH, integer
from schedule_sgb_score import schedule


def multi_report(report):
    if not isinstance(report, dict) or report.get('schema') != 'gbb-spc-score-poly-v1':
        raise ValueError('invalid polyphonic schema')
    return {**report, 'schema': 'gbb-spc-score-multi-v1'}


def validate(report):
    multi_validate(multi_report(report))
    if (not integer(report.get('tempo'), 0, 255)
            or not integer(report.get('completion_half_cycle'), 1, 30_000_000)
            or not integer(report.get('second_pattern_tick'), 0, 1016)
            or not isinstance(report.get('peer_checks'), list) or len(report['peer_checks']) != 2
            or any(not integer(value, 0, 30_000_000) for value in report['peer_checks'])):
        raise ValueError('invalid polyphonic metadata')
    for name in ('keyons', 'keyoffs'):
        values, previous = report.get(name), 0
        if not isinstance(values, list) or len(values) > 16:
            raise ValueError('invalid polyphonic edges')
        for edge in values:
            if (not isinstance(edge, dict) or not integer(edge.get('half_cycle'), previous+1, report['completion_half_cycle'])
                    or not integer(edge.get('tick'), 0, report['end_tick'])
                    or not integer(edge.get('mask'), 4, 12) or edge['mask'] not in (4, 8, 12)
                    or not integer(edge.get('affected_mask'), 4, 12) or edge['affected_mask'] not in (4, 8, 12)
                    or not integer(edge.get('held_mask'), 0, 12) or edge['held_mask'] not in (0, 4, 8, 12)
                    or not isinstance(edge.get('pitches'), list) or len(edge['pitches']) != 2
                    or any(not integer(pitch, 0, 0x3FFF) for pitch in edge['pitches'])):
                raise ValueError('invalid polyphonic edge fields')
            previous = edge['half_cycle']
    pcm, tail = report.get('pcm'), report.get('second_tail_pcm')
    if (not isinstance(pcm, dict) or not isinstance(tail, dict)
            or not integer(pcm.get('frames'), 1, 500000) or not integer(pcm.get('peak'), 0, 32768)
            or not integer(pcm.get('fnv1a64'), 0, 2**64-1)
            or not integer(pcm.get('quiet_tail_frames'), 64, pcm['frames'])
            or not integer(tail.get('frames'), 0, pcm['frames'])):
        raise ValueError('invalid polyphonic PCM bounds')
    for key in ('left_nonzero_frames', 'right_nonzero_frames'):
        if not integer(pcm.get(key), 0, pcm['frames']) or not integer(tail.get(key), 0, tail['frames']):
            raise ValueError('invalid polyphonic channel counts')
    if not integer(pcm.get('nonzero_frames'), max(pcm['left_nonzero_frames'], pcm['right_nonzero_frames']),
                   min(pcm['frames'], pcm['left_nonzero_frames']+pcm['right_nonzero_frames'])):
        raise ValueError('invalid polyphonic nonzero count')
    if report['status'] != 2:
        if (report['keyons'] or report['keyoffs'] or pcm['nonzero_frames'] or pcm['peak']
                or report['second_pattern_tick'] or tail['frames'] or any(report['peer_checks'])):
            raise ValueError('rejected bank rendered audio')
        return
    if report['tempo'] not in (96, 192) or not 0 < report['second_pattern_tick'] < report['end_tick']:
        raise ValueError('invalid accepted polyphonic tempo/pattern')
    groups = []
    for event in report['events']:
        if event['duration'] < 2 or event['opcode'] not in (*PITCH, 0xC9):
            raise ValueError('accepted unsupported polyphonic event')
        if not groups or groups[-1][0]['tick'] != event['tick']:
            groups.append([])
        groups[-1].append(event)
    held, pitches, ons, offs = 12, [0, 0], [], []
    for group in groups:
        if len({event['channel'] for event in group}) != len(group):
            raise ValueError('duplicate channel at one tick')
        affected = sum(1 << event['channel'] for event in group)
        asserted = (12 & ~held) & affected
        release_held = held | affected
        if asserted:
            offs.append((group[0]['tick'], asserted, affected, release_held, pitches[:], group[0]['half_cycle']))
        notes = 0
        for event in group:
            if event['opcode'] != 0xC9:
                notes |= 1 << event['channel']
                pitches[event['channel']-2] = PITCH[event['opcode']]
        held = release_held & ~notes
        if notes:
            ons.append((group[0]['tick'], notes, affected, held, pitches[:], group[-1]['half_cycle']))
    if 12 & ~held:
        offs.append((report['end_tick'], 12 & ~held, 12, 12, pitches[:], report['completion_half_cycle']))
    for name, expected, is_on in (('keyons', ons, True), ('keyoffs', offs, False)):
        if len(report[name]) != len(expected):
            raise ValueError('polyphonic onset/release count differs')
        for actual, (tick, mask, affected, held, pitches, reference_half) in zip(report[name], expected):
            if (actual['tick'] != tick or actual['mask'] != mask or actual['affected_mask'] != affected
                    or actual['held_mask'] != held or actual['pitches'] != pitches):
                raise ValueError('polyphonic mask/pitch/held state differs')
            delta = actual['half_cycle']-reference_half if is_on else reference_half-actual['half_cycle']
            if not 0 <= delta <= 4096:
                raise ValueError('polyphonic edge outside event boundary')
    sounding = bool(ons)
    if bool(pcm['nonzero_frames']) != sounding or bool(pcm['peak']) != sounding:
        raise ValueError('polyphonic PCM activity differs')
    second = [event for event in report['events'] if event['tick'] >= report['second_pattern_tick']]
    if not any(event['tick'] == report['second_pattern_tick'] and event['channel'] == 2 for event in second) or not any(
            event['tick'] == report['second_pattern_tick'] and event['channel'] == 3 for event in second):
        raise ValueError('second pattern has no initial pair')
    for channel, key in ((2, 'left_nonzero_frames'), (3, 'right_nonzero_frames')):
        if not any(event['channel'] == channel and event['opcode'] != 0xC9 for event in report['events']) and pcm[key]:
            raise ValueError('rest-only voice produced PCM')
        if not any(event['channel'] == channel and event['opcode'] != 0xC9 for event in second) and tail[key]:
            raise ValueError('clipped voice did not settle in second pattern')


def align(report, data):
    validate(report)
    multi_align(multi_report(report), data)
    expected = schedule(data, int.from_bytes(data[:2], 'little'))
    if report['second_pattern_tick'] != expected['patterns'][1]['start_tick']:
        raise ValueError('native second pattern differs from symbolic score')


def native(probe, data, tempo=96):
    with tempfile.TemporaryDirectory(prefix='gbb-score-poly-') as directory:
        program, stream = Path(directory)/'program.bin', Path(directory)/'bank.bin'
        program.write_bytes(build())
        stream.write_bytes(data)
        result = subprocess.run([str(probe), str(program), str(stream), str(tempo)],
                                capture_output=True, text=True, timeout=60)
        if result.returncode or len(result.stdout) > 16384:
            raise ValueError(f'native polyphonic probe failed: {result.stderr[:1000]}')
        report = json.loads(result.stdout)
    validate(report)
    return report


def compare(candidates, references):
    if not isinstance(candidates, dict) or set(candidates) != set(CASES):
        raise ValueError('require all three native polyphonic cases')
    for case, report in candidates.items():
        align(report, bank(case))
    result = multi_compare({case: multi_report(report) for case, report in candidates.items()}, references)
    for comparison in result:
        report = candidates[comparison['case']]
        original = next(item for item in references if item['case'] == comparison['case'] and item['model'] == comparison['model'])
        if len(report['keyons']) != 3:
            raise ValueError('expected three polyphonic fixture onsets')
        for actual, expected in zip(report['keyons'], original['keyons']):
            if actual['mask'] != expected['mask'] or actual['pitches'] != [voice['pitch'] for voice in expected['voices']]:
                raise ValueError('polyphonic native/reference onset differs')
        values = report['keyons']
        actual = [(b['half_cycle']-a['half_cycle'])/2 for a, b in zip(values, values[1:])]
        if any(abs(a-b) > 4096 for a, b in zip(actual, original['onset_intervals_spc_cycles'])):
            raise ValueError('polyphonic DSP onset interval outside allowance')
        comparison['native_DSP_intervals_spc_cycles'] = actual
    return result


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
    print(json.dumps({'schema': 'gbb-score-poly-reference-v1', 'qualification': False, 'playback': False,
                      'program_sha256': hashlib.sha256(build()).hexdigest(), 'native': candidates,
                      'reference': references, 'comparisons': comparisons}, indent=2))


if __name__ == '__main__':
    main()
