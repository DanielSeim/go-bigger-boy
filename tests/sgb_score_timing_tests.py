#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Per-channel timing carry, clipping, calls and complete lifecycle checks."""
import argparse
import copy
import hashlib
import io
from pathlib import Path
import struct
import sys
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'scripts'))
from build_sgb_score_timing import build,source
from build_sgb_score_inherit import build as prior_build
from build_sgb_prototype import assemble
from build_sgb_score_timing_fixture import bank,expected,CASES
from check_sgb_score_timing_reference import native,align,validate,compare,observe
from check_sgb_score_polygate_reference import native as gate_native,native_gates
from check_sgb_score_gate_reference import PULSES
from schedule_sgb_score import schedule
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


class TimingTests(unittest.TestCase):
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

    def test_reproducible_and_prior_unchanged(self):
        self.assertEqual(build(),build());self.assertEqual(len(build()),3983)
        self.assertEqual(hashlib.sha256(build()).hexdigest(),'1b6d47611ec2b81c0e07897cd24ecacfd57587afd6d8a887b681ce59d82b4dc3')
        self.assertEqual(hashlib.sha256(prior_build()).hexdigest(),'4808a273bbad7aeacec7174038b33e37b7cc615e8c3303a844a07bf8fa19ef15')
        self.assertEqual(build()[0x1008-0x800:0x1019-0x800],prior_build()[0x1008-0x800:0x1019-0x800])

    def test_owned_fixtures_and_channel_specific_articulation(self):
        for case in CASES:
            for art in (63,127):
                data=bank(art,case);report=native(PROBE,data);align(report,data)
                self.assertEqual(report['pattern_ticks'],[0,32,64,96])
                self.assertEqual(report['pattern_masks'],[12,4,8,12])
                for event in report['events']:
                    wanted=art if event['channel']==2 and event['tick']<96 else 190-art
                    self.assertEqual(event['articulation'],wanted)
                    self.assertEqual(event['duration'],16)

    def test_uninitialized_active_channel_rejects_silently(self):
        note=(0x98,0)
        for pair in ((note,None),(None,note),((16,0x98,0),None)):
            self.reject(authored([pair]))
        self.reject(authored([((16,63,0x98,0),None),(None,note)]))

    def test_duration_only_override_keeps_articulation(self):
        report=self.render([((8,63,0x98,0),None),((24,0x99,0),None),((0x9A,0),None)])
        self.assertEqual([e['duration'] for e in report['events']],[8,24,24])
        self.assertEqual([e['articulation'] for e in report['events']],[63]*3)

    def test_last_executed_timing_overrides_earlier_setup(self):
        report=self.render([((8,127,0x98,24,63,0x99,0),None),((0xA4,0),None)])
        self.assertEqual([(e['duration'],e['articulation']) for e in report['events']],[(8,127),(24,63),(24,63)])

    def test_unplayed_tail_does_not_change_timing(self):
        short=(16,127,0x98,0)
        long=(24,63,0x98,8,127,0x99,0)
        report=self.render([(short,long),(None,(0xA4,0))])
        self.assertEqual([(e['channel'],e['duration'],e['articulation']) for e in report['events']],[(2,16,127),(3,24,63),(3,24,63)])
        self.assertEqual(report['pattern_ticks'],[0,16])
        self.assertEqual(report['end_tick'],40)

    def test_inactive_and_rest_timing_survive(self):
        report=self.render([((24,63,0xC9,0),None),(None,(8,127,0xC9,0)),((0x98,0),None),(None,(0x99,0))])
        self.assertEqual([e['duration'] for e in report['events']],[24,8,24,8])
        self.assertEqual([e['articulation'] for e in report['events']],[63,127,63,127])
        self.assertEqual(len(report['keyons']),2)

    def test_call_and_repeat_timing_returns_and_carries(self):
        data=bytearray(authored([((8,127,0x98,0xEF,0,0,2,0xA4,0),None),((0x99,0),None)]))
        position=data.index(bytes((0xEF,0,0,2)))
        struct.pack_into('<H',data,position+1,0x2B00+len(data))
        data.extend((24,63,0x9A,0))
        report=native(PROBE,bytes(data));align(report,bytes(data))
        self.assertEqual([e['duration'] for e in report['events']],[8,24,24,24,24])
        self.assertEqual([e['articulation'] for e in report['events']],[127,63,63,63,63])

    def test_all_gate_profiles_with_inherited_mix_and_prefixes(self):
        for tempo,art,duration in PULSES:
            first=(0xE0,2,0xE1,10,0xED,64,duration,art,0x98,0x99,0)
            later=(0xE0,2,0xE0,2,0x98,0x99,0)
            report=self.render([(first,first),(later,None),(None,later),(later,later)],tempo)
            self.assertEqual(len(report['instrument_writes']),40)
            self.assertTrue(all(e['volumes']==[1,1] and e['duration']==duration and e['articulation']==art for e in report['events']))
            self.assertTrue(all(g['cause']==1 for g in native_gates(report)))

    def test_inherited_unmeasured_gate_rejects_before_audio(self):
        self.reject(authored([((24,63,0x98,0),None),((12,0x99,0),None)]))
        self.reject(authored([((16,127,0x98,0),None),((16,64,0x99,0),None)]))
        self.reject(authored([((16,127,0x98,0),None),((1,0x99,0),None)]))

    def test_full_cache_and_global_tick_budget(self):
        first=(16,63,*([0x98]*8),0);later=(*([0x98]*8),0)
        report=self.render([(first,first),*([(later,later)]*3)])
        self.assertEqual(len(report['events']),64);self.assertEqual(report['end_tick'],512)
        first=(127,63,*([0xC9]*8),0);later=(*([0xC9]*8),0)
        report=self.render([(first,None),(later,None)])
        self.assertEqual(report['end_tick'],2032)
        self.reject(authored([(first,None),(later,None),((0xC9,0),None)]))

    def test_same_tick_end_note_still_rejects(self):
        self.reject(authored([((16,63,0x98,0),(8,63,0x98,0x99,0x9A,0))]))

    def test_missing_carry_walker_fails_independent_oracle(self):
        text=source().replace('    call timing_pattern\n','')
        image=bytes(assemble(text,'spc',0x0800))
        report=gate_native(PROBE,bank(),builder=lambda:image,validator=validate,output_bound=131072)
        self.assertNotEqual(report['status'],2)
        with self.assertRaises(ValueError):align(report,bank())

    def test_symbolic_inheritance_is_opt_in_and_execution_ordered(self):
        data=authored([((16,63,0x98,0),None),((0x99,0),None)])
        with self.assertRaises(ValueError):schedule(data,0x2B10)
        self.assertEqual(schedule(data,0x2B10,inherit_timing=True)['ticks'],32)
        with self.assertRaises(ValueError):schedule(data,0x2B10,inherit_timing=1)

    def test_reference_matrix_rejects_types_gates_and_models(self):
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
        for mutation in ('duplicate','bool','gate','interval'):
            bad=copy.deepcopy(refs)
            if mutation=='duplicate':bad[-1]=bad[0]
            elif mutation=='bool':bad[0]['articulation']=True
            elif mutation=='gate':bad[0]['note_gates'][0]['gate_spc_cycles']+=6000
            else:bad[0]['onset_intervals_spc_cycles'][0]+=6000
            with self.assertRaises(ValueError):compare(candidates,bad)


if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('--probe',type=Path,required=True)
    args,remaining=parser.parse_known_args();PROBE=args.probe.resolve()
    unittest.main(argv=[sys.argv[0],*remaining])
