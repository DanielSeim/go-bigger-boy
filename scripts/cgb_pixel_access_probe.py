#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Pair pixel-output snapshots and executed CPU VRAM/palette boundary accesses."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

import cgb_frame_anchor_probe as anchor

OPERATIONS = {'vram-read': bytes.fromhex('7e'), 'vram-write': bytes.fromhex('70'),
              'palette-read': bytes.fromhex('f0 69'), 'palette-write': bytes.fromhex('e0 69')}


def scenarios():
    return [dict(name=f'{"double" if speed else "normal"}-{edge}-{operation}-pad{pad}',
                 speed=speed, edge=edge, operation=operation, pad=pad)
            for speed in (False, True) for edge in ('start', 'end')
            for operation in OPERATIONS
            for pad in range((253 if speed else 120) if edge == 'start' else (339 if speed else 163),
                             ((253 if speed else 120) if edge == 'start' else (339 if speed else 163)) + 11)]


def operation_pc(case, polling=True):
    return 0x2000 + (case['pad']//3*2 + case['pad']%3 if polling else case['pad']) + 2


def startup_scenarios():
    """Shift the same CPU schedule from ordinary line one onto startup line zero."""
    cases = []
    for case in scenarios():
        pad = case['pad'] - (226 if case['speed'] else 113)  # The shortened 452-dot first line.
        cases.append(dict(case, pad=pad,
                          name=f'{"double" if case["speed"] else "normal"}-{case["edge"]}-'
                               f'{case["operation"]}-startup-pad{pad}'))
    return cases


def probe_rom(case, polling=True):
    if case not in scenarios() and case not in startup_scenarios():
        raise ValueError('unknown access scenario')
    data = bytearray(32768)
    data[0x100:0x103] = bytes.fromhex('c3 50 01')
    data[0x134:0x13e] = b'GBB ACCESS'
    data[0x143] = 0xc0
    code = bytearray.fromhex('f3 31 fe ff af e0 40 e0 41 e0 07 e0 26 e0 0f ea ff ff')
    for bank in (0, 1):
        code += bytes((0x3e, bank, 0xe0, 0x4f))
        code += bytes.fromhex('21 00 80 01 00 20 af 22 0b 78 b1 20 f9')
    code += bytes.fromhex('af e0 4f 3e a5 ea 00 81')
    # Color 0 is visible red=1; color 1 is an unused write/read sentinel.
    code += bytes.fromhex('3e 80 e0 68 3e 01 e0 69 af e0 69 3e 03 e0 69 af e0 69 3e 02 e0 68')
    code += bytes.fromhex('af e0 42 e0 43')
    if case['speed']:
        code += bytes.fromhex('3e 01 e0 4d 10 00')
    code += bytes.fromhex('21 00 81 06 5a c3 00 10')
    data[0x150:0x150 + len(code)] = code
    data[0x1000:0x1007] = bytes.fromhex('3e 91 e0 40 c3 00 20')
    operation = OPERATIONS[case['operation']]
    # Each LDH A,(STAT) replaces exactly three NOPs (12 clocks). These
    # ROM-authored reads prevent reference rendering from taking different
    # batching paths just because the host snapshots are enabled.
    padding = bytes.fromhex('f0 41')*(case['pad']//3) + bytes(case['pad']%3) if polling else bytes(case['pad'])
    window = padding + bytes.fromhex('3e 91') + operation + bytes(4) + bytes.fromhex('c3 00 11')
    data[0x2000:0x2000 + len(window)] = window
    data[0x1100:0x1103] = bytes.fromhex('76 18 fd')
    checksum = 0
    for value in data[0x134:0x14d]:
        checksum = (checksum - value - 1) & 255
    data[0x14d] = checksum
    data[0x14e:0x150] = (sum(data) & 65535).to_bytes(2, 'big')
    return bytes(data)


def validate_sample(sample):
    ranges = {'clock': 20000000, 'pc': 65535, 'af': 65535, 'bc': 65535, 'de': 65535,
              'hl': 65535, 'sp': 65535, 'ly': 153, 'mode': 3, 'vram': 255, 'palette': 255,
              'frontier': 160}
    if not isinstance(sample, dict) or any(type(sample.get(k)) is not int or
                                         not 0 <= sample[k] <= maximum for k, maximum in ranges.items()):
        raise ValueError('invalid access snapshot')


def quiet_calibration_rom():
    """Reproduce the rejected NOP-only reference snapshot calibration."""
    case = next(c for c in scenarios() if c['name'] == 'normal-end-vram-read-pad168')
    data = bytearray(probe_rom(case, False))
    position = operation_pc(case, False) - 2
    data[position:position + 2] = b'\0\0'  # Two NOPs replace LD A,91 (same 8 clocks).
    data[0x14e:0x150] = b'\0\0'
    data[0x14e:0x150] = (sum(data) & 65535).to_bytes(2, 'big')
    return bytes(data)


def validate_trace(trace, case, core, enabled=True):
    required = dict(schema='gbb-cgb-pixel-access-trace-v1', qualification=False, core=core,
                    model='cgb-e', clock_unit='8mhz', anchor='before_step_pc_1000',
                    complete=True, snapshots_enabled=enabled)
    if not isinstance(trace, dict) or any(type(trace.get(k)) is not type(v) or trace.get(k) != v
                                          for k, v in required.items()):
        raise ValueError('invalid trace metadata')
    if type(trace.get('state_bytes')) is not int or not 0 < trace['state_bytes'] <= 2*1024*1024:
        raise ValueError('invalid state size')
    if type(trace.get('state_fnv1a64')) is not int or not 0 <= trace['state_fnv1a64'] < 1 << 64:
        raise ValueError('invalid state fingerprint')
    validate_sample(trace['final'])
    if trace['final']['pc'] != 0x1100:
        raise ValueError('missing completion anchor')
    samples = trace.get('samples')
    if not isinstance(samples, list) or len(samples) > 513 or (not enabled and samples):
        raise ValueError('unbounded or unexpected snapshots')
    if enabled:
        for sample in samples:
            validate_sample(sample)
        if not samples or samples[0]['pc'] != 0x1000 or samples[0]['clock'] != 0 or samples[-1] != trace['final']:
            raise ValueError('invalid trace anchors')
        if any(a['clock'] >= b['clock'] for a, b in zip(samples, samples[1:])):
            raise ValueError('nonmonotonic trace')
        operation = operation_pc(case)
        matches = [i for i, sample in enumerate(samples[:-1]) if sample['pc'] == operation]
        if len(matches) != 1:
            raise ValueError('missing or repeated tested instruction')
        n = matches[0]
        before, after = samples[n:n + 2]
        cycles = 12 if case['operation'].startswith('palette') else 8
        scale = 1 if case['speed'] else 2
        if (before['clock'] != (44 + 4*case['pad'])*scale or
                after['clock'] - before['clock'] != cycles*scale or
                after['pc'] != operation + len(OPERATIONS[case['operation']])):
            raise ValueError('unexpected tested-instruction timing')
    return trace


def final_state(trace):
    return {key: trace[key] for key in ('final', 'state_bytes', 'state_fnv1a64')}


def edge_intervals(samples, ly):
    edges = {'first_pixel': None, 'last_pixel': None}
    for a, b in zip(samples, samples[1:]):
        if a['ly'] == b['ly'] == ly:
            if a['frontier'] == 0 < b['frontier'] and edges['first_pixel'] is None:
                edges['first_pixel'] = [a['clock'], b['clock']]
            if a['frontier'] < 160 == b['frontier'] and edges['last_pixel'] is None:
                edges['last_pixel'] = [a['clock'], b['clock']]
    return edges


def witness(trace, case):
    samples = trace['samples']
    n = next(i for i, s in enumerate(samples[:-1]) if s['pc'] == operation_pc(case))
    before, after = samples[n:n + 2]
    op = case['operation']
    value = after['af'] >> 8 if op.endswith('read') else after['vram' if op.startswith('vram') else 'palette']
    allowed, blocked = ((0xa5, 0xff) if op == 'vram-read' else (0x5a, 0xa5) if op == 'vram-write'
                        else (3, 0xff) if op == 'palette-read' else (0x91, 3))
    accepted = True if value == allowed else False if value == blocked else None
    return dict(value=value, accepted=accepted, before=before, after=after,
                line0=edge_intervals(samples, 0), line1=edge_intervals(samples, 1))


def run_probe(adapter, reference, output):
    output.mkdir(parents=True, exist_ok=False)
    boot = output / 'owned.bin'
    boot.write_bytes(anchor.owned_boot())
    rows = []
    for case in scenarios():
        rom = output / (case['name'] + '.gbc')
        rom.write_bytes(probe_rom(case))
        captures, timelines = {}, {}
        for core, executable in (('gbb', adapter), ('sameboy', reference)):
            traces = []
            for take, enabled in ((1, True), (2, True), (3, False)):
                args = [str(executable), str(rom), str(boot)] + ([] if enabled else ['--end-only'])
                result = subprocess.run(args, capture_output=True, text=True, timeout=30)
                prefix = output / f'{case["name"]}-{core}-{take}'
                prefix.with_suffix('.json').write_text(result.stdout)
                prefix.with_suffix('.stderr').write_text(result.stderr)
                if result.returncode:
                    raise ValueError(f'{core} trace failed: {result.returncode}: {result.stderr}')
                traces.append(validate_trace(json.loads(result.stdout), case, core, enabled))
            if traces[0] != traces[1] or final_state(traces[0]) != final_state(traces[2]):
                raise ValueError(f'{core} snapshots changed final state or did not repeat')
            captures[core] = dict(witness(traces[0], case), repeatable=True, snapshot_equivalent=True)
            control_rom = output / (case['name'] + '-nop.gbc')
            control_rom.write_bytes(probe_rom(case, False))
            result = subprocess.run([str(executable), str(control_rom), str(boot), '--end-only'],
                                    capture_output=True, text=True, timeout=30)
            (output / f'{case["name"]}-{core}-nop.json').write_text(result.stdout)
            if result.returncode:
                raise ValueError(f'{core} NOP control failed')
            control = validate_trace(json.loads(result.stdout), case, core, False)
            op = case['operation']
            value = (control['final']['af'] >> 8 if op.endswith('read') else
                     control['final']['vram' if op.startswith('vram') else 'palette'])
            captures[core]['nop_control_value'] = value
            captures[core]['padding_equivalent'] = value == captures[core]['value']
            timelines[core] = traces[0]['samples']
        a, b = captures['gbb'], captures['sameboy']
        fields = lambda core, keys: [[s[k] for k in keys] for s in timelines[core]]
        rows.append(dict(scenario=case['name'], rom_sha256=hashlib.sha256(rom.read_bytes()).hexdigest(),
                         captures=captures, architectural_match=(a['value'], a['accepted']) == (b['value'], b['accepted']),
                         pixel_timeline_match=fields('gbb', ('clock', 'ly', 'frontier')) == fields('sameboy', ('clock', 'ly', 'frontier')),
                         mode_timeline_match=fields('gbb', ('clock', 'ly', 'mode')) == fields('sameboy', ('clock', 'ly', 'mode'))))
    report = dict(schema='gbb-cgb-pixel-access-probe-v1', qualification=False,
                  boot_sha256=hashlib.sha256(boot.read_bytes()).hexdigest(), rows=rows)
    (output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    return report


def passes(report):
    try:
        rows = report['rows']
        expected = {case['name'] for case in scenarios()}
        return (len(rows) == len(expected) and {r['scenario'] for r in rows} == expected and
                all(all(r[key] is True for key in ('architectural_match', 'pixel_timeline_match', 'mode_timeline_match'))
                    and set(r['captures']) == {'gbb', 'sameboy'} and
                    all(c['repeatable'] is True and c['snapshot_equivalent'] is True and
                        c['padding_equivalent'] is True and type(c['accepted']) is bool for c in r['captures'].values())
                    for r in rows))
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
    except (OSError, ValueError, KeyError, subprocess.SubprocessError) as error:
        parser.exit(2, f'pixel-access probe failed: {error}\n')


if __name__ == '__main__':
    raise SystemExit(main())
