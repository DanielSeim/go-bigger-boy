#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Pinned frequency observations, fail-closed reports and optional native parity."""
import contextlib
import io
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
import samesuite_frequency_reference as probe


class Contracts(unittest.TestCase):
    def test_pin_and_packed_pcm_fields(self):
        pin = json.loads(probe.PIN.read_text())
        self.assertEqual(len(pin['tests']), 1)
        self.assertEqual(pin['cpu_speed'], 'normal_then_double')
        entry = next(iter(pin['tests'].values()))
        self.assertRegex(entry['sha256'], r'^[a-f0-9]{64}$')
        self.assertEqual(entry['sample_delay_nops'], [list(range(22, 30)), list(range(50, 58))])
        memory = bytearray(8192)
        memory[:4] = bytes((0, 15, 240, 255))
        result = probe.curve(memory, entry)
        self.assertEqual(result['observations'][:4], [[0, 0], [0, 15], [15, 0], [15, 15]])
        self.assertEqual(len(result['samples']), 16)
        memory[0] = 8
        with self.assertRaises(ValueError):
            probe.curve(memory, entry)
        with self.assertRaises(ValueError):
            probe.curve(bytes(16), entry)

    def test_missing_hash_rejects_before_execution(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            entry = next(iter(json.loads(probe.PIN.read_text())['tests'].values()))
            rom = root / entry['path']
            rom.parent.mkdir(parents=True)
            rom.write_bytes(bytes(32768))
            with patch.object(probe.reference.subprocess, 'run') as run:
                with self.assertRaisesRegex(ValueError, 'hash'):
                    probe.run_reference(root, root / 'runner', root / 'adapter', root / 'output')
                run.assert_not_called()
            self.assertFalse((root / 'output').exists())

    def test_wrong_or_incomplete_comparison_cannot_pass(self):
        name = next(iter(json.loads(probe.PIN.read_text())['tests']))
        capture = dict(passed=True, repeatable=True)
        row = dict(test=name, match=True, captures=dict(gbb=dict(capture), sameboy=dict(capture)))
        report = dict(rows=[row])
        self.assertTrue(probe.reference.passes(report, probe.PIN))
        self.assertFalse(probe.reference.passes(report))  # Alignment pin has four different cases.
        for field in ('passed', 'repeatable'):
            row['captures']['gbb'][field] = False
            self.assertFalse(probe.reference.passes(report, probe.PIN))
            row['captures']['gbb'][field] = True
        row['match'] = False
        self.assertFalse(probe.reference.passes(report, probe.PIN))
        self.assertFalse(probe.reference.passes(dict(rows=[]), probe.PIN))

    def test_cli_failure_stays_a_failure(self):
        with patch.object(probe, 'run_reference', return_value=dict(rows=[])), \
                patch.object(sys, 'argv', ['probe', '--rom-root', '/unused', '--runner', '/unused',
                    '--sameboy-capture', '/unused', '--output', '/unused']), \
                contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(probe.main(), 1)


@unittest.skipUnless(all(os.environ.get(k) for k in
    ('GBB_TEST_RUNNER', 'GBB_SAMEBOY_CGB_CAPTURE', 'GBB_SAMESUITE_ROM_DIR')),
    'set native runner, pinned SameBoy adapter and external SameSuite root')
class NativeContracts(unittest.TestCase):
    def test_complete_repeatable_frozen_curve(self):
        with tempfile.TemporaryDirectory() as tmp:
            report = probe.run_reference(Path(os.environ['GBB_SAMESUITE_ROM_DIR']).resolve(),
                Path(os.environ['GBB_TEST_RUNNER']).resolve(),
                Path(os.environ['GBB_SAMEBOY_CGB_CAPTURE']).resolve(), Path(tmp) / 'results')
            self.assertFalse(report['qualification'])
            self.assertEqual(report['cpu_speed'], 'normal_then_double')
            self.assertTrue(probe.reference.passes(report, probe.PIN))
            self.assertEqual(report['rows'][0]['different_sample_indices'], [])
            for capture in report['rows'][0]['captures'].values():
                self.assertEqual(len(capture['observations']), 16)
                self.assertGreaterEqual(capture['snapshot_clocks'], capture['completion_clocks'])


if __name__ == '__main__':
    unittest.main()
