#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Authored software-IF controls for pending interrupt dispatch across RETI."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

import cgb_pcm_stat_probe as isolated

KINDS = ('straight-call', 'timer-only', 'pending-reti', 'raised-reti', 'pending-ei-ret', 'pending-ret')
SCENARIOS = tuple(f'{kind}-phase-{phase}' for kind in KINDS for phase in range(4))


def split(scenario):
    if scenario not in SCENARIOS:
        raise ValueError('unknown software dispatch scenario')
    kind, phase = scenario.rsplit('-phase-', 1)
    return kind, int(phase)


def probe_rom(scenario, double_speed=False):
    kind, phase = split(scenario)
    single = kind in ('straight-call', 'timer-only')
    rom = bytearray(isolated.probe_rom('silence', double_speed))
    begin = bytes.fromhex('21 00 c1 af e0 04 e0 0f 3e e2 e0 05 e0 06')
    end = bytes.fromhex('3e 04 ea ff ff 3e 91 e0 40 3e 05 e0 07 fb 76 18 fd')
    start = rom.find(begin, 0x150, 0x300)
    if start < 0 or rom[start + len(begin):start + len(begin) + len(end)] != end:
        raise ValueError('isolated startup layout changed')
    # No hardware STAT IRQ; timer is a stopwatch, never reaching overflow here.
    # Marker C003 changes only after the initial interrupt sequence returns to main.
    code = bytearray.fromhex('3e a5 ea 00 c2 21 00 c1 af e0 41 e0 05 e0 06 e0 0f')
    code += bytes((0x3e, 0 if kind == 'straight-call' else 4 if single else 6))
    code += bytes.fromhex('ea ff ff 3e 91 e0 40 af e0 04')
    code += bytes(phase)  # 0..3 NOPs: sweep all four 4-T-cycle timer phases.
    code += bytes.fromhex('3e 05 e0 07')
    request = 0 if kind == 'straight-call' else 4 if single else 2 if kind == 'raised-reti' else 6
    code += bytes((0x3e, request)) + bytes.fromhex('e0 0f')
    if kind == 'straight-call':
        # 5 NOPs + CALL = 44 T, matching EI/NOP + IRQ entry + vector JP.
        # IME remains off and IE=0 until the observation has been recorded.
        code += bytes(5) + bytes.fromhex('cd 40 03')
    else:
        code += bytes.fromhex('fb 00')
    code += bytes.fromhex('3e 01 ea 03 c0 fb 00 76 18 fd')
    if start + len(code) > 0x300:
        raise ValueError('software startup overlaps handlers')
    rom[start:start + len(code)] = code
    rom[0x300:0x380] = bytes(128)
    rom[0x48:0x4b] = bytes.fromhex('c3 00 03')
    rom[0x50:0x53] = bytes.fromhex('c3 40 03')

    def log(source):
        # Same-cost entries: source, TIMA, acknowledged IF, main marker.
        return bytearray((0xf5, 0x3e, source)) + bytearray.fromhex(
            '22 f0 05 22 f0 0f e6 1f 22 fa 03 c0 22')

    stat = log(2)
    if kind == 'raised-reti':
        stat += bytes.fromhex('3e 04 e0 0f')
    stat += bytes.fromhex('f1')
    stat += bytes.fromhex('c9' if kind == 'pending-ret' else 'fb c9' if kind == 'pending-ei-ret' else 'd9')
    rom[0x300:0x300 + len(stat)] = stat
    timer = log(4)
    timer += bytes.fromhex('af e0 07 ea ff ff')
    timer += bytes((0x3e, 1 if single else 2)) + bytes.fromhex('ea 00 c0 3e 01 ea 01 c0 f1 d9')
    rom[0x340:0x340 + len(timer)] = timer
    rom[0x14d:0x150] = bytes(3)
    checksum = 0
    for value in rom[0x134:0x14d]:
        checksum = (checksum - value - 1) & 255
    rom[0x14d] = checksum
    rom[0x14e:0x150] = (sum(rom) & 65535).to_bytes(2, 'big')
    return bytes(rom)


def analyze(memory, scenario, frame):
    kind, phase = split(scenario)
    count = 1 if kind in ('straight-call', 'timer-only') else 2
    if len(memory) < 8192 or memory[:2] != bytes((count, 1)) or memory[0x200] != 0xa5:
        raise ValueError('incomplete software dispatch log or damaged guard')
    events = [list(memory[i:i + 4]) for i in range(0x100, 0x100 + count * 4, 4)]
    valid = [e[0] for e in events] == ([4] if count == 1 else [2, 4])
    valid = valid and events[-1][2:] == [0, 1 if kind == 'pending-ret' else 0]
    if count == 2:
        valid = valid and events[0][2:] == [0 if kind == 'raised-reti' else 4, 0]
    return dict(events=events, event_fields=['source', 'tima', 'if_low5', 'main_marker'],
                phase_nops=phase, authored_constraints_match=bool(valid),
                event_sha256=hashlib.sha256(memory[0x100:0x100 + count * 4]).hexdigest())


def relations(report):
    result = []
    for emulator in ('gbb', 'sameboy'):
        for speed in (False, True):
            rows = {r['scenario']: r['captures'][emulator]['events'] for r in report['rows']
                    if r['double_speed'] == speed}
            ret_deltas, ei_deltas = [], []
            call_equal = []
            for phase in range(4):
                suffix = f'-phase-{phase}'
                reti = rows['pending-reti' + suffix][-1][1]
                ret_deltas.append((rows['pending-ret' + suffix][-1][1] - reti) & 255)
                ei_deltas.append((rows['pending-ei-ret' + suffix][-1][1] - reti) & 255)
                call_equal.append(rows['straight-call' + suffix][0][1] == rows['timer-only' + suffix][0][1])
            # Plain RET adds main's LD A/LD (marker)/EI/NOP = 32 CPU T-cycles.
            # EI;RET adds EI's 4 T-cycles, visible once across four timer phases.
            valid = ret_deltas == [2] * 4 and sorted(ei_deltas) == [0, 0, 0, 1] and all(call_equal)
            result.append(dict(emulator=emulator, double_speed=speed,
                               ret_minus_reti_ticks=ret_deltas, ei_ret_minus_reti_ticks=ei_deltas,
                               call_vs_irq_tima_match=call_equal,
                               authored_timing_relations_match=valid))
    return result


def run_probe(runner, adapter, output):
    report = isolated.run_probe(runner, adapter, output, scenarios=SCENARIOS, checkpoints=(2,),
                               rom_builder=probe_rom, analyzer=analyze,
                               comparison_key='event_sha256', comparison_name='software_irq_dispatch',
                               write_report=False,
                               schema='gbb-cgb-reti-dispatch-probe-v1')
    report['timing_relations'] = relations(report)
    (output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    return report


def passes(report):
    return isolated.passes(report) and len(report['timing_relations']) == 4 and all(
        r['authored_timing_relations_match'] for r in report['timing_relations'])


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
        parser.exit(2, f'RETI dispatch probe failed: {error}\n')


if __name__ == '__main__':
    raise SystemExit(main())
