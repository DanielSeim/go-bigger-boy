#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Authored pulse frequency-write boundary controls with repeated PCM reads."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

import cgb_pcm_stat_probe as isolated

SCENARIOS = tuple(f'ch{channel}-{kind}' for channel in (1, 2) for kind in ('high', 'low', 'control'))
SAMPLES = 64


def split(scenario):
    if scenario not in SCENARIOS:
        raise ValueError('unknown pulse frequency scenario')
    channel, kind = scenario.split('-')
    return int(channel[2:]), kind


def probe_rom(scenario, double_speed=False):
    channel, kind = split(scenario)
    rom = bytearray(isolated.probe_rom('silence', double_speed))
    start = rom.find(bytes.fromhex('21 00 c1 af e0 04 e0 0f 3e e2 e0 05 e0 06'), 0x150, 0x300)
    if start < 0:
        raise ValueError('isolated startup layout changed')
    rom[start:start + 7] = bytes.fromhex('af ea ff ff c3 00 10')
    code = bytearray.fromhex('21 00 c1')
    offset = 0 if channel == 1 else 5
    for delay in range(SAMPLES):
        code += bytes.fromhex('af e0 26 e0 04 3e 80 e0 26')
        # 12.5% duty isolates one high sample in each eight-step period.
        for register, value in ((0x11 + offset, 0), (0x12 + offset, 0xf0),
                                (0x13 + offset, 0xfc), (0x14 + offset, 0x87)):
            code += bytes((0x3e, value, 0xe0, register))
        value, register = ((0, 0x14) if kind == 'high' else
                           (0xf8, 0x13) if kind == 'low' else (7, 0x14))
        code += bytes((0x3e, value)) + bytes(delay) + bytes((0xe0, register + offset))
        code += bytes.fromhex('f0 76 22') + bytes(16) + bytes.fromhex('f0 76 22 f0 26 22')
    code += bytes.fromhex('af e0 26 3e 40 ea 00 c0 3e 01 ea 01 c0 3e 91 e0 40 f3 76 18 fd')
    if 0x1000 + len(code) > len(rom):
        raise ValueError('pulse frequency program exceeds ROM')
    rom[0x1000:0x1000 + len(code)] = code
    rom[0x14d:0x150] = bytes(3)
    checksum = 0
    for value in rom[0x134:0x14d]:
        checksum = (checksum - value - 1) & 255
    rom[0x14d] = checksum
    rom[0x14e:0x150] = (sum(rom) & 65535).to_bytes(2, 'big')
    return bytes(rom)


def analyze(memory, scenario, frame):
    channel, kind = split(scenario)
    if len(memory) < 8192 or memory[:2] != bytes((SAMPLES, 1)) or memory[0x200] != 0xa5:
        raise ValueError('incomplete pulse frequency log or damaged guard')
    raw = [list(memory[i:i + 3]) for i in range(0x100, 0x100 + 3 * SAMPLES, 3)]
    amplitude = 15 if channel == 1 else 240
    status = 0xf0 | (1 << (channel - 1))
    valid = (all(a in (0, amplitude) and b in (0, amplitude) and s == status for a, b, s in raw)
             and {v for a, b, _ in raw for v in (a, b)} == {0, amplitude})
    divisor = 1 if channel == 1 else 16
    events = [[a // divisor, b // divisor] for a, b, _ in raw]
    return dict(events=events, observations=raw, observation_fields=['immediate_pcm12', 'late_pcm12', 'nr52'],
                channel=channel, write_kind=kind, write_delay_nops=list(range(SAMPLES)),
                write_after_trigger_cpu_t_cycles=[20 + 4 * i for i in range(SAMPLES)],
                pcm_read_after_write_cpu_t_cycles=[12, 96],
                record_structure_match=valid, authored_constraints_match=valid,
                event_sha256=hashlib.sha256(bytes(v for pair in events for v in pair)).hexdigest())


def run_probe(runner, adapter, output, case=None):
    if case is not None:
        split(case)
    return isolated.run_probe(runner, adapter, output,
        scenarios=(case,) if case is not None else SCENARIOS, checkpoints=(2,),
        rom_builder=probe_rom, analyzer=analyze,
        comparison_key='event_sha256', comparison_name='pulse_frequency_write_boundary',
        schema='gbb-cgb-pulse-frequency-probe-v1')


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
        parser.exit(2, f'pulse frequency probe failed: {error}\n')


if __name__ == '__main__':
    raise SystemExit(main())
