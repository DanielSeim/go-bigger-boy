#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Guard complete CI coverage, public-input boundaries and CTest execution."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET

import run_sgb_firmware_shard as runner


def diagnostic(name, timeout=10, labels=None):
    return dict(name=name, command=['/build/gameboy_probe'], properties=[
        dict(name='LABELS', value=labels or [runner.LABEL]),
        dict(name='TIMEOUT', value=timeout)])


class ShardContracts(unittest.TestCase):
    def test_partition_retains_every_test_once_and_is_order_independent(self):
        tests = runner.select_tests(dict(tests=[diagnostic(str(i), i+1) for i in range(63)]))
        shards = runner.partition(tests, 8)
        self.assertTrue(all(shards))
        self.assertEqual(shards, runner.partition(list(reversed(tests)), 8))
        names = [test['name'] for shard in shards for test in shard]
        self.assertEqual(len(names), len(set(names)))
        self.assertEqual(set(names), {test['name'] for test in tests})
        for count in (0, 33, 64, True):
            with self.assertRaises(ValueError):
                runner.partition(tests, count)
        with self.assertRaises(ValueError):
            runner.partition(tests+tests[:1], 8)

    def test_selection_excludes_unlabelled_and_rejects_private_or_invalid_tests(self):
        public = diagnostic('public')
        unlabelled = diagnostic('ordinary', labels=['ordinary'])
        self.assertEqual([t['name'] for t in runner.select_tests(dict(tests=[public, unlabelled]))], ['public'])
        invalid = [dict(tests=[]), dict(tests=[public, public]), {},
                   dict(tests=[dict(public, command=[])]),
                   dict(tests=[dict(public, name='')])]
        invalid += [dict(tests=[diagnostic('private', labels=[runner.LABEL, label])])
                    for label in ('local', 'private-reference')]
        invalid += [dict(tests=[diagnostic('bad', timeout=value)])
                    for value in (0, -1, 7201, True, '600', float('nan'), float('inf'))]
        for document in invalid:
            with self.subTest(document=document), self.assertRaises(ValueError):
                runner.select_tests(document)

    def test_targets_include_every_probe_and_reject_missing_targets(self):
        root = Path.cwd()/'build with spaces'
        tests = [dict(name='multi', command=['/usr/bin/python3', 'check.py', '--probe',
                     str(root/'gameboy_first'), '--other', str(root/'gameboy_second.exe')])]
        self.assertEqual(runner.targets_for(tests, root), ['gameboy_first', 'gameboy_second'])
        for command in ([str(root.parent/'elsewhere'/'gameboy_probe')], ['gameboy_probe'], [str(root/'unknown')]):
            with self.assertRaises(ValueError):
                runner.targets_for([dict(name='missing', command=command)], root)

    def test_real_ctest_selection_build_and_failure_propagation(self):
        with tempfile.TemporaryDirectory(prefix='gbb shard with spaces ') as directory:
            root = Path(directory)
            build = root/'build'
            (root/'probe.cpp').write_text('#include <cstdlib>\nint main(int argc, char** argv) { return argc > 1 ? std::atoi(argv[1]) : 0; }\n')
            (root/'check.py').write_text('import subprocess, sys\nsys.exit(subprocess.run(sys.argv[1:]).returncode)\n')
            (root/'CMakeLists.txt').write_text('''cmake_minimum_required(VERSION 3.20)
project(shard_fixture LANGUAGES CXX)
enable_testing()
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_executable(gameboy_fixture probe.cpp)
set_target_properties(gameboy_fixture PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}" RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}")
add_test(NAME matrix.a COMMAND ${Python3_EXECUTABLE} "${CMAKE_SOURCE_DIR}/check.py" $<TARGET_FILE:gameboy_fixture> 0)
add_test(NAME matrix+b COMMAND ${Python3_EXECUTABLE} "${CMAKE_SOURCE_DIR}/check.py" $<TARGET_FILE:gameboy_fixture> 0)
add_test(NAME matrix_fail COMMAND ${Python3_EXECUTABLE} "${CMAKE_SOURCE_DIR}/check.py" $<TARGET_FILE:gameboy_fixture> 7)
set_tests_properties(matrix.a matrix+b matrix_fail PROPERTIES LABELS sgb-firmware-extended TIMEOUT 10)
add_test(NAME ordinary_failure COMMAND gameboy_fixture 9)
''')
            configure = subprocess.run(['cmake', '-S', str(root), '-B', str(build), '-DCMAKE_BUILD_TYPE=Release'], capture_output=True, text=True)
            self.assertEqual(configure.returncode, 0, configure.stdout+configure.stderr)
            command = [sys.executable, str(Path(runner.__file__).resolve()), '--build-dir', str(build), '--shard-count', '3']
            plan = subprocess.run(command+['--shard-index', '0', '--plan-only'], capture_output=True, text=True)
            self.assertEqual(plan.returncode, 0, plan.stdout+plan.stderr)
            shards = json.loads(plan.stdout)['shards']
            self.assertFalse((build/'sgb-firmware-ctest.xml').exists())
            for index, shard in enumerate(shards):
                result = subprocess.run(command+['--shard-index', str(index)], capture_output=True, text=True)
                fails = shard['tests'] == ['matrix_fail']
                self.assertEqual(result.returncode != 0, fails, result.stdout+result.stderr)
                report = ET.parse(build/'sgb-firmware-ctest.xml')
                self.assertEqual([test.attrib['name'] for test in report.iter('testcase')], shard['tests'])
                self.assertEqual(bool(list(report.iter('failure'))), fails)


if __name__ == '__main__':
    unittest.main()
