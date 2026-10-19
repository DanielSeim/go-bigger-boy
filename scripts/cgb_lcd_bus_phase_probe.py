#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Compare LCD write instruction forms and equal-cost IF read forms."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

import cgb_frame_anchor_probe as anchor
import cgb_pcm_stat_probe as isolated
import cgb_stat_clock_probe as clock
import cgb_stat_request_phase_probe as phase
import cgb_stat_vector_probe as vector

WRITES = {
    'ldh': bytes.fromhex('3e 87 e0 40'),
    'absolute': bytes.fromhex('3e 87 ea 40 ff'),
    'c': bytes.fromhex('0e 40 3e 87 e2 01 00 00'),
    'hl': bytes.fromhex('21 40 ff 3e 87 77 21 10 c0'),
}
# All alternatives consume sixteen clocks, with the memory-read bus cycle
# last. Preceding NOPs equalize cost; no per-emulator delays are fitted.
READS = {'ldh': bytes.fromhex('00 f0 0f'), 'absolute': bytes.fromhex('fa 0f ff'),
         'c': bytes.fromhex('00 00 f2')}
SCENARIOS = tuple(f'{form}-phase-{n}' for form in READS for n in range(13))


def checksum(rom):
    rom[0x14d:0x150] = bytes(3)
    value = 0
    for byte in rom[0x134:0x14d]:
        value = (value - byte - 1) & 255
    rom[0x14d] = value
    rom[0x14e:0x150] = (sum(rom) & 65535).to_bytes(2, 'big')
    return bytes(rom)


def write_rom(form, speed=False):
    if form not in WRITES:
        raise ValueError('unknown LCD write form')
    rom = bytearray(vector.probe_rom('stat-incjr-phase0', speed))
    rom[0x1000:0x1100] = WRITES[form].ljust(256, b'\x00')
    return checksum(rom)


def split(scenario):
    if scenario not in SCENARIOS:
        raise ValueError('unknown IF read form')
    form, n = scenario.split('-phase-')
    return form, int(n)


def read_rom(scenario, double_speed=False):
    form, n = split(scenario)
    rom = bytearray(phase.probe_rom(f'if-phase-{n}', double_speed))
    begin = 0x1000 + 4 + 108 * (2 if double_speed else 1) + n
    if rom[begin:begin + 2] != bytes.fromhex('f0 0f'):
        raise ValueError('primary IF-read layout changed')
    # Set C=0F for every form, before LCD enable. Everything following the
    # primary read retains identical instruction costs across all three forms.
    body = bytes.fromhex('0e 0f') + bytes(rom[0x1000:begin]) + READS[form] + bytes(rom[begin + 2:0x1400])
    if any(body[0x400:]):
        raise ValueError('read form overlaps completion')
    rom[0x1000:0x1400] = body[:0x400]
    return checksum(rom)


def analyze(memory, scenario, frame):
    _, n = split(scenario)
    return phase.analyze(memory, f'if-phase-{n}', frame)


def run_probe(gbb, sameboy, runner, capture, output):
    output.mkdir(parents=True, exist_ok=False)
    boot = output / 'owned-cgb-boot.bin'
    boot.write_bytes(anchor.owned_boot())
    sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
    report = dict(schema='gbb-cgb-lcd-bus-phase-probe-v1', qualification=False,
                  boot_sha256=sha(boot), binaries={name: sha(path) for name, path in
                    [('gbb_clock', gbb), ('sameboy_clock', sameboy), ('runner', runner), ('capture', capture)]},
                  writes=[], commands=[])
    for speed in (False, True):
        for form in WRITES:
            rom = output / f'{form}-{"double" if speed else "normal"}.gbc'
            rom.write_bytes(write_rom(form, speed))
            captures = {}
            for core, tool in [('gbb', gbb), ('sameboy', sameboy)]:
                traces = []
                for take in (1, 2, 0):
                    cmd = [str(tool), str(rom), str(boot)] + (['--no-observers'] if take == 0 else [])
                    report['commands'].append(cmd)
                    result = subprocess.run(cmd, capture_output=True, text=True, timeout=30)
                    if result.returncode:
                        raise ValueError(f'{core} write-form trace failed: {result.returncode}')
                    trace = clock.validate_trace(json.loads(result.stdout), 'stat-nop', speed, core, take != 0)
                    (output / f'{rom.stem}-{core}-{take}.json').write_text(json.dumps(trace, indent=2) + '\n')
                    traces.append(trace)
                captures[core] = dict(clock.summarize(traces[0]), repeatable=traces[0] == traces[1],
                    observer_equivalent=clock.architectural_state(traces[0]) == clock.architectural_state(traces[2]))
            report['writes'].append(dict(form=form, double_speed=speed, rom_sha256=sha(rom), captures=captures,
                lcd_phase_match=captures['gbb']['lcd_accept'] == captures['sameboy']['lcd_accept'],
                match=all(captures['gbb'][key] == captures['sameboy'][key]
                          for key in ('dispatch_begin', 'dispatch_end', 'return_pc', 'witness'))))
    reads = isolated.run_probe(runner, capture, output / 'reads', scenarios=SCENARIOS, checkpoints=(4,),
        rom_builder=read_rom, analyzer=analyze, comparison_key='event_sha256',
        comparison_name='equal_cost_if_read_forms', schema='gbb-cgb-if-read-forms-v1')
    for row in reads['rows']:
        for c in row['captures'].values():
            c['authored_constraints_match'] = c['authored_constraints_match'] and c['key1_speed'] == (128 if row['double_speed'] else 0)
    # Equal costs must also give the same result across forms within each core.
    relations = []
    for speed in (False, True):
        for n in range(13):
            for core in ('gbb', 'sameboy'):
                records = [r['captures'][core]['events'] for r in reads['rows']
                           if r['double_speed'] == speed and r['scenario'].endswith(f'-phase-{n}')]
                relations.append(dict(double_speed=speed, phase=n, core=core,
                                      match=len(records) == 3 and all(x == records[0] for x in records)))
    report.update(reads=reads['rows'], read_form_relations=relations)
    (output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    return report


def passes(report):
    try:
        writes, reads, relations = report['writes'], report['reads'], report['read_form_relations']
        return (len(writes) == 8 and {(r['form'], r['double_speed']) for r in writes} ==
                {(f, s) for f in WRITES for s in (False, True)} and
                len(reads) == 78 and {(r['scenario'], r['double_speed'], r['frame']) for r in reads} ==
                {(f, s, 4) for f in SCENARIOS for s in (False, True)} and
                all(type(r['double_speed']) is bool for r in writes + reads) and
                all(type(r['lcd_phase_match']) is bool for r in writes) and
                all(r['match'] is True and set(r['captures']) == {'gbb', 'sameboy'} and all(
                    c['repeatable'] is True and c['observer_equivalent'] is True
                    for c in r['captures'].values()) for r in writes) and
                all(r['match'] is True and set(r['captures']) == {'gbb', 'sameboy'} and all(
                    c['repeatable'] is True and c['authored_constraints_match'] is True
                    for c in r['captures'].values()) for r in reads) and
                len(relations) == 52 and all(type(r['double_speed']) is bool and type(r['phase']) is int for r in relations) and
                {(r['double_speed'], r['phase'], r['core']) for r in relations} ==
                {(s, n, c) for s in (False, True) for n in range(13) for c in ('gbb', 'sameboy')} and
                all(r['match'] is True for r in relations))
    except (KeyError, TypeError):
        return False


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('gbb-clock-trace', 'sameboy-clock-trace', 'runner', 'sameboy-capture', 'output'):
        parser.add_argument('--' + name, type=Path, required=True)
    args = parser.parse_args()
    try:
        report = run_probe(args.gbb_clock_trace.resolve(), args.sameboy_clock_trace.resolve(),
                           args.runner.resolve(), args.sameboy_capture.resolve(), args.output.resolve())
        print(json.dumps(report))
        return 0 if passes(report) else 1
    except (OSError, ValueError, KeyError, subprocess.SubprocessError) as error:
        parser.exit(2, f'LCD bus-phase probe failed: {error}\n')


if __name__ == '__main__':
    raise SystemExit(main())
