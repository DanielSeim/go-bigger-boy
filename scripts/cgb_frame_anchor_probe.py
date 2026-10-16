#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Measure CGB capture anchors using authored ROMs; never shift frames to match."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[1]
CHECKPOINTS = (1, 2, 4, 8)


def probe_rom(double_speed=False, delay=0):
    if not 0 <= delay <= 65535:
        raise ValueError('delay must fit BC')
    rom = bytearray(32768)
    rom[0x100:0x103] = bytes.fromhex('c3 50 01')  # JP cartridge entry
    rom[0x134:0x13e] = b'GBB ANCHOR'
    rom[0x143] = 0xc0  # CGB only; ROM-only cartridge, 32 KiB
    # VBlank: preserve registers, increment C000, set BG color zero to counter+1.
    # Changes during VBlank affect the *next* visible image.
    handler = bytes.fromhex(
        'f5 e5 21 00 c0 34 3e 80 e0 68 7e e6 1f 3c e0 69 af e0 69 e1 f1 d9')
    rom[0x40:0x40 + len(handler)] = handler
    code = bytearray.fromhex('f3 31 fe ff af e0 40 e0 4f')  # DI; SP; LCD off; bank 0
    # Explicitly clear bank-zero tiles/map; do not depend on cold VRAM contents.
    code += bytes.fromhex('21 00 80 01 00 20 af 22 0b 78 b1 20 f9')
    if delay:
        code += bytes((0x01, delay & 255, delay >> 8))
        code += bytes.fromhex('0b 78 b1 20 fb')
    if double_speed:
        code += bytes.fromhex('3e 01 e0 4d 10 00')  # KEY1 request; STOP
    code += bytes.fromhex(
        'af ea 00 c0 e0 42 e0 43'  # counter, SCY, SCX = 0
        '3e 80 e0 68 3e 01 e0 69 af e0 69'  # BG color 0 = raw red 1
        'e0 0f 3e 01 ea ff ff'  # clear IF; enable only VBlank
        '3e 91 e0 40 fb 76 18 fd')  # enable LCD; EI; HALT loop
    rom[0x150:0x150 + len(code)] = code
    checksum = 0
    for value in rom[0x134:0x14d]:
        checksum = (checksum - value - 1) & 255
    rom[0x14d] = checksum
    total = sum(rom) & 65535
    rom[0x14e:0x150] = total.to_bytes(2, 'big')
    return bytes(rom)


def owned_boot():
    header = (ROOT / 'firmware/gameboy/cgb_boot_image.hpp').read_text()
    data = bytes(int(n, 16) for n in re.findall(r'0x([0-9a-fA-F]{2})', header))
    if len(data) != 256:
        raise ValueError('unexpected owned CGB boot image')
    return data


def read_capture(path):
    data = path.read_bytes()
    header = b'P6\n160 144\n255\n'
    if not data.startswith(header) or len(data) != len(header) + 160 * 144 * 3:
        raise ValueError('unexpected capture dimensions/encoding')
    data = data[len(header):]
    colors = set(zip(data[::3], data[1::3], data[2::3]))
    solid = next(iter(colors)) if len(colors) == 1 else None
    return dict(rgb_sha256=hashlib.sha256(data).hexdigest(), unique_colors=len(colors),
                solid_rgb=list(solid) if solid else None)


def run_probe(runner, adapter, output):
    output.mkdir(parents=True, exist_ok=False)
    boot = output / 'owned-cgb-boot.bin'
    boot_data = owned_boot()
    boot.write_bytes(boot_data)
    rows = []
    commands = []
    for double_speed in (False, True):
        for delay in (0, 4096):
            scenario = f'{"double" if double_speed else "normal"}-delay-{delay}'
            rom_data = probe_rom(double_speed, delay)
            rom = output / (scenario + '.gbc')
            rom.write_bytes(rom_data)
            for frame in CHECKPOINTS:
                captures = {}
                for emulator in ('gbb', 'sameboy'):
                    takes = []
                    for take in (1, 2):
                        image = output / f'{scenario}-{frame}-{emulator}-{take}.ppm'
                        if emulator == 'gbb':
                            command = [str(runner), str(rom), '--model', 'cgb-e', '--dmg-boot',
                                       '--frames', str(frame), '--max-cycles', '20000000',
                                       '--frame-output', str(image)]
                        else:
                            command = [str(adapter), str(rom), str(boot), str(frame), str(image),
                                       'cgb-e', '40000000']
                        commands.append(command)
                        result = subprocess.run(command, capture_output=True, text=True, timeout=30)
                        if result.returncode:
                            raise ValueError(f'{emulator} capture failed: exit {result.returncode}')
                        capture = read_capture(image)
                        if emulator == 'sameboy':
                            metadata = json.loads(result.stdout)
                            required = dict(schema='gbb-sameboy-cgb-capture-v1', qualification=False,
                                            completed_ppu_frames=frame, model='cgb-e', random_seed=0,
                                            color_correction='disabled',
                                            capture_anchor='normal_vblank_including_boot')
                            if not isinstance(metadata, dict) or any(
                                    metadata.get(key) != value for key, value in required.items()):
                                raise ValueError('SameBoy capture metadata does not match request')
                            capture['metadata'] = metadata
                        takes.append(capture)
                    captures[emulator] = dict(takes[0], repeatable=takes[0] == takes[1])
                expected_rgb = [frame << 3 | frame >> 2, 0, 0]
                rows.append(dict(scenario=scenario, frame=frame,
                                 rom_sha256=hashlib.sha256(rom_data).hexdigest(),
                                 expected_rgb=expected_rgb, captures=captures,
                                 pixel_match=captures['gbb']['rgb_sha256'] == captures['sameboy']['rgb_sha256'],
                                 expected_marker_match=all(c['solid_rgb'] == expected_rgb
                                                           for c in captures.values())))
    report = dict(schema='gbb-cgb-frame-anchor-probe-v1', qualification=False,
                  model='cgb-e', anchor='normal_vblank_including_boot',
                  boot_sha256=hashlib.sha256(boot_data).hexdigest(),
                  runner_sha256=hashlib.sha256(runner.read_bytes()).hexdigest(),
                  adapter_sha256=hashlib.sha256(adapter.read_bytes()).hexdigest(),
                  commands=commands, rows=rows)
    (output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--runner', type=Path, required=True)
    parser.add_argument('--sameboy-capture', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    try:
        report = run_probe(args.runner.resolve(), args.sameboy_capture.resolve(), args.output.resolve())
        print(json.dumps(report))
        # Research mismatches are explicit failures, never a qualified validation.
        return 0 if all(row['expected_marker_match'] and row['pixel_match'] and
                        all(c['repeatable'] for c in row['captures'].values())
                        for row in report['rows']) else 1
    except (OSError, ValueError, KeyError, subprocess.SubprocessError) as error:
        parser.exit(2, f'frame-anchor probe failed: {error}\n')


if __name__ == '__main__':
    raise SystemExit(main())
