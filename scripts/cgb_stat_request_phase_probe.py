#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Sweep first mode-2 IF/mode/LY reads and isolate software STAT recognition."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

import cgb_lcd_startup_probe as startup
import cgb_pcm_stat_probe as isolated

PHASES = tuple(range(13))
SCENARIOS = tuple(f'{register}-phase-{phase}' for register in ('if', 'mode', 'ly') for phase in PHASES)
SCENARIOS += tuple(f'software-phase-{phase}' for phase in range(4))
CHECKPOINT = 4


def split(scenario):
    if scenario not in SCENARIOS:
        raise ValueError('unknown STAT request phase')
    kind, phase = scenario.split('-phase-')
    return kind, int(phase)


def probe_rom(scenario, double_speed=False):
    kind, phase = split(scenario)
    rom = bytearray(startup.probe_rom('stat-poll-pad0', double_speed))
    rom[0x300:0x380] = bytes(128)
    rom[0x1000:0x1500] = bytes(0x500)
    if kind == 'software':
        # LCD stays off and STAT sources are disabled. The only request is the
        # authored IF write with IE/IME already armed. BC counts foreground INCs.
        code = bytearray.fromhex('af e0 41 e0 0f 3e 02 ea ff ff fb 00')
        code += bytes(phase) + bytes.fromhex('e0 0f 03 18 fd')
        logger = bytes.fromhex('78 22 79 22 f0 0f e6 03 22 f0 04 22 f0 4d e6 80 22 '
                               '3e 5a 22 c3 00 14')
        rom[0x300:0x300 + len(logger)] = logger
    else:
        # CPU NOPs after the accepted LCD write, not an empirically fitted
        # per-emulator offset. Double speed doubles the common 432-dot delay.
        delay = 108 * (2 if double_speed else 1) + phase
        code = bytearray.fromhex('3e 87 e0 40') + bytes(delay)
        register = {'if': 0x0f, 'mode': 0x41, 'ly': 0x44}[kind]
        code += bytes((0xf0, register))
        if kind != 'ly':
            code += bytes.fromhex('e6 03')
        code += bytes.fromhex('22 f0 0f e6 03 22 f0 41 e6 03 22 f0 44 22 '
                              'f0 04 22 f0 4d e6 80 22 c3 00 14')
    if len(code) >= 0x400:
        raise ValueError('phase sweep overlaps completion marker')
    rom[0x1000:0x1000 + len(code)] = code
    # Software IRQ controls need the LCD enabled here to permit frame capture.
    # No foreground path can publish completion before their handler executes.
    finish = bytes.fromhex('f3 af ea ff ff e0 41 3e 87 e0 40 '
                           '3e 01 ea 00 c0 ea 01 c0 76 18 fd')
    rom[0x1400:0x1400 + len(finish)] = finish
    rom[0x14d:0x150] = bytes(3)
    checksum = 0
    for value in rom[0x134:0x14d]:
        checksum = (checksum - value - 1) & 255
    rom[0x14d] = checksum
    rom[0x14e:0x150] = (sum(rom) & 65535).to_bytes(2, 'big')
    return bytes(rom)


def analyze(memory, scenario, frame):
    kind, phase = split(scenario)
    if len(memory) < 8192 or memory[:2] != b'\x01\x01' or memory[0x3f] != 0xa5:
        raise ValueError('incomplete request-phase observation or damaged guard')
    sample = list(memory[0x10:0x16])
    if kind == 'software':
        fields = ['bc_high', 'bc_low', 'if_low2', 'div', 'key1_speed', 'handler_guard']
        valid = sample[0] == 0 and sample[1] <= 1 and sample[2] == 0 and sample[5] == 0x5a
        speed = sample[4]
    else:
        fields = [f'first_{kind}', 'later_if_low2', 'later_stat_mode', 'later_ly', 'div', 'key1_speed']
        valid = (sample[0] in ((0, 1) if kind == 'ly' else (0, 2)) and sample[1] in (0, 2) and
                 sample[2] < 4 and sample[3] <= 1)
        speed = sample[5]
    valid = valid and speed in (0, 128)
    return dict(observation=sample, observation_fields=fields, events=[sample], event_fields=fields,
                phase_nops=phase, key1_speed=speed,
                event_sha256=hashlib.sha256(bytes(sample)).hexdigest(),
                authored_constraints_match=bool(valid))


def run_probe(runner, adapter, output):
    report = isolated.run_probe(runner, adapter, output, scenarios=SCENARIOS, checkpoints=(CHECKPOINT,),
                                rom_builder=probe_rom, analyzer=analyze, write_report=False,
                                comparison_key='event_sha256', comparison_name='stat_request_phase',
                                schema='gbb-cgb-stat-request-phase-probe-v1')
    for row in report['rows']:
        for capture in row['captures'].values():
            capture['authored_constraints_match'] = (capture['authored_constraints_match'] and
                capture['key1_speed'] == (128 if row['double_speed'] else 0))
    report['scope'] = ('Common post-LCD CPU NOP delays, followed by one primary IF/mode/LY read; '
                       'software IF controls keep LCD/STAT sources off until handler completion. '
                       'Reads within each record occur at different times; no timestamp is fitted.')
    (output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    return report


def passes(report):
    try:
        rows = report['rows']
        expected = {(s, speed, CHECKPOINT) for s in SCENARIOS for speed in (False, True)}
        return (len(rows) == len(expected) and
                all(type(r['double_speed']) is bool for r in rows) and
                {(r['scenario'], r['double_speed'], r['frame']) for r in rows} == expected and
                all(r['match'] is True and set(r['captures']) == {'gbb', 'sameboy'} and all(
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
        parser.exit(2, f'STAT request phase probe failed: {error}\n')


if __name__ == '__main__':
    raise SystemExit(main())
