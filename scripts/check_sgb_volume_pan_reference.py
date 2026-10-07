#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Observe bounded voice-volume setup from caller-owned original SGB firmware."""
import argparse
import csv
import hashlib
import json
from pathlib import Path
import re
import subprocess
import tempfile

from build_sgb_volume_pan_fixture import build, CASES

ROOT = Path(__file__).resolve().parents[1]
MAX_ROWS = 32768
WATCHED = (0x20, 0x21, 0x22, 0x23, 0x24, 0x3D)
# Limited black-box register observations, not a copied volume/pan table.
EXPECTED = {'pan': ((7, 7), (0, 11), (11, 0)),
            'volume': ((7, 7), (1, 1), (1, 1))}


def observe(source):
    reader = csv.DictReader(source)
    if reader.fieldnames != ['kind', 'master_clock', 'spc_cycle', 'pcm_sample', 'address', 'value']:
        raise ValueError('incompatible reference trace header')
    registers, notes = {}, []
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
        if value != 4 or any(key not in registers for key in WATCHED):
            raise ValueError('unexpected voice or incomplete key-on setup')
        if registers[0x24] != 2 or registers[0x3D] & 4:
            raise ValueError('unexpected reference source or noise voice')
        if (registers[0x22] | registers[0x23] << 8) != 1068:
            raise ValueError('reference pitch differs from the fixture contract')
        notes.append({'voice': 2, 'base_note': 24, 'pitch': 1068, 'srcn': 2,
                      'voll': registers[0x20], 'volr': registers[0x21]})
    if len(notes) != 3:
        raise ValueError('expected exactly three reference note key-ons')
    return {'notes': notes}


def check_contract(result, case):
    values = [(note['voll'], note['volr']) for note in result['notes']]
    if tuple(values) != EXPECTED[case]:
        raise ValueError('reference voice-volume setup differs from the observed contract')


def run(trace, firmware_directory, model, case):
    program = firmware_directory / ('sgb1.program.rom' if model == 'sgb' else 'sgb2.program.rom')
    ipl = firmware_directory / 'spc700.rom'
    with tempfile.TemporaryDirectory(prefix='gbb-volume-pan-reference-') as directory:
        base = Path(directory)
        game, boot, inputs, output = [base / name for name in ('volume-pan.gb', 'boot.rom', 'none.script', 'dsp.csv')]
        image = build(case)
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
        check_contract(result, case)
        result['notes'] = [{'request': index, 'pan': controls[0], 'song_volume': controls[1],
                            'track_volume': controls[2], **note}
                           for index, (controls, note) in enumerate(zip(CASES[case], result['notes']), 1)]
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
        for index in (0, 2):
            if results[index]['notes'] != results[index+1]['notes']:
                raise ValueError('two-model reference voice-volume setup differs')
    except (OSError, ValueError, subprocess.TimeoutExpired) as error:
        parser.error(str(error))
    print(json.dumps({'schema': 'gbb-sgb-volume-pan-reference-v1', 'qualification': False,
                      'playback': False, 'evidence': 'original_program_DSP_writes_with_owned_note_fixture',
                      'runs': results}, indent=2))


if __name__ == '__main__':
    main()
