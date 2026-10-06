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
CONTROLS_EXAMPLE = ROOT / 'firmware/sgb/resident_controls_example.json'
TRACKS_EXAMPLE = ROOT / 'firmware/sgb/resident_tracks_example.json'
PHRASES_EXAMPLE = ROOT / 'firmware/sgb/resident_phrases_example.json'
REPEATS_EXAMPLE = ROOT / 'firmware/sgb/resident_repeats_example.json'
TRANSPOSE_EXAMPLE = ROOT / 'firmware/sgb/resident_transpose_example.json'
RUNNER = None


class ResidentScoreContracts(unittest.TestCase):
    def test_controls_bank_and_limits(self):
        payload = build(json.loads(CONTROLS_EXAMPLE.read_text()))
        size = struct.unpack_from('<H', payload)[0]
        self.assertEqual(payload[4:8], b'GBS2')
        events = decode(payload[4:4+size], 0x2B08)['patterns'][0]['tracks'][0]['events']
        self.assertEqual([(e['kind'], e.get('value')) for e in events[:3]],
                         [('instrument', 0), ('pan', 10), ('volume', 64)])
        self.assertEqual([e['tick'] for e in events if e['kind'] == 'pan'], [0, 16, 24, 32])
        for key, maximum in (('instrument', 3), ('pan', 20), ('volume', 127)):
            for value in (-1, maximum+1, True, 1.0, '1'):
                with self.subTest(key=key, value=value), self.assertRaises(ValueError):
                    build({'format': 'GBS2', 'events': [{key: value}]})
            build({'format': 'GBS2', 'events': [{key: maximum}]})
            with self.assertRaises(ValueError):
                build({'events': [{key: 0}]})
        for document in ({'format': 'GBS3', 'events': [{'rest': True, 'ticks': 1}]},
                         {'format': [], 'events': []},
                         {'format': 'GBS2', 'events': [{'tempo': 4}]}):
            with self.assertRaises(ValueError):
                build(document)

    def test_two_tracks_and_independent_oracle(self):
        payload = build(json.loads(TRACKS_EXAMPLE.read_text()))
        size, address = struct.unpack_from('<HH', payload)
        self.assertEqual((size, address), (60, 0x2B00))
        bank = payload[4:4+size]
        self.assertEqual(bank[:6], b'GBS3\x3c\x2f')
        self.assertEqual(struct.unpack_from('<HH', bank, 16), (0x2B20, 0x2B2F))
        tracks = decode(bank, 0x2B08)['patterns'][0]['tracks']
        self.assertEqual(len(tracks), 2)
        self.assertEqual([[e['tick'] for e in t['events'] if e['kind'] in ('note', 'rest', 'tie')] for t in tracks],
                         [[0, 4, 8], [0, 8, 16]])
        note = {'note': 0, 'ticks': 1}
        maximum = build({'format': 'GBS3', 'tracks': [[note] * 109, [note]]})
        self.assertEqual(struct.unpack_from('<H', maximum)[0], 254)
        for document in (
            {'format': 'GBS3', 'tracks': [[note] * 110, [note]]},
            {'format': 'GBS3', 'tracks': [[note], []]},
            {'format': 'GBS3', 'tracks': [[note]]},
            {'format': 'GBS3', 'tracks': [[note], [note], [note]]},
            {'format': 'GBS3', 'tracks': [[note], [{'tie': True, 'ticks': 1}]]},
            {'format': 'GBS3', 'tracks': [[note], [{'pan': 21}]]},
            {'format': 'GBS2', 'tracks': [[note], [note]]},
            {'tracks': [[note], [note]]},
        ):
            with self.subTest(document=document), self.assertRaises(ValueError):
                build(document)

    def test_finite_phrases_and_fresh_track_state(self):
        payload = build(json.loads(PHRASES_EXAMPLE.read_text()))
        size, address = struct.unpack_from('<HH', payload)
        self.assertEqual((size, address), (124, 0x2B00))
        bank = payload[4:4+size]
        self.assertEqual(bank[:6], b'GBS4\x7c\x03')
        self.assertEqual(struct.unpack_from('<HHHH', bank, 8), (0x2B20, 0x2B30, 0x2B40, 0))
        patterns = decode(bank, 0x2B08)['patterns']
        self.assertEqual([[t['ticks'] for t in p['tracks']] for p in patterns], [[4, 8], [8, 12], [8, 8]])
        note = [{'note': 0, 'ticks': 1}]
        for count in range(1, 5):
            build({'format': 'GBS4', 'patterns': [[note, note]] * count})
        maximum = build({'format': 'GBS4', 'patterns': [[note * 101, note]]})
        self.assertEqual(struct.unpack_from('<H', maximum)[0], 254)
        invalid = [None, [], [[], note], [[note]], [[note, note, note]], [[[], note]],
                   [[note, note]] * 5, [[note * 102, note]],
                   [[note, note], [[{'tie': True, 'ticks': 1}], note]]]
        for patterns in invalid:
            with self.subTest(patterns=patterns), self.assertRaises(ValueError):
                build({'format': 'GBS4', 'patterns': patterns})
        for document in ({'patterns': [[note, note]]}, {'format': 'GBS4', 'events': note},
                         {'format': 'GBS4', 'patterns': [[note, note]], 'tempo': 1}):
            with self.assertRaises(ValueError):
                build(document)

    def test_bounded_repeats_and_strict_play_count(self):
        document = json.loads(REPEATS_EXAMPLE.read_text())
        payload = build(document)
        size = struct.unpack_from('<H', payload)[0]
        self.assertEqual(payload[4:11], b'GBS5\x58\x02\x02')
        self.assertEqual(size, 88)
        self.assertEqual(len(decode(payload[4:4+size], 0x2B08)['patterns']), 2)
        for plays in range(1, 5):
            result = build(dict(document, plays=plays))
            self.assertEqual(result[10], plays)
        for plays in (0, 5, 128, True, 1.0, '2', None):
            with self.subTest(plays=plays), self.assertRaises(ValueError):
                build(dict(document, plays=plays))
        for changed in ({k: v for k, v in document.items() if k != 'plays'},
                        dict(document, format='GBS4'), dict(document, repeat=2)):
            with self.assertRaises(ValueError):
                build(changed)
        # Repeats do not weaken fresh per-pattern tie validation.
        document['patterns'][1][0] = [{'tie': True, 'ticks': 1}]
        with self.assertRaises(ValueError):
            build(document)

    def test_transpose_bounds_effective_notes_and_fresh_state(self):
        payload = build(json.loads(TRANSPOSE_EXAMPLE.read_text()))
        size = struct.unpack_from('<H', payload)[0]
        self.assertEqual(payload[4:11], b'GBS6\x74\x02\x02')
        events = decode(payload[4:4+size], 0x2B08)['patterns'][0]['tracks'][0]['events']
        self.assertEqual([e['value'] for e in events if e['kind'] == 'transpose'], [12, -12])
        for value in (-13, 13, 128, True, 1.0, '1'):
            with self.subTest(value=value), self.assertRaises(ValueError):
                build({'format': 'GBS6', 'plays': 1, 'patterns': [[[{'transpose': value}], [{'rest': True, 'ticks': 1}]]]})
        for note, transpose in ((0, -1), (31, 1), (11, -12), (20, 12)):
            with self.subTest(note=note, transpose=transpose), self.assertRaises(ValueError):
                build({'format': 'GBS6', 'plays': 1, 'patterns': [[[{'transpose': transpose}, {'note': note, 'ticks': 1}], [{'rest': True, 'ticks': 1}]]]})
        for note, transpose in ((12, -12), (19, 12)):
            build({'format': 'GBS6', 'plays': 1, 'patterns': [[[{'transpose': transpose}, {'note': note, 'ticks': 1}], [{'rest': True, 'ticks': 1}]]]})
        old = json.loads(TRANSPOSE_EXAMPLE.read_text())
        old['format'] = 'GBS5'
        with self.assertRaises(ValueError):
            build(old)
        # A transpose change on a tie does not re-evaluate or retune the held note.
        build({'format': 'GBS6', 'plays': 1, 'patterns': [[[{'note': 31, 'ticks': 1}, {'transpose': 12}, {'tie': True, 'ticks': 1}], [{'note': 0, 'ticks': 1}]]]})
        # Transpose resets at each new track and phrase.
        build({'format': 'GBS6', 'plays': 1, 'patterns': [
            [[{'transpose': -12}, {'note': 12, 'ticks': 1}], [{'note': 0, 'ticks': 1}]],
            [[{'note': 0, 'ticks': 1}], [{'note': 31, 'ticks': 1}]]
        ]})

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

    def test_real_uploaded_controls(self):
        if RUNNER is None:
            self.skipTest('supply --runner for SPC/DSP integration')
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'controls.bin'
            path.write_bytes(build(json.loads(CONTROLS_EXAMPLE.read_text())))
            subprocess.run([str(RUNNER), '--resident-controls', str(path)], check=True)

    def test_real_uploaded_tracks(self):
        if RUNNER is None:
            self.skipTest('supply --runner for SPC/DSP integration')
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'tracks.bin'
            path.write_bytes(build(json.loads(TRACKS_EXAMPLE.read_text())))
            subprocess.run([str(RUNNER), '--resident-tracks', str(path)], check=True)

    def test_real_uploaded_phrases(self):
        if RUNNER is None:
            self.skipTest('supply --runner for SPC/DSP integration')
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'phrases.bin'
            path.write_bytes(build(json.loads(PHRASES_EXAMPLE.read_text())))
            subprocess.run([str(RUNNER), '--resident-phrases', str(path)], check=True)

    def test_real_uploaded_repeats(self):
        if RUNNER is None:
            self.skipTest('supply --runner for SPC/DSP integration')
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'repeats.bin'
            path.write_bytes(build(json.loads(REPEATS_EXAMPLE.read_text())))
            subprocess.run([str(RUNNER), '--resident-repeats', str(path)], check=True)

    def test_real_uploaded_transpose(self):
        if RUNNER is None:
            self.skipTest('supply --runner for SPC/DSP integration')
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'transpose.bin'
            path.write_bytes(build(json.loads(TRANSPOSE_EXAMPLE.read_text())))
            subprocess.run([str(RUNNER), '--resident-transpose', str(path)], check=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--runner', type=Path)
    RUNNER = parser.parse_args().runner
    unittest.main(argv=[sys.argv[0]])
