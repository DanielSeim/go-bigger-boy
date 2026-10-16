#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build pinned external Mooneye timer tests and compare explicit result protocols."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

import cgb_frame_anchor_probe as anchor

PIN = Path(__file__).resolve().parents[1] / 'tests/fixtures/external-reference/mooneye-timer.json'
MODELS = ('cgb0', 'cgb-c', 'cgb-e')


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def checkout(path, commit):
    def git(*args):
        return subprocess.check_output(['git', '-C', str(path), *args], text=True, timeout=20).strip()
    if git('rev-parse', 'HEAD') != commit or git('status', '--porcelain', '--untracked-files=no'):
        raise ValueError('external checkout must match the clean pinned revision')


def passed(emulator, result, model):
    if result.returncode != 0:
        return False
    if emulator == 'gbb':
        return result.stdout.strip() == 'PASS (Mooneye)'
    try:
        data = json.loads(result.stdout)
    except (ValueError, TypeError):
        return False
    required = dict(schema='gbb-sameboy-mooneye-v1', result='pass', model=model,
                    bc=0x0305, de=0x080d, hl=0x1522)
    return isinstance(data, dict) and all(data.get(k) == v for k, v in required.items())


def run_reference(source, assembler_source, assembler_build, runner, adapter, output):
    pin = json.loads(PIN.read_text())
    checkout(source, pin['commit'])
    checkout(assembler_source, pin['assembler_commit'])
    if any(output == path or path in output.parents for path in (source, assembler_source)):
        raise ValueError('output must be outside source checkouts')
    output.mkdir(parents=True, exist_ok=False)
    assembler_build = assembler_build or output / 'tools'
    if any(assembler_build == path or path in assembler_build.parents for path in (source, assembler_source)):
        raise ValueError('assembler build must be outside source checkouts')
    commands = []
    def build(command):
        commands.append(command)
        result = subprocess.run(command, capture_output=True, text=True, timeout=180)
        with (output / 'build.log').open('a') as log:
            log.write(result.stdout + result.stderr)
        if result.returncode:
            raise ValueError('external build failed; inspect build.log')
    build(['cmake', '-S', str(assembler_source), '-B', str(assembler_build), '-DCMAKE_BUILD_TYPE=Release'])
    build(['cmake', '--build', str(assembler_build), '--target', 'wla-gb', 'wlalink', '-j2'])
    wla = assembler_build / 'binaries/wla-gb'
    link = assembler_build / 'binaries/wlalink'
    roms = {name: output / 'roms/acceptance/timer' / (name + '.gb') for name in pin['tests']}
    build(['make', '-C', str(source), 'BUILD_PATH=' + str(output / 'roms'),
           'WLA=' + str(wla), 'WLALINK=' + str(link), *map(str, roms.values())])
    for name, rom in roms.items():
        if digest(rom) != pin['tests'][name]:
            raise ValueError('external ROM hash does not match pin')
    boots = {}
    for model in MODELS:
        boots[model] = output / ('owned-' + model + '-boot.bin')
        boots[model].write_bytes(anchor.owned_boot(model))
    report = dict(schema='gbb-mooneye-timer-reference-v1', qualification=False, pin=pin,
                  tools=dict(wla_sha256=digest(wla), wlalink_sha256=digest(link)),
                  boot_sha256={model: digest(boot) for model, boot in boots.items()},
                  runner_sha256=digest(runner), adapter_sha256=digest(adapter),
                  build_commands=commands, rows=[])
    for model in MODELS:
        boot = boots[model]
        for name, rom in roms.items():
            captures = {}
            for emulator in ('gbb', 'sameboy'):
                command = ([str(runner), str(rom), '--model', model, '--dmg-boot',
                            '--protocol', 'mooneye', '--max-cycles', '20000000'] if emulator == 'gbb'
                           else [str(adapter), '--mooneye', str(rom), str(boot), model, '200000000'])
                takes = []
                for _ in range(2):
                    result = subprocess.run(command, capture_output=True, text=True, timeout=30)
                    takes.append(dict(exit=result.returncode, passed=passed(emulator, result, model),
                                      stdout=result.stdout, stderr=result.stderr))
                captures[emulator] = dict(takes[0], repeatable=takes[0] == takes[1], command=command)
            report['rows'].append(dict(model=model, test=name, rom_sha256=pin['tests'][name], captures=captures))
    (output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    return report


def passes(report):
    try:
        expected = {(model, name) for model in MODELS for name in json.loads(PIN.read_text())['tests']}
        rows = report['rows']
        return len(rows) == len(expected) and {(r['model'], r['test']) for r in rows} == expected and all(
            set(row['captures']) == {'gbb', 'sameboy'} and all(c['passed'] is True and c['repeatable'] is True
            for c in row['captures'].values()) for row in rows)
    except (KeyError, TypeError):
        return False


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--assembler-source', type=Path, required=True)
    parser.add_argument('--assembler-build', type=Path)
    parser.add_argument('--runner', type=Path, required=True)
    parser.add_argument('--sameboy-capture', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    try:
        report = run_reference(args.source.resolve(), args.assembler_source.resolve(),
            args.assembler_build.resolve() if args.assembler_build else None,
            args.runner.resolve(), args.sameboy_capture.resolve(), args.output.resolve())
        print(json.dumps(report))
        return 0 if passes(report) else 1
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        parser.exit(2, f'external Mooneye timer validation failed: {error}\n')


if __name__ == '__main__':
    raise SystemExit(main())
