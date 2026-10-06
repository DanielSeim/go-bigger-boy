#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Original structural fixtures; no vendor data or playback qualification."""
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
import inventory_sgb_score as scanner


def bank_for(tracks, phrases=1):
    header = bytearray(2 * (phrases + 1) + 16)
    pattern = scanner.BASE + 2 * (phrases + 1)
    for index in range(phrases):
        struct.pack_into('<H', header, 2*index, pattern)
    for channel, stream in tracks.items():
        struct.pack_into('<H', header, pattern - scanner.BASE + 2*channel, scanner.BASE + len(header))
        header.extend(stream)
    return bytes(header)


class ScoreInventoryContracts(unittest.TestCase):
    def test_control_widths_skip_operands_and_report_requirements(self):
        track = bytearray()
        for opcode, (_, width) in scanner.CONTROLS.items():
            if opcode == 0xEF:
                continue
            track.extend([opcode] + [0xFF] * width)
        track.extend(bytes.fromhex('032580c8c9ca00'))
        report = scanner.inventory(bank_for({7: track}), scanner.BASE)
        self.assertEqual(report['profile'], 'limited_common_nspc_widths')
        self.assertFalse(report['qualification'])
        self.assertFalse(report['playback'])
        self.assertTrue(report['linear_scan_complete'])
        self.assertEqual(report['channels_observed'], [7])
        self.assertEqual(report['instrument_ids_observed'], [255])
        self.assertEqual(report['base_note_range'], [0, 0])
        for name in ('echo_enable', 'echo_setup', 'tempo', 'tempo_fade', 'vibrato',
                     'fine_tune', 'articulation', 'duration', 'note', 'tie', 'rest', 'percussion'):
            self.assertEqual(report['structural_counts'][name], 1)
        self.assertIn('quantization_and_velocity_tables', report['required_contracts'])
        self.assertIn('vendor_pitch_and_tuning', report['required_contracts'])
        self.assertIn('vendor_transpose_range_and_pitch', report['required_contracts'])
        self.assertIn('percussion', report['required_contracts'])
        self.assertNotIn('events', report)
        self.assertNotIn('operands', report)

    def test_all_eight_channels_and_wider_pitch_range(self):
        report = scanner.inventory(bank_for({c: bytes([1, 0x80+c*10, 0]) for c in range(8)}), scanner.BASE)
        self.assertEqual(report['channels_observed'], list(range(8)))
        self.assertEqual(report['tracks_scanned'], 8)
        self.assertEqual(report['base_note_range'], [0, 70])
        self.assertEqual(report['structural_counts']['note'], 8)
        self.assertIn('more_than_two_music_tracks', report['required_contracts'])
        self.assertIn('wider_base_note_range', report['required_contracts'])

    def test_subroutine_frontier_does_not_follow_or_claim_continuation(self):
        # Deliberately invalid target: it is metadata, not an authorized read.
        report = scanner.inventory(bank_for({0: bytes.fromhex('ef000108018000'),
                                             3: bytes.fromhex('018900')}), scanner.BASE)
        self.assertFalse(report['linear_scan_complete'])
        self.assertEqual(report['structural_counts'], {'duration': 1, 'note': 1, 'subroutine': 1})
        self.assertEqual(report['frontiers'], [{'kind': 'subroutine', 'address': scanner.BASE + 20,
                                               'channel': 0, 'target': 0x100}])
        self.assertIn('subroutine_execution', report['required_contracts'])

    def test_unknown_commands_and_phrase_controls_stop_paths(self):
        for opcode in (0xE2, 0xE6, 0xFF):
            report = scanner.inventory(bank_for({0: bytes([opcode, 0x80, 0]),
                                                 1: bytes.fromhex('018100')}), scanner.BASE)
            self.assertEqual(report['structural_counts']['note'], 1)
            self.assertEqual(report['frontiers'][0]['opcode'], f'0x{opcode:02X}')
            self.assertIn('unrecognized_variant_commands', report['required_contracts'])
        for control in (1, 0x7F, 0x80, 0xFF):
            report = scanner.inventory(struct.pack('<HH', control, 0xFFFF), scanner.BASE)
            self.assertEqual(report['patterns_scanned'], 0)
            self.assertEqual(report['frontiers'], [{'kind': 'phrase_control', 'address': scanner.BASE}])
            self.assertFalse(report['linear_scan_complete'])

    def test_truncation_pointers_and_input_types(self):
        bank = bank_for({0: bytes.fromhex('f5112233e003041f80c900')})
        for size in range(1, len(bank)):
            with self.subTest(size=size), self.assertRaises(ValueError):
                scanner.inventory(bank[:size], scanner.BASE)
        for invalid in (None, b'', bytes(8193), bytearray(bank)):
            with self.assertRaises(ValueError):
                scanner.inventory(invalid, scanner.BASE)
        for root in (True, None, scanner.BASE-1, scanner.BASE+len(bank), 0xFFFF):
            with self.assertRaises(ValueError):
                scanner.inventory(bank, root)
        for stream in (b'\x01', bytes.fromhex('01020380'), bytes.fromhex('0100')):
            with self.assertRaises(ValueError):
                scanner.inventory(bank_for({0: stream}), scanner.BASE)
        # Empty songs are scanned without inventing channels or notes.
        report = scanner.inventory(bytes(8192), scanner.BASE)
        self.assertEqual(report['channels_observed'], [])
        self.assertIsNone(report['base_note_range'])

    def test_budgets_bound_repeated_references(self):
        with self.assertRaisesRegex(ValueError, 'pattern budget'):
            scanner.inventory(bank_for({}, phrases=65), scanner.BASE)
        with self.assertRaisesRegex(ValueError, 'event budget'):
            scanner.inventory(bank_for({0: b'\x80' * 4100 + b'\0'}, phrases=2), scanner.BASE)
        bank = bank_for({0: bytes.fromhex('01008000')})
        with patch.object(scanner, 'MAX_READS', 5), self.assertRaisesRegex(ValueError, 'read budget'):
            scanner.inventory(bank, scanner.BASE)

    def test_cli_bounded_metadata_and_failure(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'bank.bin'
            path.write_bytes(bank_for({2: bytes.fromhex('e003018c00')}))
            command = [sys.executable, str(ROOT / 'scripts/inventory_sgb_score.py'),
                       str(path), '--phrase', hex(scanner.BASE)]
            completed = subprocess.run(command, check=True, capture_output=True, text=True)
            self.assertEqual(json.loads(completed.stdout), scanner.inventory(path.read_bytes(), scanner.BASE))
            for invalid in (bytes(8193), b'', b'\xff'):
                path.write_bytes(invalid)
                completed = subprocess.run(command, capture_output=True)
                self.assertEqual(completed.returncode, 2)
                self.assertEqual(completed.stdout, b'')
            path.unlink()
            self.assertEqual(subprocess.run(command, capture_output=True).returncode, 2)


if __name__ == '__main__':
    unittest.main()
