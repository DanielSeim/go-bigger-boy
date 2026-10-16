#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Measure a single STOP and one-/two-cycle resume controls at public boundaries."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

import cgb_pcm_stat_probe as isolated
import cgb_frame_anchor_probe as anchor

SCENARIOS = tuple(f'{state}-{operation}' for state in ('active', 'armed', 'off')
                  for operation in ('switch', 'nop'))
SCENARIOS += ('active-switch-inc', 'active-nop-inc', 'active-switch-load', 'active-nop-load')
START_PC = 0x1000


def split(scenario):
    if scenario not in SCENARIOS:
        raise ValueError('unknown STOP timing scenario')
    return scenario.split('-')[:2]


def resume_instruction(scenario):
    split(scenario)
    if scenario.endswith('-load'):
        return bytes((0x06, 0x11))  # LD B,11: two machine cycles.
    return bytes((4 if scenario.endswith('-inc') else 0,))


def resume_pc(scenario):
    return START_PC + (2 if split(scenario)[1] == 'switch' else 1)


def end_pc(scenario):
    return resume_pc(scenario) + len(resume_instruction(scenario))


def probe_rom(scenario, double_speed=False):
    state, operation = split(scenario)
    rom = bytearray(isolated.probe_rom('silence', double_speed))
    start = rom.find(bytes.fromhex('21 00 c1 af e0 04 e0 0f 3e e2 e0 05 e0 06'), 0x150, 0x300)
    if start < 0:
        raise ValueError('isolated startup layout changed')
    rom[start:start + 3] = bytes.fromhex('c3 00 06')
    code = bytearray.fromhex('af ea ff ff e0 26 e0 07 e0 04 e0 06 e0 0f 3e 30 e0 05 3e 04 e0 07')
    code += bytes((0x3e, 0 if state == 'off' else 0x80, 0xe0, 0x26))
    # CH1: period 28, constant volume 15, 12.5% duty. Armed leaves DAC on
    # without triggering; off keeps NR52 clear through the measured window.
    code += bytes.fromhex('af e0 11 3e f0 e0 12 3e f9 e0 13 3e 87')
    code += bytes.fromhex('e0 14') if state == 'active' else bytes(3)
    code += bytes.fromhex('01 00 10')
    code += bytes((0x3e, 1 if operation == 'switch' else 0, 0xe0, 0x4d)) + bytes.fromhex('c3 00 10')
    rom[0x600:0x600 + len(code)] = code
    window = (bytes.fromhex('10 00') if operation == 'switch' else bytes(1)) + resume_instruction(scenario) + bytes(1)
    # First post-operation instruction belongs to the interval. A NOP marks
    # exit, so a PC reached during a CPU pause cannot masquerade as resumption.
    window += bytes.fromhex('01 05 03 11 0d 08 21 22 15 40 76 18 fd')
    rom[START_PC:START_PC + len(window)] = window
    rom[0x14d:0x150] = bytes(3)
    checksum = 0
    for value in rom[0x134:0x14d]:
        checksum = (checksum - value - 1) & 255
    rom[0x14d] = checksum
    rom[0x14e:0x150] = (sum(rom) & 65535).to_bytes(2, 'big')
    return bytes(rom)


def first_pc(result, target):
    if result.returncode != 0 or result.stdout.strip() != 'PASS (Mooneye)':
        raise ValueError('GBB did not complete the timing ROM protocol')
    records = [line for line in result.stderr.splitlines() if line.startswith('CPU_FIRST_PC ')]
    if len(records) != 1:
        raise ValueError('missing or ambiguous first-PC witness')
    record = dict(token.split('=', 1) for token in records[0].split()[1:])
    if int(record['next_pc'], 16) != target or record['transition'] != 'instruction':
        raise ValueError('unexpected first-PC transition')
    cycle, duration = int(record['cycle']), int(record['duration'])
    if duration <= 0 or cycle < duration:
        raise ValueError('invalid first-PC clock')
    stacks = []
    for line in result.stderr.splitlines():
        if line.startswith('CPU_STACK_DIAGNOSTICS '):
            fields = dict(token.split('=', 1) for token in line.split()[1:])
            if (int(fields['cycle']) == cycle - duration and fields['pc'] == record['pc']
                    and fields['next_pc'] == record['next_pc']):
                stacks.append(fields)
    if len(stacks) != 1 or stacks[0]['double_speed'] not in ('true', 'false'):
        raise ValueError('missing CPU speed witness')
    return dict(cpu_t_cycles=cycle, instruction_t_cycles=duration,
                previous_pc=int(record['pc'], 16), opcode=int(record['opcode'], 16),
                double_speed=stacks[0]['double_speed'] == 'true', bc=int(record['bc'], 16))


def reference_window(result, scenario, initial_double, target=None, exit_opcode=0):
    if result.returncode == 2:
        raise ValueError('reference adapter rejected arguments; rebuild with --pc-clock-window support')
    if target is None:
        target = end_pc(scenario)
    data = json.loads(result.stdout)
    required = dict(schema='gbb-sameboy-pc-clock-window-v1', qualification=False, model='cgb-e',
                    result='complete', anchor='gb_run_call_before_instruction_callback',
                    start_pc=START_PC, end_pc=target, entry_seen=True, exit_seen=True,
                    entry_opcode=0x10 if split(scenario)[1] == 'switch' else 0, exit_opcode=exit_opcode)
    if result.returncode != 0 or not isinstance(data, dict) or any(data.get(k) != v for k, v in required.items()):
        raise ValueError('incomplete or unexpected reference clock window')
    if (data['qualification'] is not False or data['entry_seen'] is not True or data['exit_seen'] is not True or
            any(type(data[k]) is not int for k in ('start_pc', 'end_pc', 'entry_opcode', 'exit_opcode'))):
        raise ValueError('invalid reference clock window types')
    for key in ('entry_8mhz_clocks', 'exit_8mhz_clocks', 'elapsed_8mhz_clocks'):
        if type(data.get(key)) is not int or data[key] <= 0:
            raise ValueError('invalid reference clock count')
    if data['exit_8mhz_clocks'] - data['entry_8mhz_clocks'] != data['elapsed_8mhz_clocks']:
        raise ValueError('inconsistent reference clock interval')
    switched = split(scenario)[1] == 'switch'
    entry = (0x80 if initial_double else 0) | int(switched)
    final = (0x80 if initial_double else 0) ^ (0x80 if switched else 0)
    if any(type(data.get(k)) is not int or not 0 <= data[k] <= 255 for k in
           ('entry_key1', 'exit_key1', 'entry_div', 'exit_div', 'entry_tima', 'exit_tima', 'entry_pcm12', 'exit_pcm12')):
        raise ValueError('invalid reference IO witness')
    if data['entry_key1'] & 0x81 != entry or data['exit_key1'] & 0x81 != final:
        raise ValueError('reference CPU speed witness mismatch')
    trace = data.get('callback_trace')
    if (data.get('callback_trace_truncated') is not False or type(data.get('instruction_callbacks')) is not int or
            not isinstance(trace, list) or not 2 <= len(trace) <= 8 or data['instruction_callbacks'] != len(trace)):
        raise ValueError('incomplete or unbounded callback trace')
    for key in ('entry_bc', 'exit_bc'):
        if type(data.get(key)) is not int or not 0 <= data[key] <= 65535:
            raise ValueError('invalid reference register witness')
    previous = data['entry_8mhz_clocks']
    for record in trace:
        if not isinstance(record, dict) or any(type(record.get(k)) is not int for k in ('pc', 'opcode', 'bc', 'clocks_8mhz')):
            raise ValueError('invalid callback record')
        if (not 0 <= record['pc'] <= 65535 or not 0 <= record['opcode'] <= 255 or not 0 <= record['bc'] <= 65535 or
                not previous <= record['clocks_8mhz'] <= data['exit_8mhz_clocks']):
            raise ValueError('invalid callback value or clock order')
        previous = record['clocks_8mhz']
    for record, pc, opcode, bc, clocks in (
            (trace[0], START_PC, data['entry_opcode'], data['entry_bc'], data['entry_8mhz_clocks']),
            (trace[-1], target, exit_opcode, data['exit_bc'], data['exit_8mhz_clocks'])):
        if record != dict(pc=pc, opcode=opcode, bc=bc, clocks_8mhz=clocks):
            raise ValueError('callback endpoint witness mismatch')
    return data


def run_probe(runner, adapter, output):
    output.mkdir(parents=True, exist_ok=False)
    boot = output / 'owned-cgb-boot.bin'
    boot.write_bytes(anchor.owned_boot())
    sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
    report = dict(schema='gbb-cgb-stop-timing-probe-v1', qualification=False, model='cgb-e',
                  runner_sha256=sha(runner), adapter_sha256=sha(adapter), boot_sha256=sha(boot),
                  interval='operation_and_first_resumed_instruction',
                  conversion_assumption='Current GBB atomic operation cycles at initial speed; resumed instruction at final speed',
                  commands=[], rows=[])
    def execute(command):
        report['commands'].append(list(map(str, command)))
        return subprocess.run(list(map(str, command)), capture_output=True, text=True, timeout=30)
    for initial in (False, True):
        for scenario in SCENARIOS:
            rom = output / f'{scenario}-{"double" if initial else "normal"}.gbc'
            rom.write_bytes(probe_rom(scenario, initial))
            switched = split(scenario)[1] == 'switch'
            final = initial ^ switched
            captures = {}
            for emulator in ('gbb', 'sameboy'):
                takes = []
                for take in (1, 2):
                    if emulator == 'gbb':
                        points = []
                        for target in (START_PC, end_pc(scenario)):
                            result = execute([runner, rom, '--model', 'cgb-e', '--dmg-boot', '--protocol', 'mooneye',
                                              '--trace-first-pc', hex(target), '--max-cycles', '20000000'])
                            points.append(first_pc(result, target))
                        entry, exit = points
                        if (entry['opcode'] != 0xc3 or exit['opcode'] != resume_instruction(scenario)[0] or
                                exit['previous_pc'] != resume_pc(scenario) or
                                entry['double_speed'] != initial or exit['double_speed'] != final):
                            raise ValueError('GBB window instruction/speed witness mismatch')
                        elapsed = exit['cpu_t_cycles'] - entry['cpu_t_cycles']
                        tail = exit['instruction_t_cycles']
                        if tail != 4 * len(resume_instruction(scenario)) or elapsed <= tail:
                            raise ValueError('invalid GBB clock window')
                        converted = (elapsed - tail) * (1 if initial else 2) + tail * (1 if final else 2)
                        measured = dict(entry=entry, exit=exit, elapsed_cpu_t_cycles=elapsed,
                                        partitioned_8mhz_clocks=converted)
                    else:
                        result = execute([adapter, '--pc-clock-window', rom, boot, hex(START_PC),
                                          hex(end_pc(scenario)), 'cgb-e', '20000000'])
                        measured = reference_window(result, scenario, initial)
                        first_resume = execute([adapter, '--pc-clock-window', rom, boot, hex(START_PC),
                                                hex(resume_pc(scenario)), 'cgb-e', '20000000'])
                        resume = reference_window(first_resume, scenario, initial, resume_pc(scenario),
                                                  resume_instruction(scenario)[0])
                        if any(measured[k] != resume[k] for k in ('entry_8mhz_clocks', 'entry_opcode',
                                                                 'entry_key1', 'entry_div', 'entry_tima', 'entry_pcm12', 'entry_bc')):
                            raise ValueError('reference entry changed between resume boundary controls')
                        cadence = measured['exit_8mhz_clocks'] - resume['exit_8mhz_clocks']
                        steady_nominal = 4 if final else 8
                        nominal_cadence = steady_nominal * len(resume_instruction(scenario))
                        if cadence <= 0 or (not switched and cadence != nominal_cadence):
                            raise ValueError('reference NOP clock calibration failed')
                        second_resume = execute([adapter, '--pc-clock-window', rom, boot, hex(START_PC),
                                                 hex(end_pc(scenario) + 1), 'cgb-e', '20000000'])
                        steady = reference_window(second_resume, scenario, initial, end_pc(scenario) + 1, 1)
                        steady_gap = steady['exit_8mhz_clocks'] - measured['exit_8mhz_clocks']
                        if steady['entry_8mhz_clocks'] != measured['entry_8mhz_clocks'] or steady_gap != steady_nominal:
                            raise ValueError('reference steady-resume NOP calibration failed')
                        measured.update(first_resume_window=resume, steady_resume_window=steady,
                                        first_resume_call_gap_8mhz_clocks=cadence,
                                        steady_resume_call_gap_8mhz_clocks=steady_gap,
                                        nominal_resume_8mhz_clocks=nominal_cadence)
                    takes.append(measured)
                captures[emulator] = dict(takes[0], repeatable=takes[0] == takes[1])
            report['rows'].append(dict(scenario=scenario, initial_double_speed=initial, final_double_speed=final,
                rom_sha256=sha(rom), captures=captures,
                architectural_match=captures['gbb']['exit']['bc'] == captures['sameboy']['exit_bc'],
                match=captures['gbb']['partitioned_8mhz_clocks'] == captures['sameboy']['elapsed_8mhz_clocks']))
    (output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    return report


def passes(report):
    try:
        expected = {(s, speed) for s in SCENARIOS for speed in (False, True)}
        rows = report['rows']
        if any(type(r['initial_double_speed']) is not bool for r in rows):
            return False
        return len(rows) == len(expected) and {(r['scenario'], r['initial_double_speed']) for r in rows} == expected and all(
            r['match'] is True and r['architectural_match'] is True and set(r['captures']) == {'gbb', 'sameboy'} and all(
                c['repeatable'] is True for c in r['captures'].values()) for r in rows)
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
    except (OSError, ValueError, KeyError, TypeError, subprocess.SubprocessError) as error:
        parser.exit(2, f'STOP timing probe failed: {error}\n')


if __name__ == '__main__':
    raise SystemExit(main())
