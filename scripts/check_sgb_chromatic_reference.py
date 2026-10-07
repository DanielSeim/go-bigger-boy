#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Observe chromatic octave setup and gates from opaque private original execution."""
import argparse
import csv
import json
from pathlib import Path
import subprocess
from build_sgb_chromatic_fixture import NOTES, CASES, build as fixture_build
from check_sgb_phrase_reference import run as phrase_run, MAX_ROWS
from check_sgb_score_polygate_reference import observe as gate_observe
from check_sgb_score_duet_reference import integer

FIELDS = ('srcn', 'adsr1', 'adsr2', 'gain')
# Pinned DSP writes from the owned chromatic fixture on both private originals.
# These are instrument-2 register values, not a general tuning algorithm.
PITCHES = dict(zip(NOTES, (1068, 1132, 1200, 1272, 1348, 1428, 1512,
                          1604, 1700, 1800, 1908, 2020, 2140)))


def onsets(source, case='chromatic'):
    expected_notes = CASES[case]
    reader = csv.DictReader(source)
    if reader.fieldnames != ['kind', 'master_clock', 'spc_cycle', 'pcm_sample', 'address', 'value']:
        raise ValueError('incompatible chromatic trace header')
    registers, keyons, cycles = {}, [], []
    previous = -1
    for count, row in enumerate(reader, 1):
        if count >= MAX_ROWS:
            raise ValueError('chromatic trace reached row bound')
        if row['kind'] != 'D':
            continue
        if None in row or any(value is None for value in row.values()):
            raise ValueError('invalid chromatic DSP event')
        try:
            address, value, cycle = (int(row[key]) for key in ('address', 'value', 'spc_cycle'))
        except (TypeError, ValueError):
            raise ValueError('invalid chromatic DSP event') from None
        if not 0 <= address < 128 or not 0 <= value < 256 or cycle < 0 or cycle < previous:
            raise ValueError('invalid or unordered chromatic DSP event')
        previous = cycle
        if address in (*range(0x22, 0x28), *range(0x32, 0x38), 0x3D):
            registers[address] = value
        if address != 0x4C or not value:
            continue
        if value != 12 or len(keyons) >= len(expected_notes) or registers.get(0x3D, 12) & 12:
            raise ValueError('unexpected chromatic voices, noise or count')
        voices = []
        for voice in (2, 3):
            base = 16*voice
            if any(base+offset not in registers for offset in range(2, 8)):
                raise ValueError('incomplete chromatic voice setup')
            pitch = registers[base+2] | registers[base+3] << 8
            setup = tuple(registers[base+offset] for offset in range(4, 8))
            if not 0 < pitch <= 0x3FFF or setup != (2, 0x8F, 0x6F, 0xB8):
                raise ValueError('unexpected chromatic instrument setup')
            voices.append({'voice': voice, 'pitch': pitch, **dict(zip(FIELDS, setup))})
        if voices[0]['pitch'] != voices[1]['pitch'] or cycles and cycle <= cycles[-1]:
            raise ValueError('chromatic peers or onset ordering differ')
        keyons.append({'mask': value, 'voices': voices})
        cycles.append(cycle)
    if len(keyons) != len(expected_notes):
        raise ValueError('incomplete chromatic octave')
    pitches = [edge['voices'][0]['pitch'] for edge in keyons]
    if pitches != [PITCHES[note] for note in expected_notes]:
        raise ValueError('chromatic pitches differ from measured contract')
    return {'keyons': keyons, 'base_notes': list(expected_notes),
            'onset_intervals_spc_cycles': [b-a for a, b in zip(cycles, cycles[1:])]}


def observe(source, case='chromatic'):
    return gate_observe(source, onset_observer=lambda trace: onsets(trace, case),
                        expected_notes=2*len(CASES[case]), handoff_note_count=None)


def contract(result, case):
    expected = [{'mask': 12, 'voices': [{'voice': voice, 'pitch': PITCHES[note],
                  'srcn': 2, 'adsr1': 0x8F, 'adsr2': 0x6F, 'gain': 0xB8} for voice in (2, 3)]}
                for note in CASES[case]]
    if result.get('keyons') != expected or result.get('base_notes') != list(CASES[case]):
        raise ValueError('chromatic reference sequence/setup differs')
    if any(type(edge['mask']) is not int or any(type(value) is not int for voice in edge['voices']
            for value in voice.values()) for edge in result['keyons']) or any(
            type(note) is not int for note in result['base_notes']):
        raise ValueError('chromatic reference register metadata must be integers')
    intervals = result.get('onset_intervals_spc_cycles')
    if not isinstance(intervals, list) or len(intervals) != len(CASES[case])-1 or any(type(value) is not int or not 84000 <= value <= 92000
            for value in intervals):
        raise ValueError('chromatic reference onset outside existing fixture bounds')
    gates = result.get('note_gates')
    if not isinstance(gates, list) or len(gates) != 2*len(CASES[case]) or any(
            not isinstance(note, dict) or not integer(note.get('voice'), 2, 3) or note['voice'] != 2+i%2
            or note.get('release_kind') != 'keyoff' or not integer(note.get('gate_spc_cycles'), 1, 200000)
            for i, note in enumerate(gates)):
        raise ValueError('chromatic reference requires observed key-offs')


def run(trace, firmware_dir, model, articulation=127, case='chromatic'):
    result = phrase_run(trace, firmware_dir, model, case,
                        fixture_builder=lambda name: fixture_build(articulation, name),
                        observer=lambda source: observe(source, case), contract=contract,
                        pattern_durations=None)
    result['articulation'] = articulation
    result['instruction_limit'] = 8000000
    return result


def check_models(results):
    if not isinstance(results, list) or len(results) != 2 or any(not isinstance(result, dict)
            or result.get('model') not in ('sgb', 'sgb2') for result in results) or {result['model'] for result in results} != {'sgb', 'sgb2'}:
        raise ValueError('require both chromatic reference models')
    first, second = results
    if first.get('case') not in CASES or first.get('case') != second.get('case') or not integer(
            first.get('articulation'), 63, 127) or first['articulation'] not in (63, 127) or first['articulation'] != second.get('articulation'):
        raise ValueError('chromatic reference cases/articulations differ')
    for result in results:
        contract(result, result['case'])
    if first['keyons'] != second['keyons'] or any(abs(a-b) > 2048 for a, b in zip(
            first['onset_intervals_spc_cycles'], second['onset_intervals_spc_cycles'])):
        raise ValueError('two-model chromatic reference differs')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--trace', type=Path, required=True)
    parser.add_argument('--firmware-dir', type=Path, required=True)
    parser.add_argument('--articulation', type=int, choices=(63, 127), default=127)
    parser.add_argument('--case', choices=tuple(CASES), default='chromatic')
    args = parser.parse_args()
    try:
        results = [run(args.trace.resolve(), args.firmware_dir.resolve(), model, args.articulation, args.case)
                   for model in ('sgb', 'sgb2')]
        check_models(results)
    except (OSError, ValueError, subprocess.TimeoutExpired) as error:
        parser.error(str(error))
    print(json.dumps({'schema': 'gbb-sgb-chromatic-reference-v1', 'qualification': False,
                      'playback': False, 'runs': results}, indent=2))


if __name__ == '__main__':
    main()
