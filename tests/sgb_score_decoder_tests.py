#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Original score grammar fixtures and adversarial bounds, without private ROMs."""
import importlib.util
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / 'scripts/decode_sgb_score.py'
SPEC = importlib.util.spec_from_file_location('score_decoder', SCRIPT)
decoder = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(decoder)


def example():
    document = json.loads((ROOT / 'firmware/sgb/score_subset_example.json').read_text())
    bank = bytearray(document['size'])
    occupied = set()
    for segment in document['segments']:
        offset = segment['address'] - document['base']
        data = bytes.fromhex(segment['hex'])
        assert 0 <= offset <= len(bank) - len(data)
        addresses = set(range(offset, offset + len(data)))
        assert not occupied & addresses
        occupied.update(addresses)
        bank[offset:offset + len(data)] = data
    return bytes(bank), document['phrase']


def track_bank(track):
    bank = bytearray(32)
    bank[:4] = struct.pack('<HH', decoder.BASE + 4, 0)
    bank[4:6] = struct.pack('<H', decoder.BASE + 32)
    return bytes(bank) + track


class ScoreDecoderContracts(unittest.TestCase):
    def test_original_two_pattern_example(self):
        bank, phrase = example()
        decoded = decoder.decode(bank, phrase)
        self.assertFalse(decoded['qualified'])
        self.assertFalse(decoded['playback'])
        self.assertEqual(decoded['event_count'], 13)
        first, second = decoded['patterns']
        self.assertEqual([t['channel'] for t in first['tracks']], [0, 1])
        a, b = first['tracks']
        self.assertEqual(a['ticks'], 14)
        self.assertEqual(b['ticks'], 16)
        notes = [e for e in a['events'] if 'duration' in e]
        self.assertEqual([(e['kind'], e['tick'], e['duration']) for e in notes],
                         [('note', 0, 4), ('tie', 4, 4), ('rest', 8, 4), ('note', 12, 2)])
        self.assertEqual([e['articulation'] for e in notes], [31] * 4)
        self.assertEqual(b['events'][1]['value'], -2)
        inherited = second['tracks'][0]['events'][0]
        self.assertEqual((inherited['note'], inherited['duration'], inherited['articulation']),
                         (30, 2, 31))

    def test_note_range_and_zero_articulation(self):
        result = decoder.decode(track_bank(bytes.fromhex('010080c7c8c900')), decoder.BASE)
        events = result['patterns'][0]['tracks'][0]['events']
        self.assertEqual([e.get('note') for e in events], [0, 71, 71, None])
        self.assertEqual([e['articulation'] for e in events], [0] * 4)

    def test_all_supported_controls(self):
        track = bytes.fromhex('e003e114e5ffe700eaffed80018000')
        events = decoder.decode(track_bank(track), decoder.BASE)['patterns'][0]['tracks'][0]['events']
        self.assertEqual([(e['kind'], e['value']) for e in events[:-1]],
                         [('instrument', 3), ('pan', 20), ('song_volume', 255),
                          ('tempo', 0), ('transpose', -1), ('volume', 128)])

    def test_empty_song_and_inactive_pattern(self):
        self.assertEqual(decoder.decode(b'\0\0', decoder.BASE)['patterns'], [])
        bank = struct.pack('<HH', decoder.BASE + 4, 0) + bytes(16)
        self.assertEqual(decoder.decode(bank, decoder.BASE)['patterns'][0]['tracks'], [])

    def test_unknown_opcodes_fail_closed(self):
        for opcode in range(0xCA, 0x100):
            if opcode in decoder.CONTROLS:
                continue
            with self.subTest(opcode=opcode), self.assertRaisesRegex(ValueError, 'unsupported track'):
                decoder.decode(track_bank(bytes([opcode, 0, 0, 0])), decoder.BASE)
        for control in (1, 0x7F, 0x80, 0x81, 0x82, 0xFF):
            with self.subTest(control=control), self.assertRaisesRegex(ValueError, 'unsupported phrase'):
                decoder.decode(struct.pack('<HH', control, decoder.BASE), decoder.BASE)

    def test_duration_and_tie_errors(self):
        for track in (b'\x80\0', b'\x01\xc8\0', b'\x01\xc9\xc8\0',
                      b'\x01\x02\x03\x80\0', b'\x01\0\0'):
            with self.subTest(track=track), self.assertRaises(ValueError):
                decoder.decode(track_bank(track), decoder.BASE)

    def test_no_implicit_ram_or_wrapped_pointers(self):
        for pointer in (0x100, decoder.BASE - 1, decoder.BASE + 8192, 0xFFFF):
            bank = struct.pack('<HH', pointer, 0) + bytes(16)
            with self.subTest(pointer=pointer), self.assertRaisesRegex(ValueError, 'out-of-bank'):
                decoder.decode(bank, decoder.BASE)
            bank = struct.pack('<HHH', decoder.BASE + 4, 0, pointer) + bytes(14)
            with self.subTest(track=pointer), self.assertRaisesRegex(ValueError, 'out-of-bank'):
                decoder.decode(bank, decoder.BASE)
        for bank, root in ((b'', decoder.BASE), (bytes(8193), decoder.BASE),
                           (b'\0', decoder.BASE), (b'\0\0', decoder.BASE - 1),
                           (b'\0\0', True), (bytearray(2), decoder.BASE)):
            with self.subTest(root=root, size=len(bank)), self.assertRaises(ValueError):
                decoder.decode(bank, root)

    def test_truncation_at_every_required_byte(self):
        bank = track_bank(bytes.fromhex('e003041f80c8c900'))
        for size in range(1, len(bank)):
            with self.subTest(size=size), self.assertRaises(ValueError):
                decoder.decode(bank[:size], decoder.BASE)
        decoder.decode(bank, decoder.BASE)

    def test_bounded_patterns_events_and_work(self):
        bank = struct.pack('<H', decoder.BASE + 256) * 65 + bytes(142)
        with self.assertRaisesRegex(ValueError, 'pattern budget'):
            decoder.decode(bank, decoder.BASE)
        with self.assertRaisesRegex(ValueError, 'event budget'):
            decoder.decode(track_bank(b'\x01' + b'\x80' * 1025 + b'\0'), decoder.BASE)
        # Repeated duration/articulation reads must hit the work limit before the event limit.
        with self.assertRaisesRegex(ValueError, 'operation budget'):
            decoder.decode(track_bank(bytes.fromhex('010080') * 1400), decoder.BASE)

    def test_cli_success_and_errors(self):
        bank, phrase = example()
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'original.bin'
            path.write_bytes(bank)
            command = [sys.executable, str(SCRIPT), str(path), '--phrase', hex(phrase)]
            result = subprocess.run(command, capture_output=True, text=True, check=True)
            self.assertEqual(json.loads(result.stdout), decoder.decode(bank, phrase))
            for data in (b'', bytes(8193), b'\x01'):
                path.write_bytes(data)
                result = subprocess.run(command, capture_output=True, text=True)
                self.assertEqual(result.returncode, 2)
                self.assertEqual(result.stdout, '')
            path.unlink()
            self.assertEqual(subprocess.run(command, capture_output=True).returncode, 2)


if __name__ == '__main__':
    unittest.main()
