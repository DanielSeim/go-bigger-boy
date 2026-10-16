#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Authored pulse retrigger phase sweep with equal-cost no-retrigger controls."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

import cgb_pcm_stat_probe as isolated

GAPS = (0, 1, 2, 3, 8, 32)
SCENARIOS = tuple(f'ch{channel}-{operation}-gap-{gap}' for channel in (1, 2)
                  for operation in ('retrigger', 'observe') for gap in GAPS)
SAMPLES = 64


def split(scenario):
    if scenario not in SCENARIOS:
        raise ValueError('unknown pulse retrigger scenario')
    channel, operation, _, gap = scenario.split('-')
    return int(channel[2:]), operation, int(gap)


def probe_rom(scenario, double_speed=False):
    channel, operation, gap = split(scenario)
    rom = bytearray(isolated.probe_rom('silence', double_speed))
    start = rom.find(bytes.fromhex('21 00 c1 af e0 04 e0 0f 3e e2 e0 05 e0 06'), 0x150, 0x300)
    if start < 0:
        raise ValueError('isolated startup layout changed')
    rom[start:start + 7] = bytes.fromhex('af ea ff ff c3 00 10')
    code = bytearray.fromhex('21 00 c1 f0 4d ea 02 c0')
    code += bytes((0x3e, 0x80 if double_speed else 0)) + bytes.fromhex('ea 03 c0')
    offset = 0 if channel == 1 else 5
    for delay in range(SAMPLES):
        code += bytes.fromhex('af e0 26 e0 04 3e 80 e0 26')
        for register, value in ((0x11 + offset, 0), (0x12 + offset, 0xf0),
                                (0x13 + offset, 0xfc), (0x14 + offset, 0x87)):
            code += bytes((0x3e, value, 0xe0, register))
        # The control substitutes three NOPs for LDH (12 CPU T-cycles).
        code += bytes(gap) + bytes.fromhex('3e 87')
        code += bytes((0xe0, 0x14 + offset)) if operation == 'retrigger' else bytes(3)
        # No register write or STOP occurs between this anchor and the PCM read.
        code += bytes(delay) + bytes.fromhex('f0 76 22 f0 26 22 f0 4d 22')
    code += bytes.fromhex('af e0 26 3e 40 ea 00 c0 3e 01 ea 01 c0 3e 91 e0 40 f3 76 18 fd')
    if 0x1000 + len(code) > len(rom):
        raise ValueError('pulse retrigger program exceeds ROM')
    rom[0x1000:0x1000 + len(code)] = code
    rom[0x14d:0x150] = bytes(3)
    checksum = 0
    for value in rom[0x134:0x14d]:
        checksum = (checksum - value - 1) & 255
    rom[0x14d] = checksum
    rom[0x14e:0x150] = (sum(rom) & 65535).to_bytes(2, 'big')
    return bytes(rom)


def analyze(memory, scenario, frame):
    channel, operation, gap = split(scenario)
    if len(memory) < 8192 or memory[:2] != bytes((SAMPLES, 1)) or memory[0x200] != 0xa5:
        raise ValueError('incomplete pulse retrigger log or damaged guard')
    if memory[3] not in (0, 0x80) or memory[2] & 0x81 != memory[3]:
        raise ValueError('initial CPU speed witness mismatch')
    raw = [list(memory[i:i + 3]) for i in range(0x100, 0x1c0, 3)]
    amplitude = 15 if channel == 1 else 240
    status = 0xf0 | (1 << (channel - 1))
    valid = ({r[0] for r in raw} == {0, amplitude} and
             all(r[1] == status and r[2] & 0x81 == memory[3] for r in raw))
    events = [r[0] // (1 if channel == 1 else 16) for r in raw]
    return dict(events=events, observations=raw, observation_fields=['pcm12', 'nr52', 'key1'],
                channel=channel, operation=operation, gap_nops=gap, double_speed=bool(memory[3]),
                operation_after_first_trigger_cpu_t_cycles=20 + 4 * gap,
                pcm_read_after_operation_cpu_t_cycles=[12 + 4 * i for i in range(SAMPLES)],
                first_nonzero_sample=next((i for i, value in enumerate(events) if value), None),
                record_structure_match=valid, authored_constraints_match=valid,
                event_sha256=hashlib.sha256(bytes(events)).hexdigest())


def run_probe(runner, adapter, output, case=None):
    if case is not None:
        split(case)
    return isolated.run_probe(runner, adapter, output,
        scenarios=(case,) if case is not None else SCENARIOS, checkpoints=(2,),
        rom_builder=probe_rom, analyzer=analyze,
        comparison_key='event_sha256', comparison_name='pulse_retrigger_phase',
        schema='gbb-cgb-pulse-retrigger-probe-v1')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--runner', type=Path, required=True)
    parser.add_argument('--sameboy-capture', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--case', choices=SCENARIOS)
    args = parser.parse_args()
    try:
        report = run_probe(args.runner.resolve(), args.sameboy_capture.resolve(), args.output.resolve(), args.case)
        print(json.dumps(report))
        return 0 if isolated.passes(report) else 1
    except (OSError, ValueError, KeyError, subprocess.SubprocessError) as error:
        parser.exit(2, f'pulse retrigger probe failed: {error}\n')


if __name__ == '__main__':
    raise SystemExit(main())
