#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""ROM-free calibrated native gate, lifecycle and rejection checks."""
import argparse
import copy
import hashlib
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'scripts'))
from build_sgb_score_gate import build
from check_sgb_score_gate_reference import align, compare, fixture, native, validate

PROBE = None


class GateTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.reports = {case: native(PROBE, fixture(case), tempo) for case, tempo in
                       (('baseline', 96), ('double-tempo', 192), ('short-gate', 96))}

    def test_reproducible(self):
        self.assertEqual(build(), build())
        self.assertEqual(hashlib.sha256(build()).hexdigest(),
                         'd6d5e71c079c3d65db9c579a3735a802303712736f59626d13504a82e2b33fde')

    def test_gates_and_onset_spacing(self):
        gates, intervals = {}, {}
        for case, report in self.reports.items():
            align(report, fixture(case))
            gates[case] = [(off-on['half_cycle'])/2 for on, off in
                           zip(report['keyons'], report['keyoff_half_cycles'])]
            intervals[case] = [(b['half_cycle']-a['half_cycle'])/2 for a, b in
                               zip(report['keyons'], report['keyons'][1:])]
        for slow, fast, short in zip(gates['baseline'], gates['double-tempo'], gates['short-gate']):
            self.assertTrue(0.47 <= fast/slow <= 0.53)
            self.assertTrue(0.59 <= short/slow <= 0.67)
        for slow, fast, short in zip(intervals['baseline'], intervals['double-tempo'], intervals['short-gate']):
            self.assertTrue(0.47 <= fast/slow <= 0.53)
            self.assertTrue(0.97 <= short/slow <= 1.03)
        self.assertLess(self.reports['short-gate']['pcm']['nonzero_frames'],
                        self.reports['baseline']['pcm']['nonzero_frames'])

    def test_articulation_changes_and_inheritance(self):
        track = bytes((16,127,0x98,16,63,0x99,0xA4,16,127,0x98,8,0xC9,0))
        report = native(PROBE, track, 96)
        align(report, track)
        self.assertEqual([event['articulation'] for event in report['events']], [127,63,63,127,127])

    def test_rest_only_silence(self):
        for art in (63,127):
            track = bytes((16,art,0xC9,8,0xC9,0))
            report = native(PROBE, track, 96)
            align(report, track)
            self.assertEqual(report['keyoff_half_cycles'], [])
            self.assertEqual(report['pcm']['nonzero_frames'], 0)

    def test_profile_rejects_before_audio(self):
        tracks = [bytes((duration,127,0x98,0)) for duration in (1,2,8,15,17,127)]
        tracks += [bytes((16,art,0x98,0)) for art in (0,62,64,126)]
        tracks += [bytes((16,127,0x98,8,63,0x99,0)), b'\0', bytes(129),
                   bytes((16,127,0x98,0x80,0)), bytes((16,127,0x98)),
                   bytes((16,127,*([0x98]*17),0))]
        for track in tracks:
            with self.subTest(track=track.hex()):
                report = native(PROBE, track, 96)
                self.assertEqual(report['status'], 0xE2)
                self.assertEqual(report['keyons'], [])
                self.assertEqual(report['keyoff_half_cycles'], [])
                self.assertEqual(report['pcm']['nonzero_frames'], 0)
        self.assertEqual(native(PROBE, fixture('short-gate'), 192)['status'], 0xE2)
        for tempo in (0,95,193,255):
            self.assertEqual(native(PROBE, fixture('baseline'), tempo)['status'], 0xE1)

    def test_reports_fail_closed(self):
        report = self.reports['baseline']
        for malformed in (None, {}, {**report, 'playback': True}, {**report, 'keyoff_half_cycles': []}):
            with self.assertRaises(ValueError):
                validate(malformed)
        for off in (True, report['keyons'][0]['half_cycle'], report['events'][2]['half_cycle']):
            changed = copy.deepcopy(report)
            changed['keyoff_half_cycles'][0] = off
            with self.assertRaises(ValueError):
                validate(changed)

    def test_reference_tolerance_fails_closed(self):
        references = []
        for model in ('sgb','sgb2'):
            for case, tempo, interval, gate in (('baseline',96,87000,74000),
                    ('double-tempo',192,43500,36000), ('short-gate',96,87000,47000)):
                references.append({'model': model,'case': case,'tempo': tempo,
                                   'onset_intervals_spc_cycles': [interval]*2,'gate_spc_cycles': [gate]*3})
        self.assertEqual(len(compare(self.reports, references)),6)
        with self.assertRaises(ValueError):
            compare(self.reports,references[:-1])
        with self.assertRaises(ValueError):
            compare({'baseline': self.reports['baseline']},references)
        changed = copy.deepcopy(references)
        # Keep the reference's valid baseline range, but exceed the native tolerance.
        for result in changed:
            if result['case'] == 'baseline':
                result['gate_spc_cycles'] = [75900]*3
        with self.assertRaises(ValueError):
            compare(self.reports,changed)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--probe', type=Path, required=True)
    args, remaining = parser.parse_known_args()
    PROBE = args.probe.resolve()
    unittest.main(argv=[sys.argv[0], *remaining])
