#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check packed-score scheduling reports and rejection of noncanonical uploads."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
from build_sgb_resident_score import build, transport
from inspect_sgb_resident_score import inspect


def example(name):
    return build(json.loads((ROOT / f'firmware/sgb/resident_{name}_example.json').read_text()))


class InspectionContracts(unittest.TestCase):
    def test_all_existing_formats(self):
        for number, name in enumerate(('score', 'controls', 'tracks', 'phrases', 'repeats', 'transpose'), 1):
            with self.subTest(name=name):
                result = inspect(example(name))
                self.assertEqual(result['format'], f'GBS{number}')
                self.assertEqual(result['mailbox_version'], 4 + number)
                self.assertFalse(result['qualified'])
                self.assertFalse(result['playback'])
                self.assertEqual(result['nominal_microseconds'], result['total_ticks'] * 16000)
                self.assertEqual(len(result['upload_sha256']), 64)
                voices = [t['dsp_voice'] for t in result['timeline'][0]['tracks']]
                self.assertEqual(voices, [4] if number <= 2 else [4, 3])

    def test_phrase_barriers_and_defaults(self):
        result = inspect(example('phrases'))
        timeline = result['timeline']
        self.assertEqual([(p['start_tick'], p['end_tick']) for p in timeline], [(0, 8), (8, 20), (20, 28)])
        self.assertEqual([[t['end_tick'] for t in p['tracks']] for p in timeline], [[4, 8], [16, 20], [28, 28]])
        first = timeline[1]['tracks'][0]['events'][0]
        self.assertEqual(first['tick'], 8)
        self.assertEqual(first['controls'], {'instrument': 1, 'pan': 10, 'volume': 80, 'transpose': 0})
        self.assertIsNone(timeline[1]['tracks'][1]['events'][0]['effective_note'])

    def test_repeats_transpose_and_held_ties(self):
        result = inspect(example('transpose'))
        self.assertEqual(result['total_ticks'], 40)
        self.assertEqual(result['nominal_microseconds'], 640000)
        self.assertEqual([(p['play'], p['pattern'], p['start_tick'], p['end_tick'])
                          for p in result['timeline']], [(1, 0, 0, 16), (1, 1, 16, 20), (2, 0, 20, 36), (2, 1, 36, 40)])
        for index in (0, 2):
            tracks = result['timeline'][index]['tracks']
            notes = [e for e in tracks[0]['events'] if 'effective_note' in e]
            self.assertEqual([e['effective_note'] for e in notes], [12, 12, 24, 0])
            self.assertEqual(notes[1]['kind'], 'tie')
            self.assertEqual(notes[1]['controls']['transpose'], 12)
            self.assertEqual(tracks[1]['events'][-1]['effective_note'], 12)
        for index in (1, 3):
            self.assertEqual(result['timeline'][index]['tracks'][0]['events'][3]['effective_note'], 12)

    def test_zero_time_patterns_and_maximum_finite_expansion(self):
        controls = [[{'pan': 0}], [{'pan': 20}]]
        payload = build({'format': 'GBS6', 'plays': 4, 'patterns': [controls] * 4})
        result = inspect(payload)
        self.assertEqual(len(result['timeline']), 16)
        self.assertEqual(result['total_ticks'], 0)
        self.assertTrue(all(p['start_tick'] == p['end_tick'] == 0 for p in result['timeline']))
        note = [{'note': 31, 'ticks': 127}]
        result = inspect(build({'events': note * 111}))
        self.assertEqual(result['bank_bytes'], 255)
        self.assertEqual(result['total_ticks'], 14097)

    def test_corrupt_transport_header_pointers_and_streams(self):
        payload = example('transpose')
        mutations = {0: 0, 2: 1, 4: ord('X'), 8: 115, 9: 0, 10: 5,
                     11: 1, 12: 0, 37: 0, 52: 0xEA, 4095: 1}
        # The stream mutation below makes the first track start with a tie.
        first = payload[4 + 32]
        mutations[4 + first + 7] = 0xC8
        for offset, value in mutations.items():
            changed = bytearray(payload)
            changed[offset] = value
            with self.subTest(offset=offset), self.assertRaises(ValueError):
                inspect(bytes(changed))
        for invalid in (None, bytearray(payload), payload[:-1], payload + b'\0'):
            with self.assertRaises(ValueError):
                inspect(invalid)
        # Reject trailing blocks, jump changes and missing initial duration.
        score = example('score')
        size = score[0]
        for offset, value in ((4 + size, 1), (6 + size, 1), (4 + 32, 0x8C)):
            changed = bytearray(score)
            changed[offset] = value
            with self.assertRaises(ValueError):
                inspect(bytes(changed))

    def test_cli_bounded_read_json_and_errors(self):
        script = ROOT / 'scripts/inspect_sgb_resident_score.py'
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'upload.bin'
            command = [sys.executable, str(script), str(path)]
            payload = example('transpose')
            path.write_bytes(payload)
            result = subprocess.run(command, check=True, capture_output=True, text=True)
            self.assertEqual(json.loads(result.stdout), inspect(payload))
            for bad in (payload[:-1], payload + b'\0' * 10000, bytes(4096)):
                path.write_bytes(bad)
                result = subprocess.run(command, capture_output=True)
                self.assertEqual(result.returncode, 2)
                self.assertEqual(result.stdout, b'')
            path.unlink()
            self.assertEqual(subprocess.run(command, capture_output=True).returncode, 2)

    def test_inherited_state_and_noncanonical_encodings(self):
        payload = example('score')
        bank = bytearray(payload[4:4 + payload[0]])
        # General syntax allows reusing duration, but the packer writes it for
        # every event. Valid syntax alone must not bypass canonical validation.
        del bank[34]
        bank[4] -= 1
        with self.assertRaisesRegex(ValueError, 'canonical'):
            inspect(transport(bank))
        payload = example('transpose')
        bank = bytearray(payload[4:4 + payload[0]])
        second_pattern = bank[10]
        second_track = bank[second_pattern]
        # A held note from the previous phrase must not authorize this tie.
        bank[second_track + 7] = 0xC8
        with self.assertRaisesRegex(ValueError, 'tie without held note'):
            inspect(transport(bank))
        # Optional raw articulation is outside the resident grammar.
        payload = example('score')
        bank = bytearray(payload[4:4 + payload[0]])
        bank.insert(33, 0x10)
        bank[4] += 1
        with self.assertRaisesRegex(ValueError, 'articulation'):
            inspect(transport(bank))


if __name__ == '__main__':
    unittest.main()
