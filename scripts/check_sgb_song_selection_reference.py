#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check explicit song IDs against caller-owned original program images."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import tempfile

from build_sgb_song_selection_fixture import build

ROOT = Path(__file__).resolve().parents[1]


def check_reads(report, order):
    build(order)
    if (not isinstance(report, dict) or
        report.get('schema') != 'gbb-sgb-score-table-reads-v1' or
        report.get('qualification') is not False or report.get('overflow') is not False or
        report.get('base') != 0x2B00 or report.get('bytes') != 6 or
        report.get('capacity') != 256):
        raise ValueError('missing, overflowing or incompatible address observation')
    events = report.get('events')
    if not isinstance(events, list) or len(events) > 256 or report.get('reads') != len(events):
        raise ValueError('inconsistent address observation length')
    fields = {'master_clock', 'spc_half_clock', 'address', 'audible_packets', 'base_writes'}
    for event in events:
        if (not isinstance(event, dict) or set(event) != fields or
            any(type(event[key]) is not int or event[key] < 0 for key in fields) or
            not 0x2B00 <= event['address'] < 0x2B06):
            raise ValueError('invalid address observation event')
    if any(a['spc_half_clock'] >= b['spc_half_clock'] or
           a['master_clock'] > b['master_clock'] or
           a['base_writes'] > b['base_writes'] or
           a['audible_packets'] > b['audible_packets'] for a, b in zip(events, events[1:])):
        raise ValueError('address observations are out of order')
    observations = []
    for number, song in enumerate(order, 1):
        request_events = [event for event in events if event['audible_packets'] == number]
        low = 0x2B00 + 2 * (song - 1)
        if [event['address'] for event in request_events] != [low + 1, low]:
            raise ValueError(f'request {number}, code {song}: expected high/low directory reads')
        if any(event['base_writes'] < 2 for event in request_events):
            raise ValueError('selection occurred before the fixture replaced the directory')
        observations.append({'request': number, 'code': song, 'word_address': low})
    if any(event['audible_packets'] > len(order) for event in events):
        raise ValueError('unexpected additional nonzero SOUND delivery')
    if len({event['base_writes'] for event in events if event['audible_packets']}) != 1:
        raise ValueError('directory was rewritten between fixture requests')
    return observations


def run(trace, firmware_directory, model, order, instructions=8000000):
    program_path = firmware_directory / ('sgb1.program.rom' if model == 'sgb' else 'sgb2.program.rom')
    ipl_path = firmware_directory / 'spc700.rom'
    with tempfile.TemporaryDirectory(prefix='gbb-song-selection-') as directory:
        base = Path(directory)
        game, boot, inputs, output = [base / name for name in ('selection.gb', 'boot.rom', 'none.script', 'reads.json')]
        image = build(order)
        game.write_bytes(image)
        header = (ROOT / f'firmware/gameboy/{model}_boot_image.hpp').read_text()
        boot_image = bytes(int(value, 16) for value in re.findall(r'0x([0-9A-F]{2})', header))
        if len(boot_image) != 256:
            raise ValueError('invalid bundled original GB bootstrap')
        boot.write_bytes(boot_image)
        inputs.write_text('GBB SGB input v1\n0 none\n')
        command = [str(trace), str(program_path), str(ipl_path),
                   '--sync-gb-sgb1' if model == 'sgb' else '--sync-gb-sgb2', str(game), str(boot),
                   '--fractional-apu-sync', '--native-gb-input', '--input-script', str(inputs),
                   '--ppu-dma-timing', '--host-bus-timing', '--instruction-limit', str(instructions),
                   '--score-table-read-output', str(output)]
        # Child diagnostics can include private firmware data. Never forward
        # them into a metadata report, including when the child fails.
        completed = subprocess.run(command, capture_output=True, timeout=180)
        if completed.returncode != 4 or not output.exists():
            raise ValueError(f'{model}: reference did not finish at the instruction bound')
        observations = check_reads(json.loads(output.read_text()), order)
        return {'model': model, 'observations': observations,
                'fixture_sha256': hashlib.sha256(image).hexdigest(),
                'gb_boot_sha256': hashlib.sha256(boot_image).hexdigest(),
                'program_sha256': hashlib.sha256(program_path.read_bytes()).hexdigest(),
                'ipl_sha256': hashlib.sha256(ipl_path.read_bytes()).hexdigest()}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--trace', required=True, type=Path)
    parser.add_argument('--firmware-dir', required=True, type=Path)
    parser.add_argument('--order', default='1,2,3')
    args = parser.parse_args()
    try:
        order = [int(value) for value in args.order.split(',')]
        build(order)  # Validate before reading private images or running a child.
        results = [run(args.trace.resolve(), args.firmware_dir.resolve(), model, order)
                   for model in ('sgb', 'sgb2')]
    except (OSError, ValueError, subprocess.TimeoutExpired) as error:
        parser.error(str(error))
    print(json.dumps({'schema': 'gbb-sgb-song-selection-reference-v1',
                      'qualification': False, 'playback': False,
                      'evidence': 'original_program_with_original_empty_song_fixture',
                      'runs': results}, indent=2))


if __name__ == '__main__':
    main()
