#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Compare HALT and NOP waiting at a fixed overflow, with/without dispatch."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

import cgb_pcm_stat_probe as isolated
import cgb_timer_boundary_probe as boundary

WAIT_SCENARIOS = tuple(f'{wait}-ime{ime}-{modulo:02x}-read-{read}'
                  for wait in ('busy', 'halt') for ime in (0, 1)
                  for modulo in (0, 254) for read in range(4))
EDGE_SCENARIOS = tuple(f'edge-ime0-00-read-{read}' for read in range(4))
DIV_SCENARIOS = tuple(f'{wait}div-ime1-{modulo:02x}-read-{read}'
                     for wait in ('busy', 'halt') for modulo in (0, 254) for read in (8, 9))
SCENARIOS = WAIT_SCENARIOS + EDGE_SCENARIOS + DIV_SCENARIOS


def split(scenario):
    if scenario not in SCENARIOS:
        raise ValueError('unknown HALT wake control')
    wait, ime, modulo, _, read = scenario.split('-')
    return wait, int(ime[-1]), int(modulo, 16), int(read)


def probe_rom(scenario, double_speed=False):
    wait, ime, modulo, read = split(scenario)
    div = wait.endswith('div')
    style = wait[:-3] if div else wait
    rom = bytearray(boundary.probe_rom(f'halt-{modulo:02x}-phase-0', double_speed))
    tail = bytes.fromhex('fb 00 76 18 fd')
    start = rom.find(tail, 0x150, 0x300)
    if start < 0:
        raise ValueError('boundary startup layout changed')
    if wait == 'edge':
        # IE=0, IME=0. Inline IF-first reads sample T32/T36/T40/T44 around
        # first overflow/reload; no HALT or interrupt dispatch precedes them.
        ie = rom.find(bytes.fromhex('3e 04 ea ff ff'), 0x150, start)
        if ie < 0:
            raise ValueError('IE setup layout changed')
        rom[ie + 1] = 0
        code = bytearray(read)
        for _ in range(8):
            code += bytes.fromhex('f0 0f 22 f0 05 22 f0 44 22 f0 41 22')
        code += bytes.fromhex('af e0 07 ea ff ff 3e 08 ea 00 c0 3e 01 ea 01 c0 f3 76 18 fd')
    elif ime:
        # EI/NOP completes before IF is asserted. NOPs and HALT idle sample at 4T.
        code = bytes.fromhex('fb 00 76 18 fd') if style == 'halt' else bytes.fromhex('fb 00') + bytes(16) + bytes.fromhex('18 fe')
    else:
        # IE=4 but IME stays off. Four NOPs reach the expected request at T36;
        # HALT starts at T20 and wakes on that request, without vector entry.
        code = (bytes.fromhex('76') if wait == 'halt' else bytes(4)) + bytes.fromhex('c3 00 03')
    if start + len(code) > 0x300:
        raise ValueError('wake startup overlaps recorder')
    rom[start:start + max(len(tail), len(code))] = code.ljust(max(len(tail), len(code)), b'\x00')
    # Extend the original zero-padding recorder with independently varied read NOPs.
    original = bytes(rom[0x300:0x380])
    if read and any(original[-read:]):
        raise ValueError('recorder does not fit phase padding')
    if wait != 'edge':
        rom[0x300:0x380] = bytes(read) + original[:128 - read]
        if div:
            # Replace STAT with DIV at identical LDH cost; retain every read time.
            block = bytes(rom[0x300:0x380])
            if block.count(bytes.fromhex('f0 41')) != 8:
                raise ValueError('STAT sample layout changed')
            rom[0x300:0x380] = block.replace(bytes.fromhex('f0 41'), bytes.fromhex('f0 04'))
    rom[0x14d:0x150] = bytes(3)
    checksum = 0
    for value in rom[0x134:0x14d]:
        checksum = (checksum - value - 1) & 255
    rom[0x14d] = checksum
    rom[0x14e:0x150] = (sum(rom) & 65535).to_bytes(2, 'big')
    return bytes(rom)


def analyze(memory, scenario, frame):
    wait, ime, modulo, read = split(scenario)
    div = wait.endswith('div')
    if len(memory) < 8192 or memory[:2] != bytes((8, 1)) or memory[0x200] != 0xa5:
        raise ValueError('incomplete HALT wake log or damaged guard')
    observations = [list(memory[i:i + 4]) for i in range(0x100, 0x120, 4)]
    edge = wait == 'edge'
    events = [[r[1], r[0] & 31] if edge else [r[0], r[1] & 31] for r in observations]
    # DI NOP/JP reaches the recorder 20T before the predicted interrupt path.
    mode = 'halt' if ime else 'poll'
    padding = read + 1 if edge else read if ime else read + 4
    if_offset = -20 if edge else 20
    expected = boundary.expected_timer(mode, modulo, 0, padding, if_read_offset=if_offset)
    if div:
        for i, time in enumerate(boundary.read_times(mode, 0, padding)):
            expected[i].append(((time + 60) // 256) & 255)
            events[i].append(observations[i][3])
    structural = all(r[2] < 144 and (div or r[3] & 0x80) for r in observations)
    # TMA=0 cannot overflow a second time during recording. This witnesses IF
    # acknowledgment when IME=1 and its absence when IME=0, independently of TIMA.
    acknowledged = edge or modulo != 0 or all(r[1] == (0 if ime else 4) for r in events)
    return dict(events=events, observations=observations,
                event_fields=(['tima', 'if_low5', 'div'] if div else ['tima', 'if_low5']),
                observation_fields=(['if', 'tima', 'ly', 'stat'] if edge else ['tima', 'if', 'ly', 'div' if div else 'stat']),
                wait_style=wait[:-3] if div else wait, div_observer=div,
                ime_enabled=bool(ime), read_phase_nops=read,
                predicted_tima_read_t_cycles=boundary.read_times(mode, 0, padding),
                predicted_if_read_t_cycles=[t + if_offset for t in boundary.read_times(mode, 0, padding)],
                prediction_assumptions='next_4T_request_sample; end_of_bus_cycle; 20T_entry_only_when_IME',
                expected_timer=expected, timer_prediction_match=events == expected,
                record_structure_match=bool(structural and acknowledged),
                authored_constraints_match=bool(structural and acknowledged and events == expected),
                event_sha256=hashlib.sha256(bytes(v for r in events for v in r)).hexdigest())


def relations(report):
    result = []
    for emulator in ('gbb', 'sameboy'):
        for speed in (False, True):
            lookup = {r['scenario']: r['captures'][emulator]['events'] for r in report['rows']
                      if r['double_speed'] == speed}
            for ime in (0, 1):
                for modulo in (0, 254):
                    for read in range(4):
                        suffix = f'-ime{ime}-{modulo:02x}-read-{read}'
                        a, b = lookup['busy' + suffix], lookup['halt' + suffix]
                        different = [i for i in range(8) if a[i] != b[i]]
                        result.append(dict(emulator=emulator, double_speed=speed,
                            ime_enabled=bool(ime), modulo=modulo, read_phase_nops=read,
                            busy_vs_halt_match=not different, different_event_indices=different,
                            first_event_difference=(dict(index=different[0], busy=a[different[0]], halt=b[different[0]])
                                                    if different else None)))
    return result


def run_probe(runner, adapter, output):
    report = isolated.run_probe(runner, adapter, output, scenarios=SCENARIOS, checkpoints=(2,),
                               rom_builder=probe_rom, analyzer=analyze,
                               comparison_key='event_sha256', comparison_name='halt_wake_controls',
                               write_report=False, schema='gbb-cgb-halt-wake-probe-v1')
    report['wait_relations'] = relations(report)
    (output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    return report


def passes(report):
    return isolated.passes(report) and len(report['wait_relations']) == 64 and all(
        r['busy_vs_halt_match'] for r in report['wait_relations'])


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
        parser.exit(2, f'HALT wake probe failed: {error}\n')


if __name__ == '__main__':
    raise SystemExit(main())
