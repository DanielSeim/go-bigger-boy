#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Pinned PCM curves, complete protocols and optional paired native snapshots."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
import samesuite_alignment_reference as reference


class Contracts(unittest.TestCase):
    def test_pin_and_curve_fields(self):
        pin = json.loads(reference.PIN.read_text())
        self.assertEqual(len(pin['tests']), 4)
        for entry in pin['tests'].values():
            self.assertRegex(entry['sha256'], r'^[a-f0-9]{64}$')
            self.assertIn(entry['group_variable'], ('channel_trigger_phase', 'nr52_enable_phase'))
        for channel, value in ((1, 8), (2, 128)):
            memory = bytearray(8192)
            for group, start in enumerate((13, 12, 12)):
                memory[group * 16 + start:(group + 1) * 16] = bytes([value]) * (16 - start)
            self.assertEqual(reference.curve(memory, channel)['first_nonzero'], [13, 12, 12])
            memory[0] = 15
            with self.assertRaises(ValueError):
                reference.curve(memory, channel)
        with self.assertRaises(ValueError):
            reference.curve(bytes(48), 1)

    def test_complete_explicit_protocols(self):
        hit = 'CPU_FIRST_PC cycle=123 duration=12 pc=0527 opcode=00 next_pc=0528 sp=fffe\n'
        result = subprocess.CompletedProcess([], 0, 'PASS (Mooneye)\n', hit)
        self.assertTrue(reference.protocol('gbb', result)['passed'])
        result.stdout = '\x03\x05\x08\n\x15"PASS (Mooneye)\n'
        self.assertTrue(reference.protocol('gbb', result)['passed'])
        result.stdout = 'arbitrary text PASS (Mooneye)\n'
        with self.assertRaises(ValueError):
            reference.protocol('gbb', result)
        result.returncode, result.stdout = 1, 'BBBBBB'
        result.stderr = hit + 'FAIL (Mooneye result registers)\n'
        self.assertFalse(reference.protocol('gbb', result)['passed'])
        result.stderr = 'TIMEOUT'
        with self.assertRaises(ValueError):
            reference.protocol('gbb', result)
        data = dict(schema='gbb-sameboy-mooneye-v1', result='pass', model='cgb-e',
                    clocks_8mhz=123, bc=0x0305, de=0x080d, hl=0x1522)
        result = subprocess.CompletedProcess([], 0, json.dumps(data), '')
        self.assertTrue(reference.protocol('sameboy', result)['passed'])
        for value in ([], dict(data, result='timeout'), dict(data, model='cgb-c'),
                      dict(data, clocks_8mhz=True), dict(data, bc=0)):
            result.stdout = json.dumps(value)
            with self.assertRaises(ValueError):
                reference.protocol('sameboy', result)

    def test_incomplete_or_failing_comparison_cannot_pass(self):
        pin = json.loads(reference.PIN.read_text())
        rows = [dict(test=name, match=True, captures={e: dict(passed=True, repeatable=True)
                for e in ('gbb', 'sameboy')}) for name in pin['tests']]
        self.assertTrue(reference.passes(dict(rows=rows)))
        self.assertFalse(reference.passes(dict(rows=rows[:-1])))
        self.assertFalse(reference.passes(dict(rows=[rows[0]] * 4)))
        rows[0]['captures']['gbb']['passed'] = False
        self.assertFalse(reference.passes(dict(rows=rows)))
        rows[0]['captures']['gbb']['passed'] = True
        rows[0]['captures']['sameboy']['repeatable'] = False
        self.assertFalse(reference.passes(dict(rows=rows)))

    def test_wrong_hash_fails_before_execution_or_output_creation(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            for entry in json.loads(reference.PIN.read_text())['tests'].values():
                rom = root / entry['path']
                rom.parent.mkdir(parents=True, exist_ok=True)
                rom.write_bytes(bytes(32768))
            with patch.object(reference.subprocess, 'run') as run:
                with self.assertRaisesRegex(ValueError, 'hash'):
                    reference.run_reference(root, root / 'runner', root / 'adapter', root / 'output')
                run.assert_not_called()
            self.assertFalse((root / 'output').exists())


@unittest.skipUnless(all(os.environ.get(k) for k in
    ('GBB_TEST_RUNNER', 'GBB_SAMEBOY_CGB_CAPTURE', 'GBB_SAMESUITE_ROM_DIR')),
    'set native runner, pinned SameBoy adapter and external SameSuite root')
class NativeContracts(unittest.TestCase):
    def test_complete_repeatable_frozen_curves(self):
        with tempfile.TemporaryDirectory() as tmp:
            report = reference.run_reference(Path(os.environ['GBB_SAMESUITE_ROM_DIR']).resolve(),
                Path(os.environ['GBB_TEST_RUNNER']).resolve(),
                Path(os.environ['GBB_SAMEBOY_CGB_CAPTURE']).resolve(), Path(tmp) / 'results')
            self.assertFalse(report['qualification'])
            self.assertEqual(len(report['rows']), 4)
            self.assertTrue(reference.passes(report))
            for row in report['rows']:
                a, b = (row['captures'][e]['samples'] for e in ('gbb', 'sameboy'))
                self.assertEqual(row['match'], a == b)
                self.assertEqual(row['different_sample_indices'], [i for i in range(48) if a[i] != b[i]])
                self.assertTrue(row['captures']['sameboy']['passed'])
                for capture in row['captures'].values():
                    self.assertTrue(capture['repeatable'])
                    self.assertEqual(len(capture['samples']), 48)
                    self.assertGreaterEqual(capture['snapshot_clocks'], capture['completion_clocks'])
            for suffix in ('align', 'align_cpu'):
                rows = [r for r in report['rows'] if r['test'].endswith('_' + suffix)]
                for emulator in ('gbb', 'sameboy'):
                    self.assertEqual(rows[0]['captures'][emulator]['samples'],
                                     [v >> 4 for v in rows[1]['captures'][emulator]['samples']])


if __name__ == '__main__':
    unittest.main()
