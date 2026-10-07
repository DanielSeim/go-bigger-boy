#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""ROM-free native DSP setup, owned PCM, lifecycle and rejection checks."""
import argparse
import copy
import hashlib
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'scripts'))
from build_sgb_score_render import build
from check_sgb_score_render_reference import align, compare, fixture, native, validate

PROBE = None


class RenderTests(unittest.TestCase):
    def test_reproducible(self):
        self.assertEqual(build(), build())
        self.assertEqual(hashlib.sha256(build()).hexdigest(),
                         '4a323acb4c71ae6189819da020295a6299c5a8a3b390e4bc7732d78964f88b4b')

    def test_owned_pcm_and_onset_timing(self):
        reports = {tempo: native(PROBE, fixture(tempo), tempo) for tempo in (96, 192)}
        for tempo, report in reports.items():
            align(report, fixture(tempo))
            self.assertGreater(report['pcm']['nonzero_frames'], 100)
            self.assertTrue(0 < report['pcm']['peak'] < 32768)
            self.assertGreaterEqual(report['pcm']['quiet_tail_frames'], 64)
            self.assertEqual([event['pitch'] for event in report['keyons']], [1068, 1132, 2140])
        slow, fast = [[(b['half_cycle']-a['half_cycle'])/2 for a, b in
                       zip(report['keyons'], report['keyons'][1:])] for report in reports.values()]
        for a, b in zip(slow, fast):
            self.assertTrue(0.47 <= b/a <= 0.53)

    def test_notes_rests_and_duration_inheritance(self):
        for track in (bytes((2, 127, 0x98, 0x99, 0xA4, 0)),
                      bytes((8, 127, 0x98, 16, 0xC9, 0x99, 0)),
                      bytes((16, 127, *([0x98]*16), 0)),
                      bytes((127, 127, 0x98, 0x99, 0xA4, 0))):
            with self.subTest(track=track.hex()):
                report = native(PROBE, track, 96)
                align(report, track)
                if track == bytes((8, 127, 0x98, 16, 0xC9, 0x99, 0)):
                    self.assertGreaterEqual(report['keyons'][1]['quiet_tail_frames'], 64)

    def test_rest_only_is_silent(self):
        track = bytes((16, 127, 0xC9, 0xC9, 0))
        report = native(PROBE, track, 96)
        align(report, track)
        self.assertEqual(report['pcm']['nonzero_frames'], 0)
        self.assertEqual(report['pcm']['peak'], 0)
        self.assertEqual(report['keyons'], [])

    def test_invalid_tracks_reject_before_events(self):
        tracks = [bytes((1,127,0x98,0)), bytes((16,63,0x98,0)),
                  bytes((16,127,0x98,0x80,0)),b'\0', bytes((16, 127, 0)), bytes((0x98, 0)),
                  bytes((16, 0x98, 0)), bytes((16, 127, 0x98)),
                  bytes((16, 127, 0x98, 0, 0)), bytes((16, 1, 2, 0x98, 0)),
                  bytes((16, 127, *([0x98]*17), 0)), bytes(129)]
        tracks.extend(bytes((16, 127, opcode, 0)) for opcode in (0x80, 0x97, 0x9A, 0xC7, 0xC8, 0xCA, 0xE0, 0xE7, 0xEF, 0xFF))
        for track in tracks:
            with self.subTest(track=track.hex()):
                report = native(PROBE, track, 96)
                self.assertEqual(report['status'], 0xE2)
                self.assertEqual(report['events'], [])
                self.assertEqual(report['end_tick'], 0)

    def test_unsupported_tempo(self):
        for tempo in (0, 95, 193, 255):
            report = native(PROBE, fixture(), tempo)
            self.assertEqual(report['status'], 0xE1)
            self.assertEqual(report['events'], [])

    def test_reports_and_alignment_fail_closed(self):
        track = fixture()
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
        changed['events'][1]['opcode'] = 0xA4
        with self.assertRaises(ValueError):
            align(changed, track)

    def test_reference_comparison_fails_closed(self):
        candidates = {tempo: native(PROBE, fixture(tempo), tempo) for tempo in (96, 192)}
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
            compare({96: candidates[96]}, references)
        changed = copy.deepcopy(candidates)
        changed[96]['keyons'][1]['half_cycle'] += 20000
        with self.assertRaises(ValueError):
            compare(changed, references)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--probe', type=Path, required=True)
    args, remaining = parser.parse_known_args()
    PROBE = args.probe.resolve()
    unittest.main(argv=[sys.argv[0], *remaining])
