#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""ROM-free two-voice onsets, clipped release, rests, owned PCM and lifecycle."""
import argparse
import copy
import hashlib
from pathlib import Path
import sys
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'scripts'))
from build_sgb_score_duet import build
from build_sgb_score_phrase import build as phrase_build
from check_sgb_score_duet_reference import align, compare, fixture, native, validate
from build_sgb_phrase_fixture import CASES
from sgb_score_phrase_tests import authored, tracks

PROBE = None


class DuetTests(unittest.TestCase):
    def test_reproducible_and_parser_unchanged(self):
        self.assertEqual(build(), build())
        self.assertEqual(hashlib.sha256(build()).hexdigest(),
                         'a201bc486cd106d4db28c0bd151ac86535f63acb20d21f2ef29995c377c2dac8')
        self.assertEqual(hashlib.sha256(phrase_build()).hexdigest(),
                         '1c2572175ae8c458d3c551c0fe44220c3d102087e3a0634b9f2371617c055178')

    def test_owned_reference_fixtures(self):
        for case in CASES:
            with self.subTest(case=case):
                bank = fixture(case)
                report = native(PROBE, bank)
                align(report, bank)
                self.assertEqual([on['mask'] for on in report['keyons']], [12, 12])
                self.assertEqual([on['pitches'] for on in report['keyons']], [[1068, 1132], [2140, 2140]])
                self.assertEqual(report['keyoffs'][0]['tick'], min(CASES[case]))

    def test_clipped_voice_settles_during_peer_audio(self):
        for pair, silent in ((bytes((32, 0x98, 16, 0x99, 16, 0xC9, 16, 0xA4)), 'left'),
                             (bytes((16, 0x98, 32, 0x99, 16, 0xA4, 16, 0xC9)), 'right')):
            for tempo in (96, 192):
                with self.subTest(silent=silent, tempo=tempo):
                    bank = authored(pair, relocated=True)
                    report = native(PROBE, bank, tempo)
                    align(report, bank)
                    self.assertEqual([on['mask'] for on in report['keyons']], [12, 8 if silent == 'left' else 4])
                    self.assertEqual(report['keyoffs'][0]['tick'], 16)
                    tail = report['second_tail_pcm']
                    self.assertGreaterEqual(tail['frames'], 64)
                    self.assertEqual(tail[f'{silent}_nonzero_frames'], 0)
                    active = 'right' if silent == 'left' else 'left'
                    self.assertGreater(tail[f'{active}_nonzero_frames'], 64)

    def test_rest_patterns_and_short_events(self):
        for pair in (bytes((16, 0xC9, 16, 0xC9, 16, 0x98, 16, 0x99)),
                     bytes((32, 0x98, 16, 0x99, 16, 0xC9, 16, 0xC9)),
                     bytes((16, 0xC9, 32, 0xC9, 16, 0xC9, 16, 0xC9)),
                     bytes((2, 0x98, 127, 0xC9, 127, 0xC9, 2, 0x99)),
                     bytes((16, 0x98, 32, 0xC9, 16, 0xA4, 16, 0xC9)),
                     bytes((32, 0xC9, 16, 0x99, 16, 0xC9, 16, 0xA4))):
            for tempo in (96, 192):
                with self.subTest(pair=pair.hex(), tempo=tempo):
                    bank = authored(pair)
                    align(native(PROBE, bank, tempo), bank)

    def test_malformed_or_unsupported_reject_before_audio(self):
        bank = fixture('short-first')
        invalid = [bank[:n] for n in range(1, len(bank))] + [bytes(129)]
        plain = authored(bytes((16, 0x98, 32, 0x99, 16, 0xA4, 16, 0xA4)))
        for cursor in tracks(plain):
            for relative, values in ((0, (0, 1, 128)), (1, (63, 0, 128)),
                                     (2, (0x80, 0xC8, 0xEF)), (3, (1, 0x98))):
                for value in values:
                    changed = bytearray(plain)
                    changed[cursor+relative] = value
                    invalid.append(bytes(changed))
        for stream in invalid:
            with self.subTest(bank=stream.hex()):
                self.assertEqual(native(PROBE, stream)['status'], 0xE2)
        for tempo in (0, 95, 193, 255):
            self.assertEqual(native(PROBE, plain, tempo)['status'], 0xE1)

    def test_report_and_reference_guards(self):
        candidates = {case: native(PROBE, fixture(case)) for case in CASES}
        report = candidates['short-first']
        for changed in (None, {}, {**report, 'playback': True}, {**report, 'keyons': []},
                        {**report, 'keyoffs': []}, {**report, 'pcm': {}}, {**report, 'tempo': True}):
            with self.assertRaises(ValueError):
                validate(changed)
        for name, key, value in (('keyons', 'mask', 4), ('keyons', 'pitches', [1068, 1068]),
                                 ('keyoffs', 'tick', 32), ('keyoffs', 'mask', 4)):
            changed = copy.deepcopy(report)
            changed[name][0][key] = value
            with self.assertRaises(ValueError):
                validate(changed)
        refs = [{'model': model, 'case': case,
                 'pattern_interval_spc_cycles': 174000 if case == 'both-long' else 87000,
                 'keyons': [{'mask': 12, 'voices': [
                     {'voice': voice+2, 'srcn': 2, 'pitch': pitch} for voice, pitch in enumerate(pitches)]}
                            for pitches in ([1068, 1132], [2140, 2140])]}
                for case in CASES for model in ('sgb', 'sgb2')]
        self.assertEqual(len(compare(candidates, refs)), 6)
        for changed in (refs[:-1], refs[:-1]+refs[:1]):
            with self.assertRaises(ValueError):
                compare(candidates, changed)
        changed = copy.deepcopy(refs)
        changed[0]['keyons'][0]['voices'][1]['pitch'] = 1068
        with self.assertRaises(ValueError):
            compare(candidates, changed)
        changed = copy.deepcopy(candidates)
        changed['short-first']['keyons'][1]['half_cycle'] += 10000
        with self.assertRaises(ValueError):
            compare(changed, refs)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--probe', type=Path, required=True)
    args, remaining = parser.parse_known_args()
    PROBE = args.probe.resolve()
    unittest.main(argv=[sys.argv[0], *remaining])
