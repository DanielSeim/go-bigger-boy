#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Observe bounded DSP note setup from caller-owned original SGB firmware."""
import argparse
import csv
import hashlib
import json
from pathlib import Path
import re
import subprocess
import tempfile

from build_sgb_pitch_fixture import build, NOTES

ROOT = Path(__file__).resolve().parents[1]
MAX_ROWS = 32768
FIELDS = ('pitch_low', 'pitch_high', 'srcn', 'adsr1', 'adsr2', 'gain')
WATCHED = {0x22 + index: name for index, name in enumerate(FIELDS)}
# Limited black-box observations, not a resident instrument-table dump.
EXPECTED = {2: ((1068, 1132, 2140), (2, 0x8F, 0x6F, 0xB8)),
            10: ((7993, 8472, 16016), (10, 0x8E, 0xAF, 0xB8))}


def observe(source):
    """Retain only voice-2 setup at KON; never retain RAM or sample bytes."""
    reader = csv.DictReader(source)
    if reader.fieldnames != ['kind', 'master_clock', 'spc_cycle', 'pcm_sample', 'address', 'value']:
        raise ValueError('incompatible reference trace header')
    registers = {}
    keyons = []
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
        if address in WATCHED or address == 0x3D:
            registers[address] = value
        if address != 0x4C or value == 0:
            continue
        if value != 4 or any(address not in registers for address in (*WATCHED, 0x3D)):
            raise ValueError('unexpected voice or incomplete key-on setup')
        pitch = registers[0x22] | registers[0x23] << 8
        if not 0 < pitch <= 0x3FFF or registers[0x3D] & 4:
            raise ValueError('fixture must key on a pitched voice')
        keyons.append({'voice': 2, 'pitch': pitch,
                       **{WATCHED[address]: registers[address] for address in range(0x24, 0x28)}})
    if len(keyons) != len(NOTES):
        raise ValueError('expected exactly three reference note key-ons')
    setup = ('srcn', 'adsr1', 'adsr2', 'gain')
    if any(any(k[field] != keyons[0][field] for field in setup) for k in keyons):
        raise ValueError('reference instrument setup changed between notes')
    pitches = [k['pitch'] for k in keyons]
    ratios = [pitches[1] / pitches[0], pitches[2] / pitches[0]]
    if abs(ratios[0] - 2**(1/12)) > 0.005 or abs(ratios[1] - 2) > 0.01:
        raise ValueError('reference pitch intervals are outside the fixture tolerance')
    return {'notes': [{'base_note': note, **keyon} for note, keyon in zip(NOTES, keyons)],
            'semitone_ratio': ratios[0], 'octave_ratio': ratios[1]}


def check_contract(result, instrument):
    pitches, setup = EXPECTED[instrument]
    for note, pitch in zip(result['notes'], pitches):
        if note['pitch'] != pitch or tuple(note[key] for key in ('srcn', 'adsr1', 'adsr2', 'gain')) != setup:
            raise ValueError('reference note setup differs from the observed contract')


def run(trace, firmware_directory, model, instrument):
    program = firmware_directory / ('sgb1.program.rom' if model == 'sgb' else 'sgb2.program.rom')
    ipl = firmware_directory / 'spc700.rom'
    with tempfile.TemporaryDirectory(prefix='gbb-pitch-reference-') as directory:
        base = Path(directory)
        game, boot, inputs, output = [base / name for name in ('pitch.gb', 'boot.rom', 'none.script', 'dsp.csv')]
        image = build(instrument)
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
            result = observe(source)
        check_contract(result, instrument)
        return {'model': model, 'instrument': instrument, **result,
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
        results = [run(args.trace.resolve(), args.firmware_dir.resolve(), model, instrument)
                   for instrument in (2, 10) for model in ('sgb', 'sgb2')]
        for index in (0, 2):
            if results[index]['notes'] != results[index+1]['notes']:
                raise ValueError('two-model reference note setup differs')
    except (OSError, ValueError, subprocess.TimeoutExpired) as error:
        parser.error(str(error))
    print(json.dumps({'schema': 'gbb-sgb-pitch-reference-v1',
                      'qualification': False, 'playback': False,
                      'evidence': 'original_program_DSP_writes_with_original_note_fixture',
                      'runs': results}, indent=2))


if __name__ == '__main__':
    main()
