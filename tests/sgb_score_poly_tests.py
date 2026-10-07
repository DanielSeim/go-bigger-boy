#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Owned independent voice updates, peer continuity, release and lifecycle checks."""
import argparse
import copy
import hashlib
from pathlib import Path
import sys
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'scripts'))
from build_sgb_score_poly import build
from build_sgb_score_multi import build as multi_build
from build_sgb_multi_fixture import CASES, bank
from check_sgb_score_poly_reference import align, compare, native, validate
from check_sgb_score_multi_reference import PITCHES
from check_sgb_score_phrase_reference import fixture as single_fixture
from sgb_score_multi_tests import authored

PROBE = None


class PolyTests(unittest.TestCase):
    def test_reproducible_and_muted_artifact_unchanged(self):
        self.assertEqual(build(), build())
        self.assertEqual(hashlib.sha256(build()).hexdigest(),
                         '6202e7b38f6cee051d9cc0fa2eab64642ab99d8a19470ae7afb9e76a2e88b97c')
        self.assertEqual(hashlib.sha256(multi_build()).hexdigest(),
                         '738341ce61c29b0492795c0fbedfe8dc96c3b598c8b140f8f588b8a8137859ac')

    def test_new_and_legacy_owned_banks(self):
        for case in CASES:
            for data in (bank(case), single_fixture(case)):
                report = native(PROBE, data)
                align(report, data)
                self.assertTrue(all(edge['mask'] == 12 for edge in report['keyons']))

    def test_asynchronous_updates_preserve_both_peers(self):
        streams = ((4, 127, 0x98, 7, 0xC9, 0x99, 0),
                   (6, 127, 0x99, 9, 0xA4, 3, 127, 0x98, 0),
                   (3, 127, 0xC9, 0x98, 0x99, 0),
                   (5, 127, 0xA4, 5, 0xC9, 0))
        for tempo in (96, 192):
            data = authored(streams, relocated=True)
            report = native(PROBE, data, tempo)
            align(report, data)
            self.assertEqual([edge['mask'] for edge in report['keyons']], [12, 8, 4, 8, 8, 4, 4])
            self.assertGreater(min(report['peer_checks']), 100)
            self.assertEqual(report['end_tick'], 27)
            self.assertGreater(report['pcm']['left_nonzero_frames'], 64)
            self.assertGreater(report['pcm']['right_nonzero_frames'], 64)

    def test_clipping_either_voice_then_rest_preserves_peer_audio(self):
        short = (8, 127, 0x98, 0x99, 0)
        long = (4, 127, 0x99, 24, 0xA4, 0)
        rest, note = (16, 127, 0xC9, 0), (16, 127, 0xA4, 0)
        for streams, silent in (((long, short, rest, note), 'left'),
                                ((short, long, note, rest), 'right')):
            for tempo in (96, 192):
                data = authored(streams)
                report = native(PROBE, data, tempo)
                align(report, data)
                tail = report['second_tail_pcm']
                self.assertEqual(report['second_pattern_tick'], 16)
                self.assertGreaterEqual(tail['frames'], 64)
                self.assertEqual(tail[f'{silent}_nonzero_frames'], 0)
                active = 'right' if silent == 'left' else 'left'
                self.assertGreater(tail[f'{active}_nonzero_frames'], 64)

    def test_rest_only_tracks_and_full_bounds(self):
        cases = (((8, 127, 0xC9, 0xC9, 0),)*4,
                 ((4, 127, 0x98, 0x99, 0), (8, 127, 0xC9, 0),
                  (16, 127, 0xC9, 0), (16, 127, 0xA4, 0)),
                 ((8, 127, 0xC9, 0), (4, 127, 0x99, 0x98, 0),
                  (16, 127, 0xA4, 0), (16, 127, 0xC9, 0)),
                 ((127, 127, 0x98, 0x99, 0xC9, 0xA4, 0),)*4,
                 ((2, 127, 0x98, 0xC9, 0),)*4)
        for streams in cases:
            data = authored(streams)
            align(native(PROBE, data), data)

    def test_invalid_or_ambiguous_reject_before_audio(self):
        good = (16, 127, 0x98, 0)
        for bad in ((0,), (1, 127, 0x98, 0), (16, 63, 0x98, 0),
                    (16, 127, 0x80, 0), (16, 127, *([0x98]*5), 0),
                    (16, 127, 0x98, 4, 63, 0x99, 0)):
            for slot in range(4):
                streams = [good]*4
                streams[slot] = bad
                self.assertEqual(native(PROBE, authored(streams))['status'], 0xE2)
        short, changed = (8, 127, 0x98, 0), (8, 127, 0x99, 0xA4, 0)
        for streams in ((short, changed, good, good), (changed, short, good, good),
                        (good, good, short, changed), (good, good, changed, short)):
            self.assertEqual(native(PROBE, authored(streams))['status'], 0xE2)
        data = bank('short-first')
        for size in range(1, len(data)):
            self.assertEqual(native(PROBE, data[:size])['status'], 0xE2)
        self.assertEqual(native(PROBE, bytes(129))['status'], 0xE2)
        for tempo in (0, 95, 193, 255):
            self.assertEqual(native(PROBE, authored((good,)*4), tempo)['status'], 0xE1)

    def test_report_and_reference_guards(self):
        candidates = {case: native(PROBE, bank(case)) for case in CASES}
        report = candidates['short-first']
        for changed in (None, {}, {**report, 'playback': True}, {**report, 'keyons': []},
                        {**report, 'keyoffs': []}, {**report, 'pcm': {}}, {**report, 'peer_checks': [True, 0]}):
            with self.assertRaises(ValueError):
                validate(changed)
        for name, key, value in (('keyons', 'mask', 4), ('keyons', 'held_mask', 4),
                                 ('keyons', 'affected_mask', 4), ('keyoffs', 'mask', 4),
                                 ('keyoffs', 'tick', 16), ('keyons', 'pitches', [1068, 1068])):
            changed = copy.deepcopy(report)
            changed[name][0][key] = value
            with self.assertRaises(ValueError):
                validate(changed)
        keyons = [{'mask': 12, 'voices': [{'voice': voice+2, 'srcn': 2, 'pitch': pitch}
                   for voice, pitch in enumerate(pitches)]} for pitches in PITCHES]
        refs = [{'case': case, 'model': model, 'keyons': keyons,
                 'onset_intervals_spc_cycles': [43500, 87000 if case == 'both-long' else 43500]}
                for case in CASES for model in ('sgb', 'sgb2')]
        self.assertEqual(len(compare(candidates, refs)), 6)
        for changed in (refs[:-1], refs[:-1]+refs[:1]):
            with self.assertRaises(ValueError):
                compare(candidates, changed)
        changed = copy.deepcopy(refs)
        changed[0]['keyons'][0]['voices'][1]['pitch'] = 1068
        with self.assertRaises(ValueError):
            compare(candidates, changed)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--probe', type=Path, required=True)
    args, remaining = parser.parse_known_args()
    PROBE = args.probe.resolve()
    unittest.main(argv=[sys.argv[0], *remaining])
