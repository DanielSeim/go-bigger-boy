#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Authored CGB-E pulse phase controls, independent of external test ROMs."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

import cgb_pcm_stat_probe as isolated

SCENARIOS = tuple(f'ch{channel}-enable-{enable}-trigger-{trigger}'
                  for channel in (1, 2) for enable in range(4) for trigger in range(4))
SAMPLES = 32


def split(scenario):
    if scenario not in SCENARIOS:
        raise ValueError('unknown pulse phase scenario')
    channel, _, enable, _, trigger = scenario.split('-')
    return int(channel[2:]), int(enable), int(trigger)


def probe_rom(scenario, double_speed=False):
    channel, enable, trigger = split(scenario)
    rom = bytearray(isolated.probe_rom('silence', double_speed))
    begin = bytes.fromhex('21 00 c1 af e0 04 e0 0f 3e e2 e0 05 e0 06')
    start = rom.find(begin, 0x150, 0x300)
    if start < 0:
        raise ValueError('isolated startup layout changed')
    # DI and disabled timer/LCD from startup remain in force. No IRQ or sample
    # handler runs during this straight-line experiment. Clear IE explicitly.
    rom[start:start + 7] = bytes.fromhex('af ea ff ff c3 00 10')
    code = bytearray.fromhex('21 00 c1')
    offset = 0 if channel == 1 else 5
    for delay in range(SAMPLES):
        # Each observation restarts the APU and DIV. Enable padding shifts NR52
        # relative to the CPU divider; trigger padding shifts it relative to NR52.
        code += bytes.fromhex('af e0 26 e0 04 3e 80') + bytes(enable) + bytes.fromhex('e0 26')
        for register, value in ((0x11 + offset, 0x80), (0x12 + offset, 0x80),
                                (0x13 + offset, 0xff)):
            code += bytes((0x3e, value, 0xe0, register))
        code += bytes.fromhex('3e 87') + bytes(trigger) + bytes((0xe0, 0x14 + offset))
        code += bytes(delay) + bytes.fromhex('f0 76 22 f0 26 22')
    # Freeze the log, then enable the LCD solely for the existing frame anchor.
    code += bytes.fromhex('af e0 26 3e 20 ea 00 c0 3e 01 ea 01 c0 3e 91 e0 40 f3 76 18 fd')
    if 0x1000 + len(code) > len(rom):
        raise ValueError('pulse phase program exceeds ROM')
    rom[0x1000:0x1000 + len(code)] = code
    rom[0x14d:0x150] = bytes(3)
    checksum = 0
    for value in rom[0x134:0x14d]:
        checksum = (checksum - value - 1) & 255
    rom[0x14d] = checksum
    rom[0x14e:0x150] = (sum(rom) & 65535).to_bytes(2, 'big')
    return bytes(rom)


def analyze(memory, scenario, frame):
    channel, enable, trigger = split(scenario)
    if len(memory) < 8192 or memory[:2] != bytes((SAMPLES, 1)) or memory[0x200] != 0xa5:
        raise ValueError('incomplete pulse phase log or damaged guard')
    raw = [list(memory[i:i + 2]) for i in range(0x100, 0x100 + 2 * SAMPLES, 2)]
    amplitude = 8 if channel == 1 else 128
    status = 0xf0 | (1 << (channel - 1))
    valid = ({pcm for pcm, _ in raw} == {0, amplitude} and
             all(nr52 == status for _, nr52 in raw))
    events = [pcm // (1 if channel == 1 else 16) for pcm, _ in raw]
    return dict(events=events, observations=raw, observation_fields=['pcm12', 'nr52'],
                channel=channel, enable_phase_nops=enable, trigger_phase_nops=trigger,
                trigger_after_enable_cpu_t_cycles=80 + 4 * trigger,
                pcm_read_after_trigger_cpu_t_cycles=[12 + 4 * i for i in range(SAMPLES)],
                first_nonzero_sample=next((i for i, value in enumerate(events) if value), None),
                record_structure_match=valid, authored_constraints_match=valid,
                event_sha256=hashlib.sha256(bytes(events)).hexdigest())


def run_probe(runner, adapter, output, case=None):
    if case is not None:
        split(case)  # Reject before creating output or invoking either emulator.
    return isolated.run_probe(runner, adapter, output,
        scenarios=(case,) if case is not None else SCENARIOS, checkpoints=(2,),
        rom_builder=probe_rom, analyzer=analyze,
        comparison_key='event_sha256', comparison_name='pulse_enable_trigger_phase',
        schema='gbb-cgb-pulse-phase-probe-v1')


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
        parser.exit(2, f'pulse phase probe failed: {error}\n')


if __name__ == '__main__':
    raise SystemExit(main())
