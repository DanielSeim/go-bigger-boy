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
LIFECYCLE_SCENARIOS = tuple(f'ch{channel}-{kind}-{mode}' for channel in (1, 2)
                            for kind in ('high', 'low', 'control', 'observe')
                            for mode in ('retrigger', 'switch', 'switch-retrigger'))
SAMPLES = 64


def split(scenario):
    if scenario not in SCENARIOS + LIFECYCLE_SCENARIOS:
        raise ValueError('unknown pulse frequency scenario')
    channel, kind = scenario.split('-')[:2]
    return int(channel[2:]), kind


def lifecycle_mode(scenario):
    split(scenario)
    return '-'.join(scenario.split('-')[2:]) or 'fresh'


def probe_rom(scenario, double_speed=False):
    channel, kind = split(scenario)
    mode = lifecycle_mode(scenario)
    switched = 'switch' in mode
    rom = bytearray(isolated.probe_rom('silence', double_speed))
    start = rom.find(bytes.fromhex('21 00 c1 af e0 04 e0 0f 3e e2 e0 05 e0 06'), 0x150, 0x300)
    if start < 0:
        raise ValueError('isolated startup layout changed')
    rom[start:start + 7] = bytes.fromhex('af ea ff ff c3 00 10')
    code = bytearray.fromhex('21 00 c1')
    if mode != 'fresh':
        # Witness the requested initial speed independently of each sample's
        # final KEY1. Keep the old controls byte-for-byte unchanged.
        code += bytes.fromhex('f0 4d ea 02 c0')
        code += bytes((0x3e, 0x80 if double_speed else 0)) + bytes.fromhex('ea 03 c0')
    offset = 0 if channel == 1 else 5
    for delay in range(SAMPLES):
        if switched and delay:
            # Restore the initial speed before resetting NR52/DIV for the next
            # observation. This STOP belongs outside the measured interval.
            code += bytes.fromhex('3e 01 e0 4d 10 00')
        code += bytes.fromhex('af e0 26 e0 04 3e 80 e0 26')
        # 12.5% duty isolates one high sample in each eight-step period.
        for register, value in ((0x11 + offset, 0), (0x12 + offset, 0xf0),
                                (0x13 + offset, 0xfc), (0x14 + offset, 0x87)):
            code += bytes((0x3e, value, 0xe0, register))
        if switched:
            code += bytes.fromhex('3e 01 e0 4d 10 00')
        if 'retrigger' in mode:
            code += bytes.fromhex('00 3e 87') + bytes((0xe0, 0x14 + offset))
        value, register = ((0, 0x14) if kind == 'high' else
                           (0xf8, 0x13) if kind == 'low' else (7, 0x14))
        code += bytes((0x3e, value)) + bytes(delay)
        # Same 12 CPU T-cycles as LDH, without touching a frequency register.
        code += bytes(3) if kind == 'observe' else bytes((0xe0, register + offset))
        code += bytes.fromhex('f0 76 22') + bytes(16) + bytes.fromhex('f0 76 22 f0 26 22')
        if mode != 'fresh':
            code += bytes.fromhex('f0 4d 22')
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
    mode = lifecycle_mode(scenario)
    if len(memory) < 8192 or memory[:2] != bytes((SAMPLES, 1)) or memory[0x200] != 0xa5:
        raise ValueError('incomplete pulse frequency log or damaged guard')
    stride = 3 if mode == 'fresh' else 4
    raw = [list(memory[i:i + stride]) for i in range(0x100, 0x100 + stride * SAMPLES, stride)]
    amplitude = 15 if channel == 1 else 240
    status = 0xf0 | (1 << (channel - 1))
    valid = (all(r[0] in (0, amplitude) and r[1] in (0, amplitude) and r[2] == status for r in raw)
             and {v for r in raw for v in r[:2]} == {0, amplitude})
    if mode != 'fresh':
        if memory[3] not in (0, 0x80) or memory[2] & 0x81 != memory[3]:
            raise ValueError('initial CPU speed witness mismatch')
        expected_speed = memory[3] ^ (0x80 if 'switch' in mode else 0)
        valid = valid and all(r[3] & 0x81 == expected_speed for r in raw)
    divisor = 1 if channel == 1 else 16
    events = [[r[0] // divisor, r[1] // divisor] for r in raw]
    result = dict(events=events, observations=raw, observation_fields=['immediate_pcm12', 'late_pcm12', 'nr52'],
                channel=channel, write_kind=kind, write_delay_nops=list(range(SAMPLES)),
                write_after_trigger_cpu_t_cycles=[20 + 4 * i for i in range(SAMPLES)],
                pcm_read_after_write_cpu_t_cycles=[12, 96],
                record_structure_match=valid, authored_constraints_match=valid,
                event_sha256=hashlib.sha256(bytes(v for pair in events for v in pair)).hexdigest())
    if mode != 'fresh':
        result['observation_fields'].append('key1')
        result.update(lifecycle=mode, initial_double_speed=bool(memory[3]),
                      final_double_speed=bool(expected_speed),
                      timing_anchor='retrigger_write' if 'retrigger' in mode else 'speed_switch_completion',
                      operation='equal_cost_nops' if kind == 'observe' else 'frequency_write',
                      operation_after_anchor_cpu_t_cycles=result.pop('write_after_trigger_cpu_t_cycles'))
    return result


def run_probe(runner, adapter, output, case=None, lifecycle=False):
    scenarios = LIFECYCLE_SCENARIOS if lifecycle else SCENARIOS
    if case is not None:
        if case not in scenarios:
            raise ValueError('case does not belong to the selected frequency matrix')
    return isolated.run_probe(runner, adapter, output,
        scenarios=(case,) if case is not None else scenarios, checkpoints=(2,),
        rom_builder=probe_rom, analyzer=analyze,
        comparison_key='event_sha256', comparison_name='pulse_frequency_write_boundary',
        schema='gbb-cgb-pulse-frequency-lifecycle-probe-v1' if lifecycle else 'gbb-cgb-pulse-frequency-probe-v1')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--runner', type=Path, required=True)
    parser.add_argument('--sameboy-capture', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--case', choices=SCENARIOS + LIFECYCLE_SCENARIOS)
    parser.add_argument('--lifecycle', action='store_true', help='active retrigger and CPU speed-switch controls')
    args = parser.parse_args()
    try:
        report = run_probe(args.runner.resolve(), args.sameboy_capture.resolve(), args.output.resolve(), args.case,
                           lifecycle=args.lifecycle)
        print(json.dumps(report))
        return 0 if isolated.passes(report) else 1
    except (OSError, ValueError, KeyError, subprocess.SubprocessError) as error:
        parser.exit(2, f'pulse frequency probe failed: {error}\n')


if __name__ == '__main__':
    raise SystemExit(main())
