#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Validate bounded mix inheritance and opaque two-model DSP evidence."""
import argparse
import csv
import hashlib
import json
from pathlib import Path
import subprocess
import struct
from build_sgb_score_mix import build
from build_sgb_mix_fixture import CASES, NOTES, bank, volumes, build as fixture_build
from check_sgb_score_chromatic_reference import PITCH
from check_sgb_chromatic_reference import PITCHES
from check_sgb_score_polygate_reference import validate as gate_validate, align as gate_align, native as gate_native, native_gates, observe as gate_observe
from check_sgb_score_duet_reference import integer
from check_sgb_phrase_reference import run as phrase_run, MAX_ROWS
from schedule_sgb_score import schedule

SCHEMA = 'gbb-spc-score-mix-v1'
KEYS = tuple(f'{case}-{art}' for case in CASES for art in (127, 63))


def selected(pan, track, song, rest=False):
    if pan not in (0, 10, 20) or track not in (64, 127) or song not in (80, 160):
        raise ValueError('unmeasured mix control')
    if rest:
        return [0, 0]
    if pan != 10:
        if (song, track) != (160, 127):
            raise ValueError('unmeasured endpoint reduction')
        return [11, 0] if pan == 20 else [0, 11]
    if (song, track) == (80, 64):
        raise ValueError('unmeasured combined reduction')
    return [7, 7] if (song, track) == (160, 127) else [1, 1]


def validate(report):
    gate_validate(report, schema=SCHEMA, max_events=32, max_ticks=2032, pitch_table=PITCH, isolated_voices=False)
    pcm = report['pcm']
    if type(pcm.get('stereo_equal')) is not bool or any(not integer(pcm.get(key), 0, pcm['peak']) for key in ('left_peak', 'right_peak')) or max(pcm['left_peak'], pcm['right_peak']) != pcm['peak']:
        raise ValueError('invalid mix stereo metrics')
    counts = report.get('settled_envelope_checks')
    if not isinstance(counts, list) or len(counts) != 2 or any(not integer(x, 0, 30_000_000) for x in counts):
        raise ValueError('invalid mix envelope checks')
    if report['status'] != 2:
        if any(counts):
            raise ValueError('rejected mix checked live envelopes')
        return
    for event in report['events']:
        if any(type(event.get(key)) is not int for key in ('pan', 'track_volume', 'song_volume')):
            raise ValueError('invalid mix event controls')
        pair = event.get('volumes')
        if not isinstance(pair, list) or len(pair) != 2 or any(not integer(x, 0, 11) for x in pair) or pair != selected(
                event['pan'], event['track_volume'], event['song_volume'], event['opcode'] == 0xC9):
            raise ValueError('event mix differs from measured settings')
    held = [[0, 0], [0, 0]]
    timeline = sorted([(e['half_cycle'], 'event', e) for e in report['events']]+
                      [(e['half_cycle'], 'edge', e) for name in ('keyons', 'keyoffs') for e in report[name]])
    for _, kind, item in timeline:
        if kind == 'event':
            if item['opcode'] != 0xC9:
                held[item['channel']-2] = item['volumes']
        else:
            actual = item.get('volumes')
            if not isinstance(actual, list) or len(actual) != 2 or any(not isinstance(pair, list) or len(pair) != 2 or
                    any(not integer(x, 0, 11) for x in pair) for pair in actual) or actual != held:
                raise ValueError('actual DSP volumes or unchanged peer differ')
    for lane, key in enumerate(('left_nonzero_frames', 'right_nonzero_frames')):
        if not any(e['volumes'][lane] for e in report['events']) and (report['pcm'][key] or report['pcm'][('left_peak', 'right_peak')[lane]]):
            raise ValueError('unrouted stereo lane produced PCM')
    # Separate VOLL/VOLR writes can change a still-decaying outgoing tail.
    # Exact stereo equality is a constant-center contract, not dynamic PCM equivalence.
    sounding = [e['volumes'] for e in report['events'] if e['opcode'] != 0xC9]
    if sounding and all(pair == sounding[0] and pair[0] == pair[1] for pair in sounding) and not pcm['stereo_equal']:
        raise ValueError('centered mix produced unequal stereo')


def align(report, data):
    validate(report)
    gate_align(report, data, schema=SCHEMA, max_events=32, max_ticks=2032, pitch_table=PITCH, isolated_voices=False)
    expected = schedule(data, int.from_bytes(data[:2], 'little'))
    song, actual = 160, iter(report['events'])
    # Schedule each pattern separately so zero-time commands at an old track's
    # end cannot accidentally become startup state for the next pattern.
    for pattern in expected['patterns']:
        controls = {2: [10, 127], 3: [10, 127]}
        isolated = data+struct.pack('<HH', pattern['address'], 0)
        events = schedule(isolated, 0x2B00+len(data))['events']
        for event in events:
            channel, kind = event['channel'], event['kind']
            if kind == 'pan':
                controls[channel][0] = event['value']
            elif kind == 'track_volume':
                controls[channel][1] = event['value']
            elif kind == 'song_volume':
                song = event['value']
            elif kind in ('note', 'rest'):
                note = next(actual)
                pan, track = controls[channel]
                if [note['pan'], note['track_volume'], note['song_volume'], note['volumes']] != [pan, track, song, selected(pan, track, song, kind == 'rest')]:
                    raise ValueError('native mix inheritance differs from symbolic score')


def native(probe, data, tempo=96):
    return gate_native(probe, data, tempo, builder=build, validator=validate, output_bound=49152)


def onsets(source, case):
    reader = csv.DictReader(source)
    if reader.fieldnames != ['kind', 'master_clock', 'spc_cycle', 'pcm_sample', 'address', 'value']:
        raise ValueError('incompatible mix trace header')
    registers, keyons, cycles, previous = {}, [], [], -1
    for count, row in enumerate(reader, 1):
        if count >= MAX_ROWS:
            raise ValueError('mix trace reached row bound')
        if row['kind'] != 'D':
            continue
        try:
            address, value, cycle = (int(row[key]) for key in ('address', 'value', 'spc_cycle'))
        except (TypeError, ValueError):
            raise ValueError('invalid mix DSP event') from None
        if None in row or any(x is None for x in row.values()) or not 0 <= address < 128 or not 0 <= value < 256 or cycle < 0 or cycle < previous:
            raise ValueError('invalid or unordered mix DSP event')
        previous = cycle
        registers[address] = value
        if address != 0x4C or not value:
            continue
        if value != 12 or len(keyons) >= len(NOTES) or registers.get(0x3D, 12) & 12:
            raise ValueError('unexpected mix onset/voice/noise')
        voices = []
        for voice in (2, 3):
            base = voice*16
            if any(base+i not in registers for i in range(8)):
                raise ValueError('incomplete mix voice setup')
            voices.append({'voice': voice, 'pitch': registers[base+2] | registers[base+3] << 8,
                           'volumes': [registers[base], registers[base+1]],
                           'srcn': registers[base+4], 'adsr1': registers[base+5],
                           'adsr2': registers[base+6], 'gain': registers[base+7]})
        keyons.append({'mask': value, 'voices': voices})
        cycles.append(cycle)
    result = {'keyons': keyons, 'onset_intervals_spc_cycles': [b-a for a,b in zip(cycles,cycles[1:])]}
    onset_contract(result, case)
    return result


def onset_contract(result, case):
    expected = [{'mask': 12, 'voices': [{'voice': voice, 'pitch': PITCHES[note], 'volumes': pair,
                  'srcn': 2, 'adsr1': 0x8F, 'adsr2': 0x6F, 'gain': 0xB8}
                 for voice, pair in zip((2, 3), pairs)]} for note, pairs in zip(NOTES, volumes(case))]
    if result.get('keyons') != expected:
        raise ValueError('reference mix/pitch/setup differs from measured contract')
    for edge in result['keyons']:
        if type(edge['mask']) is not int or any(type(value) is not int for voice in edge['voices'] for key, value in voice.items() if key != 'volumes') or any(
                type(value) is not int for voice in edge['voices'] for value in voice['volumes']):
            raise ValueError('reference mix metadata must be integers')
    intervals = result.get('onset_intervals_spc_cycles')
    if not isinstance(intervals, list) or len(intervals) != len(NOTES)-1 or any(not integer(x, 84000, 92000) for x in intervals):
        raise ValueError('reference mix onset outside existing fixture bounds')


def observe(source, case='pan-calls'):
    return gate_observe(source, onset_observer=lambda trace: onsets(trace, case), expected_notes=16, handoff_note_count=None)


def contract(result, case):
    onset_contract(result, case)
    gates = result.get('note_gates')
    if not isinstance(gates, list) or len(gates) != 16 or any(not isinstance(note, dict) or
            not integer(note.get('voice'), 2, 3) or note['voice'] != 2+i%2 or note.get('release_kind') != 'keyoff' or
            not integer(note.get('gate_spc_cycles'), 1, 200000) for i,note in enumerate(gates)):
        raise ValueError('reference mix requires observed key-offs')


def run(trace, firmware_dir, model, articulation, case):
    result = phrase_run(trace, firmware_dir, model, case,
                        fixture_builder=lambda name: fixture_build(articulation, name),
                        observer=lambda source: observe(source, case), contract=contract, pattern_durations=None)
    result.update(articulation=articulation, instruction_limit=8000000)
    return result


def compare(candidates, references):
    if not isinstance(candidates, dict) or set(candidates) != set(KEYS) or not isinstance(references, list) or len(references) != 8:
        raise ValueError('require both mix cases/articulations/models')
    seen, comparisons = set(), []
    for reference in references:
        if not isinstance(reference, dict):
            raise ValueError('invalid mix reference')
        case, art, model = (reference.get(k) for k in ('case', 'articulation', 'model'))
        if case not in CASES or type(art) is not int or art not in (63, 127) or model not in ('sgb', 'sgb2') or (case,art,model) in seen:
            raise ValueError('invalid or duplicate mix reference')
        seen.add((case,art,model))
        contract(reference, case)
        report = candidates[f'{case}-{art}']
        align(report, bank(art, case))
        if [e['volumes'] for e in report['keyons']] != volumes(case) or [e['pitches'] for e in report['keyons']] != [[PITCHES[n]]*2 for n in NOTES]:
            raise ValueError('native mix fixture onset differs')
        actual, original = native_gates(report), reference['note_gates']
        differences = [abs(a['gate_spc_cycles']-b['gate_spc_cycles']) for a,b in zip(actual,original)]
        if len(actual) != 16 or any(a['cause'] != 1 or a['voice'] != b['voice'] for a,b in zip(actual,original)) or any(x > 4096 for x in differences):
            raise ValueError('native mix gate outside existing allowance')
        intervals = [(b['half_cycle']-a['half_cycle'])/2 for a,b in zip(report['keyons'],report['keyons'][1:])]
        if len(intervals) != 7 or any(abs(a-b) > 4096 for a,b in zip(intervals,reference['onset_intervals_spc_cycles'])):
            raise ValueError('native mix onset outside existing allowance')
        comparisons.append({'case':case, 'articulation':art, 'model':model, 'absolute_gate_difference_spc_cycles':differences,
                            'native_DSP_intervals_spc_cycles':intervals, 'reference_DSP_intervals_spc_cycles':reference['onset_intervals_spc_cycles']})
    for case in CASES:
        for art in (63,127):
            first, second = [r for r in references if r['case'] == case and r['articulation'] == art]
            if first['keyons'] != second['keyons'] or any(abs(a-b) > 2048 for a,b in zip(first['onset_intervals_spc_cycles'],second['onset_intervals_spc_cycles'])):
                raise ValueError('two-model mix reference differs')
    return comparisons


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--probe', type=Path, required=True)
    parser.add_argument('--trace', type=Path, required=True)
    parser.add_argument('--firmware-dir', type=Path, required=True)
    args = parser.parse_args()
    try:
        candidates = {f'{case}-{art}':native(args.probe.resolve(), bank(art,case)) for case in CASES for art in (127,63)}
        references = [run(args.trace.resolve(),args.firmware_dir.resolve(),model,art,case) for case in CASES for art in (127,63) for model in ('sgb','sgb2')]
        comparisons = compare(candidates,references)
    except (OSError,ValueError,subprocess.TimeoutExpired) as error:
        parser.error(str(error))
    print(json.dumps({'schema':'gbb-score-mix-reference-v1', 'qualification':False, 'playback':False,
                      'program_sha256':hashlib.sha256(build()).hexdigest(), 'native':candidates,
                      'reference':references, 'comparisons':comparisons},indent=2))


if __name__ == '__main__':
    main()
