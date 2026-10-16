#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Combine authored timer PCM production, STAT scrolling and VBlank APU updates."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

import cgb_pcm_stat_probe as isolated

SCENARIOS = ('combined-pulse', 'combined-wave', 'vblank-pulse', 'vblank-wave')


def probe_rom(scenario, double_speed=False):
    if scenario not in SCENARIOS:
        raise ValueError('unknown combined scenario')
    channel = scenario.split('-')[1]
    updates = scenario.startswith('vblank')
    rom = bytearray(isolated.probe_rom(channel, double_speed))
    # Keep instruction cost unchanged, but record TIMA rather than DIV for finer
    # interrupt-service phase evidence (16 CPU T-cycles per TIMA increment).
    marker = bytes.fromhex('f0 44 22 f0 04 22')
    phase = rom.find(marker, 0x300, 0x380)
    if phase < 0:
        raise ValueError('timer sample layout changed')
    rom[phase:phase + len(marker)] = bytes.fromhex('f0 44 22 f0 05 22')
    # Extend our authored startup, retaining the isolated timer ISR and controls.
    begin = bytes.fromhex('21 00 c1 af e0 04 e0 0f 3e e2 e0 05 e0 06')
    end = bytes.fromhex('3e 04 ea ff ff 3e 91 e0 40 3e 05 e0 07 fb 76 18 fd')
    start = rom.find(begin, 0x150, 0x300)
    if start < 0 or bytes(rom[start + len(begin):start + len(begin) + len(end)]) != end:
        raise ValueError('isolated probe layout changed')
    tile = bytearray.fromhex('21 00 80')
    for _ in range(8):
        tile += bytes.fromhex('3e 55 22 3e 33 22')
    ending = bytes.fromhex('3e 20 e0 41') + bytes((0x3e, 7 if updates else 6))
    ending += bytes.fromhex('ea ff ff 3e 91 e0 40 3e 05 e0 07 fb 76 18 fd')
    code = tile + begin + ending
    if start + len(code) > 0x300:
        raise ValueError('combined startup overlaps timer ISR')
    rom[start:start + len(code)] = code
    # Mode-2 IRQ: preserve the timer's HL cursor, select sample (LY & 63),
    # convert its channel amplitude to a small horizontal scroll, then restore.
    # This intentionally includes interrupt latency and may write during mode 3.
    rom[0x48:0x4b] = bytes.fromhex('c3 00 04')
    stat = bytearray.fromhex('f5 e5 f0 44 e6 3f 87 87 6f 26 c1')
    if channel == 'wave':
        stat += bytes.fromhex('2c')  # PCM34 is the second tuple byte.
    stat += bytes.fromhex('7e cb 3f cb 3f e6 07 e0 43 e1 f1 d9')
    rom[0x400:0x400 + len(stat)] = stat
    if updates:
        rom[0x40:0x43] = bytes.fromhex('c3 80 03')
        # Stop timer; reset completion/cursor; alternate volume; retrigger; restart.
        vblank = bytearray.fromhex('f5 af e0 07 ea 00 c0 ea 01 c0 fa 02 c0 3c ea 02 c0 e6 01')
        if channel == 'pulse':
            vblank += bytes.fromhex('28 04 3e 50 18 02 3e f0 e0 12 3e 87 e0 14')
        else:
            vblank += bytes.fromhex('28 04 3e 40 18 02 3e 20 e0 1c 3e 87 e0 1e')
        vblank += bytes.fromhex('21 00 c1 3e e2 e0 05 3e 05 e0 07 f1 d9')
        rom[0x380:0x380 + len(vblank)] = vblank
    rom[0x14d:0x150] = bytes(3)
    checksum = 0
    for value in rom[0x134:0x14d]:
        checksum = (checksum - value - 1) & 255
    rom[0x14d] = checksum
    rom[0x14e:0x150] = (sum(rom) & 65535).to_bytes(2, 'big')
    return bytes(rom)


def analyze(memory, scenario, frame):
    if len(memory) < 8192 or memory[:2] != b'\x40\x01' or memory[0x200] != 0xa5:
        raise ValueError('incomplete combined samples or damaged guard')
    updates = scenario.startswith('vblank')
    channel = scenario.split('-')[1]
    count = memory[2]
    expected_count = frame - 1 if updates else 0
    amplitude = (5 if channel == 'pulse' else 7) if updates and expected_count & 1 else 15
    rows = [list(memory[i:i + 4]) for i in range(0x100, 0x200, 4)]
    pcm = [row[:2] for row in rows]
    active = (amplitude, 0) if channel == 'pulse' else (0, amplitude)
    constraints = count == expected_count and set(map(tuple, pcm)) == {(0, 0), active}
    return dict(samples=rows, pcm=pcm, vblank_updates=count, expected_vblank_updates=expected_count,
                sample_fields=['pcm12', 'pcm34', 'ly', 'tima'],
                expected_amplitude=amplitude, authored_constraints_match=constraints,
                pcm_sha256=hashlib.sha256(bytes(v for row in pcm for v in row)).hexdigest())


def run_probe(runner, adapter, output):
    return isolated.run_probe(runner, adapter, output, scenarios=SCENARIOS, checkpoints=(2, 3, 4),
                              rom_builder=probe_rom, analyzer=analyze, compare_pixels=True,
                              schema='gbb-cgb-combined-irq-probe-v1')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--runner', type=Path, required=True)
    parser.add_argument('--sameboy-capture', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    try:
        report = run_probe(args.runner.resolve(), args.sameboy_capture.resolve(), args.output.resolve())
        print(json.dumps(report))
        return 0 if isolated.passes(report) else 1
    except (OSError, ValueError, KeyError, subprocess.SubprocessError) as error:
        parser.exit(2, f'combined IRQ probe failed: {error}\n')


if __name__ == '__main__':
    raise SystemExit(main())
