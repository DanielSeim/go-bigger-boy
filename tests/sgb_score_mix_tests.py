#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Actual two-voice mix controls, finite-call inheritance and lifecycle."""
import argparse
import copy
import hashlib
import io
from pathlib import Path
import sys
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'scripts'))
from build_sgb_score_mix import build
from build_sgb_score_chromatic import build as prior_build
from build_sgb_mix_fixture import CASES, NOTES, bank, volumes, build as fixture_build
from check_sgb_score_mix_reference import align, compare, native, native_gates, observe, validate, PITCHES
from check_sgb_score_gate_reference import PULSES
from sgb_score_calls_tests import CALL, called
from sgb_score_multi_tests import authored

PROBE = None
HEADER = 'kind,master_clock,spc_cycle,pcm_sample,address,value\n'


def trace(case='pan-calls'):
    rows = ['D,0,0,0,61,0\n']
    for i,(note,pairs) in enumerate(zip(NOTES,volumes(case))):
        tick,pitch = 1000+i*87500,PITCHES[note]
        for voice,pair in zip((2,3),pairs):
            for register,value in enumerate((*pair,pitch & 255,pitch >> 8,2,0x8F,0x6F,0xB8)):
                rows.append(f'D,0,{tick},0,{16*voice+register},{value}\n')
        rows.extend((f'D,0,{tick},0,76,12\n',f'D,0,{tick+70000},0,92,12\n'))
    return HEADER+''.join(rows)


class MixTests(unittest.TestCase):
    def test_reproducible_and_prior_unchanged(self):
        self.assertEqual(build(),build())
        self.assertEqual(hashlib.sha256(build()).hexdigest(),'210fec34a40227286ae3bc5c6006b43580cb4d537c228c2fea4c0f17d28f6667')
        self.assertEqual(hashlib.sha256(prior_build()).hexdigest(),'d9bec07d99181ca2232515a181f6e3fc7693eab6bdaf47a1d157c2d151b5b783')
        for case in CASES:
            for art in (63,127):
                self.assertEqual(len(bank(art,case)),125)
                self.assertEqual(fixture_build(art,case),fixture_build(art,case))
        for art in (True,63.0,64,0):
            with self.assertRaises(ValueError):
                bank(art)

    def test_measured_calls_repeats_and_returns(self):
        for case in CASES:
            for art in (63,127):
                data=bank(art,case)
                r=native(PROBE,data)
                align(r,data)
                self.assertEqual([e['volumes'] for e in r['keyons']],volumes(case))
                self.assertEqual(r['end_tick'],128)
                self.assertEqual(len(native_gates(r)),16)
                self.assertTrue(all(e['cause']==1 for e in r['keyoffs']))

    def test_controls_with_every_gate_profile_and_chromatic_pitch(self):
        for tempo,art,duration in PULSES:
            stream=(0xE1,10,16,art,0x98,0xED,64,0xA4,0xED,127,0x9B,0)
            # Duration replaces the inherited initial 16 across all commands.
            stream=stream[:2]+(duration,)+stream[3:]
            r=native(PROBE,authored((stream,)*4),tempo)
            align(r,authored((stream,)*4))
            self.assertEqual([e['volumes'][0] for e in r['keyons']],[[7,7],[1,1],[7,7]]*2)

    def test_endpoints_and_volume_pcm(self):
        peaks=[]
        for prefix in ((0xE1,10),(0xE1,10,0xED,64),(0xE5,80,0xE1,10)):
            stream=(16,127,0x98,0xA4,0)
            data=authored(((*prefix,*stream),stream,stream,stream))
            if 0xE5 not in prefix:
                data=authored(((*prefix,*stream),)*4)
            else:
                data=authored(((*prefix,*stream),(0xE1,10,*stream),(0xE1,10,*stream),(0xE1,10,*stream)))
            r=native(PROBE,data);align(r,data)
            self.assertTrue(r['pcm']['stereo_equal'])
            peaks.append(r['pcm']['peak'])
        self.assertGreater(peaks[0],peaks[1]);self.assertEqual(peaks[1],peaks[2])
        for pan,lane in ((0,'left'),(20,'right')):
            stream=(0xE1,pan,16,63,0x9A,0xA3,0)
            r=native(PROBE,authored((stream,)*4));align(r,authored((stream,)*4))
            self.assertEqual(r['pcm'][lane+'_nonzero_frames'],0)
            self.assertEqual(r['pcm'][lane+'_peak'],0)

    def test_asynchronous_updates_preserve_peer_and_release(self):
        for pan in (0,10,20):
            peerpan=20 if pan==0 else 0 if pan==20 else 10
            stream=(0xE1,pan,8,63,0x98,*CALL,1,0xA4,0)
            peer=(0xE1,peerpan,24,127,0x9C,0xA1,0)
            data=called((stream,peer,peer,stream),(16,63,0x9A,0),relocated=True)
            r=native(PROBE,data);align(r,data)
            self.assertGreater(min(r['peer_checks']),100)
            self.assertGreater(min(r['settled_envelope_checks']),100)
            if pan!=10:
                self.assertGreater(sum(r['settled_peer_nonzero_frames']),64)
        stream=(16,63,0x98,*CALL,2,0xA4,0)
        peer=(24,127,0x9C,0xA1,0xA2,0xA0,0)
        data=called((stream,peer,peer,stream),(0xED,64,0xE1,10,0x9A,0),True)
        r=native(PROBE,data);align(r,data)
        self.assertGreater(min(r['peer_checks']),100)
        self.assertGreater(min(r['settled_envelope_checks']),100)

    def test_all_repeat_counts_and_rest_only(self):
        for count in (1,2,3):
            stream=(16,63,0x98,*CALL,count,0xA4,0)
            data=called((stream,)*4,(0xED,64,0x99,0xED,127,0x9A,0))
            r=native(PROBE,data);align(r,data)
            self.assertEqual(len(r['events']),4*(2+2*count))
        stream=(0xE1,0,0xED,64,8,63,0xC9,0xE1,20,0xC9,0)
        r=native(PROBE,authored((stream,)*4));align(r,authored((stream,)*4))
        self.assertEqual(r['pcm']['peak'],0)
        self.assertEqual(r['keyons'],[])
        rest=(16,127,0xC9,0xC9,0)
        note=(0xE1,10,16,127,0x98,0x99,0)
        r=native(PROBE,authored((rest,note,rest,note)));align(r,authored((rest,note,rest,note)))
        self.assertGreater(r['pcm']['left_nonzero_frames'],0)
        self.assertTrue(r['pcm']['stereo_equal'])

    def test_unmeasured_and_late_invalid_commands_silent(self):
        good=(16,127,0x98,0)
        bads=((0xE1,1),(0xED,63),(0xE5,79),(0xE0,10),(0xE7,95),
              (0xE1,0,0xED,64),(0xE5,80,0xED,64),(0xE5,80,0xE1,20),
              (0xE5,160,0xE5,160))
        for prefix in bads:
            data=authored(((*prefix,*good),good,good,good))
            self.assertEqual(native(PROBE,data)['status'],0xE2)
        for command in ((0xE5,160),(0xE0,2),(0xE7,96),(0xE1,1),(0xED,0),(0xE2,0)):
            bad=(16,127,0x98,*command,0xA4,0)
            for slot in range(4):
                streams=[good]*4;streams[slot]=bad
                self.assertEqual(native(PROBE,authored(streams))['status'],0xE2)
            data=called(((*CALL,1,0),)*4,(16,127,0x98,*command,0xA4,0))
            self.assertEqual(native(PROBE,data)['status'],0xE2)
        data=authored((good,(0xE5,160,*good),good,good))
        self.assertEqual(native(PROBE,data)['status'],0xE2)

    def test_control_expansion_budget_and_truncated_banks(self):
        stream=(*CALL,3,0)
        # Thirty executed body controls plus two/three caller controls exercise
        # the exact 32-command acceptance boundary, including repeated bodies.
        for prefix,accepted in (((0xE1,10,0xED,127),True),((0xE1,10,0xED,127,0xE0,2),False)):
            data=called(((*prefix,*stream),stream,stream,stream),(*(0xE1,10)*10,16,63,0x98,0))
            r=native(PROBE,data)
            if accepted:
                align(r,data)
            else:
                self.assertEqual(r['status'],0xE2)
        for case in CASES:
            data=bank(63,case)
            for size in range(1,len(data)):
                self.assertEqual(native(PROBE,data[:size])['status'],0xE2)

    def test_reference_observer_guards(self):
        data=trace()
        self.assertEqual(len(observe(io.StringIO(data+'R,0,2000000,0,10,private sentinel\n'))['note_gates']),16)
        for changed in (HEADER,data.replace('D,0,1000,0,32,11\n','D,0,1000,0,32,10\n'),
                        data.replace('D,0,1000,0,34,44\n','D,0,1000,0,34,45\n'),
                        data.replace(',0,76,12\n',',0,76,4\n'),data.replace('D,0,71000,0,92,12\n','')):
            with self.assertRaises(ValueError):
                observe(io.StringIO(changed))

    def test_reference_and_native_report_guards(self):
        candidates={f'{case}-{art}':native(PROBE,bank(art,case)) for case in CASES for art in (127,63)}
        refs=[]
        for case in CASES:
            for art in (127,63):
                r=observe(io.StringIO(trace(case)),case)
                r['note_gates']=[{'voice':e['voice'],'release_kind':'keyoff','gate_spc_cycles':int(e['gate_spc_cycles'])}
                                 for e in native_gates(candidates[f'{case}-{art}'])]
                refs.extend({**copy.deepcopy(r),'case':case,'articulation':art,'model':model} for model in ('sgb','sgb2'))
        self.assertEqual(len(compare(candidates,refs)),8)
        for bad in (None,refs[:-1],refs[:-1]+refs[:1]):
            with self.assertRaises(ValueError):
                compare(candidates,bad)
        for target in ('event','edge','peer','type','stereo','envelope'):
            bad=copy.deepcopy(candidates['pan-calls-127'])
            if target=='event': bad['events'][2]['volumes'][0]+=1
            if target=='edge': bad['keyons'][1]['volumes'][0][0]+=1
            if target=='peer': bad['keyoffs'][0]['volumes'][1][0]+=1
            if target=='type': bad['events'][0]['pan']=20.0
            if target=='stereo': bad['pcm']['left_peak']=40000
            if target=='envelope': bad['settled_envelope_checks'][0]=True
            with self.assertRaises(ValueError): validate(bad)
        for key in ('volume','type','gate','model'):
            bad=copy.deepcopy(refs)
            if key=='volume': bad[0]['keyons'][1]['voices'][0]['volumes'][0]+=1
            if key=='type': bad[0]['keyons'][0]['voices'][0]['volumes'][0]=11.0
            if key=='gate': bad[0]['note_gates'][0]['gate_spc_cycles']+=6000
            if key=='model': bad[-1]['model']='sgb'
            with self.assertRaises(ValueError): compare(candidates,bad)


if __name__=='__main__':
    parser=argparse.ArgumentParser()
    parser.add_argument('--probe',type=Path,required=True)
    args,remaining=parser.parse_known_args()
    PROBE=args.probe.resolve()
    unittest.main(argv=[sys.argv[0],*remaining])
