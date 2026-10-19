#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Read STAT IF after equal-cost foreground prefixes with interrupt service disabled."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

import cgb_stat_vector_probe as vector
import cgb_pcm_stat_probe as isolated

# Each tuple is (instruction bytes, nominal CPU T-cycles). Forward JR +0
# retains the taken branch's cost and flags while allowing finite unrolling.
INSTRUCTIONS = {'incjr': ((b'\x03', 8), (b'\x18\x00', 12)),
                'original': ((b'\x03', 8), (b'\x7a', 4), (b'\xb7', 4), (b'\x20\x00', 12))}
SCENARIOS = tuple(f'{pattern}-phase{phase}-lap{lap}-slot{slot}'
                  for pattern, instructions in INSTRUCTIONS.items() for phase in range(8)
                  for lap in (-1, 0, 1) for slot in range(len(instructions)))
CHECKPOINT = 4
FIELDS = ['first_if_low2', 'bc_high', 'bc_low', 'later_stat_mode', 'later_ly',
          'later_if_low2', 'div', 'key1_speed', 'completion_guard']


def split(scenario):
    if scenario not in SCENARIOS:
        raise ValueError('unknown STAT boundary read scenario')
    # Negative lap numbers are parsed by named separators, not split("-").
    pattern, tail = scenario.split('-phase')
    phase, tail = tail.split('-lap')
    lap, slot = tail.split('-slot')
    return pattern, int(phase), int(lap), int(slot)


def prefix(scenario, double_speed=False):
    pattern, phase, lap, slot = split(scenario)
    instructions = INSTRUCTIONS[pattern]
    loop_cost = sum(cost for _, cost in instructions)
    # Sweep the three whole-loop counts nearest the established 452-dot
    # first-line duration. This common analytic grid is never fitted per core.
    iterations = (452 * (2 if double_speed else 1)) // loop_cost + lap
    body = b''.join(code for code, _ in instructions)
    partial = b''.join(code for code, _ in instructions[:slot])
    code = bytes(phase) + body * iterations + partial
    cycles = phase * 4 + iterations * loop_cost + sum(cost for _, cost in instructions[:slot])
    count = iterations + (1 if slot else 0)
    return code, cycles, count


def probe_rom(scenario, double_speed=False):
    pattern, phase, _, _ = split(scenario)
    rom = bytearray(vector.probe_rom(f'stat-{pattern}-phase{phase}', double_speed))
    setup = rom[0x150:0x300]
    # DI remains effective. Replace EI by a same-cost NOP; keep IE=2 and all
    # setup timing identical to the vector witness. No ISR can acknowledge IF.
    needle = bytes.fromhex('fb 00 af e0 04 c3 00 10')
    if setup.count(needle) != 1:
        raise ValueError('unexpected vector startup layout')
    rom[0x150:0x300] = setup.replace(needle, bytes.fromhex('00 00 af e0 04 c3 00 10'))
    code, _, _ = prefix(scenario, double_speed)
    window = bytes.fromhex('3e 87 e0 40') + code
    # The primary read is the first observation. Subsequent stores and reads
    # advance time, but cannot change that recorded value or the BC witness.
    window += bytes.fromhex('f0 0f e6 03 22 78 22 79 22 '
                            'f0 41 e6 03 22 f0 44 22 f0 0f e6 03 22 '
                            'f0 04 22 f0 4d e6 80 22 3e 5a 22 c3 00 11')
    if len(window) >= 256:
        raise ValueError('boundary prefix overlaps completion marker')
    rom[0x1000:0x1100] = bytes(256)
    rom[0x1000:0x1000 + len(window)] = window
    rom[0x14d:0x150] = bytes(3)
    checksum = 0
    for value in rom[0x134:0x14d]:
        checksum = (checksum - value - 1) & 255
    rom[0x14d] = checksum
    rom[0x14e:0x150] = (sum(rom) & 65535).to_bytes(2, 'big')
    return bytes(rom)


def analyze(memory, scenario, frame):
    split(scenario)
    if len(memory) < 8192 or memory[:2] != b'\x01\x01' or memory[0x3f] != 0xa5:
        raise ValueError('incomplete boundary read witness or damaged guard')
    sample = list(memory[0x10:0x19])
    speed = sample[7]
    _, cycles, count = prefix(scenario, speed == 128)
    valid = (sample[0] in (0, 2) and sample[1:3] == [0, count] and sample[3] < 4
             and sample[4] <= 1 and sample[5] in (0, 2) and speed in (0, 128)
             and sample[8] == 0x5a and (sample[0] == 0 or sample[5] == 2))
    return dict(events=[sample], event_fields=FIELDS, key1_speed=speed,
                prefix_cpu_t_cycles=cycles, expected_bc=count,
                authored_constraints_match=bool(valid),
                event_sha256=hashlib.sha256(bytes(sample)).hexdigest())


def run_probe(runner, adapter, output):
    report = isolated.run_probe(runner, adapter, output, scenarios=SCENARIOS, checkpoints=(CHECKPOINT,),
                                rom_builder=probe_rom, analyzer=analyze, write_report=False,
                                comparison_key='event_sha256', comparison_name='stat_boundary_if_read',
                                schema='gbb-cgb-stat-boundary-read-probe-v1')
    for row in report['rows']:
        for capture in row['captures'].values():
            capture['authored_constraints_match'] = (capture['authored_constraints_match'] and
                capture['key1_speed'] == (128 if row['double_speed'] else 0))
    report['scope'] = ('IME off with equal-cost EI replacement, IE=2 retained. Finite foreground '
                       'prefixes retain taken-JR costs with displacement zero. Primary IF reads '
                       'start at swept instruction boundaries; later reads are sequential. '
                       'CPU-visible read agreement does not establish physical request-edge timing.')
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
        parser.exit(2, f'STAT boundary read probe failed: {error}\n')


if __name__ == '__main__':
    raise SystemExit(main())
