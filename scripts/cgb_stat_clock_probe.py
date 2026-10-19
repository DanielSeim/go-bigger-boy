#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Compare LCD write phases, bounded STAT observations and first IRQ dispatch."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

import cgb_frame_anchor_probe as anchor
import cgb_stat_vector_probe as vector

SCENARIOS = vector.SCENARIOS + ('stat-nop',)
MAX_STEPS = 512
PHASES = {'gbb': 'after_opcode_fetch_cycle', 'sameboy': 'before_opcode_fetch_pending_cycle'}


def probe_rom(scenario, double_speed=False):
    if scenario in vector.SCENARIOS:
        return vector.probe_rom(scenario, double_speed)
    if scenario != 'stat-nop':
        raise ValueError('unknown STAT clock scenario')
    rom = bytearray(vector.probe_rom('stat-incjr-phase0', double_speed))
    # A four-CPU-clock foreground calibrates observation resolution without
    # moving the request to a fitted time. Interrupt service exits through the
    # same terminal logger as the other ROMs. It must happen before PC=1100.
    rom[0x1000:0x1100] = bytes.fromhex('3e 87 e0 40') + bytes(252)
    rom[0x14d:0x150] = bytes(3)
    checksum = 0
    for value in rom[0x134:0x14d]:
        checksum = (checksum - value - 1) & 255
    rom[0x14d] = checksum
    rom[0x14e:0x150] = (sum(rom) & 65535).to_bytes(2, 'big')
    return bytes(rom)


def integer(value, upper=2**64 - 1):
    return type(value) is int and 0 <= value <= upper


def validate_trace(trace, scenario, speed, core, observers=True):
    if scenario not in SCENARIOS or type(speed) is not bool or core not in PHASES:
        raise ValueError('invalid clock probe case')
    required = dict(schema='gbb-stat-clock-trace-v1', qualification=False, core=core,
                    clock_unit='8mhz', anchor='before_step_pc_1000', model='cgb-e',
                    opcode_callback_phase=PHASES[core], observers_enabled=observers,
                    complete=True, overflow=False, window_seen=True,
                    dispatch_seen=True, vector_seen=observers, state_excluded_host_bytes=8 if core == 'sameboy' else 0)
    if not isinstance(trace, dict) or any(type(trace.get(k)) is not type(v) or trace[k] != v
                                          for k, v in required.items()):
        raise ValueError('invalid or incomplete clock trace metadata')
    for key in ('window_clock', 'lcd_accept_clock', 'request_lower_exclusive',
                'request_upper_inclusive', 'vector_opcode_clock', 'final_clock'):
        if not integer(trace.get(key), 20_000_100):
            raise ValueError('invalid trace clock')
    if not integer(trace.get('state_fnv1a64')) or not integer(trace.get('state_bytes'), 2*1024*1024) or not trace['state_bytes']:
        raise ValueError('missing serialized-state fingerprint')
    origin, end = trace['window_clock'], trace['final_clock']
    if type(trace.get('request_seen')) is not bool:
        raise ValueError('missing request observation marker')
    if trace['request_seen']:
        if not origin <= trace['request_lower_exclusive'] < trace['request_upper_inclusive'] <= end:
            raise ValueError('invalid request observation interval')
    elif observers or core != 'gbb' or trace['request_lower_exclusive'] or trace['request_upper_inclusive']:
        raise ValueError('unobserved request outside observer-disabled control')
    software = scenario.startswith('software-')
    if type(trace.get('lcd_seen')) is not bool or trace['lcd_seen'] != (observers and not software):
        raise ValueError('missing or unexpected accepted LCD write')
    if trace['lcd_seen'] and not origin <= trace['lcd_accept_clock'] <= trace['request_upper_inclusive']:
        raise ValueError('invalid LCD acceptance clock')
    if observers and not origin <= trace['vector_opcode_clock'] <= end:
        raise ValueError('invalid opcode callback clock')
    regs, memory, steps = trace.get('registers'), trace.get('wram'), trace.get('steps')
    if (not isinstance(regs, list) or len(regs) != 6 or not all(integer(x, 65535) for x in regs)
            or regs[:2] != [0x1100, 0xfffe] or not isinstance(memory, list) or len(memory) != 64
            or not all(integer(x, 255) for x in memory) or memory[:2] != [0, 0]
            or memory[0x3f] != 0xa5 or not isinstance(steps, list) or not 1 <= len(steps) <= MAX_STEPS):
        raise ValueError('invalid terminal state or bounded step log')
    previous = origin
    for i, step in enumerate(steps):
        if not isinstance(step, dict) or set(step) != {'begin', 'end', 'pc_before', 'pc_after',
                                                     'bc_before', 'bc_after', 'if_before', 'if_after'}:
            raise ValueError('invalid step schema')
        if (not all(integer(step[k], 65535) for k in ('pc_before', 'pc_after', 'bc_before', 'bc_after'))
                or not all(integer(step[k], 31) for k in ('if_before', 'if_after'))
                or not integer(step['begin']) or not integer(step['end'])
                or step['begin'] != previous or not step['begin'] < step['end'] <= end):
            raise ValueError('noncontiguous or invalid step timeline')
        if i and (steps[i-1]['pc_after'], steps[i-1]['bc_after'], steps[i-1]['if_after']) != (
                step['pc_before'], step['bc_before'], step['if_before']):
            raise ValueError('discontinuous architectural step state')
        previous = step['end']
    if steps[0]['pc_before'] != 0x1000 or steps[-1]['pc_after'] != 0x1100 or previous != end:
        raise ValueError('wrong trace endpoints')
    dispatch = trace.get('dispatch')
    if (dispatch not in steps or dispatch['pc_after'] != 0x48 or dispatch['if_before'] not in (0, 2)
            or dispatch['if_after'] != 0 or dispatch['bc_before'] != dispatch['bc_after']
            or dispatch['end'] - dispatch['begin'] != (20 if speed else 40)):
        raise ValueError('invalid interrupt dispatch interval')
    sample_seen = trace.get('interrupt_sample_seen', False)
    if type(sample_seen) is not bool:
        raise ValueError('invalid interrupt sample marker')
    if sample_seen:
        after_fetch = trace.get('interrupt_sample_after_fetch')
        if type(after_fetch) is not bool or not observers or (core != 'gbb' and after_fetch):
            raise ValueError('invalid interrupt sample phase')
        sample_clock = trace.get('interrupt_sample_clock')
        expected_clock = dispatch['begin'] + ((4 if speed else 8) if after_fetch else 0)
        if (not integer(sample_clock) or sample_clock != expected_clock or
                trace.get('interrupt_sample_pc') != dispatch['pc_before'] or
                trace.get('interrupt_sample_pending') != 2):
            raise ValueError('invalid interrupt recognition sample')
        if trace['request_seen'] and trace['request_upper_inclusive'] > sample_clock:
            raise ValueError('recognition precedes observed request')
    else:
        after_fetch = False
        if trace['request_seen'] and trace['request_upper_inclusive'] > dispatch['begin']:
            raise ValueError('dispatch precedes observed request')
    if dispatch['if_before'] == 0 and not (core == 'gbb' and
            ((sample_seen and after_fetch) or (not observers and not trace['request_seen']))):
        raise ValueError('unaccounted request during discarded fetch')
    sample = memory[0x10:0x1a]
    pc = sample[2] | sample[3] << 8
    boundaries = range(0x1004, 0x1100) if scenario == 'stat-nop' else vector.return_boundaries(scenario)
    if (sample[0] != 0 or sample[1] >= 128 or pc not in boundaries or sample[5] >= 154
            or sample[6] >= 4 or sample[7] != 0 or sample[8] != (128 if speed else 0)
            or sample[9] != 0x5a or pc != dispatch['pc_before'] or sample[1] != dispatch['bc_before']):
        raise ValueError('invalid vector/stacked-return witness')
    if software and (sample[:2] != [0, 0] or pc != 0x1004 or sample[5:7] != [0, 0]):
        raise ValueError('invalid LCD-off software control')
    if scenario == 'stat-nop' and sample[:2] != [0, 0]:
        raise ValueError('NOP calibration changed counter')
    return trace


def architectural_state(trace):
    return {key: trace[key] for key in ('window_clock', 'final_clock', 'registers', 'wram', 'state_bytes', 'state_fnv1a64')}


def summarize(trace):
    origin = trace['window_clock']
    dispatch = trace['dispatch']
    return dict(lcd_accept=trace['lcd_accept_clock'] - origin if trace['lcd_seen'] else None,
                request_interval=[trace['request_lower_exclusive'] - origin,
                                  trace['request_upper_inclusive'] - origin] if trace['request_seen'] else None,
                dispatch_begin=dispatch['begin'] - origin, dispatch_end=dispatch['end'] - origin,
                dispatch_bc=dispatch['bc_before'], return_pc=dispatch['pc_before'],
                vector_opcode=trace['vector_opcode_clock'] - origin if trace['vector_seen'] else None,
                interrupt_sample=trace['interrupt_sample_clock'] - origin if trace.get('interrupt_sample_seen') else None,
                interrupt_sample_after_fetch=trace.get('interrupt_sample_after_fetch', False),
                opcode_callback_phase=trace['opcode_callback_phase'], witness=trace['wram'][0x10:0x1a],
                steps=len(trace['steps']))


def run_probe(gbb, sameboy, output):
    output.mkdir(parents=True, exist_ok=False)
    boot = output / 'owned-cgb-boot.bin'
    boot.write_bytes(anchor.owned_boot())
    sha = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
    report = dict(schema='gbb-cgb-stat-clock-probe-v1', qualification=False,
                  clock_unit='8mhz', anchor='before_step_pc_1000',
                  boot_sha256=sha(boot), adapter_sha256={'gbb': sha(gbb), 'sameboy': sha(sameboy)},
                  commands=[], rows=[])
    for speed in (False, True):
        for scenario in SCENARIOS:
            name = f'{scenario}-{"double" if speed else "normal"}'
            rom = output / (name + '.gbc')
            rom.write_bytes(probe_rom(scenario, speed))
            captures = {}
            for core, executable in (('gbb', gbb), ('sameboy', sameboy)):
                takes = []
                for take in (1, 2, 0):
                    cmd = [str(executable), str(rom), str(boot)] + (['--no-observers'] if take == 0 else [])
                    report['commands'].append(cmd)
                    result = subprocess.run(cmd, capture_output=True, text=True, timeout=30)
                    (output / f'{name}-{core}-{take}.stderr').write_text(result.stderr)
                    if result.returncode:
                        raise ValueError(f'{core} clock trace failed with exit {result.returncode}')
                    trace = validate_trace(json.loads(result.stdout), scenario, speed, core, take != 0)
                    (output / f'{name}-{core}-{take}.json').write_text(json.dumps(trace, indent=2) + '\n')
                    takes.append(trace)
                captures[core] = dict(summarize(takes[0]), repeatable=takes[0] == takes[1],
                    observer_equivalent=architectural_state(takes[0]) == architectural_state(takes[2]))
            report['rows'].append(dict(scenario=scenario, double_speed=speed, rom_sha256=sha(rom),
                captures=captures, match=captures['gbb']['witness'] == captures['sameboy']['witness']))
    (output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    return report


def passes(report):
    try:
        rows = report['rows']
        expected = {(s, speed) for s in SCENARIOS for speed in (False, True)}
        return (len(rows) == len(expected) and all(type(r['double_speed']) is bool for r in rows)
                and {(r['scenario'], r['double_speed']) for r in rows} == expected
                and all(r['match'] is True and set(r['captures']) == {'gbb', 'sameboy'} and all(
                    c['repeatable'] is True and c['observer_equivalent'] is True
                    for c in r['captures'].values()) for r in rows))
    except (KeyError, TypeError):
        return False


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--gbb-clock-trace', type=Path, required=True)
    parser.add_argument('--sameboy-clock-trace', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    try:
        report = run_probe(args.gbb_clock_trace.resolve(), args.sameboy_clock_trace.resolve(), args.output.resolve())
        print(json.dumps(report))
        return 0 if passes(report) else 1
    except (OSError, ValueError, KeyError, subprocess.SubprocessError) as error:
        parser.exit(2, f'STAT clock probe failed: {error}\n')


if __name__ == '__main__':
    raise SystemExit(main())
