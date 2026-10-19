#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Run one deterministic, complete-coverage shard of the TSan CTest suite."""
import argparse
import json
from pathlib import Path
import re
import subprocess


# Rounded execution seconds from successful Actions run 39145185764, job
# 117727475623 (2026-10-18). These only balance work; they never select coverage
# or impose timeouts. New tests automatically join the partition.
ESTIMATED_SECONDS = {
    'gameboy_sgb_original_transfer_contract': 863,
    'gameboy_sgb_resident_score_contract': 759,
    'gameboy_sgb_original_score_transfer': 434,
    'gameboy_sgb_host_contract': 222,
    'gameboy_sgb_replacement_boot': 198,
    'gameboy_cgb_boot': 182,
    'gameboy_snes_spc_replay_cache_contract': 104,
    'gameboy_dmg_boot_audio': 84,
    'gameboy_dmg0_boot': 51,
    'gameboy_mgb_boot': 40,
}
# Relative costs from the split local Release run, scaled to the former
# instrumented contract's total. CI JUnit timings can refine these estimates.
FIRMWARE_CASE_SECONDS = {
    'startup': 60,
    'instruments': 3,
    'sound_native': 33,
    'sound_combined': 52,
    'attributes': 34,
    'modulation_state': 97,
    'bank_and_scores': 313,
    'modulation_effects': 162,
    'unsupported': 216,
}


def select_tests(document):
    if not isinstance(document, dict) or not isinstance(document.get('tests'), list):
        raise ValueError('invalid CTest discovery document')
    selected = []
    names = set()
    for test in document['tests']:
        properties = {p['name']: p['value'] for p in test.get('properties', [])}
        if any('sgb-firmware-extended' in label for label in properties.get('LABELS', [])):
            continue
        name = test.get('name')
        if not isinstance(name, str) or not name or name in names:
            raise ValueError('invalid or duplicate contract')
        # There are currently no dependent fixtures. Fail explicitly rather
        # than silently break their ordering or duplicate them across runners.
        if any(properties.get(key) for key in (
                'DEPENDS', 'FIXTURES_SETUP', 'FIXTURES_REQUIRED', 'FIXTURES_CLEANUP')):
            raise ValueError('cross-test dependencies require explicit shard grouping: ' + name)
        estimate = ESTIMATED_SECONDS.get(name, 10)
        firmware = re.fullmatch(r'gameboy_sgb_original_firmware_sgb2?_(.+)_contract', name)
        if firmware:
            estimate = FIRMWARE_CASE_SECONDS.get(firmware[1], 108)
        names.add(name)
        selected.append(dict(name=name, estimated_seconds=estimate))
    if not selected:
        raise ValueError('no TSan contracts discovered')
    return selected


def partition(tests, count):
    if type(count) is not int or not 1 <= count <= min(16, len(tests)):
        raise ValueError('requires 1..16 nonempty shards')
    if len({test['name'] for test in tests}) != len(tests):
        raise ValueError('duplicate contract')
    shards = [[] for _ in range(count)]
    totals = [0] * count
    for test in sorted(tests, key=lambda t: (-t['estimated_seconds'], t['name'])):
        index = min(range(count), key=lambda i: (totals[i], i))
        shards[index].append(test)
        totals[index] += test['estimated_seconds']
    return [sorted(shard, key=lambda t: t['name']) for shard in shards]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', type=Path, required=True)
    parser.add_argument('--config', default='RelWithDebInfo')
    parser.add_argument('--shard-index', type=int, required=True)
    parser.add_argument('--shard-count', type=int, default=4)
    parser.add_argument('--parallel', type=int, default=2)
    parser.add_argument('--plan-only', action='store_true')
    args = parser.parse_args()
    try:
        if not 0 <= args.shard_index < args.shard_count or not 1 <= args.parallel <= 4:
            raise ValueError('invalid shard or parallelism')
        root = args.build_dir.resolve()
        discovery = subprocess.run(
            ['ctest', '--test-dir', str(root), '-C', args.config, '--show-only=json-v1'],
            check=True, capture_output=True, text=True, timeout=60)
        tests = select_tests(json.loads(discovery.stdout))
        shards = partition(tests, args.shard_count)
        selected = shards[args.shard_index]
        plan = dict(schema='gbb-tsan-shard-v1', shard_index=args.shard_index,
                    shard_count=args.shard_count, config=args.config, total_tests=len(tests),
                    tests=[test['name'] for test in selected],
                    shards=[dict(tests=[test['name'] for test in shard],
                                 estimated_seconds=sum(t['estimated_seconds'] for t in shard))
                            for shard in shards])
        (root / 'tsan-shard.json').write_text(json.dumps(plan, indent=2) + '\n')
        print(json.dumps(plan, indent=2), flush=True)
        if args.plan_only:
            return 0
        pattern = '^(' + '|'.join(re.escape(test['name']) for test in selected) + ')$'
        return subprocess.run([
            'ctest', '--test-dir', str(root), '-C', args.config, '--tests-regex', pattern,
            '--label-exclude', 'sgb-firmware-extended', '--no-tests=error',
            '--output-on-failure', '--parallel', str(args.parallel),
            '--output-junit', str(root / 'tsan-ctest.xml')]).returncode
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        parser.error(str(error))


if __name__ == '__main__':
    raise SystemExit(main())
