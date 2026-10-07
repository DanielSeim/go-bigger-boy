#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Observe bounded two-channel phrase transitions in caller-owned original firmware."""
import argparse
import csv
import hashlib
import json
from pathlib import Path
import re
import subprocess
import tempfile

from build_sgb_phrase_fixture import build, CASES

ROOT = Path(__file__).resolve().parents[1]
MAX_ROWS = 32768
WATCHED = (0x22, 0x23, 0x24, 0x32, 0x33, 0x34, 0x3D)


def observe(source, expected_pitches=((1068, 1132), (2140, 2140))):
    reader = csv.DictReader(source)
    if reader.fieldnames != ['kind', 'master_clock', 'spc_cycle', 'pcm_sample', 'address', 'value']:
        raise ValueError('incompatible reference trace header')
    registers, keyons, cycles = {}, [], []
    previous_cycle = -1
    count = 0
    for row in reader:
        count += 1
        if count >= MAX_ROWS:
            raise ValueError('reference trace reached its row bound')
        if row['kind'] != 'D':
            continue
        if None in row or any(value is None for value in row.values()):
            raise ValueError('invalid reference DSP event')
        try:
            address, value, cycle = (int(row[key]) for key in ('address', 'value', 'spc_cycle'))
        except (TypeError, ValueError):
            raise ValueError('invalid reference DSP event') from None
        if not 0 <= address < 128 or not 0 <= value < 256 or cycle < 0 or cycle < previous_cycle:
            raise ValueError('invalid or unordered reference DSP event')
        previous_cycle = cycle
        if address in WATCHED:
            registers[address] = value
        if address != 0x4C or value == 0:
            continue
        if value != 12 or any(key not in registers for key in WATCHED):
            raise ValueError('unexpected voices or incomplete key-on setup')
        if registers[0x3D] & 12:
            raise ValueError('unexpected reference noise voices')
        voices = []
        for voice in (2, 3):
            base = 16*voice
            pitch = registers[base+2] | registers[base+3] << 8
            if registers[base+4] != 2 or not 0 < pitch <= 0x3FFF:
                raise ValueError('unexpected reference source or pitch')
            voices.append({'voice': voice, 'srcn': 2, 'pitch': pitch})
        keyons.append({'mask': value, 'voices': voices})
        cycles.append(cycle)
    if len(keyons) != len(expected_pitches) or any(b <= a for a, b in zip(cycles, cycles[1:])):
        raise ValueError('expected ordered two-channel fixture key-ons')
    if [[voice['pitch'] for voice in event['voices']] for event in keyons] != [list(pair) for pair in expected_pitches]:
        raise ValueError('reference pattern pitches differ from the fixture contract')
    result = {'keyons': keyons, 'pattern_interval_spc_cycles': cycles[-1]-cycles[0]}
    if len(expected_pitches) > 2:
        result['onset_intervals_spc_cycles'] = [b-a for a, b in zip(cycles, cycles[1:])]
    return result


def check_contract(result, case):
    low, high = (170000,180000) if case == 'both-long' else (84000,90000)
    if not low <= result['pattern_interval_spc_cycles'] <= high:
        raise ValueError('reference phrase timing differs from the observed contract')


def run(trace, firmware_directory, model, case, *, fixture_builder=build,
        observer=observe, contract=check_contract):
    program = firmware_directory / ('sgb1.program.rom' if model == 'sgb' else 'sgb2.program.rom')
    ipl = firmware_directory / 'spc700.rom'
    with tempfile.TemporaryDirectory(prefix='gbb-phrase-reference-') as directory:
        base = Path(directory)
        game, boot, inputs, output = [base / name for name in ('phrase.gb', 'boot.rom', 'none.script', 'dsp.csv')]
        image = fixture_builder(case)
        game.write_bytes(image)
        header = (ROOT / f'firmware/gameboy/{model}_boot_image.hpp').read_text()
        boot_image = bytes(int(value, 16) for value in re.findall(r'0x([0-9A-F]{2})', header))
        if len(boot_image) != 256:
            raise ValueError('invalid bundled original GB bootstrap')
        boot.write_bytes(boot_image)
        inputs.write_text('GBB SGB input v1\n0 none\n')
        command = [str(trace), str(program), str(ipl),
                   '--sync-gb-sgb1' if model == 'sgb' else '--sync-gb-sgb2', str(game), str(boot),
                   '--fractional-apu-sync', '--native-gb-input', '--input-script', str(inputs),
                   '--ppu-dma-timing', '--host-bus-timing', '--instruction-limit', '8000000',
                   '--sound-event-trace-output', str(output)]
        completed = subprocess.run(command, capture_output=True, timeout=180)
        if completed.returncode != 4 or not output.exists():
            raise ValueError(f'{model}: reference did not finish at the instruction bound')
        if output.stat().st_size > 16 * 1024 * 1024:
            raise ValueError('reference DSP trace exceeds the byte bound')
        with output.open() as source:
            result = observer(source)
        contract(result, case)
        result['first_pattern_durations'] = list(CASES[case])
        return {'model': model, 'case': case, **result,
                'fixture_sha256': hashlib.sha256(image).hexdigest(),
                'gb_boot_sha256': hashlib.sha256(boot_image).hexdigest(),
                'program_sha256': hashlib.sha256(program.read_bytes()).hexdigest(),
                'ipl_sha256': hashlib.sha256(ipl.read_bytes()).hexdigest()}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--trace', type=Path, required=True)
    parser.add_argument('--firmware-dir', type=Path, required=True)
    args = parser.parse_args()
    try:
        results = [run(args.trace.resolve(), args.firmware_dir.resolve(), model, case)
                   for case in CASES for model in ('sgb', 'sgb2')]
        for index in range(0, len(results), 2):
            first, second = results[index:index+2]
            if first['keyons'] != second['keyons'] or abs(first['pattern_interval_spc_cycles']-second['pattern_interval_spc_cycles']) > 2048:
                raise ValueError('two-model reference phrase transition differs')
    except (OSError, ValueError, subprocess.TimeoutExpired) as error:
        parser.error(str(error))
    print(json.dumps({'schema': 'gbb-sgb-phrase-reference-v1', 'qualification': False,
                      'playback': False, 'evidence': 'original_program_DSP_writes_with_owned_phrase_fixture',
                      'runs': results}, indent=2))


if __name__ == '__main__':
    main()
