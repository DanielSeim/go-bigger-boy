#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Authored STOP speed-switch clock witnesses with active/armed/powered-off APU."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

import cgb_pcm_stat_probe as isolated

SCENARIOS = tuple(f'ch{channel}-{state}-{operation}-period-{period}'
                  for channel in (1, 2) for state in ('active', 'armed', 'off')
                  for operation in ('switch', 'nop') for period in (16, 28))
SAMPLES = 128
MAX_CPU_CYCLES = 100_000_000
MAX_REFERENCE_CLOCKS = 200_000_000


def split(scenario):
    if scenario not in SCENARIOS:
        raise ValueError('unknown STOP/APU scenario')
    channel, state, operation, _, period = scenario.split('-')
    return int(channel[2:]), state, operation, int(period)


def probe_rom(scenario, double_speed=False):
    channel, state, operation, period = split(scenario)
    switched = operation == 'switch'
    rom = bytearray(isolated.probe_rom('silence', double_speed))
    start = rom.find(bytes.fromhex('21 00 c1 af e0 04 e0 0f 3e e2 e0 05 e0 06'), 0x150, 0x300)
    if start < 0:
        raise ValueError('isolated startup layout changed')
    rom[start:start + 7] = bytes.fromhex('af ea ff ff c3 00 10')
    code = bytearray.fromhex('21 00 c1 3e a5 ea 00 c4 f0 4d ea 02 c0')
    code += bytes((0x3e, 0x80 if double_speed else 0)) + bytes.fromhex('ea 03 c0')
    offset = 0 if channel == 1 else 5
    setup = bytearray()
    for register, value in ((0x11 + offset, 0), (0x12 + offset, 0xf0),
                            (0x13 + offset, (2048 - period // 4) & 255)):
        setup += bytes((0x3e, value, 0xe0, register))
    trigger = bytes((0x3e, 0x87, 0xe0, 0x14 + offset))
    for delay in range(SAMPLES):
        if switched and delay:
            code += bytes.fromhex('3e 01 e0 4d 10 00')
        # Slow TIMA avoids fast-reload boundary reads. No IRQ is serviced.
        code += bytes.fromhex('af e0 26 e0 07 e0 04 e0 06 e0 0f 3e 30 e0 05 3e 04 e0 07')
        code += bytes((0x3e, 0 if state == 'off' else 0x80, 0xe0, 0x26)) + setup
        code += trigger if state == 'active' else bytes.fromhex('3e 87 00 00 00')
        code += bytes((0x3e, 1 if switched else 0, 0xe0, 0x4d))
        # NOP matches this core's four-T-cycle STOP instruction path, but does
        # not reset DIV/change speed. No equivalence of transition time is assumed.
        code += bytes.fromhex('10 00') if switched else bytes(1)
        code += trigger if state == 'armed' else bytes.fromhex('3e 87 00 00 00')
        if state == 'off':
            code += bytes.fromhex('3e 80 e0 26') + setup + trigger
        code += bytes(delay) + bytes.fromhex('f0 76 22 f0 26 22 f0 4d 22 f0 04 22 f0 05 22 f0 0f 22')
    code += bytes.fromhex('af e0 07 e0 26 3e 80 ea 00 c0 3e 01 ea 01 c0 3e 91 e0 40 f3 76 18 fd')
    if 0x1000 + len(code) > len(rom):
        raise ValueError('STOP/APU program exceeds ROM')
    rom[0x1000:0x1000 + len(code)] = code
    rom[0x14d:0x150] = bytes(3)
    checksum = 0
    for value in rom[0x134:0x14d]:
        checksum = (checksum - value - 1) & 255
    rom[0x14d] = checksum
    rom[0x14e:0x150] = (sum(rom) & 65535).to_bytes(2, 'big')
    return bytes(rom)


def analyze(memory, scenario, frame):
    channel, state, operation, period = split(scenario)
    if len(memory) < 8192 or memory[:2] != bytes((SAMPLES, 1)) or memory[0x400] != 0xa5:
        raise ValueError('incomplete STOP/APU log or damaged guard')
    if memory[3] not in (0, 0x80) or memory[2] & 0x81 != memory[3]:
        raise ValueError('initial CPU speed witness mismatch')
    expected_speed = memory[3] ^ (0x80 if operation == 'switch' else 0)
    raw = [list(memory[i:i + 6]) for i in range(0x100, 0x400, 6)]
    amplitude = 15 if channel == 1 else 240
    valid = ({r[0] for r in raw} == {0, amplitude} and
             all(r[1] == (0xf0 | (1 << (channel - 1))) and r[2] & 0x81 == expected_speed for r in raw))
    pcm = [r[0] // (1 if channel == 1 else 16) for r in raw]
    timer = [[r[3], r[4], r[5] & 31] for r in raw]
    events = [[p, *t] for p, t in zip(pcm, timer)]
    digest = lambda values: hashlib.sha256(bytes(values)).hexdigest()
    return dict(events=events, event_fields=['pcm', 'div', 'tima', 'if_low5'], pcm=pcm, timer=timer,
                observations=raw, observation_fields=['pcm12', 'nr52', 'key1', 'div', 'tima', 'if'],
                initial_double_speed=bool(memory[3]), final_double_speed=bool(expected_speed),
                channel=channel, apu_during_operation=state, operation=operation, pulse_period_apu_cycles=period,
                timing_anchor=('post_operation_trigger' if state == 'armed' else
                               'post_operation_power_on_trigger' if state == 'off' else 'post_operation_20T_control'),
                pcm_read_after_anchor_cpu_t_cycles=[12 + 4 * i for i in range(SAMPLES)],
                timer_read_after_pcm_cpu_t_cycles=[60, 80, 100],
                record_structure_match=valid, authored_constraints_match=valid,
                pcm_sha256=digest(pcm), timer_sha256=digest(v for t in timer for v in t),
                event_sha256=digest(v for e in events for v in e))


def run_probe(runner, adapter, output, case=None):
    if case is not None:
        split(case)
    report = isolated.run_probe(runner, adapter, output,
        scenarios=(case,) if case is not None else SCENARIOS, checkpoints=(2,),
        rom_builder=probe_rom, analyzer=analyze, write_report=False,
        max_cpu_cycles=MAX_CPU_CYCLES, max_reference_clocks=MAX_REFERENCE_CLOCKS,
        comparison_key='event_sha256', comparison_name='stop_apu_and_timer_clocks',
        schema='gbb-cgb-stop-apu-probe-v1')
    report['capture_budgets'] = dict(cpu_t_cycles=MAX_CPU_CYCLES, reference_8mhz_clocks=MAX_REFERENCE_CLOCKS)
    for row in report['rows']:
        a, b = (row['captures'][n] for n in ('gbb', 'sameboy'))
        row.update(pcm_match=a['pcm'] == b['pcm'], timer_match=a['timer'] == b['timer'])
    (output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    return report


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
        parser.exit(2, f'STOP/APU probe failed: {error}\n')


if __name__ == '__main__':
    raise SystemExit(main())
