#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""ROM-free native flat-track parsing, timing and rejection checks."""
import argparse
import copy
import hashlib
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'scripts'))
from build_sgb_score_track import build
from check_sgb_score_track_reference import align, compare, fixture, native, validate

PROBE = None


class TrackTests(unittest.TestCase):
    def test_reproducible(self):
        self.assertEqual(build(), build())
        self.assertEqual(hashlib.sha256(build()).hexdigest(),
                         '8674090e26739e16f24a4451206b7321e973a0031028a1a68d584d98281caa27')

    def test_owned_timing_fixtures(self):
        reports = {}
        for case, tempo in (('baseline', 96), ('double-tempo', 192), ('short-gate', 96)):
            track = fixture(case)
            report = native(PROBE, track, tempo)
            align(report, track)
            reports[case] = report
        def intervals(report):
            notes = [event for event in report['events'] if event['opcode'] != 0xC9]
            return [(b['half_cycle']-a['half_cycle'])/2 for a, b in zip(notes, notes[1:])]
        slow, fast, shorter = [intervals(reports[case]) for case in reports]
        for a, b, c in zip(slow, fast, shorter):
            self.assertTrue(0.47 <= b/a <= 0.53)
            self.assertEqual(a, c)

    def test_duration_articulation_and_rest_inheritance(self):
        for track in (bytes((1, 0, 0x80, 0xC9, 0xC7, 0)),
                      bytes((5, 63, 0x98, 3, 0x99, 2, 127, 0xC9, 0xA4, 0)),
                      bytes((16, 127, *([0x98]*16), 0)),
                      bytes((127, 127, 0x98, 127, 0, 0xC9, 0xC7, 0))):
            with self.subTest(track=track.hex()):
                align(native(PROBE, track, 96), track)

    def test_invalid_tracks_reject_before_events(self):
        tracks = [b'\0', bytes((16, 127, 0)), bytes((0x98, 0)),
                  bytes((16, 0x98, 0)), bytes((16, 127, 0x98)),
                  bytes((16, 127, 0x98, 0, 0)), bytes((16, 1, 2, 0x98, 0)),
                  bytes((16, 127, *([0x98]*17), 0)), bytes(129)]
        tracks.extend(bytes((16, 127, opcode, 0)) for opcode in (0xC8, 0xCA, 0xE0, 0xE7, 0xEF, 0xFF))
        for track in tracks:
            with self.subTest(track=track.hex()):
                report = native(PROBE, track, 96)
                self.assertEqual(report['status'], 0xE2)
                self.assertEqual(report['events'], [])
                self.assertEqual(report['end_tick'], 0)

    def test_unsupported_tempo(self):
        for tempo in (0, 95, 193, 255):
            report = native(PROBE, fixture('baseline'), tempo)
            self.assertEqual(report['status'], 0xE1)
            self.assertEqual(report['events'], [])

    def test_reports_and_alignment_fail_closed(self):
        track = fixture('baseline')
        report = native(PROBE, track, 96)
        for malformed in (None, {}, {**report, 'playback': True}, {**report, 'events': []}):
            with self.assertRaises(ValueError):
                validate(malformed)
        for key, value in (('tick', 0), ('duration', 0), ('opcode', 0xEF), ('half_cycle', True)):
            changed = copy.deepcopy(report)
            changed['events'][1][key] = value
            with self.assertRaises(ValueError):
                validate(changed)
        changed = copy.deepcopy(report)
        changed['events'][1]['opcode'] = 0x80
        with self.assertRaises(ValueError):
            align(changed, track)

    def test_reference_comparison_fails_closed(self):
        candidates = {case: native(PROBE, fixture(case), tempo) for case, tempo in
                      (('baseline', 96), ('double-tempo', 192), ('short-gate', 96))}
        references = []
        for model in ('sgb', 'sgb2'):
            for case, tempo, interval, gate in (('baseline', 96, 87000, 74000),
                    ('double-tempo', 192, 43500, 36000), ('short-gate', 96, 87000, 46000)):
                references.append({'model': model, 'case': case, 'tempo': tempo,
                                   'onset_intervals_spc_cycles': [interval]*2,
                                   'gate_spc_cycles': [gate]*3})
        self.assertEqual(len(compare(candidates, references)), 6)
        with self.assertRaises(ValueError):
            compare(candidates, references[:-1])
        with self.assertRaises(ValueError):
            compare({'baseline': candidates['baseline']}, references)
        changed = copy.deepcopy(candidates)
        changed['baseline']['events'][2]['half_cycle'] += 20000
        with self.assertRaises(ValueError):
            compare(changed, references)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--probe', type=Path, required=True)
    args, remaining = parser.parse_known_args()
    PROBE = args.probe.resolve()
    unittest.main(argv=[sys.argv[0], *remaining])
