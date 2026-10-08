#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Trailing mix fields, channel priority and explicit clipping diagnostics."""
import argparse
import copy
import hashlib
import io
from pathlib import Path
import sys
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'scripts'))
from build_sgb_score_tail import build,source
from build_sgb_score_timing import build as prior_build
from build_sgb_prototype import assemble
from build_sgb_score_tail_fixture import bank,expected,CASES,QUALIFIED_CASES
from check_sgb_score_tail_reference import native,align,validate,observe,contract,compare
from check_sgb_score_polygate_reference import native as gate_native,native_gates
from check_sgb_score_gate_reference import PULSES
from schedule_sgb_score import schedule
from sgb_score_sparse_tests import authored
PROBE=None


def trace(case,art=127):
    rows=['kind,master_clock,spc_cycle,pcm_sample,address,value\n','D,0,0,0,61,0\n']
    for index,edge in enumerate(expected(case)):
        cycle=1000+index*87500
        for voice in edge['voices']:
            values=(*voice['volumes'],voice['pitch']&255,voice['pitch']>>8,2,0x8F,0x6F,0xB8)
            rows.extend(f'D,0,{cycle},0,{16*voice["voice"]+reg},{value}\n' for reg,value in enumerate(values))
        off=edge['mask']
        if case.startswith('clip') and art==127 and index==1:off=1<<int(case[-1])
        rows.extend((f'D,0,{cycle},0,76,{edge["mask"]}\n',f'D,0,{cycle+70000},0,92,{off}\n'))
    return ''.join(rows)


class TailTests(unittest.TestCase):
    def render(self,patterns,tempo=96):
        data=authored(patterns);report=native(PROBE,data,tempo);align(report,data)
        self.assertEqual(report['status'],2)
        self.assertTrue(report['source_unmodified']);self.assertTrue(report['cache_guards_equal'])
        return report

    def reject(self,data):
        report=native(PROBE,data)
        self.assertNotEqual(report['status'],2)
        for key in ('events','keyons','instrument_writes'):self.assertEqual(report[key],[])
        self.assertTrue(report['source_unmodified']);self.assertTrue(report['cache_guards_equal'])

    def test_reproducible_prior_and_owned_source(self):
        self.assertEqual(build(),build());self.assertEqual(len(build()),4016)
        self.assertEqual(hashlib.sha256(build()).hexdigest(),'f5455964ec1246b4cf6798d0d38c812cdd26dad87924029a3341ac1b3e70caf2')
        self.assertEqual(hashlib.sha256(prior_build()).hexdigest(),'1b6d47611ec2b81c0e07897cd24ecacfd57587afd6d8a887b681ce59d82b4dc3')
        self.assertEqual(build()[0x1008-0x800:0x1019-0x800],prior_build()[0x1008-0x800:0x1019-0x800])

    def test_complete_ending_owned_fixtures(self):
        for case in QUALIFIED_CASES:
            for art in (63,127):
                data=bank(art,case);report=native(PROBE,data);align(report,data)
                self.assertEqual(report['pattern_ticks'],[0,32,64,96])
                for edge,wanted in zip(report['keyons'],expected(case)):
                    for voice in wanted['voices']:self.assertEqual(edge['volumes'][voice['voice']-2],voice['volumes'])

    def test_solo_channel_ends_apply_both_fields(self):
        for channel in (2,3):
            first=(16,63,0x98,0xE1,10,0xED,64,0);later=(0x99,0)
            pairs=[(first,None),(later,None)] if channel==2 else [(None,first),(None,later)]
            report=self.render(pairs)
            self.assertEqual([e['volumes'] for e in report['events']],[[7,7],[1,1]])

    def test_simultaneous_end_skips_channel_three_controls(self):
        first=(16,63,0x98,0xED,64,0);later=(0x99,0)
        report=self.render([(first,first),(later,later)])
        self.assertEqual([e['volumes'] for e in report['events']],[[7,7],[7,7],[1,1],[7,7]])

    def test_clipped_unplayed_tail_does_not_apply_mix(self):
        short=(16,63,0x98,0xED,64,0)
        long=(24,63,0x98,0xED,64,0)
        later=(16,63,0x99,0)
        report=self.render([(short,long),(later,later)])
        self.assertEqual([e['volumes'] for e in report['events']],[[7,7],[7,7],[1,1],[7,7]])
        # This checks control state only: clipped-peer release is not qualified.

    def test_tail_combinations_validate_only_when_a_note_uses_them(self):
        first=(16,63,0x98,0xE1,20,0xED,64,0)
        rest=(0xC9,0);note=(0x98,0);recover=(0xE1,10,0x98,0)
        report=self.render([(first,None),(rest,None),(recover,None)])
        self.assertEqual(report['events'][-1]['volumes'],[1,1])
        self.reject(authored([(first,None),(note,None)]))

    def test_call_tail_returns_and_final_track_control(self):
        data=bytearray(authored([((16,63,0x98,0xEF,0,0,2,0xA4,0xED,127,0),None),((0x99,0),None)]))
        import struct
        pos=data.index(bytes((0xEF,0,0,2)));struct.pack_into('<H',data,pos+1,0x2B00+len(data))
        data.extend((0x99,0xED,64,0))
        report=native(PROBE,bytes(data));align(report,bytes(data))
        self.assertEqual([e['volumes'] for e in report['events']],[[7,7],[7,7],[1,1],[1,1],[7,7]])

    def test_all_gate_profiles_and_full_end_cache_slot(self):
        for tempo,art,duration in PULSES:
            first=(0xE0,2,duration,art,*([0x98]*8),0xED,64,0)
            later=(*([0x99]*8),0)
            report=self.render([(first,first),(later,later), (later,later),(later,later)],tempo)
            self.assertEqual(len(report['events']),64)
            self.assertEqual(report['events'][-1]['volumes'],[7,7])
            self.assertEqual(report['events'][-2]['volumes'],[1,1])
            self.assertTrue(all(g['cause']==1 for g in native_gates(report)))

    def test_other_trailing_opcodes_and_ambiguous_note_order_reject(self):
        for command in ((0xE0,2),(0xE7,96),(0xE5,160),(16,63)):
            self.reject(authored([((16,63,0x98,*command,0),None)]))
        for pair in (((16,63,0x98,0),(8,63,0x98,0x99,0x9A,0)),((8,63,0x98,0x99,0x9A,0),(16,63,0x98,0))):self.reject(authored([pair]))

    def test_omitted_boundary_resolution_fails_oracle(self):
        image=bytes(assemble(source().replace('    call tail_end\n',''),'spc',0x0800))
        report=gate_native(PROBE,bank(),builder=lambda:image,validator=validate,output_bound=131072)
        with self.assertRaises(ValueError):align(report,bank())

    def test_oracle_priority_is_explicit(self):
        stream=(16,63,0x98,0xED,64,0)
        data=authored([(stream,stream)])
        default=schedule(data,0x2B10,inherit_timing=True)
        priority=schedule(data,0x2B10,inherit_timing=True,end_priority=True)
        self.assertEqual([e['channel'] for e in default['events'] if e['kind']=='track_volume'],[2,3])
        self.assertEqual([e['channel'] for e in priority['events'] if e['kind']=='track_volume'],[2])
        with self.assertRaises(ValueError):schedule(data,0x2B10,end_priority=1)

    def test_clipping_observer_never_infers_missing_keyoff(self):
        for case in ('clip-2','clip-3'):
            for art in (63,127):
                result=observe(io.StringIO(trace(case,art)),case);result['articulation']=art;contract(result,case)
                self.assertEqual(len(result['note_gates']),12 if art==63 else 11)
                self.assertEqual(len(result['unreleased_retriggers']),0 if art==63 else 1)
                bad=copy.deepcopy(result);bad['unreleased_retriggers']=[]
                if art==127:
                    with self.assertRaises(ValueError):contract(bad,case)

    def test_reference_matrix_and_mutation_guards(self):
        for art in (63,127):
            candidates={};refs=[]
            for case in QUALIFIED_CASES:
                report=native(PROBE,bank(art,case));candidates[case]=report
                observation=observe(io.StringIO(trace(case,art)),case)
                for model in ('sgb','sgb2'):
                    result=copy.deepcopy(observation);result.update(model=model,case=case,articulation=art)
                    for gate,actual in zip(result['note_gates'],native_gates(report)):gate['gate_spc_cycles']=int(actual['gate_spc_cycles'])
                    result['onset_intervals_spc_cycles']=[int((b['half_cycle']-a['half_cycle'])/2) for a,b in zip(report['keyons'],report['keyons'][1:])]
                    refs.append(result)
            self.assertEqual(len(compare(candidates,refs,art)),8)
            for mutation in ('duplicate','gate','type','volume'):
                bad=copy.deepcopy(refs)
                if mutation=='duplicate':bad[-1]=bad[0]
                elif mutation=='gate':bad[0]['note_gates'][0]['gate_spc_cycles']+=6000
                elif mutation=='type':bad[0]['note_gates'][0]['voice']=True
                else:bad[0]['keyons'][2]['voices'][0]['volumes']=[7,7]
                with self.assertRaises(ValueError):compare(candidates,bad,art)
        with self.assertRaises(ValueError):observe(io.StringIO(trace('tail-2').replace('kind,master_clock','wrong,master_clock')),'tail-2')


if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('--probe',type=Path,required=True)
    args,remaining=parser.parse_known_args();PROBE=args.probe.resolve()
    unittest.main(argv=[sys.argv[0],*remaining])
