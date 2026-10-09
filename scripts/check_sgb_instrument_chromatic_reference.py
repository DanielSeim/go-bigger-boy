#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Register-only octave observations for resident instruments 2 and 10."""
import argparse
import csv
import json
from pathlib import Path
import subprocess

from build_sgb_chromatic_fixture import NOTES, build
from check_sgb_chromatic_reference import PITCHES as INSTRUMENT2
from check_sgb_phrase_reference import MAX_ROWS, run as phrase_run

FIELDS = ('srcn', 'adsr1', 'adsr2', 'gain')
SETUPS = {2: (2, 0x8F, 0x6F, 0xB8), 10: (10, 0x8E, 0xAF, 0xB8)}
PITCHES = {2: tuple(INSTRUMENT2.values()), 10: (7993, 8472, 8981, 9520, 10088, 10687, 11316,
                                            12004, 12723, 13471, 14280, 15118, 16016)}


def observe(source, instrument):
    """Discard all but DSP setup at the thirteen combined voice-2/3 KONs."""
    if type(instrument) is not int or instrument not in SETUPS:
        raise ValueError('unsupported instrument')
    reader = csv.DictReader(source)
    if reader.fieldnames != ['kind', 'master_clock', 'spc_cycle', 'pcm_sample', 'address', 'value']:
        raise ValueError('incompatible instrument chromatic trace header')
    registers, notes = {}, []
    previous = -1
    watched = {*range(0x22, 0x28), *range(0x32, 0x38), 0x3D}
    for count, row in enumerate(reader, 1):
        if count >= MAX_ROWS:
            raise ValueError('instrument chromatic trace reached row bound')
        if row['kind'] != 'D':
            continue
        if None in row or any(value is None for value in row.values()):
            raise ValueError('invalid instrument chromatic DSP event')
        try:
            address, value, cycle = (int(row[key]) for key in ('address', 'value', 'spc_cycle'))
        except (TypeError, ValueError):
            raise ValueError('invalid instrument chromatic DSP event') from None
        if not 0 <= address < 128 or not 0 <= value < 256 or cycle < 0 or cycle < previous:
            raise ValueError('invalid or unordered instrument chromatic DSP event')
        previous = cycle
        if address in watched:
            registers[address] = value
        if address != 0x4C or value == 0:
            continue
        if value != 12 or len(notes) >= len(NOTES) or 0x3D not in registers or registers[0x3D] & 12:
            raise ValueError('unexpected instrument chromatic voices, noise or count')
        voices = []
        for voice in (2, 3):
            base = 16*voice
            if any(base+offset not in registers for offset in range(2, 8)):
                raise ValueError('incomplete instrument chromatic voice setup')
            pitch = registers[base+2] | registers[base+3] << 8
            setup = tuple(registers[base+offset] for offset in range(4, 8))
            if not 0 < pitch <= 0x3FFF or setup != SETUPS[instrument]:
                raise ValueError('unexpected instrument chromatic pitch or setup')
            voices.append({'voice': voice, 'pitch': pitch, **dict(zip(FIELDS, setup))})
        if voices[0]['pitch'] != voices[1]['pitch']:
            raise ValueError('instrument chromatic peer pitches differ')
        notes.append({'base_note': NOTES[len(notes)], 'mask': value, 'voices': voices})
    if len(notes) != len(NOTES):
        raise ValueError('incomplete instrument chromatic octave')
    return {'notes': notes}


def contract(result, instrument):
    expected = [{'base_note': note, 'mask': 12, 'voices': [
        {'voice': voice, 'pitch': pitch, **dict(zip(FIELDS, SETUPS[instrument]))}
        for voice in (2, 3)]} for note, pitch in zip(NOTES, PITCHES[instrument])]
    if result.get('notes') != expected or any(type(note[key]) is not int
            for note in result['notes'] for key in ('base_note', 'mask')) or any(
            type(value) is not int for note in result['notes'] for voice in note['voices']
            for value in voice.values()):
        raise ValueError('instrument chromatic register contract differs')


def run(trace, firmware_dir, model, instrument):
    result = phrase_run(trace, firmware_dir, model, 'chromatic',
                        fixture_builder=lambda case: build(instrument=instrument),
                        observer=lambda source: observe(source, instrument),
                        contract=lambda result, case: contract(result, instrument),
                        pattern_durations=None)
    result['instrument'] = instrument
    result['instruction_limit'] = 8000000
    return result


def check_models(results):
    if not isinstance(results, list) or len(results) != 4 or any(
            not isinstance(result, dict) or type(result.get('instrument')) is not int or
            result.get('instrument') not in SETUPS or result.get('model') not in ('sgb', 'sgb2')
            or result.get('case') != 'chromatic' for result in results) or {
            (result['model'], result['instrument']) for result in results} != {
            (model, instrument) for model in ('sgb', 'sgb2') for instrument in SETUPS}:
        raise ValueError('require both instruments on both original models')
    for result in results:
        contract(result, result['instrument'])
    # Exact contracts above imply model agreement; keep an explicit pair check.
    for instrument in SETUPS:
        pair = [result for result in results if result['instrument'] == instrument]
        if pair[0]['notes'] != pair[1]['notes']:
            raise ValueError('two-model instrument chromatic register setup differs')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--trace', type=Path, required=True)
    parser.add_argument('--firmware-dir', type=Path, required=True)
    args = parser.parse_args()
    try:
        results = [run(args.trace.resolve(), args.firmware_dir.resolve(), model, instrument)
                   for instrument in SETUPS for model in ('sgb', 'sgb2')]
        check_models(results)
    except (OSError, ValueError, subprocess.TimeoutExpired) as error:
        parser.error(str(error))
    print(json.dumps({'schema': 'gbb-sgb-instrument-chromatic-reference-v1',
                      'qualification': False, 'playback': False,
                      'evidence': 'original_program_DSP_writes_with_owned_chromatic_fixture',
                      'runs': results}, indent=2))


if __name__ == '__main__':
    main()
