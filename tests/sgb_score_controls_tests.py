#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""ROM-free bounded native instrument/volume, owned PCM and rejection checks."""
from copy import deepcopy
import argparse
import hashlib
from pathlib import Path
import sys
import unittest

sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'scripts'))
from build_sgb_score_controls import build
from check_sgb_score_controls_reference import align,compare,fixture,native,validate
from check_sgb_pitch_reference import EXPECTED

PROBE=None


def matrix():
    references=[]
    for model in ('sgb','sgb2'):
        for instrument in (2,10):
            pitches,setup=EXPECTED[instrument]
            references.append({'model':model,'native_case':f'instrument-{instrument}',
                               'notes':[{'pitch':pitch,**dict(zip(('srcn','adsr1','adsr2','gain'),setup))}
                                        for pitch in pitches]})
        references.append({'model':model,'native_case':'volume','notes':[
            {'pitch':1068,'srcn':2,'voll':volume,'volr':volume} for volume in (7,1,1)]})
    return references


class ControlsTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.reports={case:native(PROBE,fixture(case)) for case in ('instrument-2','instrument-10','volume')}

    def test_reproducible(self):
        self.assertEqual(build(),build())
        self.assertEqual(hashlib.sha256(build()).hexdigest(),
                         '2367ca355562edd645a0c753dcc574673427c0ed25c8c5b101224ecdcf6b5a6d')

    def test_register_reference_contracts(self):
        for case,report in self.reports.items():
            align(report,fixture(case))
            self.assertGreater(report['pcm']['nonzero_frames'],100)
            self.assertGreaterEqual(report['pcm']['quiet_tail_frames'],64)
        self.assertEqual(len(compare(self.reports,matrix())),6)
        self.assertEqual([on['voll'] for on in self.reports['volume']['keyons']],[7,1,1])

    def test_changes_defaults_and_inheritance(self):
        track=bytes((16,127,0x98,0xE0,10,0x99,0xA4,0xE0,2,0xE5,80,0x98,
                     0xE5,160,0xED,64,0x99,0xED,127,0xA4,8,0xC9,0))
        report=native(PROBE,track)
        align(report,track)
        self.assertEqual([on['instrument'] for on in report['keyons']],[2,10,10,2,2,2])
        self.assertEqual([on['voll'] for on in report['keyons']],[7,7,7,1,1,7])

    def test_reduced_volume_reduces_owned_pcm(self):
        reports=[]
        for song,volume in ((160,127),(80,127),(160,64)):
            track=bytes((0xE0,2,0xE5,song,0xED,volume,16,127,0x98,8,0xC9,0))
            report=native(PROBE,track)
            align(report,track)
            reports.append(report)
        for report in reports[1:]:
            self.assertGreater(report['pcm']['nonzero_frames'],100)
            self.assertLess(report['pcm']['peak'],reports[0]['pcm']['peak'])
            self.assertNotEqual(report['pcm']['fnv1a64'],reports[0]['pcm']['fnv1a64'])

    def test_rest_only_controls_are_silent(self):
        track=bytes((0xE0,10,0xE5,80,0xED,64,16,127,0xC9,8,0xC9,0))
        report=native(PROBE,track)
        align(report,track)
        self.assertEqual(report['pcm']['nonzero_frames'],0)
        self.assertEqual(report['keyons'],[])

    def test_invalid_values_and_combinations_reject_entire_stream(self):
        tails=[bytes((0xE0,value,0x98,0)) for value in (0,1,3,9,11,255)]
        tails += [bytes((opcode,value,0x98,0)) for opcode,values in
                  ((0xE5,(0,79,81,159,161,255)),(0xED,(0,63,65,126,128,255))) for value in values]
        tails += [bytes((0xE5,80,0xED,64,0x98,0)),bytes((0xE0,10,0xE5,80,0x98,0)),
                  bytes((0xE0,10,0xED,64,0x98,0)),bytes((0xE1,10,0x98,0)),
                  bytes((0xE7,96,0x98,0)),bytes((0xE0,)),bytes((0xE5,)),bytes((0xED,)),
                  bytes((8,127,0x98,0)),bytes((16,63,0x98,0)),bytes((16,127,0x80,0))]
        for tail in tails:
            with self.subTest(tail=tail.hex()):
                report=native(PROBE,bytes((16,127,0x98))+tail)
                self.assertEqual(report['status'],0xE2)
                self.assertEqual(report['events'],[])
                self.assertEqual(report['pcm']['nonzero_frames'],0)
        for tempo in (0,95,128,192,255):
            self.assertEqual(native(PROBE,fixture('instrument-2'),tempo)['status'],0xE1)

    def test_zero_time_controls_remain_bounded(self):
        # A control-only stream cannot form an empty pattern, regardless of valid operands.
        track=bytes((0xE0,2))*63+b'\0'
        self.assertEqual(native(PROBE,track)['status'],0xE2)
        self.assertEqual(native(PROBE,bytes(129))['status'],0xE2)
        track=bytes((16,127,*([0x98]*17),0))
        self.assertEqual(native(PROBE,track)['status'],0xE2)

    def test_reports_and_reference_matrices_fail_closed(self):
        for bad in (matrix()[:-1],matrix()+[matrix()[0]],matrix()[:2]*3):
            with self.assertRaises(ValueError):compare(self.reports,bad)
        changed=deepcopy(matrix())
        changed[0]['notes'][0]['gain']=0
        with self.assertRaises(ValueError):compare(self.reports,changed)
        for field,value in (('srcn',0),('adsr1',0),('instrument',True),('voll',80)):
            report=deepcopy(self.reports['instrument-2'])
            report['keyons'][0][field]=value
            with self.assertRaises(ValueError):validate(report)
        report=deepcopy(self.reports['volume'])
        report['keyons'][1]['song_volume']=160
        with self.assertRaises(ValueError):align(report,fixture('volume'))


if __name__=='__main__':
    parser=argparse.ArgumentParser()
    parser.add_argument('--probe',type=Path,required=True)
    args,remaining=parser.parse_known_args()
    PROBE=args.probe.resolve()
    unittest.main(argv=[sys.argv[0],*remaining])
