#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Authored timer/PCM and STAT/SCX comparisons, independent of demo assets."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

import cgb_frame_anchor_probe as anchor

SCENARIOS = ('silence', 'pulse', 'wave', 'scroll-0', 'scroll-3', 'scroll-ly')
PALETTE = (0, 8, 16, 31)


def probe_rom(scenario, double_speed=False):
    if scenario not in SCENARIOS:
        raise ValueError('unknown probe scenario')
    rom = bytearray(32768)
    rom[0x100:0x103] = bytes.fromhex('c3 50 01')
    rom[0x134:0x13d] = b'GBB PROBE'
    rom[0x143] = 0xc0
    code = bytearray.fromhex('f3 31 fe ff af e0 40 e0 4f e0 07 e0 26 e0 41')
    # Clear VRAM bank zero and the bounded WRAM sample area plus guard byte.
    code += bytes.fromhex('21 00 80 01 00 20 af 22 0b 78 b1 20 f9')
    code += bytes.fromhex('21 00 c0 01 01 02 af 22 0b 78 b1 20 f9')
    if double_speed:
        code += bytes.fromhex('3e 01 e0 4d 10 00')
    code += bytes.fromhex('af e0 42 e0 43 3e 80 e0 68 21 00 05 06 08 2a e0 69 05 20 fa')
    rom[0x500:0x508] = bytes(value for red in PALETTE for value in (red, 0))
    if scenario.startswith('scroll'):
        code += bytes.fromhex('21 00 80')
        # Tile zero repeats color indices 0,1,2,3,0,1,2,3 on every row.
        for _ in range(8):
            code += bytes.fromhex('3e 55 22 3e 33 22')
        if scenario == 'scroll-ly':
            # Direct mode-2 handler writes SCX before mode 3, including at normal speed.
            handler = bytes.fromhex('f5 f0 44 e6 07 e0 43 f1 d9')
            rom[0x48:0x48 + len(handler)] = handler
            code += bytes.fromhex('3e 20 e0 41 3e 02 ea ff ff')
        else:
            code += bytes((0x3e, 3 if scenario == 'scroll-3' else 0, 0xe0, 0x43))
            code += bytes.fromhex('af ea ff ff')
        code += bytes.fromhex('af e0 0f 3e 91 e0 40 fb 76 18 fd')
    else:
        # Timer handler records 64 tuples: PCM12, PCM34, LY, DIV. HL is its cursor.
        rom[0x50:0x53] = bytes.fromhex('c3 00 03')
        handler = bytes.fromhex(
            'f5 f0 76 22 f0 77 22 f0 44 22 f0 04 22'
            'fa 00 c0 3c ea 00 c0 fe 40 20 08'
            'af e0 07 3e 01 ea 01 c0 f1 d9')
        rom[0x300:0x300 + len(handler)] = handler
        code += bytes.fromhex('3e a5 ea 00 c2 af e0 04 3e 80 e0 26')
        if scenario == 'pulse':
            # CH1: 50% duty, constant volume 15, frequency 2040, no sweep/length.
            code += bytes.fromhex('af e0 10 3e 80 e0 11 3e f0 e0 12 3e f8 e0 13 3e 87 e0 14')
        elif scenario == 'wave':
            # CH3: alternating pairs of 15/0 samples, full volume, frequency 2040.
            code += bytes.fromhex('21 30 ff')
            for _ in range(8):
                code += bytes.fromhex('3e ff 22 af 22')
            code += bytes.fromhex('3e 80 e0 1a 3e 20 e0 1c 3e f8 e0 1d 3e 87 e0 1e')
        # 30 timer ticks at 16 CPU T-cycles/tick; freeze after sample 64.
        code += bytes.fromhex(
            '21 00 c1 af e0 04 e0 0f 3e e2 e0 05 e0 06'
            '3e 04 ea ff ff 3e 91 e0 40 3e 05 e0 07 fb 76 18 fd')
    if 0x150 + len(code) > 0x300:
        raise ValueError('probe program overlaps handler')
    rom[0x150:0x150 + len(code)] = code
    checksum = 0
    for value in rom[0x134:0x14d]:
        checksum = (checksum - value - 1) & 255
    rom[0x14d] = checksum
    rom[0x14e:0x150] = (sum(rom) & 65535).to_bytes(2, 'big')
    return bytes(rom)


def expected_scroll(scenario):
    if scenario not in ('scroll-0', 'scroll-3', 'scroll-ly'):
        raise ValueError('not a scrolling scenario')
    rgb = bytearray()
    for y in range(144):
        shift = y & 7 if scenario == 'scroll-ly' else (3 if scenario == 'scroll-3' else 0)
        for x in range(160):
            red = PALETTE[(x + shift) & 3]
            rgb.extend((red << 3 | red >> 2, 0, 0))
    return bytes(rgb)


def samples(data, scenario):
    if len(data) < 8192 or data[:2] != b'\x40\x01' or data[0x200] != 0xa5:
        raise ValueError('incomplete timer samples or damaged guard')
    rows = [list(data[i:i + 4]) for i in range(0x100, 0x200, 4)]
    pcm = [row[:2] for row in rows]
    allowed = {'silence': {(0, 0)}, 'pulse': {(0, 0), (15, 0)},
               'wave': {(0, 0), (0, 15)}}[scenario]
    observed = set(map(tuple, pcm))
    valid = observed <= allowed and (scenario == 'silence' or observed == allowed)
    return dict(samples=rows, pcm=pcm, authored_constraints_match=valid,
                pcm_sha256=hashlib.sha256(bytes(v for row in pcm for v in row)).hexdigest())


def run_probe(runner, adapter, output, *, scenarios=SCENARIOS, checkpoints=(2, 4),
              rom_builder=probe_rom, analyzer=None, compare_pixels=False,
              comparison_key=None, comparison_name=None,
              write_report=True,
              max_cpu_cycles=20_000_000, max_reference_clocks=40_000_000,
              schema='gbb-cgb-pcm-stat-probe-v1'):
    if (type(max_cpu_cycles) is not int or type(max_reference_clocks) is not int or
            max_cpu_cycles <= 0 or max_reference_clocks <= 0):
        raise ValueError('capture budgets must be positive integer clock counts')
    output.mkdir(parents=True, exist_ok=False)
    boot_data = anchor.owned_boot()
    boot = output / 'owned-cgb-boot.bin'
    boot.write_bytes(boot_data)
    report = dict(schema=schema, qualification=False, model='cgb-e',
                  boot_sha256=hashlib.sha256(boot_data).hexdigest(),
                  runner_sha256=hashlib.sha256(runner.read_bytes()).hexdigest(),
                  adapter_sha256=hashlib.sha256(adapter.read_bytes()).hexdigest(), commands=[], rows=[])
    for double_speed in (False, True):
        for scenario in scenarios:
            name = f'{scenario}-{"double" if double_speed else "normal"}'
            rom_data = rom_builder(scenario, double_speed)
            rom = output / (name + '.gbc')
            rom.write_bytes(rom_data)
            for frame in checkpoints:
                captures = {}
                pixel_bytes = {}
                for emulator in ('gbb', 'sameboy'):
                    takes = []
                    for take in (1, 2):
                        prefix = output / f'{name}-{frame}-{emulator}-{take}'
                        image = Path(str(prefix) + f'-{frame}.ppm')
                        state = Path(str(prefix) + f'-{frame}.state')
                        if emulator == 'gbb':
                            command = [str(runner), str(rom), '--model', 'cgb-e', '--dmg-boot',
                                       '--frame-series', str(frame), str(frame), str(prefix),
                                       '--frame-state-series', '--max-cycles', str(max_cpu_cycles)]
                        else:
                            command = [str(adapter), str(rom), str(boot), str(frame), str(image),
                                       'cgb-e', str(max_reference_clocks), str(state)]
                        report['commands'].append(command)
                        result = subprocess.run(command, capture_output=True, text=True, timeout=30)
                        if result.returncode:
                            raise ValueError(f'{emulator} capture failed: exit {result.returncode}')
                        capture = anchor.read_capture(image)
                        if compare_pixels:
                            pixel_bytes[emulator] = image.read_bytes()[15:]
                        if emulator == 'sameboy':
                            metadata = json.loads(result.stdout)
                            required = dict(schema='gbb-sameboy-cgb-capture-v1', qualification=False,
                                            completed_ppu_frames=frame, model='cgb-e', random_seed=0,
                                            color_correction='disabled',
                                            capture_anchor='normal_vblank_including_boot')
                            if not isinstance(metadata, dict) or any(metadata.get(k) != v for k, v in required.items()):
                                raise ValueError('reference metadata mismatch')
                            capture['metadata'] = metadata
                        memory = state.read_bytes()
                        if len(memory) != (8192 if emulator == 'sameboy' else 8352):
                            raise ValueError('unexpected state size')
                        if analyzer is not None:
                            capture.update(analyzer(memory, scenario, frame))
                        elif scenario.startswith('scroll'):
                            capture['authored_constraints_match'] = capture['rgb_sha256'] == hashlib.sha256(
                                expected_scroll(scenario)).hexdigest()
                        else:
                            capture.update(samples(memory, scenario))
                        takes.append(capture)
                    captures[emulator] = dict(takes[0], repeatable=takes[0] == takes[1])
                row = dict(scenario=scenario, double_speed=double_speed, frame=frame,
                           rom_sha256=hashlib.sha256(rom_data).hexdigest(), captures=captures)
                key = comparison_key or ('rgb_sha256' if scenario.startswith('scroll') else 'pcm_sha256')
                row['comparison'] = comparison_name or ('pixels' if key == 'rgb_sha256' else 'pcm_samples')
                row['match'] = captures['gbb'][key] == captures['sameboy'][key]
                if compare_pixels:
                    row['pixel_match'] = captures['gbb']['rgb_sha256'] == captures['sameboy']['rgb_sha256']
                    row['pcm_match'] = row['match']
                    row['match'] = row['match'] and row['pixel_match']
                    row['comparison'] = 'pcm_samples_and_pixels'
                    a, b = pixel_bytes['gbb'], pixel_bytes['sameboy']
                    differences = [i for i in range(160 * 144) if a[i * 3:i * 3 + 3] != b[i * 3:i * 3 + 3]]
                    row['different_pixel_count'] = len(differences)
                    row['first_pixel_difference'] = None
                    if differences:
                        i = differences[0]
                        row['first_pixel_difference'] = dict(x=i % 160, y=i // 160,
                            gbb=list(a[i * 3:i * 3 + 3]), sameboy=list(b[i * 3:i * 3 + 3]))
                if key == 'pcm_sha256':
                    row['different_sample_indices'] = [i for i, (a, b) in enumerate(zip(
                        captures['gbb']['pcm'], captures['sameboy']['pcm'])) if a != b]
                    row['first_sample_difference'] = None
                    if row['different_sample_indices']:
                        i = row['different_sample_indices'][0]
                        row['first_sample_difference'] = dict(index=i,
                            gbb=captures['gbb']['samples'][i], sameboy=captures['sameboy']['samples'][i])
                    if captures['gbb'].get('sample_fields') == ['pcm12', 'pcm34', 'ly', 'tima']:
                        row['different_timer_phase_indices'] = [i for i, (a, b) in enumerate(zip(
                            captures['gbb']['samples'], captures['sameboy']['samples'])) if a[3] != b[3]]
                elif key == 'event_sha256':
                    a, b = captures['gbb']['events'], captures['sameboy']['events']
                    row['different_event_indices'] = [i for i in range(len(a)) if a[i] != b[i]]
                    row['first_event_difference'] = None
                    if row['different_event_indices']:
                        i = row['different_event_indices'][0]
                        row['first_event_difference'] = dict(index=i, gbb=a[i], sameboy=b[i])
                report['rows'].append(row)
    if write_report:
        (output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    return report


def passes(report):
    return bool(report['rows']) and all(row['match'] and all(
        c['repeatable'] and c['authored_constraints_match'] for c in row['captures'].values())
        for row in report['rows'])


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
    except (OSError, ValueError, KeyError, subprocess.SubprocessError) as error:
        parser.exit(2, f'PCM/STAT probe failed: {error}\n')


if __name__ == '__main__':
    raise SystemExit(main())
