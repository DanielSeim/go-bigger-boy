#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Compare pinned SameSuite CH1/CH2 alignment curves and explicit protocols."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess

import cgb_frame_anchor_probe as anchor

PIN = Path(__file__).resolve().parents[1] / 'tests/fixtures/external-reference/samesuite-alignment.json'


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def protocol(emulator, result):
    if emulator == 'gbb':
        # SameSuite emits the six Mooneye register bytes over serial without a
        # terminating newline; text-mode subprocess capture normalizes CR to LF.
        output = result.stdout.removeprefix('\x03\x05\x08\n\x15"')
        passed = result.returncode == 0 and output == 'PASS (Mooneye)\n'
        failed = result.returncode == 1 and 'FAIL (Mooneye result registers)' in result.stderr.splitlines()
        hit = re.search(r'^CPU_FIRST_PC cycle=(\d+) .*next_pc=0528\b', result.stderr, re.M)
        if not (passed or failed) or hit is None:
            raise ValueError('missing complete GBB result or breakpoint witness')
        return dict(passed=passed, clock_unit='cpu_t_cycles', completion_clocks=int(hit[1]))
    data = json.loads(result.stdout)
    if not isinstance(data, dict):
        raise ValueError('invalid SameBoy result object')
    status = data.get('result')
    regs = (data.get('bc'), data.get('de'), data.get('hl'))
    expected = (0x0305, 0x080d, 0x1522) if status == 'pass' else (0x4242,) * 3
    if (data.get('schema') != 'gbb-sameboy-mooneye-v1' or data.get('model') != 'cgb-e' or
            status not in ('pass', 'fail') or result.returncode != (0 if status == 'pass' else 1) or
            regs != expected or type(data.get('clocks_8mhz')) is not int or data['clocks_8mhz'] <= 0):
        raise ValueError('missing complete SameBoy result protocol')
    return dict(passed=status == 'pass', clock_unit='8mhz_clocks', completion_clocks=data['clocks_8mhz'])


def curve(memory, channel):
    if channel not in (1, 2):
        raise ValueError('unsupported PCM channel')
    if len(memory) not in (8192, 8352):
        raise ValueError('unexpected WRAM snapshot size')
    values = list(memory[:48])
    allowed = {0, 8 if channel == 1 else 128}
    if not set(values) <= allowed:
        raise ValueError('unexpected PCM nibble or volume')
    groups = [values[i:i + 16] for i in range(0, 48, 16)]
    return dict(samples=values, first_nonzero=[next((i for i, v in enumerate(g) if v), None) for g in groups],
                sample_sha256=hashlib.sha256(bytes(values)).hexdigest())


def run_reference(rom_root, runner, adapter, output, *, pin_path=PIN, curve_reader=None,
                  schema='gbb-samesuite-alignment-reference-v1'):
    pin = json.loads(pin_path.read_text())
    roms = {name: rom_root / entry['path'] for name, entry in pin['tests'].items()}
    for name, rom in roms.items():
        if digest(rom) != pin['tests'][name]['sha256']:
            raise ValueError('external ROM hash does not match pin')
    output.mkdir(parents=True, exist_ok=False)
    boot = output / 'owned-cgb-boot.bin'
    boot.write_bytes(anchor.owned_boot('cgb-e'))
    report = dict(schema=schema, qualification=False,
        pin=pin, model='cgb-e', cpu_speed=pin.get('cpu_speed', 'double'), boot_sha256=digest(boot),
        runner_sha256=digest(runner), adapter_sha256=digest(adapter), rows=[])
    def execute(command):
        result = subprocess.run(list(map(str, command)), capture_output=True, text=True, timeout=30)
        return result
    for name, rom in roms.items():
        captures = {}
        for emulator in ('gbb', 'sameboy'):
            takes = []
            commands = []
            for take in (1, 2):
                prefix = output / f'{name}-{emulator}-{take}'
                command = ([runner, rom, '--model', 'cgb-e', '--dmg-boot', '--protocol', 'mooneye',
                            '--trace-first-pc', '0x0528', '--max-cycles', '20000000'] if emulator == 'gbb'
                           else [adapter, '--mooneye', rom, boot, 'cgb-e', '200000000'])
                result = execute(command)
                measured = protocol(emulator, result)
                (Path(str(prefix) + '.log')).write_text(result.stdout + result.stderr)
                commands.append(list(map(str, command)))
                if emulator == 'gbb':
                    command = [runner, rom, '--model', 'cgb-e', '--dmg-boot', '--frame-series', '16', '16',
                               prefix, '--frame-state-series', '--max-cycles', '20000000']
                    state = Path(str(prefix) + '-16.state')
                else:
                    state = Path(str(prefix) + '.state')
                    command = [adapter, rom, boot, '16', str(prefix) + '.ppm', 'cgb-e', '200000000', state]
                snapshot = execute(command)
                commands.append(list(map(str, command)))
                if snapshot.returncode:
                    raise ValueError('alignment snapshot failed')
                if emulator == 'gbb':
                    metadata = dict(token.split('=') for token in Path(str(prefix) + '-16.meta').read_text().split())
                    clocks = int(metadata['cycles'])
                    signature = (0x0305, 0x080d, 0x1522) if measured['passed'] else (0x4242,) * 3
                    if int(metadata['frame']) != 16 or tuple(int(metadata[k], 16) for k in ('bc', 'de', 'hl')) != signature:
                        raise ValueError('snapshot has no frozen completion signature')
                else:
                    metadata = json.loads(snapshot.stdout)
                    if (metadata.get('schema') != 'gbb-sameboy-cgb-capture-v1' or
                            metadata.get('model') != 'cgb-e' or metadata.get('completed_ppu_frames') != 16):
                        raise ValueError('unexpected reference snapshot metadata')
                    clocks = metadata['clocks_8mhz']
                if clocks < measured['completion_clocks']:
                    raise ValueError('snapshot precedes completed test')
                measured['snapshot_clocks'] = clocks
                measured.update(curve_reader(state.read_bytes(), pin['tests'][name]) if curve_reader
                                else curve(state.read_bytes(), pin['tests'][name]['channel']))
                takes.append(measured)
            captures[emulator] = dict(takes[0], repeatable=takes[0] == takes[1], commands=commands)
        a, b = captures['gbb']['samples'], captures['sameboy']['samples']
        if len(a) != len(b):
            raise ValueError('curve length mismatch')
        differences = [i for i in range(len(a)) if a[i] != b[i]]
        report['rows'].append(dict(test=name, channel=pin['tests'][name]['channel'],
            group_variable=pin['tests'][name]['group_variable'],
            phase_nops=pin['tests'][name].get('phase_nops', [0, 1, 3]),
            sample_delay_nops=pin['tests'][name].get('sample_delay_nops', list(range(16))),
            captures=captures, match=not differences,
            different_sample_indices=differences))
    (output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    return report


def passes(report, pin_path=PIN):
    try:
        expected = set(json.loads(pin_path.read_text())['tests'])
        rows = report['rows']
        return len(rows) == len(expected) and {r['test'] for r in rows} == expected and all(
            row['match'] is True and set(row['captures']) == {'gbb', 'sameboy'} and all(
                c['passed'] is True and c['repeatable'] is True for c in row['captures'].values()) for row in rows)
    except (KeyError, TypeError):
        return False


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--rom-root', type=Path, required=True)
    parser.add_argument('--runner', type=Path, required=True)
    parser.add_argument('--sameboy-capture', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    try:
        report = run_reference(args.rom_root.resolve(), args.runner.resolve(),
                               args.sameboy_capture.resolve(), args.output.resolve())
        print(json.dumps(report))
        return 0 if passes(report) else 1
    except (OSError, ValueError, KeyError, TypeError, subprocess.SubprocessError) as error:
        parser.exit(2, f'alignment reference failed: {error}\n')


if __name__ == '__main__':
    raise SystemExit(main())
