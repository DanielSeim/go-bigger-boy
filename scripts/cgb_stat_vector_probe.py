#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Witness first STAT-vector entry across authored foreground instruction boundaries."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

import cgb_lcd_startup_probe as startup
import cgb_pcm_stat_probe as isolated

PATTERNS = {'incjr': bytes.fromhex('03 18 fd'),
            'original': bytes.fromhex('03 7a b7 20 fb')}
SCENARIOS = tuple(f'{source}-{pattern}-phase{phase}' for source in ('stat', 'software')
                  for pattern in PATTERNS for phase in range(8))
CHECKPOINT = 4
FIELDS = ['bc_high', 'bc_low', 'return_pc_low', 'return_pc_high', 'div',
          'ly', 'stat_mode', 'if_low2', 'key1_speed', 'handler_guard']


def split(scenario):
    if scenario not in SCENARIOS:
        raise ValueError('unknown STAT vector scenario')
    source, pattern, phase = scenario.split('-')
    return source, pattern, int(phase[5:])


def window(scenario):
    source, pattern, phase = split(scenario)
    # Both controls have IME and IE armed before this window. The software
    # control disables STAT and keeps LCD off until its terminal handler.
    trigger = bytes.fromhex('3e 87 e0 40') if source == 'stat' else bytes.fromhex('3e 02 e0 0f')
    return trigger + bytes(phase) + PATTERNS[pattern]


def probe_rom(scenario, double_speed=False):
    source, _, _ = split(scenario)
    rom = bytearray(startup.probe_rom('stat-irq-pad0', double_speed))
    if source == 'software':
        setup = rom[0x150:0x300]
        needle = bytes.fromhex('3e 20 e0 41')
        if setup.count(needle) != 1:
            raise ValueError('unexpected startup STAT setup')
        setup = setup.replace(needle, bytes.fromhex('3e 00 e0 41'))
        rom[0x150:0x300] = setup
    rom[0x48:0x4c] = bytes.fromhex('78 c3 00 03')
    # First vector instruction copies B into A; neither vector instruction
    # changes BC, HL or the stacked return address. The terminal handler never
    # returns, so POP DE can witness the interrupt's original return address.
    rom[0x300:0x380] = bytes(128)
    handler = bytes.fromhex('22 79 22 d1 7b 22 7a 22 f0 04 22 f0 44 22 '
                            'f0 41 e6 03 22 f0 0f e6 03 22 f0 4d e6 80 22 '
                            '3e 5a 22 c3 00 11')
    rom[0x300:0x300 + len(handler)] = handler
    rom[0x1000:0x1100] = bytes(256)
    code = window(scenario)
    rom[0x1000:0x1000 + len(code)] = code
    finish = bytes.fromhex('f3 af ea ff ff e0 41 3e 87 e0 40 '
                           '3e 01 ea 00 c0 ea 01 c0 76 18 fd')
    rom[0x1100:0x1140] = bytes(64)
    rom[0x1100:0x1100 + len(finish)] = finish
    rom[0x14d:0x150] = bytes(3)
    checksum = 0
    for value in rom[0x134:0x14d]:
        checksum = (checksum - value - 1) & 255
    rom[0x14d] = checksum
    rom[0x14e:0x150] = (sum(rom) & 65535).to_bytes(2, 'big')
    return bytes(rom)


def return_boundaries(scenario):
    _, pattern, phase = split(scenario)
    start = 0x1004
    loop = start + phase
    offsets = (0, 1) if pattern == 'incjr' else (0, 1, 2, 3)
    return set(range(start, loop)) | {loop + offset for offset in offsets}


def analyze(memory, scenario, frame):
    source, _, _ = split(scenario)
    if len(memory) < 8192 or memory[:2] != b'\x01\x01' or memory[0x3f] != 0xa5:
        raise ValueError('incomplete first-vector witness or damaged guard')
    sample = list(memory[0x10:0x1a])
    return_pc = sample[2] | sample[3] << 8
    valid = (sample[0] == 0 and sample[1] < 128 and return_pc in return_boundaries(scenario)
             and sample[5] < 154 and sample[6] < 4 and sample[7] == 0
             and sample[8] in (0, 128) and sample[9] == 0x5a)
    if source == 'software':
        valid = valid and sample[:2] == [0, 0] and return_pc == 0x1004 and sample[5:7] == [0, 0]
    return dict(events=[sample], event_fields=FIELDS, return_pc=return_pc,
                key1_speed=sample[8], authored_constraints_match=bool(valid),
                event_sha256=hashlib.sha256(bytes(sample)).hexdigest())


def run_probe(runner, adapter, output):
    report = isolated.run_probe(runner, adapter, output, scenarios=SCENARIOS, checkpoints=(CHECKPOINT,),
                                rom_builder=probe_rom, analyzer=analyze, write_report=False,
                                comparison_key='event_sha256', comparison_name='first_stat_vector',
                                schema='gbb-cgb-stat-vector-probe-v1')
    for row in report['rows']:
        for capture in row['captures'].values():
            capture['authored_constraints_match'] = (capture['authored_constraints_match'] and
                capture['key1_speed'] == (128 if row['double_speed'] else 0))
    report['scope'] = ('First terminal STAT handler records BC and stacked return PC across two '
                       'foreground loops and eight post-trigger NOP phases. Software IF controls '
                       'keep LCD/STAT sources off. Later register reads are sequential, not edge timestamps.')
    (output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    return report


def passes(report):
    try:
        rows = report['rows']
        expected = {(s, speed, CHECKPOINT) for s in SCENARIOS for speed in (False, True)}
        return (len(rows) == len(expected) and all(type(r['double_speed']) is bool for r in rows)
                and {(r['scenario'], r['double_speed'], r['frame']) for r in rows} == expected
                and all(r['match'] is True and set(r['captures']) == {'gbb', 'sameboy'} and all(
                    c['repeatable'] is True and c['authored_constraints_match'] is True
                    for c in r['captures'].values()) for r in rows))
    except (KeyError, TypeError):
        return False


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--runner', type=Path, required=True)
    parser.add_argument('--sameboy-capture', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    try:
        report = run_probe(args.runner.resolve(), args.sameboy_capture.resolve(), args.output.resolve())
        print(json.dumps(report))
        return 0 if passes(report) else 1
    except (OSError, ValueError, KeyError, subprocess.SubprocessError) as error:
        parser.exit(2, f'STAT vector probe failed: {error}\n')


if __name__ == '__main__':
    raise SystemExit(main())
