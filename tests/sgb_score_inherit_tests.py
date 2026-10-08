#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Execution-order mix carry, rejection, reference contracts and lifecycle."""
import argparse
import copy
import hashlib
import io
from pathlib import Path
import struct
import sys
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'scripts'))
from build_sgb_score_inherit import build,source
from build_sgb_score_reselect import build as prior_build
from build_sgb_prototype import assemble
from build_sgb_inherit_fixture import bank,build as fixture_build,expected,CASES
from check_sgb_score_inherit_reference import native,align,validate,compare,observe
from check_sgb_score_polygate_reference import native as gate_native,native_gates
from check_sgb_score_gate_reference import PULSES
from sgb_score_sparse_tests import authored

PROBE=None


def trace(case):
    rows=['kind,master_clock,spc_cycle,pcm_sample,address,value\n','D,0,0,0,61,0\n']
    for index,edge in enumerate(expected(case)):
        cycle=1000+index*87500
        for voice in edge['voices']:
            values=(*voice['volumes'],voice['pitch']&255,voice['pitch']>>8,2,0x8F,0x6F,0xB8)
            rows.extend(f'D,0,{cycle},0,{16*voice["voice"]+reg},{value}\n' for reg,value in enumerate(values))
        rows.extend((f'D,0,{cycle},0,76,{edge["mask"]}\n',f'D,0,{cycle+70000},0,92,{edge["mask"]}\n'))
    return ''.join(rows)


class InheritTests(unittest.TestCase):
    def render(self,patterns,tempo=96):
        data=authored(patterns);report=native(PROBE,data,tempo);align(report,data)
        self.assertEqual(report['status'],2)
        self.assertTrue(report['cache_guards_equal']);self.assertTrue(report['source_unmodified'])
        return report

    def reject(self,data):
        report=native(PROBE,data)
        self.assertNotEqual(report['status'],2)
        self.assertEqual(report['events'],[]);self.assertEqual(report['keyons'],[])
        self.assertEqual(report['instrument_writes'],[])
        self.assertTrue(report['source_unmodified']);self.assertTrue(report['cache_guards_equal'])

    def test_reproducible_prior_and_owned_sample(self):
        self.assertEqual(build(),build())
        self.assertEqual(len(build()),3797)
        self.assertEqual(hashlib.sha256(build()).hexdigest(),'4808a273bbad7aeacec7174038b33e37b7cc615e8c3303a844a07bf8fa19ef15')
        self.assertEqual(hashlib.sha256(prior_build()).hexdigest(),'879bb92d5633788c11c6afc3c34f575a124b205e2f65a3aa97c216eb43b60277')
        self.assertEqual(build()[0x1008-0x800:0x1019-0x800],prior_build()[0x1008-0x800:0x1019-0x800])

    def test_owned_onsets_and_inactive_channel_resume(self):
        for case in CASES:
            for art in (63,127):
                report=native(PROBE,bank(art,case));align(report,bank(art,case))
                self.assertEqual(report['pattern_ticks'],[0,32,64,96])
                self.assertEqual(report['pattern_masks'],[12,4,8,12])
                for actual,wanted in zip(report['keyons'],expected(case)):
                    for voice in wanted['voices']:
                        self.assertEqual(actual['volumes'][voice['voice']-2],voice['volumes'])

    def test_default_and_per_channel_partial_overrides(self):
        note=(16,63,0x98,0)
        report=self.render([((0xED,64,*note),note),(None,note),(note,None),((0xED,127,*note),note)])
        self.assertEqual([(e['channel'],e['track_volume']) for e in report['events']],[(2,64),(3,127),(3,127),(2,64),(2,127),(3,127)])

    def test_clipped_tail_controls_do_not_carry(self):
        short=(16,63,0x98,0)
        long=(24,63,0x98,0xE1,10,0xED,64,0x99,0)
        report=self.render([(short,(0xE1,0,0xED,127,*long)),(short,short)])
        self.assertEqual(report['end_tick'],32)
        self.assertEqual([e['pan'] for e in report['events'] if e['channel']==3],[0,0])
        self.assertEqual([e['track_volume'] for e in report['events'] if e['channel']==3],[127,127])
        self.assertEqual([e['volumes'] for e in report['events'] if e['channel']==3],[[0,11],[0,11]])

    def test_last_executed_control_wins(self):
        note=(16,63,0x98,0)
        for channel in (2,3):
            first=(0xE1,0,16,63,0x98,0xE1,10,0xED,64,0x99,0)
            patterns=[(first,None),(note,None)] if channel==2 else [(None,first),(None,note)]
            report=self.render(patterns)
            self.assertEqual([e['volumes'] for e in report['events']],[[0,11],[1,1],[1,1]])

    def test_rest_updates_inherited_controls_without_kon(self):
        rest=(0xE1,20,0xED,64,16,63,0xC9,0)
        note=(0xE1,10,16,63,0x98,0)
        report=self.render([(rest,None),(None,(16,63,0xC9,0)),(note,None)])
        self.assertEqual(len(report['keyons']),1)
        self.assertEqual(report['events'][-1]['track_volume'],64)
        self.assertEqual(report['events'][-1]['volumes'],[1,1])

    def test_inherited_unmeasured_pairs_reject_before_audio(self):
        rest=(0xE1,20,0xED,64,16,63,0xC9,0)
        note=(16,63,0x98,0)
        self.reject(authored([(rest,None),(note,None)]))
        reduced=(0xE5,80,16,63,0x98,0)
        self.reject(authored([(reduced,None),((0xED,64,*note),None)]))

    def test_trailing_controls_are_explicitly_unqualified(self):
        for control in ((0xE1,0),(0xED,64)):
            self.reject(authored([((16,63,0x98,*control,0),None)]))

    def test_call_control_return_carries_to_next_pattern(self):
        note=(16,63,0x98,0)
        data=bytearray(authored([((16,63,0x98,0xEF,0,0,2,0xA4,0),None),(note,None)]))
        target=0x2B00+len(data)
        call=data.index(bytes((0xEF,0,0,2)))
        struct.pack_into('<H',data,call+1,target)
        data.extend((0xE1,10,0xED,64,0x99,0))
        report=native(PROBE,bytes(data));align(report,bytes(data))
        self.assertEqual([e['volumes'] for e in report['events']],[[7,7],[1,1],[1,1],[1,1],[1,1]])

    def test_all_gate_profiles_and_actual_prefix_writes(self):
        for tempo,art,duration in PULSES:
            first=(0xE0,2,0xE1,10,0xED,64,duration,art,0x98,0x99,0)
            later=(0xE0,2,0xE0,2,duration,art,0x98,0x99,0)
            report=self.render([(first,first),(later,None),(None,later),(later,later)],tempo)
            self.assertTrue(all(e['volumes']==[1,1] for e in report['events']))
            self.assertEqual(len(report['instrument_writes']),40)
            self.assertTrue(all(g['cause']==1 for g in native_gates(report)))

    def test_full_event_cache_and_save_count_carry(self):
        first=(0xED,64,16,63,*([0x98]*8),0)
        later=(16,63,*([0x98]*8),0)
        report=self.render([(first,first),*( [(later,later)]*3 )])
        self.assertEqual(len(report['events']),64)
        self.assertEqual(report['end_tick'],512)
        self.assertTrue(all(e['track_volume']==64 for e in report['events']))

    def test_missing_live_reinitialization_is_detected(self):
        text=source().replace('multi_begin:\n    call inherit_begin\n','multi_begin:\n    mov a, $64\n    bne inherit_skip_reset\n    call inherit_begin\ninherit_skip_reset:\n')
        note=(16,63,0x98,0)
        data=authored([(note,None),((0xED,64,*note),None)])
        image=bytes(assemble(text,'spc',0x0800))
        report=gate_native(PROBE,data,builder=lambda:image,validator=validate,output_bound=131072)
        with self.assertRaises(ValueError):align(report,data)

    def test_prior_builder_fails_inheritance_oracle(self):
        report=gate_native(PROBE,bank(),builder=prior_build,validator=validate,output_bound=131072)
        with self.assertRaises(ValueError):align(report,bank())

    def test_strict_reference_matrix_and_mutation_guards(self):
        candidates={};refs=[]
        for case in CASES:
            observed=observe(io.StringIO(trace(case)),case)
            for art in (63,127):
                report=native(PROBE,bank(art,case));candidates[f'{case}-{art}']=report
                for model in ('sgb','sgb2'):
                    reference=copy.deepcopy(observed)
                    for gate,actual in zip(reference['note_gates'],native_gates(report)):gate['gate_spc_cycles']=int(actual['gate_spc_cycles'])
                    reference.update(case=case,articulation=art,model=model)
                    reference['onset_intervals_spc_cycles']=[int((b['half_cycle']-a['half_cycle'])/2) for a,b in zip(report['keyons'],report['keyons'][1:])]
                    refs.append(reference)
        self.assertEqual(len(compare(candidates,refs)),8)
        for mutation in ('duplicate','bool','volume','gate','interval'):
            bad=copy.deepcopy(refs)
            if mutation=='duplicate':bad[-1]=bad[0]
            elif mutation=='bool':bad[0]['keyons'][0]['mask']=True
            elif mutation=='volume':bad[0]['keyons'][2]['voices'][0]['volumes']=[7,7]
            elif mutation=='gate':bad[0]['note_gates'][0]['gate_spc_cycles']+=6000
            else:bad[0]['onset_intervals_spc_cycles'][0]+=6000
            with self.assertRaises(ValueError):compare(candidates,bad)
        with self.assertRaises(ValueError):observe(io.StringIO(trace('pan-hold').replace(',0,32,11\n',',0,32,7\n')),'pan-hold')


if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('--probe',type=Path,required=True)
    args,remaining=parser.parse_known_args();PROBE=args.probe.resolve()
    unittest.main(argv=[sys.argv[0],*remaining])
