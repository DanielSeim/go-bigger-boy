#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Compare fast CGB startup registers with an external boot, before cartridge execution."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

REGISTERS = ('af', 'bc', 'de', 'hl', 'sp', 'pc')
MODELS = ('cgb0', 'cgb-c', 'cgb-e')


def probe_rom(flag, logo=None):
    if flag not in (0, 0x80, 0xc0) or (logo is not None and len(logo) != 48):
        raise ValueError('invalid cartridge flag or header logo length')
    rom = bytearray(32768)
    rom[0x100:0x103] = bytes.fromhex('c3 50 01')
    if logo is not None:
        rom[0x104:0x134] = logo  # Optional private input, never a bundled asset.
    rom[0x143] = flag
    # An independent, explicit result protocol for the GBB subprocess.
    program = bytes.fromhex('01 05 03 11 0d 08 21 22 15 40 18 fe')
    rom[0x150:0x150 + len(program)] = program
    checksum = 0
    for value in rom[0x134:0x14d]:
        checksum = (checksum - value - 1) & 255
    rom[0x14d] = checksum
    rom[0x14e:0x150] = (sum(rom) & 65535).to_bytes(2, 'big')
    return bytes(rom)


def gbb_registers(result):
    if result.returncode or result.stdout.strip() != 'PASS (Mooneye)':
        raise ValueError('GBB did not complete the authored cartridge protocol')
    lines = [line for line in result.stderr.splitlines() if line.startswith('CPU_FIRST_PC ')]
    if len(lines) != 1:
        raise ValueError('missing or ambiguous initial-PC witness')
    fields = dict(part.split('=', 1) for part in lines[0].split()[1:])
    if any(fields.get(k) != v for k, v in dict(
            transition='initial', cycle='0', duration='0', pc='none', opcode='none',
            next_pc='0100', normalized_8mhz='0').items()):
        raise ValueError('GBB witness was not taken before cartridge execution')
    return {key: int(fields['next_pc' if key == 'pc' else key], 16) for key in REGISTERS}


def reference_registers(result, model):
    if result.returncode:
        raise ValueError('reference boot failed or timed out')
    data = json.loads(result.stdout)
    required = dict(schema='gbb-sameboy-cgb-boot-registers-v1', model=model,
                    result='handoff', anchor='boot_unmapped_before_cartridge_entry')
    if (not isinstance(data, dict) or data.get('qualification') is not False or
            any(data.get(k) != v for k, v in required.items()) or
            type(data.get('clocks_8mhz')) is not int or data['clocks_8mhz'] <= 0 or
            any(type(data.get(k)) is not int or not 0 <= data[k] <= 65535 for k in REGISTERS)
            or data.get('pc') != 0x100):
        raise ValueError('reference did not report the requested boot handoff')
    return {key: data[key] for key in REGISTERS}


def run_probe(runner, adapter, boot, model, output, logo=None):
    if model not in MODELS:
        raise ValueError('unsupported CGB revision')
    output.mkdir(parents=True, exist_ok=False)
    fingerprint = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
    before = {name: fingerprint(path) for name, path in
              (('runner', runner), ('adapter', adapter), ('boot', boot))}
    rows, commands = [], []
    for flag in (0, 0x80, 0xc0):
        rom = output / f'header-{flag:02x}.gbc'
        data = probe_rom(flag, logo)
        rom.write_bytes(data)
        takes = []
        for _ in range(2):
            a = [str(runner), str(rom), '--model', model, '--protocol', 'mooneye',
                 '--max-cycles', '1000000', '--trace-first-pc', '0x100']
            b = [str(adapter), '--boot-registers', str(rom), str(boot), model, '200000000']
            commands.extend((a, b))
            gbb = gbb_registers(subprocess.run(a, capture_output=True, text=True, timeout=30))
            reference = reference_registers(
                subprocess.run(b, capture_output=True, text=True, timeout=30), model)
            takes.append(dict(gbb=gbb, reference=reference))
        expected = dict(af=0x1180, bc=0, de=0xff56 if flag else 8,
                        hl=0x0d if flag else 0x7c, sp=0xfffe, pc=0x100)
        rows.append(dict(cgb_flag=flag, rom_sha256=hashlib.sha256(data).hexdigest(),
                         captures=takes[0], repeatable=takes[0] == takes[1],
                         match=takes[0]['gbb'] == takes[0]['reference'] == expected))
    if before != {name: fingerprint(path) for name, path in
                  (('runner', runner), ('adapter', adapter), ('boot', boot))}:
        raise ValueError('probe inputs changed during execution')
    report = dict(schema='gbb-cgb-boot-register-probe-v1', qualification=False,
                  model=model, fingerprints=before, commands=commands, rows=rows,
                  scope='CPU register handoff only; no peripheral phase or screenshot qualification')
    (output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--runner', type=Path, required=True)
    parser.add_argument('--sameboy-capture', type=Path, required=True)
    parser.add_argument('--boot-rom', type=Path, required=True)
    parser.add_argument('--model', choices=MODELS, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--logo-source', type=Path,
                        help='optional private cartridge providing header logo for original boot validation')
    args = parser.parse_args()
    try:
        logo = None
        if args.logo_source:
            with args.logo_source.open('rb') as source:
                source.seek(0x104)
                logo = source.read(48)
        report = run_probe(args.runner.resolve(), args.sameboy_capture.resolve(),
                           args.boot_rom.resolve(), args.model, args.output.resolve(), logo)
        print(json.dumps(report))
        return 0 if all(r['match'] and r['repeatable'] for r in report['rows']) else 1
    except (OSError, ValueError, KeyError, subprocess.SubprocessError) as error:
        parser.exit(2, f'boot-register probe failed: {error}\n')


if __name__ == '__main__':
    raise SystemExit(main())
