#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Bracket CGB first/last pixels and STAT edges on startup and ordinary lines."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

import cgb_pixel_access_probe as access


def scenarios():
    return [dict(name=f'{"double" if speed else "normal"}-{control}-scx{scx}-phase{phase}',
                 speed=speed, control=control, scx=scx, phase=phase)
            for speed in (False, True)
            for control in ('background', 'window-left', 'window-middle',
                            'object-left', 'object-middle',
                            'object-disabled-left', 'object-disabled-middle')
            for scx in (range(8) if control == 'background' else (0, 7))
            for phase in (0, 1)]


def poll_count(case):
    return 220 if case['speed'] else 110


def poll_program(case, form='hl'):
    if form == 'hl':
        program = [(b'\x7e', 8)] * poll_count(case)
    elif form == 'ldh':
        cycles = 8 * poll_count(case)
        program = [(bytes.fromhex('f0 41'), 12)] * (cycles // 12) + [(b'\0', 4)] * ((cycles % 12) // 4)
    else:
        raise ValueError('unknown STAT polling form')
    return [(b'\0', 4)] * case['phase'] + program


def probe_rom(case, form='hl'):
    if case not in scenarios():
        raise ValueError('unknown phase scenario')
    seed = next(c for c in access.scenarios() if c['speed'] == case['speed'])
    data = bytearray(access.probe_rom(seed))
    tail = bytes.fromhex('21 00 81 06 5a c3 00 10')
    position = data.index(tail, 0x150, 0x1000)
    # Clear all OAM with LCD off, then configure static transparent objects.
    # Tile zero is blank, so objects consume fetch time without changing RGB.
    setup = bytearray.fromhex('21 00 fe 06 a0 af 22 05 20 fc')
    setup += bytes((0x3e, case['scx'], 0xe0, 0x43))
    setup += bytes.fromhex('af e0 4a')  # WY=0.
    middle = case['control'].endswith('middle')
    setup += bytes((0x3e, 87 if middle else 7, 0xe0, 0x4b))
    if case['control'].startswith('object'):
        setup += bytes.fromhex('3e 10 ea 00 fe')
        setup += bytes((0x3e, 88 if middle else 8, 0xea, 1, 0xfe))
    setup += bytes.fromhex('21 41 ff c3 00 10')  # HL=STAT, before the clock anchor.
    data[position:position + len(setup)] = setup
    data[0x1001] = (0xb1 if case['control'].startswith('window') else
                    0x93 if case['control'].startswith('object') and 'disabled' not in case['control'] else 0x91)
    # LD A,(HL) polls STAT every eight CPU clocks. A four-clock prefix shifts
    # the observation grid after LCD enable without moving its clock anchor.
    body = b''.join(op for op, _ in poll_program(case, form)) + bytes.fromhex('c3 00 11')
    data[0x2000:0x2000 + len(body)] = body
    checksum = 0
    for value in data[0x134:0x14d]:
        checksum = (checksum - value - 1) & 255
    data[0x14d] = checksum
    data[0x14e:0x150] = b'\0\0'
    data[0x14e:0x150] = (sum(data) & 65535).to_bytes(2, 'big')
    return bytes(data)


def validate_trace(trace, case, core, enabled=True, form='hl'):
    required = dict(schema='gbb-cgb-pixel-access-trace-v1', qualification=False,
                    core=core, model='cgb-e', clock_unit='8mhz',
                    anchor='before_step_pc_1000', complete=True, snapshots_enabled=enabled)
    if not isinstance(trace, dict) or any(type(trace.get(k)) is not type(v) or trace.get(k) != v
                                         for k, v in required.items()):
        raise ValueError('invalid phase trace metadata')
    if (type(trace.get('state_bytes')) is not int or not 0 < trace['state_bytes'] <= 2*1024*1024 or
            type(trace.get('state_fnv1a64')) is not int or not 0 <= trace['state_fnv1a64'] < 1 << 64):
        raise ValueError('invalid phase state fingerprint')
    access.validate_sample(trace['final'])
    scale = 1 if case['speed'] else 2
    expected = (52 + 4*case['phase'] + 8*poll_count(case))*scale
    if trace['final']['pc'] != 0x1100 or trace['final']['clock'] != expected:
        raise ValueError('invalid phase completion anchor')
    samples = trace.get('samples')
    if not isinstance(samples, list) or len(samples) > 513 or (not enabled and samples):
        raise ValueError('unbounded phase snapshots')
    if enabled:
        expected_pcs = [0x1000, 0x1002, 0x1004]
        expected_clocks = [0, 8*scale, 20*scale]
        pc, clock = 0x2000, 36*scale
        for op, cycles in poll_program(case, form):
            expected_pcs.append(pc)
            expected_clocks.append(clock)
            pc += len(op)
            clock += cycles*scale
        expected_pcs += [pc, 0x1100]
        expected_clocks += [clock, expected]
        if len(samples) != len(expected_pcs) or samples[-1] != trace['final']:
            raise ValueError('invalid phase snapshot anchors')
        for sample, pc, clock in zip(samples, expected_pcs, expected_clocks):
            access.validate_sample(sample)
            if sample['pc'] != pc or sample['clock'] != clock:
                raise ValueError('unexpected phase instruction sequence')
    return trace


def line_edges(samples, ly):
    line = [s for s in samples if s['ly'] == ly]
    if not line or any(a['frontier'] > b['frontier'] for a, b in zip(line, line[1:])):
        raise ValueError('missing or nonmonotonic visible line')
    predicates = dict(first_pixel=lambda a, b: a['frontier'] == 0 < b['frontier'],
                      last_pixel=lambda a, b: a['frontier'] < 160 == b['frontier'],
                      mode3_entry=lambda a, b: a['mode'] != 3 == b['mode'],
                      mode3_exit=lambda a, b: a['mode'] == 3 and b['mode'] == 0)
    result = {}
    for name, predicate in predicates.items():
        edges = [(a, b) for a, b in zip(line, line[1:]) if predicate(a, b)]
        if len(edges) != 1:
            raise ValueError(f'missing or repeated {name} on line {ly}')
        a, b = edges[0]
        result[name] = dict(interval=[a['clock'], b['clock']],
                            frontiers=[a['frontier'], b['frontier']])
    return result


def interior_deltas(a, b):
    if len(a) != len(b) or any(x['clock'] != y['clock'] for x, y in zip(a, b)):
        raise ValueError('frontiers do not share instruction clocks')
    return [sorted({y['frontier'] - x['frontier'] for x, y in zip(a, b)
                    if x['ly'] == y['ly'] == ly and
                    0 < x['frontier'] < 160 and 0 < y['frontier'] < 160})
            for ly in (0, 1)]


def run_probe(adapter, reference, output):
    output.mkdir(parents=True, exist_ok=False)
    boot = output / 'owned.bin'
    boot.write_bytes(access.anchor.owned_boot())
    rows = []
    for case in scenarios():
        rom = output / (case['name'] + '.gbc')
        rom.write_bytes(probe_rom(case))
        captures, timelines = {}, {}
        for core, executable in (('gbb', adapter), ('sameboy', reference)):
            traces = []
            for take, enabled in ((1, True), (2, True), (3, False)):
                result = subprocess.run([str(executable), str(rom), str(boot)] +
                                        ([] if enabled else ['--end-only']),
                                        capture_output=True, text=True, timeout=30)
                prefix = output / f'{case["name"]}-{core}-{take}'
                prefix.with_suffix('.json').write_text(result.stdout)
                prefix.with_suffix('.stderr').write_text(result.stderr)
                if result.returncode:
                    raise ValueError(f'{core} phase capture failed: {result.returncode}')
                traces.append(validate_trace(json.loads(result.stdout), case, core, enabled))
            if traces[0] != traces[1] or access.final_state(traces[0]) != access.final_state(traces[2]):
                raise ValueError(f'{core} phase snapshots changed final state or did not repeat')
            captures[core] = dict(lines=[line_edges(traces[0]['samples'], ly) for ly in (0, 1)],
                                  repeatable=True, snapshot_equivalent=True)
            timelines[core] = traces[0]['samples']
        fields = lambda core, keys: [[s[k] for k in keys] for s in timelines[core]]
        rows.append(dict(scenario=case['name'], rom_sha256=hashlib.sha256(rom.read_bytes()).hexdigest(),
                         captures=captures,
                         interior_frontier_deltas=interior_deltas(timelines['gbb'], timelines['sameboy']),
                         mode_match=fields('gbb', ('clock', 'ly', 'mode')) == fields('sameboy', ('clock', 'ly', 'mode')),
                         pixel_match=fields('gbb', ('clock', 'ly', 'frontier')) == fields('sameboy', ('clock', 'ly', 'frontier'))))
    controls = []
    for case in [c for c in scenarios() if c['control'] == 'background' and c['scx'] == c['phase'] == 0]:
        rom = output / (case['name'] + '-ldh.gbc')
        rom.write_bytes(probe_rom(case, 'ldh'))
        for core, executable in (('gbb', adapter), ('sameboy', reference)):
            traces = []
            for take, enabled in ((1, True), (2, True), (3, False)):
                result = subprocess.run([str(executable), str(rom), str(boot)] +
                                        ([] if enabled else ['--end-only']),
                                        capture_output=True, text=True, timeout=30)
                prefix = output / f'{case["name"]}-ldh-{core}-{take}'
                prefix.with_suffix('.json').write_text(result.stdout)
                prefix.with_suffix('.stderr').write_text(result.stderr)
                if result.returncode:
                    raise ValueError(f'{core} polling-form control failed')
                traces.append(validate_trace(json.loads(result.stdout), case, core, enabled, 'ldh'))
            if traces[0] != traces[1] or access.final_state(traces[0]) != access.final_state(traces[2]):
                raise ValueError(f'{core} polling-form snapshots changed final state')
            original = json.loads((output / f'{case["name"]}-{core}-1.json').read_text())
            a = {s['clock']: s for s in original['samples']}
            b = {s['clock']: s for s in traces[0]['samples']}
            shared = sorted(a.keys() & b.keys())
            if any(a[t][key] != b[t][key] for t in shared for key in ('ly', 'mode', 'frontier')):
                raise ValueError(f'{core} equal-clock polling forms changed visible phase')
            controls.append(dict(scenario=case['name'], core=core,
                                 rom_sha256=hashlib.sha256(rom.read_bytes()).hexdigest(),
                                 shared_snapshots=len(shared), visible_phase_equal=True,
                                 repeatable=True, snapshot_equivalent=True))
    report = dict(schema='gbb-cgb-pixel-phase-probe-v1', qualification=False,
                  boot_sha256=hashlib.sha256(boot.read_bytes()).hexdigest(), rows=rows,
                  polling_form_controls=controls)
    (output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    return report


def passes(report):
    try:
        rows = report['rows']
        controls = report['polling_form_controls']
        expected_controls = {(c['name'], core) for c in scenarios()
                             if c['control'] == 'background' and c['scx'] == c['phase'] == 0
                             for core in ('gbb', 'sameboy')}
        return (len(controls) == 4 and {(c['scenario'], c['core']) for c in controls} == expected_controls
                and all(c['visible_phase_equal'] is True and c['repeatable'] is True and
                        c['snapshot_equivalent'] is True and c['shared_snapshots'] > 1 for c in controls)
                and len(rows) == len(scenarios()) and {r['scenario'] for r in rows} == {c['name'] for c in scenarios()}
                and all(r['mode_match'] is True and r['pixel_match'] is True and
                        set(r['captures']) == {'gbb', 'sameboy'} and
                        all(c['repeatable'] is True and c['snapshot_equivalent'] is True
                            for c in r['captures'].values()) for r in rows))
    except (KeyError, TypeError):
        return False


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--gbb-access-trace', type=Path, required=True)
    parser.add_argument('--sameboy-access-trace', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    try:
        report = run_probe(args.gbb_access_trace.resolve(), args.sameboy_access_trace.resolve(), args.output.resolve())
        print(json.dumps(report))
        return 0 if passes(report) else 1
    except (OSError, ValueError, KeyError, TypeError, subprocess.SubprocessError) as error:
        parser.exit(2, f'pixel-phase probe failed: {error}\n')


if __name__ == '__main__':
    raise SystemExit(main())
