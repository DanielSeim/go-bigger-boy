#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""ROM-free raw-bank pointer parsing, first-end scheduling and rejection checks."""
import argparse
import copy
import hashlib
from pathlib import Path
import struct
import sys
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'scripts'))
from build_sgb_score_phrase import build
from build_sgb_phrase_fixture import CASES
from check_sgb_score_phrase_reference import align, compare, fixture, native, validate

PROBE = None


def word(bank, offset):
    return int.from_bytes(bank[offset:offset+2], 'little') - 0x2B00


def tracks(bank):
    phrase = word(bank, 0)
    return [word(bank, word(bank, phrase + pattern*2) + channel*2)
            for pattern in range(2) for channel in (2, 3)]


def authored(pair, commands=(), relocated=False):
    bank = bytearray(64)
    phrase = 48 if relocated else 16
    tables = (32, 16) if relocated else (32, 48)
    struct.pack_into('<H', bank, 0, 0x2B00+phrase)
    struct.pack_into('<HHH', bank, phrase, *(0x2B00+table for table in tables), 0)
    for pattern, table in enumerate(tables):
        for channel in ((3, 2) if relocated else (2, 3)):
            struct.pack_into('<H', bank, table + channel*2, 0x2B00+len(bank))
            bank.extend(commands)
            index = pattern*4 + (channel-2)*2
            bank.extend((pair[index], 127, pair[index+1], 0))
    return bytes(bank)


class PhraseTests(unittest.TestCase):
    def test_reproducible(self):
        self.assertEqual(build(), build())
        self.assertEqual(hashlib.sha256(build()).hexdigest(),
                         '1c2572175ae8c458d3c551c0fe44220c3d102087e3a0634b9f2371617c055178')

    def test_existing_raw_phrase_fixtures(self):
        for case in CASES:
            with self.subTest(case=case):
                bank = fixture(case)
                align(native(PROBE, bank), bank)

    def test_relocated_tables_rests_and_duration_limits(self):
        for pair in (bytes((1, 0x80, 127, 0xC7, 127, 0xC9, 2, 0x98)),
                     bytes((127, 0xC9, 127, 0xC9, 127, 0x80, 127, 0xC7)),
                     bytes((32, 0x99, 16, 0x98, 7, 0xC9, 19, 0xA4))):
            for relocated in (False, True):
                bank = authored(pair, relocated=relocated)
                for tempo in (96, 192):
                    with self.subTest(pair=pair.hex(), relocated=relocated, tempo=tempo):
                        align(native(PROBE, bank, tempo), bank)

    def test_controls_aliases_and_exact_end_bound(self):
        pair = bytes((16, 0x98, 32, 0x99, 16, 0xA4, 16, 0xA4))
        for tempo in (96, 192):
            bank = authored(pair, (0xED, 127, 0xE7, tempo, 0xE0, 2, 0xE5, 160, 0xE1, 10))
            align(native(PROBE, bank, tempo), bank)
        bank = bytearray(authored(pair))
        # Same read-only pattern/track pointers are finite and safe to revisit.
        struct.pack_into('<H', bank, 18, 0x2B20)
        struct.pack_into('<H', bank, 38, 0x2B40)
        align(native(PROBE, bytes(bank)), bytes(bank))
        bank = bytearray(authored(pair))
        last = tracks(bank)[-1]
        stream = bank[last:last+4]
        bank.extend(bytes(128-len(bank)))
        bank[124:128] = stream
        struct.pack_into('<H', bank, 54, 0x2B7C)
        align(native(PROBE, bytes(bank)), bytes(bank))

    def test_bad_pointers_and_truncation_emit_nothing(self):
        bank = fixture('short-first')
        invalid = [bank[:n] for n in range(1, len(bank))] + [bank+bytes(129-len(bank))]
        offsets = [0, 16, 18, 36, 38, 52, 54]
        for offset in offsets:
            for pointer in (0, 0x2AFF, 0x2C00, 0x2B00+len(bank), 0x2B7F, 0xFFFF):
                changed = bytearray(bank)
                struct.pack_into('<H', changed, offset, pointer)
                invalid.append(bytes(changed))
        for offset in (32, 34, 40, 42, 44, 46, 48, 50, 56, 58, 60, 62, 20):
            changed = bytearray(bank)
            struct.pack_into('<H', changed, offset, 0x2B40)
            invalid.append(bytes(changed))
        for changed in invalid:
            with self.subTest(bank=changed.hex()):
                self.assertEqual(native(PROBE, changed)['status'], 0xE2)

    def test_bad_track_syntax_and_late_validation(self):
        bank = bytearray(authored(bytes((16, 0x98, 32, 0x99, 16, 0xA4, 16, 0xA4))))
        for cursor in tracks(bank):
            for relative, values in ((0, (0, 128, 255)), (1, (0, 63, 126, 128)),
                                     (2, (0, 127, 0xC8, 0xCA, 0xEF)), (3, (1, 0x98, 0xFF))):
                for value in values:
                    changed = bytearray(bank)
                    changed[cursor+relative] = value
                    with self.subTest(cursor=cursor, relative=relative, value=value):
                        self.assertEqual(native(PROBE, bytes(changed))['status'], 0xE2)
        pair = bytes((16, 0x98, 32, 0x99, 16, 0xA4, 16, 0xA4))
        for commands in ((0xE0, 10), (0xE1, 0), (0xED, 64), (0xE5, 80),
                         (0xE7, 192), (0xEF, 2), (0xE2, 10), (0xE0, 2)*6):
            # One stream carries controls so even six commands stay within 128 bytes.
            changed = bytearray(authored(pair))
            struct.pack_into('<H', changed, 54, 0x2B00+len(changed))
            changed.extend((*commands, 16, 127, 0xA4, 0))
            self.assertEqual(native(PROBE, bytes(changed))['status'], 0xE2)
        for tempo in (0, 95, 193, 255):
            self.assertEqual(native(PROBE, bytes(bank), tempo)['status'], 0xE1)

    def test_reports_alignment_and_reference_comparison(self):
        candidates = {case: native(PROBE, fixture(case)) for case in CASES}
        report = candidates['short-first']
        for changed in (None, {}, {**report, 'schema': 'gbb-spc-score-pair-v1'},
                        {**report, 'playback': True}, {**report, 'events': []}):
            with self.assertRaises(ValueError):
                validate(changed)
        changed = copy.deepcopy(report)
        changed['events'][0]['opcode'] = 0x80
        with self.assertRaises(ValueError):
            align(changed, fixture('short-first'))
        refs = [{'model': model, 'case': case, 'pattern_interval_spc_cycles':
                 174000 if case == 'both-long' else 87000}
                for case in CASES for model in ('sgb', 'sgb2')]
        self.assertEqual(len(compare(candidates, refs)), 6)
        for changed in (refs[:-1], refs[:-1]+refs[:1]):
            with self.assertRaises(ValueError):
                compare(candidates, changed)
        changed = copy.deepcopy(refs)
        changed[0]['pattern_interval_spc_cycles'] += 3000
        with self.assertRaises(ValueError):
            compare(candidates, changed)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--probe', type=Path, required=True)
    args, remaining = parser.parse_known_args()
    PROBE = args.probe.resolve()
    unittest.main(argv=[sys.argv[0], *remaining])
