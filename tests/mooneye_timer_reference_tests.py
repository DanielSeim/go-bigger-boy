#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Offline provenance and fail-closed protocol contracts for external timer tests."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
import mooneye_timer_reference as reference


class Contracts(unittest.TestCase):
    def test_pin_is_complete(self):
        pin = json.loads(reference.PIN.read_text())
        for key in ('commit', 'assembler_commit', 'sameboy_commit'):
            self.assertRegex(pin[key], r'^[0-9a-f]{40}$')
        self.assertEqual(len(pin['tests']), 5)
        for digest in pin['tests'].values():
            self.assertRegex(digest, r'^[0-9a-f]{64}$')

    def test_matching_owned_boot_per_model(self):
        import hashlib
        self.assertEqual(reference.anchor.owned_boot('cgb-e'), reference.anchor.owned_boot('cgb-c'))
        self.assertNotEqual(reference.anchor.owned_boot('cgb0'), reference.anchor.owned_boot('cgb-e'))
        self.assertEqual(hashlib.sha256(reference.anchor.owned_boot('cgb0')).hexdigest(),
                         '9120799ac51cabffc521f126cde6789c49f885984f1d7562814eb2bd02273ea6')
        with self.assertRaises(ValueError):
            reference.anchor.owned_boot('dmg')

    def test_dirty_or_wrong_checkout_rejected(self):
        for responses in (['wrong'], ['pinned', ' M acceptance/timer/tima_reload.s']):
            with patch.object(reference.subprocess, 'check_output', side_effect=responses):
                with self.assertRaisesRegex(ValueError, 'clean pinned'):
                    reference.checkout(Path('/unused'), 'pinned')

    def test_success_requires_exact_protocol(self):
        result = subprocess.CompletedProcess([], 0, 'PASS (Mooneye)\n', '')
        self.assertTrue(reference.passed('gbb', result, 'cgb-e'))
        for stdout in ('', 'Captured frame 60', 'PASS (Mooneye)\nFAIL'):
            result.stdout = stdout
            self.assertFalse(reference.passed('gbb', result, 'cgb-e'))
        data = dict(schema='gbb-sameboy-mooneye-v1', result='pass', model='cgb-e',
                    bc=0x0305, de=0x080d, hl=0x1522)
        result.stdout = json.dumps(data)
        self.assertTrue(reference.passed('sameboy', result, 'cgb-e'))
        for key, value in (('model', 'cgb0'), ('result', 'timeout'), ('hl', 0)):
            result.stdout = json.dumps(dict(data, **{key: value}))
            self.assertFalse(reference.passed('sameboy', result, 'cgb-e'))
        result.stdout = json.dumps(data)
        result.returncode = 1
        self.assertFalse(reference.passed('sameboy', result, 'cgb-e'))

    def test_all_models_and_tests_required(self):
        rows = [dict(model=m, test=t, captures={name: dict(passed=True, repeatable=True)
                for name in ('gbb', 'sameboy')})
                for m in reference.MODELS for t in json.loads(reference.PIN.read_text())['tests']]
        self.assertTrue(reference.passes(dict(rows=rows)))
        self.assertFalse(reference.passes(dict(rows=rows[:-1])))
        self.assertFalse(reference.passes(dict(rows=[rows[0]] * 15)))
        rows[0]['captures'].pop('sameboy')
        self.assertFalse(reference.passes(dict(rows=rows)))

    def test_wrong_rom_hash_fails_before_execution(self):
        pin = json.loads(reference.PIN.read_text())
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            source = root / 'source'
            assembler = root / 'assembler'
            output = root / 'output'
            def build(command, **kwargs):
                if command[0] == 'make':
                    for name in pin['tests']:
                        rom = output / 'roms/acceptance/timer' / (name + '.gb')
                        rom.parent.mkdir(parents=True, exist_ok=True)
                        rom.write_bytes(bytes(32768))
                return subprocess.CompletedProcess(command, 0, '', '')
            with patch.object(reference, 'checkout'), patch.object(reference.subprocess, 'run', side_effect=build) as run:
                with self.assertRaisesRegex(ValueError, 'ROM hash'):
                    reference.run_reference(source, assembler, None, root / 'runner', root / 'adapter', output)
                self.assertEqual(run.call_count, 3)
            self.assertFalse((output / 'report.json').exists())


if __name__ == '__main__':
    unittest.main()
