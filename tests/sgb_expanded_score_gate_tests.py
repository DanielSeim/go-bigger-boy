#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""ROM-free measured-profile rendering, mixed state, lifecycle and rejection checks."""
from copy import deepcopy
import argparse
from pathlib import Path
import sys
import unittest

sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'scripts'))
from check_sgb_expanded_score_gate_reference import CASES, compare, fixture
from check_sgb_score_gate_reference import align, native, validate

PROBE = None


def matrix():
    observations = {
        'control': (87000,74000), 'half-duration': (43000,31000),
        'long-duration': (130000,118000), 'half-duration-short': (43000,20000),
        'long-duration-short': (130000,76000), 'middle-tempo': (65000,55000),
        'middle-tempo-short': (65000,34500), 'double-tempo-short': (43000,22500),
        'double-tempo': (43500,36000), 'short-gate': (87000,47000),
    }
    return [{'model': model,'case': case,'tempo': tempo,'articulation': art,'duration': duration,
             'onset_intervals_spc_cycles': [observations[case][0]]*2,
             'gate_spc_cycles': [observations[case][1]]*3}
            for model in ('sgb','sgb2') for case,(tempo,art,duration) in CASES.items()]


class ExpandedGateTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.reports={case: native(PROBE,fixture(case),parameters[0]) for case,parameters in CASES.items()}

    def test_all_measured_profiles_render_and_align(self):
        for case,report in self.reports.items():
            with self.subTest(case=case):
                align(report,fixture(case))
                self.assertEqual(report['status'],2)
                self.assertGreater(report['pcm']['nonzero_frames'],100)
                self.assertGreaterEqual(report['pcm']['quiet_tail_frames'],64)
                self.assertEqual([event['pitch'] for event in report['keyons']],[1068,1132,2140])
                self.assertEqual(len(report['keyoff_half_cycles']),3)
        self.assertEqual(len(compare(self.reports,matrix())),20)

    def test_short_articulation_preserves_spacing_and_reduces_pcm(self):
        for normal,short in (('control','short-gate'), ('half-duration','half-duration-short'),
                             ('long-duration','long-duration-short'), ('middle-tempo','middle-tempo-short'),
                             ('double-tempo','double-tempo-short')):
            a,b=self.reports[normal],self.reports[short]
            self.assertLess(b['pcm']['nonzero_frames'],a['pcm']['nonzero_frames'])
            for first,second in zip(a['keyons'],b['keyons']):
                self.assertEqual(first['tick'],second['tick'])
            for gate_a,gate_b in zip(
                    [(off-on['half_cycle'])/2 for on,off in zip(a['keyons'],a['keyoff_half_cycles'])],
                    [(off-on['half_cycle'])/2 for on,off in zip(b['keyons'],b['keyoff_half_cycles'])]):
                self.assertLess(gate_b,gate_a)

    def test_mixed_duration_articulation_and_inheritance(self):
        track=bytes((8,127,0x98,0x99,24,63,0xA4,0x98,16,127,0x99,8,0xC9,0))
        report=native(PROBE,track,96)
        align(report,track)
        notes=[event for event in report['events'] if event['opcode']!=0xC9]
        self.assertEqual([event['duration'] for event in notes],[8,8,24,24,16])
        self.assertEqual([event['articulation'] for event in notes],[127,127,63,63,127])
        self.assertEqual(len(report['keyoff_half_cycles']),5)

    def test_middle_tempo_rest_and_maximum_event_count(self):
        track=bytes((16,127,*([0x98]*16),0))
        report=native(PROBE,track,128)
        align(report,track)
        self.assertEqual(report['end_tick'],256)
        self.assertEqual(len(report['keyoff_half_cycles']),16)
        track=bytes((16,63,0xC9,8,0xC9,0))
        report=native(PROBE,track,128)
        align(report,track)
        self.assertEqual(report['pcm']['nonzero_frames'],0)
        self.assertEqual(report['keyons'],[])

    def test_unknown_profiles_reject_before_audio(self):
        profiles=[(96,127,duration) for duration in (1,2,7,9,15,17,23,25,127)]
        profiles += [(tempo,art,duration) for tempo in (128,192) for art in (63,127) for duration in (8,24)]
        profiles += [(tempo,art,16) for tempo in (96,128,192) for art in (0,62,64,126)]
        for tempo,art,duration in profiles:
            with self.subTest(profile=(tempo,art,duration)):
                # Unsupported event follows a supported prefix: full validation must reject it all.
                track=bytes((16,127,0x98,duration,art,0x99,0))
                report=native(PROBE,track,tempo)
                self.assertEqual(report['status'],0xE2)
                self.assertEqual(report['events'],[])
                self.assertEqual(report['keyons'],[])
                self.assertEqual(report['keyoff_half_cycles'],[])
                self.assertEqual(report['pcm']['nonzero_frames'],0)
        for tempo in (0,95,127,129,191,193,255):
            self.assertEqual(native(PROBE,fixture('control'),tempo)['status'],0xE1)

    def test_reports_and_reference_bounds_fail_closed(self):
        references=matrix()
        for bad in (references[:-1],references+[references[0]],references[:2]*10):
            with self.assertRaises(ValueError):
                compare(self.reports,bad)
        with self.assertRaises(ValueError):
            compare({'control':self.reports['control']},references)
        changed=deepcopy(references)
        for row in changed:
            if row['case']=='half-duration':
                row['gate_spc_cycles']=[33000]*3
        with self.assertRaises(ValueError):
            compare(self.reports,changed)
        changed=deepcopy(self.reports['half-duration'])
        changed['tempo']=128
        with self.assertRaises(ValueError):
            validate(changed)
        changed=deepcopy(self.reports['middle-tempo'])
        changed['keyoff_half_cycles'][0]=changed['keyons'][0]['half_cycle']
        with self.assertRaises(ValueError):
            validate(changed)


if __name__=='__main__':
    parser=argparse.ArgumentParser()
    parser.add_argument('--probe',type=Path,required=True)
    args,remaining=parser.parse_known_args()
    PROBE=args.probe.resolve()
    unittest.main(argv=[sys.argv[0],*remaining])
