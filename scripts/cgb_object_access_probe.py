#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Execute VRAM accesses around native CGB object fetches and OBJ toggles."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

import cgb_pixel_phase_probe as phase


def scenarios():
    return [dict(name=f'{"double" if speed else "normal"}-{side}-scx{scx}-{display}-{op}-dot{dot}',
                 speed=speed, side=side, scx=scx, display=display, operation=op,
                 pad=(dot + 436)//2 if speed else (dot + 420)//4)
            for speed in (False, True) for side in ('left', 'middle') for scx in (0, 7)
            for display in ('off', 'on', 'off-during', 'on-after')
            for op in ('vram-read', 'vram-write') for dot in range(244, 273, 4)]


def padding(count):
    if count < 0:
        raise ValueError('toggle overlaps tested instruction')
    return [(bytes.fromhex('f0 41'), 12)]*(count//3) + [(b'\0', 4)]*(count % 3)


def program(case):
    count = case['pad']
    result = []
    if case['display'] in ('off-during', 'on-after'):
        dot = 228 if case['display'] == 'on-after' else (100 if case['side'] == 'left' else 180)
        prefix = ((452 + dot)*(2 if case['speed'] else 1) - 36)//4
        result += padding(prefix)
        result += [(bytes((0x3e, 0x93 if case['display'] == 'on-after' else 0x91)), 8),
                   (bytes.fromhex('e0 40'), 12)]
        count -= prefix + 5
    result += padding(count)
    result += [(bytes.fromhex('3e 91'), 8),
               (b'\x7e' if case['operation'] == 'vram-read' else b'\x70', 8)]
    return result + [(b'\0', 4)]*4 + [(bytes.fromhex('c3 00 11'), 16)]


def probe_rom(case):
    if case not in scenarios():
        raise ValueError('unknown object access case')
    off = case['display'] in ('off', 'on-after')
    control = f'object-{"disabled-" if off else ""}{case["side"]}'
    seed = next(c for c in phase.scenarios() if c['speed'] == case['speed'] and
                c['scx'] == case['scx'] and c['phase'] == 0 and c['control'] == control)
    data = bytearray(phase.probe_rom(seed))
    at = data.index(bytes.fromhex('21 41 ff c3 00 10'), 0x150, 0x1000)
    setup = bytes.fromhex('21 00 81 06 5a c3 00 10')
    data[at:at + len(setup)] = setup
    body = b''.join(op for op, _ in program(case))
    data[0x2000:0x2000 + len(body)] = body
    data[0x14e:0x150] = b'\0\0'
    data[0x14e:0x150] = (sum(data) & 65535).to_bytes(2, 'big')
    return bytes(data)


def validate_trace(trace, case, core, enabled=True):
    required = dict(schema='gbb-cgb-pixel-access-trace-v1', qualification=False,
                    core=core, model='cgb-e', clock_unit='8mhz',
                    anchor='before_step_pc_1000', complete=True, snapshots_enabled=enabled)
    if not isinstance(trace, dict) or any(type(trace.get(k)) is not type(v) or trace.get(k) != v
                                         for k, v in required.items()):
        raise ValueError('invalid object trace metadata')
    if (type(trace.get('state_bytes')) is not int or not 0 < trace['state_bytes'] <= 2*1024*1024 or
            type(trace.get('state_fnv1a64')) is not int or not 0 <= trace['state_fnv1a64'] < 1 << 64):
        raise ValueError('invalid object state fingerprint')
    phase.access.validate_sample(trace['final'])
    scale = 1 if case['speed'] else 2
    if trace['final']['pc'] != 0x1100 or trace['final']['clock'] != (84 + 4*case['pad'])*scale:
        raise ValueError('invalid object completion anchor')
    samples = trace.get('samples')
    if not isinstance(samples, list) or len(samples) > 513 or (not enabled and samples):
        raise ValueError('unbounded object snapshots')
    if enabled:
        pcs, clocks = [0x1000, 0x1002, 0x1004], [0, 8*scale, 20*scale]
        pc, clock = 0x2000, 36*scale
        for op, cycles in program(case):
            pcs.append(pc)
            clocks.append(clock)
            pc += len(op)
            clock += cycles*scale
        pcs.append(0x1100)
        clocks.append(clock)
        if len(samples) != len(pcs) or samples[-1] != trace['final']:
            raise ValueError('invalid object snapshot anchors')
        for sample, pc, clock in zip(samples, pcs, clocks):
            phase.access.validate_sample(sample)
            if sample['pc'] != pc or sample['clock'] != clock:
                raise ValueError('unexpected object instruction timing')
    return trace


def value(trace, case):
    final = trace['final']
    return final['af'] >> 8 if case['operation'] == 'vram-read' else final['vram']


def run_probe(adapter, reference, output):
    output.mkdir(parents=True, exist_ok=False)
    boot = output / 'owned.bin'
    boot.write_bytes(phase.access.anchor.owned_boot())
    rows = []
    for case in scenarios():
        rom = output / (case['name'] + '.gbc')
        rom.write_bytes(probe_rom(case))
        captures = {}
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
                    raise ValueError(f'{core} object capture failed: {result.returncode}')
                traces.append(validate_trace(json.loads(result.stdout), case, core, enabled))
            if traces[0] != traces[1] or phase.access.final_state(traces[0]) != phase.access.final_state(traces[2]):
                raise ValueError(f'{core} object snapshots changed final state')
            captures[core] = dict(value=value(traces[0], case), repeatable=True, snapshot_equivalent=True)
        rows.append(dict(scenario=case['name'], rom_sha256=hashlib.sha256(rom.read_bytes()).hexdigest(),
                         captures=captures, match=captures['gbb']['value'] == captures['sameboy']['value']))
    report = dict(schema='gbb-cgb-object-access-probe-v1', qualification=False,
                  boot_sha256=hashlib.sha256(boot.read_bytes()).hexdigest(), rows=rows)
    (output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    return report


def passes(report):
    try:
        rows = report['rows']
        return (len(rows) == len(scenarios()) and {r['scenario'] for r in rows} == {c['name'] for c in scenarios()}
                and all(r['match'] is True and set(r['captures']) == {'gbb', 'sameboy'} and
                        r['captures']['gbb']['value'] == r['captures']['sameboy']['value'] and
                        all(type(c['value']) is int and 0 <= c['value'] <= 255 and
                            c['repeatable'] is True and c['snapshot_equivalent'] is True
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
        parser.exit(2, f'object-access probe failed: {error}\n')


if __name__ == '__main__':
    raise SystemExit(main())
