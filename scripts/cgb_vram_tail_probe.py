#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Separate CGB transfer-end readback from stored, tile and attribute bytes."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

import cgb_object_access_probe as objects


def scenarios():
    cases = []
    for speed in (False, True):
        for dot in ((260, 262, 264, 266, 268) if speed else (260, 264, 268)):
            for cpu_bank in (0, 1):
                for sentinel in (0xa5, 0x3c):
                    for low, high in ((0, 0), (0xaa, 0x55), (0xff, 0xff)):
                        for palette in (0, 1):
                            for bg_bank in (0, 1):
                                addresses = (0x8100, 0x8201) if low == high == palette == bg_bank == 0 else (0x8100,)
                                for address in addresses:
                                    cases.append(dict(speed=speed, dot=dot, cpu_bank=cpu_bank,
                                                      sentinel=sentinel, low=low, high=high,
                                                      palette=palette, bg_bank=bg_bank, address=address,
                                                      other_high=0))
    cases += [dict(c, other_high=0x96) for c in cases
              if c['cpu_bank'] != c['bg_bank'] and c['address'] == 0x8100 and
              c['low'] == c['high'] == c['palette'] == 0]
    cases += [dict(c, row_ramp=True, scy=scy, last_tile=last_tile)
              for c in cases if c['dot'] == 264 and c['sentinel'] == 0xa5 and
              c['address'] == 0x8100 and c['low'] == c['high'] == c['palette'] == c['other_high'] == 0
              for scy in (0, 3, 7) for last_tile in (1, 2)]
    for c in cases:
        c['name'] = (f'{"double" if c["speed"] else "normal"}-dot{c["dot"]}-cpu{c["cpu_bank"]}'
                     f'-{c["address"]:04x}-{c["sentinel"]:02x}-tile{c["low"]:02x}{c["high"]:02x}'
                     f'-palette{c["palette"]}-bg{c["bg_bank"]}-other{c["other_high"]:02x}')
        if c.get('row_ramp'):
            c['name'] += f'-ramp-scy{c["scy"]}-last{c["last_tile"]}'
    return cases


def schedule(case):
    base = next(c for c in objects.scenarios() if c['speed'] == case['speed'] and
                c['side'] == 'left' and c['scx'] == 0 and c['display'] == 'on' and
                c['operation'] == 'vram-read' and c['name'].endswith('dot264'))
    return dict(base, pad=(case['dot'] + 436)//2 if case['speed'] else (case['dot'] + 420)//4)


def loop(code, body):
    code += body
    code += bytes((0x20, (-(len(body) + 2)) & 255))


def probe_rom(case):
    if case not in scenarios():
        raise ValueError('unknown VRAM-tail scenario')
    seed = next(c for c in objects.scenarios() if c['speed'] == case['speed'] and
                c['side'] == 'left' and c['scx'] == 0 and c['display'] == 'on' and
                c['operation'] == 'vram-read' and c['name'].endswith('dot264'))
    data = bytearray(objects.probe_rom(seed))
    at = data.index(bytes.fromhex('21 00 81 06 5a c3 00 10'), 0x150, 0x1000)
    # Every BG color is red=1, so data and palette-index changes preserve
    # the exact RGB marker. OBJ tile zero stays transparent in both banks.
    setup = bytearray.fromhex('3e 80 e0 68 06 20')
    loop(setup, bytes.fromhex('3e 01 e0 69 af e0 69 05'))
    for bank, value in ((0, 1), (1, case['palette'] | case['bg_bank'] << 3)):
        setup += bytes((0x3e, bank, 0xe0, 0x4f))
        setup += bytes.fromhex('21 00 98 01 00 04')
        loop(setup, bytes((0x3e, value, 0x22, 0x0b, 0x78, 0xb1)))
    if case.get('row_ramp'):
        for bank in (0, 1):
            setup += bytes((0x3e, bank, 0xe0, 0x4f, 0x21, 0x10, 0x80))
            for row in range(8):
                high = (row + 1)*0x11 if bank == case['bg_bank'] else 0x81 + row
                setup += bytes((0x3e, 0, 0x22, 0x3e, high, 0x22))
            high = 0xd2 if bank == case['bg_bank'] else 0xe3
            for _ in range(8):
                setup += bytes((0x3e, 0, 0x22, 0x3e, high, 0x22))
        if case['last_tile'] == 2:
            setup += bytes.fromhex('af e0 4f 3e 02')
            for row in range(32):
                address = 0x9814 + row*32
                setup += bytes((0xea, address & 255, address >> 8))
        setup += bytes((0x3e, case['scy'], 0xe0, 0x42))
    else:
        for bank, low, high in ((1 - case['bg_bank'], 0, case['other_high']),
                                (case['bg_bank'], case['low'], case['high'])):
            setup += bytes((0x3e, bank, 0xe0, 0x4f))
            setup += bytes.fromhex('21 10 80 06 08')
            loop(setup, bytes((0x3e, low, 0x22, 0x3e, high, 0x22, 0x05)))
    setup += bytes((0x3e, case['cpu_bank'], 0xe0, 0x4f,
                    0x21, case['address'] & 255, case['address'] >> 8,
                    0x3e, case['sentinel'], 0x77, 0x06, 0x5a, 0xc3, 0, 0x10))
    if at + len(setup) >= 0x1000:
        raise ValueError('setup overlaps trace anchor')
    data[at:at + len(setup)] = setup
    body = b''.join(op for op, _ in objects.program(schedule(case)))
    data[0x2000:0x2000 + len(body)] = body
    data[0x14e:0x150] = b'\0\0'
    data[0x14e:0x150] = (sum(data) & 65535).to_bytes(2, 'big')
    return bytes(data)


def classification(value, case):
    return 'stored' if value == case['sentinel'] else 'ff' if value == 255 else 'other'


def timing(trace):
    return [[s[k] for k in ('clock', 'ly', 'mode', 'frontier')] for s in trace['samples']]


def run_probe(adapter, reference, output):
    output.mkdir(parents=True, exist_ok=False)
    boot = output / 'owned.bin'
    boot.write_bytes(objects.phase.access.anchor.owned_boot())
    rows, controls = [], {}
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
                    raise ValueError(f'{core} tail capture failed: {result.returncode}')
                traces.append(objects.validate_trace(json.loads(result.stdout), schedule(case), core, enabled))
            if traces[0] != traces[1] or objects.phase.access.final_state(traces[0]) != objects.phase.access.final_state(traces[2]):
                raise ValueError(f'{core} tail snapshots changed final state')
            key = core, case['speed'], case['dot']
            view = timing(traces[0])
            if key in controls and controls[key] != view:
                raise ValueError(f'{core} data/bank variation changed visible transfer timing')
            controls[key] = view
            value = objects.value(traces[0], schedule(case))
            captures[core] = dict(value=value, classification=classification(value, case),
                                  repeatable=True, snapshot_equivalent=True, timing_control_equal=True)
        rows.append(dict(scenario=case['name'], rom_sha256=hashlib.sha256(rom.read_bytes()).hexdigest(),
                         captures=captures, match=captures['gbb']['value'] == captures['sameboy']['value']))
    report = dict(schema='gbb-cgb-vram-tail-probe-v1', qualification=False,
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
                            all(c[k] is True for k in ('repeatable', 'snapshot_equivalent', 'timing_control_equal'))
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
        parser.exit(2, f'VRAM-tail probe failed: {error}\n')


if __name__ == '__main__':
    raise SystemExit(main())
