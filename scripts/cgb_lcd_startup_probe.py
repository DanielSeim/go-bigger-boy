#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Isolate first LCD requests from interrupt dispatch with authored CGB cartridges."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

import cgb_pcm_stat_probe as isolated

PADS = (0, 1, 57)
SCENARIOS = tuple(f'{source}-{observation}-pad{pad}'
                  for source in ('vblank', 'stat') for observation in ('poll', 'irq') for pad in PADS)
CHECKPOINTS = (4, 8)


def split(scenario):
    if scenario not in SCENARIOS:
        raise ValueError('unknown LCD startup scenario')
    source, observation, pad = scenario.split('-')
    return source, observation, int(pad[3:])


def probe_rom(scenario, double_speed=False):
    source, observation, pad = split(scenario)
    irq = observation == 'irq'
    rom = bytearray(32768)
    rom[0x100:0x103] = bytes.fromhex('c3 50 01')
    rom[0x134:0x13c] = b'GBB LCD '
    rom[0x143] = 0xc0
    # Start with LCD/APU/timer/interrupts disabled. Clear every sampled WRAM
    # byte; neither a boot logo nor undefined memory may supply a result.
    code = bytearray.fromhex('f3 31 fe ff af e0 40 e0 41 e0 07 e0 26 ea ff ff e0 0f')
    code += bytes.fromhex('21 00 c0 06 40 af 22 05 20 fc 3e a5 ea 3f c0')
    if double_speed:
        code += bytes.fromhex('3e 01 e0 4d 10 00')
    code += bytes.fromhex('01 00 00 16 02 21 10 c0')  # BC work counter, D=two samples, HL cursor.
    code += bytes((0x3e, 0x20 if source == 'stat' else 0, 0xe0, 0x41))
    code += bytes.fromhex('af e0 0f')
    code += bytes((0x3e, (2 if source == 'stat' else 1) if irq else 0))
    code += bytes.fromhex('ea ff ff')
    if irq:
        code += bytes.fromhex('fb 00')  # Arm IME before enabling the LCD.
    code += bytes.fromhex('af e0 04 c3 00 10')
    rom[0x150:0x150 + len(code)] = code

    # Log BC (workload count), LY, STAT mode, IF low bits, DIV, KEY1 speed.
    # The logger itself consumes time, explicitly shared by both emulators.
    logger = bytes.fromhex('f5 78 22 79 22 f0 44 22 f0 41 e6 03 22 f0 0f e6 03 22 '
                           'f0 04 22 f0 4d e6 80 22 15 f1') + bytes((0xd9 if irq else 0xc9,))
    rom[0x300:0x300 + len(logger)] = logger
    vector = 0x48 if source == 'stat' else 0x40
    rom[vector:vector + 3] = bytes.fromhex('c3 00 03')

    window = bytearray(bytes(pad) + bytes.fromhex('3e 87 e0 40'))
    labels, relative = {}, []
    def mark(label):
        labels[label] = len(window)
    def branch(opcode, label):
        window.extend((opcode, 0))
        relative.append((len(window) - 1, label))
    if irq:
        mark('work')
        window += bytes.fromhex('03 7a b7')
        branch(0x20, 'work')
        window += bytes.fromhex('c3 00 11')
    else:
        # Poll the request's PPU condition without servicing any interrupt.
        # After logging, wait for it to clear before observing the second event.
        register, value = (0x44, 144) if source == 'vblank' else (0x41, 2)
        condition = bytes((0xf0, register))
        if source == 'stat':
            condition += bytes.fromhex('e6 03')
        condition += bytes((0xfe, value))
        mark('wait')
        window += bytes((0x03,)) + condition
        branch(0x20, 'wait')
        window += bytes.fromhex('cd 00 03 7a b7 ca 00 11')
        mark('leave')
        window += condition
        branch(0x28, 'leave')
        window += bytes((0xc3, (0x1000 + labels['wait']) & 255, (0x1000 + labels['wait']) >> 8))
    for address, label in relative:
        displacement = labels[label] - address - 1
        if not -128 <= displacement <= 127:
            raise ValueError('LCD probe branch exceeds JR range')
        window[address] = displacement & 255
    if len(window) > 0x100:
        raise ValueError('LCD probe overlaps its completion marker')
    rom[0x1000:0x1000 + len(window)] = window
    # Freeze the log and leave the LCD running for later frame captures.
    finish = bytes.fromhex('f3 af ea ff ff e0 41 3e 02 ea 00 c0 3e 01 ea 01 c0 76 18 fd')
    rom[0x1100:0x1100 + len(finish)] = finish
    checksum = 0
    for value in rom[0x134:0x14d]:
        checksum = (checksum - value - 1) & 255
    rom[0x14d] = checksum
    rom[0x14e:0x150] = (sum(rom) & 65535).to_bytes(2, 'big')
    return bytes(rom)


def analyze(memory, scenario, frame):
    source, observation, _ = split(scenario)
    if len(memory) < 8192 or memory[:2] != b'\x02\x01' or memory[0x3f] != 0xa5:
        raise ValueError('incomplete LCD startup log or damaged guard')
    events = [list(memory[i:i + 7]) for i in (0x10, 0x17)]
    request = 1 if source == 'vblank' else 2
    valid = all(e[2] < 154 and e[3] < 4 and e[4] < 4 and e[6] in (0, 128) for e in events)
    valid = valid and events[0][6] == events[1][6]
    if source == 'vblank':
        valid = valid and all(e[2] == 144 and e[3] == 1 for e in events)
    else:
        valid = valid and [e[2] for e in events] == [1, 2]
    valid = valid and all(bool(e[4] & request) == (observation == 'poll') for e in events)
    return dict(events=events, event_fields=['work_bc_high', 'work_bc_low', 'ly', 'stat_mode',
                                            'if_low2', 'div', 'key1_speed'],
                event_sha256=hashlib.sha256(memory[0x10:0x1e]).hexdigest(),
                authored_constraints_match=bool(valid))


def run_probe(runner, adapter, output):
    report = isolated.run_probe(runner, adapter, output, scenarios=SCENARIOS, checkpoints=CHECKPOINTS,
                                rom_builder=probe_rom, analyzer=analyze, write_report=False,
                                comparison_key='event_sha256', comparison_name='first_lcd_events',
                                schema='gbb-cgb-lcd-startup-probe-v1')
    for row in report['rows']:
        for capture in row['captures'].values():
            capture['authored_constraints_match'] = capture['authored_constraints_match'] and all(
                e[6] == (128 if row['double_speed'] else 0) for e in capture['events'])
    for row in report['rows']:
        peers = [r for r in report['rows'] if
                 (r['scenario'], r['double_speed']) == (row['scenario'], row['double_speed'])]
        row['frozen_log'] = len(peers) == len(CHECKPOINTS) and all(
            len({p['captures'][core]['event_sha256'] for p in peers}) == 1
            for core in ('gbb', 'sameboy'))
    report['scope'] = 'LCD-off to first two PPU conditions or IRQs; counters are workload witnesses, not physical edge timestamps'
    (output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    return report


def passes(report):
    try:
        rows = report['rows']
        expected = {(s, speed, frame) for s in SCENARIOS for speed in (False, True) for frame in CHECKPOINTS}
        return (len(rows) == len(expected) and
                all(type(r['double_speed']) is bool for r in rows) and
                {(r['scenario'], r['double_speed'], r['frame']) for r in rows} == expected and
                all(r['frozen_log'] is True and r['match'] is True and
                    set(r['captures']) == {'gbb', 'sameboy'} and all(
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
        parser.exit(2, f'LCD startup probe failed: {error}\n')


if __name__ == '__main__':
    raise SystemExit(main())
