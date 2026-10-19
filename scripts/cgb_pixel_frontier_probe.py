#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Measure CGB pixel frontiers without fitting reference clocks or writing palettes in mode 3."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

import cgb_elapsed_anchor_probe as elapsed
import cgb_frame_anchor_probe as anchor

OFFSETS = (-24, -16, -8, 0, 8, 16, 24)
HEADER = b'P6\n160 144\n255\n'


def scenarios():
    cases = []
    for speed in (False, True):
        for scx in range(8):
            for red in (1, 2):
                # Eight extra CPU clocks initialize SCX; both cores use the
                # same absolute target, including this authored instruction cost.
                cases.append(dict(name=f'{"double" if speed else "normal"}-first-scx{scx}-red{red}',
                                  speed=speed, delay=4096, scx=scx, red=red,
                                  center=(1040200 + 8 if speed else 900000 + 16),
                                  first_frame=True))
    for speed, center in ((False, 1600000), (True, 2000000)):
        cases.append(dict(name=f'{"double" if speed else "normal"}-steady', speed=speed,
                          delay=0, scx=0, red=1, center=center, first_frame=False))
    return cases


def probe_rom(case):
    data = bytearray(anchor.probe_rom(case['speed'], case['delay']))
    if case['first_frame']:
        pattern = bytes.fromhex('af ea 00 c0 e0 42 e0 43')
        position = data.find(pattern, 0x150)
        if position < 0:
            raise ValueError('authored SCX initialization not found')
        # Insert LD A,SCX before the original LDH (SCX),A; leave SCY zero.
        data[position + 6:position + 6] = bytes((0x3e, case['scx']))
        data = data[:32768]
        palette = bytes.fromhex('3e 80 e0 68 3e 01 e0 69 af e0 69')
        position = data.find(palette, 0x150)
        if position < 0:
            raise ValueError('authored palette initialization not found')
        data[position + 5] = case['red']
    checksum = 0
    for value in data[0x134:0x14d]:
        checksum = (checksum - value - 1) & 255
    data[0x14d] = checksum
    data[0x14e:0x150] = b'\0\0'
    data[0x14e:0x150] = (sum(data) & 65535).to_bytes(2, 'big')
    return bytes(data)


def rgb(red):
    return [red << 3 | red >> 2, 0, 0]


def frontier(path, ly, current, previous):
    anchor.read_capture(path)  # Check complete RGB encoding before consuming it.
    if type(ly) is not int or not 0 <= ly < 144:
        raise ValueError('capture is not on a visible scanline')
    data = path.read_bytes()[len(HEADER):]
    row = [list(data[i:i + 3]) for i in range(ly * 480, (ly + 1) * 480, 3)]
    if current == previous:
        raise ValueError('marker colors must differ')
    end = next((i for i, color in enumerate(row) if color != current), 160)
    if any(color != previous for color in row[end:]):
        raise ValueError('row is not a single current/previous color frontier')
    return dict(frontier=end, current_rgb=current, previous_rgb=previous,
                row_sha256=hashlib.sha256(bytes(c for pixel in row for c in pixel)).hexdigest())


def run_probe(adapter, reference, output):
    output.mkdir(parents=True, exist_ok=False)
    boot = output / 'owned.bin'
    boot.write_bytes(anchor.owned_boot())
    rows = []
    for case in scenarios():
        rom = output / (case['name'] + '.gbc')
        rom.write_bytes(probe_rom(case))
        targets = [case['center'] + offset for offset in OFFSETS]
        captures = {}
        for core in ('gbb', 'sameboy'):
            takes = []
            for take in (1, 2):
                prefix = output / f'{case["name"]}-{core}-{take}'
                args = ([str(adapter), str(rom), 'owned'] if core == 'gbb' else
                        [str(reference), str(rom), str(boot), 'cgb-e'])
                args += [','.join(map(str, targets)), str(prefix)]
                result = subprocess.run(args, capture_output=True, text=True, timeout=30)
                prefix.with_suffix('.jsonl').write_text(result.stdout)
                prefix.with_suffix('.stderr').write_text(result.stderr)
                if result.returncode:
                    raise ValueError(f'{core} capture failed: {result.returncode}')
                metadata = elapsed.validate_metadata([json.loads(line) for line in result.stdout.splitlines()], targets, core)
                witnesses = []
                for target, row in zip(targets, metadata[1:]):
                    if case['first_frame'] and row['completed_frames'] != 0:
                        raise ValueError('first-frame control reached VBlank')
                    new = rgb(case['red'] if case['first_frame'] else row['completed_frames'] + 1)
                    old = [255, 255, 255] if case['first_frame'] else rgb(row['completed_frames'])
                    path = Path(str(prefix) + f'-{target}.ppm')
                    witnesses.append(dict(metadata=row, **frontier(path, row['ly'], new, old),
                                          image_sha256=hashlib.sha256(path.read_bytes()).hexdigest()))
                takes.append(dict(handoff=metadata[0], witnesses=witnesses))
            if takes[0] != takes[1]:
                raise ValueError(f'{core} capture did not repeat')
            captures[core] = takes[0]['witnesses']
        for n, target in enumerate(targets):
            a, b = captures['gbb'][n], captures['sameboy'][n]
            if any(a['metadata'][key] != b['metadata'][key] for key in ('ly', 'completed_frames', 'clocks_8mhz')):
                raise ValueError('frontiers do not share a clock/line/frame anchor')
            rows.append(dict(scenario=case['name'], target_8mhz=target,
                             rom_sha256=hashlib.sha256(rom.read_bytes()).hexdigest(),
                             captures={c: v[n] for c, v in captures.items()}, repeatable=True,
                             frontier_delta=b['frontier'] - a['frontier'],
                             match=a['row_sha256'] == b['row_sha256']))
    report = dict(schema='gbb-cgb-pixel-frontier-probe-v1', qualification=False,
                  boot_sha256=hashlib.sha256(boot.read_bytes()).hexdigest(), rows=rows)
    (output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--gbb-capture', type=Path, required=True)
    parser.add_argument('--sameboy-capture', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    try:
        report = run_probe(args.gbb_capture.resolve(), args.sameboy_capture.resolve(), args.output.resolve())
        print(json.dumps(report))
        return 0 if all(row['match'] for row in report['rows']) else 1
    except (OSError, ValueError, KeyError, subprocess.SubprocessError) as error:
        parser.exit(2, f'pixel-frontier probe failed: {error}\n')


if __name__ == '__main__':
    raise SystemExit(main())
