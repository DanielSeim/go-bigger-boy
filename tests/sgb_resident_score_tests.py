#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Validate original bank authoring and execute the actual SPC/DSP renderer."""
import argparse
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
from build_sgb_resident_score import build
from decode_sgb_score import decode

EXAMPLE = ROOT / 'firmware/sgb/resident_score_example.json'
RUNNER = None


class ResidentScoreContracts(unittest.TestCase):
    def test_exact_bank_and_oracle(self):
        payload = build(json.loads(EXAMPLE.read_text()))
        self.assertEqual(len(payload), 4096)
        size, address = struct.unpack_from('<HH', payload)
        self.assertEqual((size, address), (43, 0x2B00))
        self.assertEqual(payload[4:9], b'GBS1\x2b')
        self.assertEqual(payload[36:47], bytes.fromhex('088c089808c808c9089300'))
        self.assertEqual(struct.unpack_from('<HH', payload, 4+size), (0, 0x0400))
        self.assertEqual(payload[8+size:], bytes(4096-8-size))
        events = decode(payload[4:4+size], 0x2B08)['patterns'][0]['tracks'][0]['events']
        self.assertEqual([e['kind'] for e in events], ['note', 'note', 'tie', 'rest', 'note'])
        self.assertEqual([e['tick'] for e in events], [0, 8, 16, 24, 32])

    def test_strict_schema_and_bounds(self):
        invalid = [None, [], {}, {'events': []}, {'events': [], 'tempo': 4}]
        for event in ({}, {'note': True, 'ticks': 1}, {'note': 32, 'ticks': 1},
                      {'note': -1, 'ticks': 1}, {'note': 1.0, 'ticks': 1},
                      {'note': 1, 'ticks': True}, {'note': 1, 'ticks': 0},
                      {'note': 1, 'ticks': 128}, {'tie': True, 'ticks': 4},
                      {'rest': False, 'ticks': 4}, {'note': 1, 'ticks': 4, 'rest': True}):
            invalid.append({'events': [event]})
        invalid.append({'events': [{'rest': True, 'ticks': 1}, {'tie': True, 'ticks': 1}]})
        invalid.append({'events': [{'note': 0, 'ticks': 1}] * 112})
        for document in invalid:
            with self.subTest(document=document), self.assertRaises(ValueError):
                build(document)
        payload = build({'events': [{'note': 31, 'ticks': 127}] * 111})
        self.assertEqual(struct.unpack_from('<H', payload)[0], 255)

    def test_cli_no_overwrite_or_partial_output(self):
        script = ROOT / 'scripts/build_sgb_resident_score.py'
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'score.bin'
            command = [sys.executable, str(script), str(EXAMPLE), '--output', str(path)]
            subprocess.run(command, check=True, capture_output=True)
            expected = path.read_bytes()
            self.assertEqual(subprocess.run(command, capture_output=True).returncode, 2)
            self.assertEqual(path.read_bytes(), expected)
            bad = Path(directory) / 'bad.json'
            for text in ('{}', '{', '{"events": [], "events": []}'):
                bad.write_text(text)
                target = Path(directory) / 'bad.bin'
                result = subprocess.run([sys.executable, str(script), str(bad), '--output', str(target)], capture_output=True)
                self.assertEqual(result.returncode, 2)
                self.assertFalse(target.exists())

    def test_real_uploaded_score(self):
        if RUNNER is None:
            self.skipTest('supply --runner for SPC/DSP integration')
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'score.bin'
            path.write_bytes(build(json.loads(EXAMPLE.read_text())))
            subprocess.run([str(RUNNER), '--resident-score', str(path)], check=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--runner', type=Path)
    RUNNER = parser.parse_args().runner
    unittest.main(argv=[sys.argv[0]])
