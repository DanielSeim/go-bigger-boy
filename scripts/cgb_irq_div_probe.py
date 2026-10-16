#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Compare first-vector DIV reads for timer and software timer-IF requests."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

import cgb_pcm_stat_probe as isolated
import cgb_timer_boundary_probe as boundary

SCENARIOS = tuple(f'{source}-start-{phase}-read-{read}' for source in ('timer', 'software')
                  for phase in (43, 44, 47, 48, 51, 52) for read in range(4))


def split(scenario):
    if scenario not in SCENARIOS:
        raise ValueError('unknown vector DIV control')
    source, _, phase, _, read = scenario.split('-')
    return source, int(phase), int(read)


def times(phase, read):
    enable = 20 + 4 * phase
    request = (enable // 16 + 1) * 16 + 4
    return request, request + 20 + 4 * read + 12


def probe_rom(scenario, double_speed=False):
    source, phase, read = split(scenario)
    rom = bytearray(boundary.probe_rom('halt-00-phase-0', double_speed))
    begin = bytes.fromhex('3e a5 ea 00 c2 21 00 c1 af e0 41 e0 0f')
    start = rom.find(begin, 0x150, 0x300)
    if start < 0:
        raise ValueError('boundary startup layout changed')
    code = bytearray.fromhex('3e a5 ea 00 c2 21 00 c1 af e0 41 e0 0f 3e 04 ea ff ff')
    code += bytes.fromhex('af e0 06 3e ff e0 05 3e 91 e0 40 fb 00 af e0 04')
    # DIV reset is T0. IME is already enabled, IF is clear and TAC is disabled.
    if source == 'timer':
        code += bytes(phase) + bytes.fromhex('3e 05 e0 07')
    else:
        request, _ = times(phase, read)
        # LD A,4 / LDH IF consumes 20T, asserting software IF at the same
        # predicted T-cycle as the hardware reload. NOPs never touch DIV.
        code += bytes((request - 20) // 4) + bytes.fromhex('3e 04 e0 0f')
    code += bytes(16) + bytes.fromhex('18 fe')
    if start + len(code) > 0x300:
        raise ValueError('vector DIV startup overlaps recorder')
    rom[start:0x300] = bytes(0x300 - start)
    rom[start:start + len(code)] = code
    # The first instruction at vector 0050 is the DIV read itself (plus the
    # selected NOPs). No JP or WRAM write occurs before that read.
    vector = bytes(read) + bytes.fromhex('f0 04 22 c3 00 03')
    rom[0x50:0x60] = vector.ljust(16, b'\x00')
    recorder = bytes.fromhex('f0 05 22 f0 0f 22 af e0 07 ea ff ff 3e 01 ea 00 c0 ea 01 c0 f3 76 18 fd')
    rom[0x300:0x380] = recorder.ljust(128, b'\x00')
    rom[0x14d:0x150] = bytes(3)
    checksum = 0
    for value in rom[0x134:0x14d]:
        checksum = (checksum - value - 1) & 255
    rom[0x14d] = checksum
    rom[0x14e:0x150] = (sum(rom) & 65535).to_bytes(2, 'big')
    return bytes(rom)


def analyze(memory, scenario, frame):
    source, phase, read = split(scenario)
    if len(memory) < 8192 or memory[:2] != bytes((1, 1)) or memory[0x200] != 0xa5:
        raise ValueError('incomplete vector DIV log or damaged guard')
    div, tima, flags = memory[0x100:0x103]
    request, sample = times(phase, read)
    events = [[div, flags & 31]]
    valid = flags & 31 == 0 and (source == 'timer' or tima == 255)
    return dict(events=events, observations=[[div, tima, flags]],
                event_fields=['div', 'if_low5'], observation_fields=['div', 'tima', 'if'],
                request_source=source, start_phase_nops=phase, read_phase_nops=read,
                predicted_request_t_cycles=request, predicted_div_read_t_cycles=sample,
                prediction_assumptions='DIV_reset_T0; reload_plus_4T; next_4T_request_sample; 20T_entry; end_of_bus_cycle',
                expected_div=(sample // 256) & 255, div_prediction_match=div == ((sample // 256) & 255),
                record_structure_match=bool(valid), authored_constraints_match=bool(valid),
                event_sha256=hashlib.sha256(bytes(events[0])).hexdigest())


def run_probe(runner, adapter, output):
    report = isolated.run_probe(runner, adapter, output, scenarios=SCENARIOS, checkpoints=(2,),
        rom_builder=probe_rom, analyzer=analyze, comparison_key='event_sha256',
        comparison_name='first_vector_div', write_report=False, schema='gbb-cgb-irq-div-probe-v1')
    relations = []
    for emulator in ('gbb', 'sameboy'):
        for speed in (False, True):
            lookup = {r['scenario']: r['captures'][emulator]['events'] for r in report['rows']
                      if r['double_speed'] == speed}
            for phase in (43, 44, 47, 48, 51, 52):
                for read in range(4):
                    suffix = f'-start-{phase}-read-{read}'
                    a, b = lookup['timer' + suffix], lookup['software' + suffix]
                    relations.append(dict(emulator=emulator, double_speed=speed,
                        start_phase_nops=phase, read_phase_nops=read,
                        timer=a, software=b, match=a == b))
    report['request_relations'] = relations
    (output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    return report


def passes(report):
    expected = {(scenario, speed) for scenario in SCENARIOS for speed in (False, True)}
    rows = report['rows']
    complete = len(rows) == len(expected) and {(r['scenario'], r['double_speed']) for r in rows} == expected
    return complete and isolated.passes(report) and len(report['request_relations']) == 96 and all(
        row['match'] for row in report['request_relations'])


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
        parser.exit(2, f'vector DIV probe failed: {error}\n')


if __name__ == '__main__':
    raise SystemExit(main())
