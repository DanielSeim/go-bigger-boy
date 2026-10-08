#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check independent owned multi-track audio and bounded three-onset references."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile
from build_sgb_score_polygate import build
from build_sgb_multi_fixture import CASES, bank
from check_sgb_score_multi_reference import validate as multi_validate, align as multi_align, compare as multi_compare, observe as multi_observe, contract as multi_contract
from check_sgb_score_duet_reference import PITCH, integer
from schedule_sgb_score import schedule
from check_sgb_score_gate_reference import PULSES
from check_sgb_phrase_reference import run as phrase_run
from build_sgb_multi_fixture import build as fixture_build
import csv
import io


def multi_report(report, schema='gbb-spc-score-polygate-v1'):
    if not isinstance(report, dict) or report.get('schema') != schema:
        raise ValueError('invalid polyphonic schema')
    return {**report, 'schema': 'gbb-spc-score-multi-v1'}


def validate(report, *, schema='gbb-spc-score-polygate-v1', max_events=16, max_ticks=1016, pitch_table=PITCH, isolated_voices=True, pattern_ticks=None):
    multi_validate(multi_report(report, schema), max_events=max_events, max_ticks=max_ticks, min_events=2 if pattern_ticks is not None else 4)
    if (not integer(report.get('tempo'), 0, 255)
            or not integer(report.get('completion_half_cycle'), 1, 30_000_000)
            or not integer(report.get('second_pattern_tick'), 0, max_ticks)
            or not isinstance(report.get('peer_checks'), list) or len(report['peer_checks']) != 2
            or any(not integer(value, 0, 30_000_000) for value in report['peer_checks'])):
        raise ValueError('invalid polyphonic metadata')
    for name in ('keyons', 'keyoffs'):
        values, previous = report.get(name), 0
        if not isinstance(values, list) or len(values) > max_events:
            raise ValueError('invalid polyphonic edges')
        for edge in values:
            if (not isinstance(edge, dict) or not integer(edge.get('half_cycle'), previous+1, report['completion_half_cycle'])
                    or not integer(edge.get('tick'), 0, report['end_tick'])
                    or not integer(edge.get('mask'), 4, 12) or edge['mask'] not in (4, 8, 12)
                    or not integer(edge.get('affected_mask'), 4, 12) or edge['affected_mask'] not in (4, 8, 12)
                    or not integer(edge.get('held_mask'), 0, 12) or edge['held_mask'] not in (0, 4, 8, 12)
                    or not integer(edge.get('cause'), 0, 1)
                    or not isinstance(edge.get('pending_pulses'), list) or len(edge['pending_pulses']) != 2
                    or any(not integer(value, 0, 58) for value in edge['pending_pulses'])
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
    for name in ('settled_gate_frames', 'settled_peer_nonzero_frames'):
        if not isinstance(report.get(name), list) or len(report[name]) != 2 or any(
                not integer(value, 0, pcm['frames']) for value in report[name]):
            raise ValueError('invalid settled-gate metadata')
    if any(a > b for a, b in zip(report['settled_peer_nonzero_frames'], report['settled_gate_frames'])):
        raise ValueError('invalid settled peer activity')
    if pattern_ticks is not None:
        if not isinstance(pattern_ticks, list) or (report['status'] != 2 and pattern_ticks) or (report['status'] == 2 and
                (not 1 <= len(pattern_ticks) <= 4 or pattern_ticks[0] != 0 or any(not integer(tick, 0, report['end_tick']-1)
                 for tick in pattern_ticks) or any(a >= b for a,b in zip(pattern_ticks,pattern_ticks[1:])))):
            raise ValueError('invalid phrase-list pattern entries')
    if report['status'] != 2:
        if (report['keyons'] or report['keyoffs'] or pcm['nonzero_frames'] or pcm['peak']
                or report['second_pattern_tick'] or tail['frames'] or any(report['peer_checks'])
                or any(report['settled_gate_frames']) or any(report['settled_peer_nonzero_frames'])):
            raise ValueError('rejected bank rendered audio')
        return
    if report['tempo'] not in (96, 128, 192) or (pattern_ticks is None and not 0 < report['second_pattern_tick'] < report['end_tick']) or (pattern_ticks is not None and report['second_pattern_tick'] != (pattern_ticks[1] if len(pattern_ticks)>1 else 0)):
        raise ValueError('invalid accepted gated tempo/pattern')
    groups = []
    for event in report['events']:
        profile = (report['tempo'], event.get('articulation'), event['duration'])
        if not integer(event.get('articulation'), 63, 127) or event['articulation'] not in (63, 127) or event['duration'] < 2 or event['opcode'] not in (*pitch_table, 0xC9) or (
                event['opcode'] != 0xC9 and profile not in PULSES):
            raise ValueError('accepted unmeasured note profile')
        if not groups or groups[-1][0]['tick'] != event['tick']:
            groups.append([])
        groups[-1].append(event)
    for group in groups:
        if len({event['channel'] for event in group}) != len(group):
            raise ValueError('duplicate voice at a tick')
    sounding = [group for group in groups if any(event['opcode'] != 0xC9 for event in group)]
    if len(sounding) != len(report['keyons']):
        raise ValueError('gated onset count differs')
    pitches, held, active, index = [0, 0], 12, [None, None], 0
    edges = sorted([(edge['half_cycle'], True, edge) for edge in report['keyons']] +
                   [(edge['half_cycle'], False, edge) for edge in report['keyoffs']])
    boundaries = (*pattern_ticks[1:],report['end_tick']) if pattern_ticks is not None else (report['second_pattern_tick'],report['end_tick'])
    for half, is_on, edge in edges:
        if is_on:
            group = sounding[index]
            index += 1
            affected = sum(1 << event['channel'] for event in group)
            notes = sum(1 << event['channel'] for event in group if event['opcode'] != 0xC9)
            if (edge['tick'] != group[0]['tick'] or edge['mask'] != notes or edge['affected_mask'] != affected
                    or edge['cause'] != 0 or not 0 <= half-group[-1]['half_cycle'] <= 4096
                    or any(active[voice] is not None and affected & (4 << voice) for voice in range(2))):
                raise ValueError('gated onset or prior release differs')
            held = (held | affected) & ~notes
            for event in group:
                if event['opcode'] != 0xC9:
                    voice = event['channel']-2
                    pulses = PULSES[(report['tempo'], event['articulation'], event['duration'])]
                    if edge['pending_pulses'][voice] != pulses:
                        raise ValueError('gated onset selected wrong pulse profile')
                    pitches[voice] = pitch_table[event['opcode']]
                    active[voice] = (half, event, pulses)
            if edge['held_mask'] != held or edge['pitches'] != pitches:
                raise ValueError('gated onset held state or pitch differs')
        else:
            live = sum(4 << voice for voice in range(2) if active[voice] is not None)
            if edge['mask'] & ~live or edge['pitches'] != pitches:
                raise ValueError('gated release has no matching active note')
            affected = edge['affected_mask']
            if edge['cause'] == 1:
                if affected != edge['mask']:
                    raise ValueError('timer gate released an unaffected peer')
            elif edge['tick'] not in boundaries or affected != 12:
                raise ValueError('unqualified scheduler release of measured note')
            if (live & affected) != edge['mask']:
                raise ValueError('held gate mask omitted an active voice')
            held |= affected
            if edge['held_mask'] != held:
                raise ValueError('gated release held state differs')
            for voice in range(2):
                if not edge['mask'] & (4 << voice):
                    continue
                on, event, pulses = active[voice]
                if half <= on:
                    raise ValueError('nonpositive native gate')
                if edge['cause'] == 1:
                    cycles = (half-on)/2
                    if not pulses*2048-2048 <= cycles <= pulses*2048+512:
                        raise ValueError('native gate outside declared pulse profile')
                elif event['tick']+event['duration'] <= edge['tick']:
                    raise ValueError('scheduler substituted for a missing gate')
                active[voice] = None
    if any(note is not None for note in active):
        raise ValueError('native note has no complete release')
    if bool(pcm['nonzero_frames']) != bool(sounding) or bool(pcm['peak']) != bool(sounding):
        raise ValueError('gated PCM activity differs')
    for channel, key in (((2, 'left_nonzero_frames'), (3, 'right_nonzero_frames')) if isolated_voices else ()):
        if not any(event['channel'] == channel and event['opcode'] != 0xC9 for event in report['events']) and pcm[key]:
            raise ValueError('rest-only voice produced PCM')


def align(report, data, *, schema='gbb-spc-score-polygate-v1', max_events=16, max_ticks=1016, pitch_table=PITCH, isolated_voices=True, pattern_ticks=None):
    validate(report, schema=schema, max_events=max_events, max_ticks=max_ticks, pitch_table=pitch_table, isolated_voices=isolated_voices, pattern_ticks=pattern_ticks)
    multi_align(multi_report(report, schema), data, max_events=max_events, max_ticks=max_ticks, min_events=2 if pattern_ticks is not None else 4)
    expected = schedule(data, int.from_bytes(data[:2], 'little'))
    timed = [event for event in expected['events'] if event['kind'] in ('note', 'rest')]
    if [event['articulation'] for event in report['events']] != [event['articulation'] for event in timed]:
        raise ValueError('gated articulation differs from symbolic score')
    if pattern_ticks is not None:
        if pattern_ticks != [pattern['start_tick'] for pattern in expected['patterns']]:
            raise ValueError('native phrase-list pattern entries differ from symbolic score')
    elif report['second_pattern_tick'] != expected['patterns'][1]['start_tick']:
        raise ValueError('gated second pattern differs from symbolic score')


def native(probe, data, tempo=96, *, builder=build, validator=validate, output_bound=16384):
    with tempfile.TemporaryDirectory(prefix='gbb-score-polygate-') as directory:
        program, stream = Path(directory)/'program.bin', Path(directory)/'bank.bin'
        program.write_bytes(builder())
        stream.write_bytes(data)
        result = subprocess.run([str(probe), str(program), str(stream), str(tempo)],
                                capture_output=True, text=True, timeout=60)
        if result.returncode or len(result.stdout) > output_bound:
            raise ValueError(f'native polyphonic gate probe failed: {result.stderr[:1000]}')
        report = json.loads(result.stdout)
    validator(report)
    return report


def native_gates(report):
    notes = []
    active = [None, None]
    for half, is_on, edge in sorted([(edge['half_cycle'], True, edge) for edge in report['keyons']] +
                                    [(edge['half_cycle'], False, edge) for edge in report['keyoffs']]):
        for voice in range(2):
            if not edge['mask'] & (4 << voice):
                continue
            if is_on:
                note = {'voice': voice+2, 'on': half}
                notes.append(note)
                active[voice] = note
            else:
                active[voice]['gate_spc_cycles'] = (half-active[voice].pop('on'))/2
                active[voice]['cause'] = edge['cause']
                active[voice] = None
    return notes


def observe(source, *, onset_observer=multi_observe, expected_notes=6, handoff_note_count=4):
    text = source.read(16*1024*1024+1)
    if len(text) > 16*1024*1024:
        raise ValueError('gated reference trace exceeds byte bound')
    result = onset_observer(io.StringIO(text))
    notes, active = [], [None, None]
    for row in csv.DictReader(io.StringIO(text)):
        if row['kind'] != 'D':
            continue
        address, value, cycle = (int(row[key]) for key in ('address', 'value', 'spc_cycle'))
        if address == 0x5C:
            for voice in range(2):
                if value & (4 << voice) and active[voice] is not None:
                    active[voice]['release_kind'] = 'keyoff'
                    active[voice]['gate_spc_cycles'] = cycle-active[voice].pop('on')
                    if active[voice]['gate_spc_cycles'] <= 0:
                        raise ValueError('nonpositive original polyphonic gate')
                    active[voice] = None
        if address == 0x4C and value:
            pattern_handoff = handoff_note_count is not None and len(notes) == handoff_note_count
            for voice in range(2):
                if active[voice] is not None:
                    if not pattern_handoff:
                        raise ValueError('reference voice retriggered without release')
                    active[voice]['release_kind'] = 'retrigger'
                    active[voice]['gate_spc_cycles'] = cycle-active[voice].pop('on')
                    if active[voice]['gate_spc_cycles'] <= 0:
                        raise ValueError('nonpositive original pattern handoff')
                    active[voice] = None
                note = {'voice': voice+2, 'on': cycle}
                notes.append(note)
                active[voice] = note
    if len(notes) != expected_notes or any('gate_spc_cycles' not in note for note in notes):
        raise ValueError(f'expected {expected_notes} complete reference gates')
    result['note_gates'] = notes
    return result


def contract(result, case):
    multi_contract(result, case)
    if len(result['note_gates']) != 6:
        raise ValueError('incomplete polyphonic gate reference')


def run(trace, firmware_dir, model, case, articulation=127):
    result = phrase_run(trace, firmware_dir, model, case,
                        fixture_builder=lambda name: fixture_build(name, articulation), observer=observe, contract=contract)
    result.pop('first_pattern_durations')
    result['articulation'] = articulation
    return result


def compare(candidates, references, articulation=127):
    if not isinstance(candidates, dict) or set(candidates) != set(CASES):
        raise ValueError('require all three native gated cases')
    for case, report in candidates.items():
        align(report, bank(case, articulation))
    if not isinstance(references, list) or any(not isinstance(reference, dict)
            or not integer(reference.get('articulation'), 63, 127)
            or reference['articulation'] != articulation for reference in references):
        raise ValueError('reference articulation differs')
    comparisons = multi_compare({case: multi_report(report) for case, report in candidates.items()}, references)
    for comparison in comparisons:
        report = candidates[comparison['case']]
        reference = next(item for item in references if item['model'] == comparison['model'] and item['case'] == comparison['case'])
        actual, expected = native_gates(report), reference.get('note_gates')
        if not isinstance(expected, list) or len(expected) != len(actual):
            raise ValueError('missing original polyphonic gates')
        differences = []
        for note, original in zip(actual, expected):
            if (not isinstance(original, dict) or note['voice'] != original.get('voice') or not integer(original.get('gate_spc_cycles'), 1, 200000)
                    or original.get('release_kind') not in ('keyoff', 'retrigger')
                    or (note['cause'] == 1 and original['release_kind'] != 'keyoff')):
                raise ValueError('invalid original polyphonic gate')
            difference = abs(note['gate_spc_cycles']-original['gate_spc_cycles'])
            if difference > 4096:
                raise ValueError('native/original polyphonic gate outside allowance')
            differences.append(difference)
        onsets = report['keyons']
        intervals = [(b['half_cycle']-a['half_cycle'])/2 for a, b in zip(onsets, onsets[1:])]
        if any(abs(a-b) > 4096 for a, b in zip(intervals, reference['onset_intervals_spc_cycles'])):
            raise ValueError('native gated DSP onset outside allowance')
        comparison['native_gate_spc_cycles'] = [note['gate_spc_cycles'] for note in actual]
        comparison['absolute_gate_difference_spc_cycles'] = differences
        comparison['reference_release_kinds'] = [note['release_kind'] for note in expected]
        comparison['native_DSP_intervals_spc_cycles'] = intervals
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
    print(json.dumps({'schema': 'gbb-score-polygate-reference-v1', 'qualification': False, 'playback': False,
                      'program_sha256': hashlib.sha256(build()).hexdigest(), 'articulation': args.articulation, 'native': candidates,
                      'reference': references, 'comparisons': comparisons}, indent=2))


if __name__ == '__main__':
    main()
