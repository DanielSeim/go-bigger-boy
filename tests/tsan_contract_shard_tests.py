#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check exhaustive partitioning, isolated execution and failure propagation."""
import argparse
import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET

import run_tsan_contract_shard as runner

FIRMWARE_RUNNER = None
BUILD_DIR = None
CONFIG = 'Release'


def contract(name, labels=None, **properties):
    return dict(name=name, command=['probe'], properties=[
        dict(name='LABELS', value=labels or [])] + [
        dict(name=key, value=value) for key, value in properties.items()])


class ShardContracts(unittest.TestCase):
    def test_partition_is_exhaustive_disjoint_and_order_independent(self):
        tests = runner.select_tests(dict(tests=[contract('test.' + str(i)) for i in range(279)]))
        tests += runner.select_tests(dict(tests=[contract(name) for name in runner.ESTIMATED_SECONDS]))
        tests += runner.select_tests(dict(tests=[
            contract('gameboy_sgb_original_firmware_' + model + '_' + case + '_contract')
            for model in ('sgb', 'sgb2') for case in runner.FIRMWARE_CASE_SECONDS]))
        shards = runner.partition(tests, 4)
        self.assertEqual(shards, runner.partition(list(reversed(tests)), 4))
        names = [test['name'] for shard in shards for test in shard]
        self.assertEqual(len(names), len(set(names)))
        self.assertEqual(set(names), {test['name'] for test in tests})
        totals = [sum(test['estimated_seconds'] for test in shard) for shard in shards]
        self.assertLessEqual(max(totals) - min(totals), 10)
        for count in (0, 17, True, len(tests) + 1):
            with self.assertRaises(ValueError):
                runner.partition(tests, count)
        with self.assertRaises(ValueError):
            runner.partition(tests + tests[:1], 4)

    def test_selection_preserves_unlabelled_and_serial_tests(self):
        tests = runner.select_tests(dict(tests=[
            contract('plain'), contract('serial', RUN_SERIAL=True),
            contract('firmware', ['firmware', 'sgb']),
            contract('extended', ['sgb-firmware-extended']),
            contract('extended_suffix', ['sgb-firmware-extended-more']),
        ]))
        self.assertEqual([test['name'] for test in tests], ['plain', 'serial', 'firmware'])
        for document in ({}, dict(tests=[]), dict(tests=[contract('a'), contract('a')]),
                         dict(tests=[contract('')])):
            with self.assertRaises(ValueError):
                runner.select_tests(document)
        for key in ('DEPENDS', 'FIXTURES_SETUP', 'FIXTURES_REQUIRED', 'FIXTURES_CLEANUP'):
            with self.assertRaises(ValueError):
                runner.select_tests(dict(tests=[contract('dependent', **{key: ['other']})]))

    def test_firmware_selection_matches_registered_scenarios_and_rejects_typos(self):
        if FIRMWARE_RUNNER is None:
            self.skipTest('firmware executable not provided')
        result = subprocess.run([str(FIRMWARE_RUNNER), '--list-cases'], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        cases = result.stdout.splitlines()
        self.assertEqual(len(cases), 9)
        self.assertEqual(len(set(cases)), 9)
        discovery = subprocess.run(['ctest', '--test-dir', str(BUILD_DIR or FIRMWARE_RUNNER.parent),
                                    '-C', CONFIG, '--show-only=json-v1'],
                                   capture_output=True, text=True, check=True)
        tests = json.loads(discovery.stdout)['tests']
        split = [test for test in tests if re.fullmatch(
            r'gameboy_sgb_original_firmware_sgb2?_.+_contract', test['name'])]
        if split:
            self.assertEqual(len(split), 18)
            self.assertEqual({tuple(test['command'][1:]) for test in split}, {
                ('--case', case, '--model', model) for case in cases for model in ('sgb', 'sgb2')})
        else:
            self.assertEqual(sum(test['name'] == 'gameboy_sgb_original_firmware_contract'
                                 for test in tests), 1)
        for args in (['--case', 'typo', '--model', 'sgb'],
                     ['--case', cases[0], '--model', 'typo'], ['--case'], ['--unknown']):
            failed = subprocess.run([str(FIRMWARE_RUNNER), *args], capture_output=True, text=True)
            self.assertNotEqual(failed.returncode, 0)

    def test_real_ctest_shards_cover_once_and_propagate_failure(self):
        with tempfile.TemporaryDirectory(prefix='gbb tsan shards ') as directory:
            root = Path(directory)
            build = root / 'build with spaces'
            (root / 'CMakeLists.txt').write_text('''cmake_minimum_required(VERSION 3.20)
project(shard_fixture NONE)
enable_testing()
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(NAME pass.a COMMAND ${Python3_EXECUTABLE} -c "pass")
add_test(NAME pass+a COMMAND ${Python3_EXECUTABLE} -c "pass")
add_test(NAME fail COMMAND ${Python3_EXECUTABLE} -c "raise SystemExit(7)")
add_test(NAME excluded COMMAND ${Python3_EXECUTABLE} -c "raise SystemExit(8)")
set_tests_properties(excluded PROPERTIES LABELS sgb-firmware-extended)
set_tests_properties(pass.a PROPERTIES RUN_SERIAL TRUE)
''')
            subprocess.run(['cmake', '-S', str(root), '-B', str(build)],
                           capture_output=True, text=True, check=True)
            command = [sys.executable, str(Path(runner.__file__).resolve()),
                       '--build-dir', str(build), '--shard-count', '3']
            plan = subprocess.run(command + ['--shard-index', '0', '--plan-only'],
                                  capture_output=True, text=True, check=True)
            shards = json.loads(plan.stdout)['shards']
            self.assertFalse((build / 'tsan-ctest.xml').exists())
            seen = []
            for index, shard in enumerate(shards):
                result = subprocess.run(command + ['--shard-index', str(index)], capture_output=True, text=True)
                self.assertEqual(result.returncode != 0, shard['tests'] == ['fail'], result.stdout + result.stderr)
                report = ET.parse(build / 'tsan-ctest.xml')
                names = [test.attrib['name'] for test in report.iter('testcase')]
                self.assertEqual(names, shard['tests'])
                self.assertEqual(bool(list(report.iter('failure'))), shard['tests'] == ['fail'])
                seen.extend(names)
            self.assertCountEqual(seen, ['pass.a', 'pass+a', 'fail'])
            self.assertNotIn('excluded', seen)
            for index in (-1, 3):
                result = subprocess.run(command + ['--shard-index', str(index)], capture_output=True, text=True)
                self.assertNotEqual(result.returncode, 0)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--firmware-runner', type=Path)
    parser.add_argument('--build-dir', type=Path)
    parser.add_argument('--config', default='Release')
    args, remaining = parser.parse_known_args()
    FIRMWARE_RUNNER = args.firmware_runner.resolve() if args.firmware_runner else None
    BUILD_DIR = args.build_dir
    CONFIG = args.config
    unittest.main(argv=[sys.argv[0], *remaining])
