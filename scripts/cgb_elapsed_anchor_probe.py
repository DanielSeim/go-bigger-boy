#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Compare authored CGB images at fixed elapsed clocks with the same owned boot."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

import cgb_frame_anchor_probe as anchor

TARGETS = (900000, 900512, 1200000, 1600000, 2000000)


def pixel_difference(first, second):
    images = [path.read_bytes()[len(b'P6\n160 144\n255\n'):] for path in (first, second)]
    # read_capture validates both headers, dimensions and lengths before this call.
    different = [i // 3 for i in range(0, len(images[0]), 3)
                 if images[0][i:i + 3] != images[1][i:i + 3]]
    return dict(different_pixels=len(different),
                bounding_box=([min(i % 160 for i in different), min(i // 160 for i in different),
                               max(i % 160 for i in different), max(i // 160 for i in different)]
                              if different else None))


def validate_metadata(metadata, targets, core):
    if core not in ('gbb', 'sameboy'):
        raise ValueError('unknown core')
    if len(metadata) != len(targets) + 1 or metadata[0].get('event') != 'handoff':
        raise ValueError('missing handoff/captures')
    if metadata[0].get('model') != 'cgb-e' or metadata[0].get('pc') != 0x100:
        raise ValueError('invalid handoff anchor')
    for row, target in zip(metadata[1:], targets):
        required = dict(event='capture', qualification=False, target_8mhz=target,
                        anchor='elapsed_8mhz_after_boot_unmap_pc_0100',
                        schema='gbb-elapsed-capture-v1' if core == 'gbb'
                        else 'gbb-sameboy-elapsed-capture-v1')
        if any(type(row.get(k)) is not type(v) or row.get(k) != v for k, v in required.items()):
            raise ValueError('invalid capture metadata')
        clocks = row.get('clocks_8mhz')
        if type(clocks) is not int or not target <= clocks < target + 128:
            raise ValueError('invalid elapsed-clock boundary')
        for key, maximum in (('completed_frames', 100000), ('pc', 65535), ('ly', 153), ('lcdc', 255)):
            if type(row.get(key)) is not int or not 0 <= row[key] <= maximum:
                raise ValueError(f'invalid capture field: {key}')
        if core == 'sameboy' and row.get('ppu_synchronized') is not True:
            raise ValueError('unsynchronized reference pixels')
    return metadata


def run_probe(adapter, reference, output):
    output.mkdir(parents=True, exist_ok=False)
    boot = output / 'owned.bin'
    boot.write_bytes(anchor.owned_boot())
    rows = []
    for speed in (False, True):
        for delay in (0, 4096):
            scenario = f'{"double" if speed else "normal"}-delay-{delay}'
            rom = output / (scenario + '.gbc')
            rom.write_bytes(anchor.probe_rom(speed, delay))
            captures = {}
            for core in ('gbb', 'sameboy'):
                takes = []
                for take in (1, 2):
                    prefix = output / f'{scenario}-{core}-{take}'
                    args = ([str(adapter), str(rom), 'owned'] if core == 'gbb' else
                            [str(reference), str(rom), str(boot), 'cgb-e'])
                    args += [','.join(map(str, TARGETS)), str(prefix)]
                    result = subprocess.run(args, capture_output=True, text=True, timeout=30)
                    prefix.with_suffix('.jsonl').write_text(result.stdout)
                    prefix.with_suffix('.stderr').write_text(result.stderr)
                    if result.returncode:
                        raise ValueError(f'{core} failed: {result.returncode}: {result.stderr}')
                    metadata = validate_metadata([json.loads(line) for line in result.stdout.splitlines()], TARGETS, core)
                    images = [anchor.read_capture(Path(str(prefix) + f'-{t}.ppm')) for t in TARGETS]
                    takes.append(dict(metadata=metadata, images=images))
                if takes[0] != takes[1]:
                    raise ValueError(f'{core} did not repeat')
                captures[core] = takes[0]
            for n, target in enumerate(TARGETS):
                rows.append(dict(scenario=scenario, target_8mhz=target,
                                 rom_sha256=hashlib.sha256(rom.read_bytes()).hexdigest(),
                                 captures={c: dict(metadata=v['metadata'][n + 1], image=v['images'][n])
                                           for c, v in captures.items()},
                                 handoffs={c: v['metadata'][0] for c, v in captures.items()},
                                 **pixel_difference(output / f'{scenario}-gbb-1-{target}.ppm',
                                                    output / f'{scenario}-sameboy-1-{target}.ppm'),
                                 match=captures['gbb']['images'][n] == captures['sameboy']['images'][n],
                                 repeatable=True))
    report = dict(schema='gbb-cgb-elapsed-anchor-probe-v1', qualification=False,
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
        parser.exit(2, f'elapsed-anchor probe failed: {error}\n')


if __name__ == '__main__':
    raise SystemExit(main())
