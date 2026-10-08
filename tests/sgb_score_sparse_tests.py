#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Solo tracks, sparse transitions and actual DSP/lifecycle ownership."""
import argparse
import copy
import hashlib
import io
from pathlib import Path
import struct
import sys
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'scripts'))
import sgb_score_list_tests as lists
from build_sgb_score_sparse import build
from build_sgb_score_list import build as prior_build
from build_sgb_sparse_fixture import bank,CASES,expected
from check_sgb_score_sparse_reference import native,align,validate,compare,observe
from check_sgb_score_polygate_reference import native_gates
from check_sgb_score_gate_reference import PULSES

for module in (lists,lists.banks,lists.banks.envelope,lists.banks.envelope.mix):
    module.native,module.align,module.validate,module.compare=native,align,validate,compare


def authored(patterns):
    data=bytearray(64+16*len(patterns))
    struct.pack_into('<H',data,0,0x2B10)
    for index,pair in enumerate(patterns):
        table=64+16*index
        struct.pack_into('<H',data,16+2*index,0x2B00+table)
        for channel,stream in zip((2,3),pair):
            if stream is None: continue
            struct.pack_into('<H',data,table+2*channel,0x2B00+len(data))
            data.extend(stream)
    return bytes(data)


def trace(case):
    rows=['kind,master_clock,spc_cycle,pcm_sample,address,value\n','D,0,0,0,61,0\n']
    for index,edge in enumerate(expected(case)):
        cycle=1000+index*87500
        for voice in edge['voices']:
            values=(*voice['volumes'],voice['pitch']&255,voice['pitch']>>8,2,0x8F,0x6F,0xB8)
            for register,value in enumerate(values): rows.append(f'D,0,{cycle},0,{16*voice["voice"]+register},{value}\n')
        rows.extend((f'D,0,{cycle},0,76,{edge["mask"]}\n',f'D,0,{cycle+70000},0,92,{edge["mask"]}\n'))
    return ''.join(rows)


class SparseTests(lists.ListTests):
    def render(self,patterns):
        data=authored(patterns)
        report=native(lists.banks.envelope.mix.PROBE,data)
        align(report,data)
        return report

    def test_reproducible_and_prior_unchanged(self):
        self.assertEqual(build(),build())
        self.assertEqual(len(build()),3345)
        self.assertEqual(hashlib.sha256(build()).hexdigest(),'a95e890efb333306c0c87b17e742eaee51178eab498b1973ddc436774c37a187')
        self.assertEqual(hashlib.sha256(prior_build()).hexdigest(),'b3b3441f7d2c32b83c042373d17226f7d81d55616a56c0b4590184f411880531')
        self.assertEqual(build()[0x1008-0x800:0x1019-0x800],prior_build()[0x1008-0x800:0x1019-0x800])

    def test_declared_end_pointer_and_word_boundaries_reject_silently(self):
        for size in (1619,2048):
            good=lists.banks.bank(63,size=size)
            for position in (0,0xFF,0x101,0x1FF,0x201,0x3FF,0x401,0x2FF,0x4FF):
                for target in (0,0x2AFF,0x2B00+size,0x3300,0x4000,0xFFFF):
                    data=bytearray(good);struct.pack_into('<H',data,position,target)
                    if target==0 and position in (0x101,0x1FF,0x201,0x3FF,0x401):
                        report=native(lists.banks.envelope.mix.PROBE,bytes(data));align(report,bytes(data))
                    else: self.reject(bytes(data))
            for cut in (1,2,3,size-1,size-8,0xFF,0x100,0x101,0x102,0x103,0x104,
                        0x1FF,0x200,0x20A,0x2FF,0x300,0x301,0x302,0x3FF,0x400,0x40A,
                        0x4FF,0x500,0x501,0x502,0x5FF,0x600,0x607): self.reject(good[:cut])
        self.reject(lists.banks.bank()+b'\0')

    def test_empty_fifth_truncated_and_late_malformed_patterns_reject(self):
        note=(16,63,0x98,0)
        self.reject(authored([]))
        self.reject(authored([(note,None)]*5))
        for empty in range(4):
            patterns=[(note,None)]*4;patterns[empty]=(None,None)
            self.reject(authored(patterns))
        for bad in ((0,), (0x98,0), (17,63,0x98,0), (0xEF,0xFF,0xFF,1,0),
                    (16,63,0x98,0xE1), (0xE5,160,16,63,0x98,0)):
            for pair in ((bad,None),(None,bad)):
                self.reject(authored([(note,None)]*3+[pair]))
        good=authored([(note,None),(None,note),(note,note),(None,note)])
        for cut in (17,18,19,23,24,25,len(good)-1): self.reject(good[:cut])
        for position,target in ((24,0x2B40),(0,0),(22,0x3300),(118,0x4000)):
            data=bytearray(good);struct.pack_into('<H',data,position,target);self.reject(bytes(data))

    def test_each_solo_voice_with_all_gate_profiles_and_one_event(self):
        for tempo,art,duration in PULSES:
            for voice in (2,3):
                note=(duration,art,0x98,0)
                data=authored([(note,None) if voice==2 else (None,note)])
                report=native(lists.banks.envelope.mix.PROBE,data,tempo);align(report,data)
                self.assertEqual(report['pattern_masks'],[1<<voice])
                self.assertEqual(len(report['events']),1)
                self.assertEqual([edge['mask'] for edge in report['keyons']],[1<<voice])
                self.assertEqual([edge['voice'] for edge in report['envelopes']],[voice])

    def test_solo_rests_endpoints_and_stereo_ownership(self):
        for voice in (2,3):
            for pan,quiet in ((0,'left'),(20,'right')):
                note=(0xE1,pan,16,63,0x98,0xA4,0)
                report=self.render([(note,None) if voice==2 else (None,note)]*4)
                self.assertEqual(report['pcm'][quiet+'_nonzero_frames'],0)
                self.assertEqual(report['pcm'][quiet+'_peak'],0)
            rest=(127,127,*([0xC9]*4),0)
            report=self.render([(rest,None) if voice==2 else (None,rest)]*4)
            self.assertEqual(report['end_tick'],2032)
            self.assertEqual(report['keyons'],[])
            self.assertEqual(report['pcm']['peak'],0)

    def test_all_pattern_mask_transitions_and_peer_release(self):
        note=(16,63,0x98,0xA4,0)
        shapes={4:(note,None),8:(None,note),12:(note,note)}
        for first in shapes:
            for second in shapes:
                report=self.render([shapes[first],shapes[second],shapes[first],shapes[second]])
                self.assertEqual(report['pattern_masks'],[first,second,first,second])
                self.assertEqual(report['pattern_ticks'],[0,32,64,96])
                self.assertEqual([edge['mask'] for edge in report['keyons']], [mask for mask in (first,second,first,second) for _ in range(2)])
        # A long paired note must be clipped when its rest peer ends, before
        # solo voice 2 starts. The outgoing voice 3 has no later KON.
        report=self.render([((16,127,0xC9,0),(24,127,0x99,0)),(note,None)])
        self.assertEqual(report['keyoffs'][0]['cause'],0)
        self.assertEqual(report['keyoffs'][0]['mask'],8)
        self.assertEqual(report['pattern_masks'],[12,4])

    def test_sparse_calls_page_crossing_and_metadata_guards(self):
        for case in CASES:
            for art in (63,127):
                data=bank(art,case);report=native(lists.banks.envelope.mix.PROBE,data);align(report,data)
                self.assertEqual(report['end_tick'],128)
                for masks in ([],[True],report['pattern_masks']+[4],[0]*len(report['pattern_masks']),[12]*len(report['pattern_masks'])):
                    if masks==report['pattern_masks']: continue
                    changed=copy.deepcopy(report);changed['pattern_masks']=masks
                    with self.assertRaises(ValueError): align(changed,data)
        note=(16,63,0x98,0)
        for voice in (0,1,4,5,6,7):
            data=bytearray(authored([(note,None)]));struct.pack_into('<H',data,64+2*voice,0x2B00+80)
            self.reject(bytes(data))

    def test_shared_song_volume_in_first_active_prefix_only(self):
        note=(16,63,0x98,0)
        reduced=(0xE5,80,*note)
        for first in ((reduced,None),(None,reduced)):
            report=self.render([first,(note,note),(None,note),(note,None)])
            self.assertTrue(all(event['song_volume']==80 and event['volumes']==[1,1] for event in report['events']))
        self.reject(authored([(note,reduced)]))
        self.reject(authored([(None,(0xE5,80,0xE5,80,*note))]))
        self.reject(authored([(None,note),(None,reduced)]))
        self.reject(authored([(None,(16,63,0x98,0xE5,80,0xA4,0))]))

    def test_sparse_observer_and_reference_matrix_guards(self):
        candidates={f'{case}-{art}':native(lists.banks.envelope.mix.PROBE,bank(art,case)) for case in CASES for art in (127,63)}
        references=[]
        for case in CASES:
            self.assertEqual(len(observe(io.StringIO(trace(case)),case)['note_gates']),sum(len(edge['voices']) for edge in expected(case)))
            for art in (127,63):
                report=observe(io.StringIO(trace(case)),case)
                report['note_gates']=[{'voice':edge['voice'],'release_kind':'keyoff','gate_spc_cycles':int(edge['gate_spc_cycles'])} for edge in native_gates(candidates[f'{case}-{art}'])]
                references.extend({**copy.deepcopy(report),'case':case,'articulation':art,'model':model} for model in ('sgb','sgb2'))
            for altered in (trace(case).replace(',0,92,',',0,91,'),trace(case).replace(',0,76,',',0,75,')):
                with self.assertRaises(ValueError): observe(io.StringIO(altered),case)
        self.assertEqual(len(compare(candidates,references)),12)
        for altered in (None,references[:-1],references[:-1]+references[:1]):
            with self.assertRaises(ValueError): compare(candidates,altered)
        for key in ('gate','setup','type','onset','model'):
            altered=copy.deepcopy(references)
            if key=='gate': altered[0]['note_gates'][-1]['gate_spc_cycles']+=6000
            if key=='setup': altered[0]['keyons'][0]['voices'][0]['srcn']=0
            if key=='type': altered[0]['keyons'][0]['mask']=4.0
            if key=='onset': altered[0]['onset_intervals_spc_cycles'][-1]=84000
            if key=='model': altered[-1]['model']='sgb'
            with self.assertRaises(ValueError): compare(candidates,altered)


if __name__=='__main__':
    parser=argparse.ArgumentParser()
    parser.add_argument('--probe',type=Path,required=True)
    args,remaining=parser.parse_known_args()
    lists.banks.envelope.mix.PROBE=args.probe.resolve()
    unittest.main(argv=[sys.argv[0],*remaining])
