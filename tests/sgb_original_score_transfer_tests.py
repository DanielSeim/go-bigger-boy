#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Validate score authoring and run the actual packaged data on the SPC/DSP."""
import argparse
import copy
import importlib.util
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / 'scripts/build_sgb_score_transfer.py'
EXAMPLE = ROOT / 'firmware/sgb/score_example.json'
SPEC = importlib.util.spec_from_file_location('score_transfer', SCRIPT)
score_transfer = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(score_transfer)
RUNNER = None


class ScoreTransferContracts(unittest.TestCase):
    def test_exact_transport(self):
        document = json.loads(EXAMPLE.read_text())
        expected = struct.pack('<HH', 16, 0x07D0) + bytes(sum(document['scores'], []))
        expected += struct.pack('<HH', 0, 0x0200) + bytes(4072)
        self.assertEqual(score_transfer.build(document), expected)
        self.assertEqual(len(expected), 4096)
        relocated = score_transfer.build(document, 0x0800)
        self.assertEqual(relocated[:20], expected[:20])
        self.assertEqual(relocated[20:24], bytes.fromhex('00000008'))
        self.assertEqual(relocated[24:], expected[24:])

    def test_reject_bad_scores_and_entries(self):
        document = json.loads(EXAMPLE.read_text())
        invalid = [None, [], {}, {'scores': []}, {'scores': document['scores'], 'tempo': 16},
                   {'scores': document['scores'][:1]}, {'scores': [[], []]}]
        for note in (0, 64, -1, True, False, 1.0, '4', None):
            changed = copy.deepcopy(document)
            changed['scores'][1][7] = note
            invalid.append(changed)
        for changed in invalid:
            with self.subTest(document=changed), self.assertRaises(ValueError):
                score_transfer.build(changed)
        for entry in (0, 0xFF, 0xFFC0, 0xFFFF, 0x10000, 0x07D0, 0x07DF, True, '0x0200'):
            with self.subTest(entry=entry), self.assertRaises(ValueError):
                score_transfer.build(document, entry)

    def test_cli_validation_and_no_overwrite(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / 'score.bin'
            command = [sys.executable, str(SCRIPT), str(EXAMPLE), '--output', str(output)]
            subprocess.run(command, check=True, capture_output=True)
            expected = output.read_bytes()
            self.assertEqual(expected, score_transfer.build(json.loads(EXAMPLE.read_text())))
            result = subprocess.run(command, capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('File exists', result.stderr)
            self.assertEqual(output.read_bytes(), expected)
            bad = Path(directory) / 'bad.json'
            for content in ('{}', '{"scores": [], "scores": []}', '{'):
                bad.write_text(content)
                target = Path(directory) / 'invalid.bin'
                result = subprocess.run([sys.executable, str(SCRIPT), str(bad), '--output', str(target)],
                                        capture_output=True, text=True)
                self.assertNotEqual(result.returncode, 0)
                self.assertFalse(target.exists(), 'invalid authoring input must create no output')

    def test_real_firmware_plays_packaged_notes(self):
        if RUNNER is None:
            self.skipTest('supply --runner for the real SPC/DSP integration check')
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / 'score.bin'
            subprocess.run([sys.executable, str(SCRIPT), str(EXAMPLE), '--output', str(output)],
                           check=True, capture_output=True)
            subprocess.run([str(RUNNER), '--score-transfer', str(output)], check=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--runner', type=Path)
    args = parser.parse_args()
    RUNNER = args.runner
    unittest.main(argv=[sys.argv[0]])
