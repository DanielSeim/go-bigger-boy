#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Owned consecutive short-event geometry and opaque lifecycle contracts."""
import copy,hashlib,io
from pathlib import Path
import sys,unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'scripts'))
from build_sgb_score_short_pair_fixture import bank,build,CASES,ARTICULATIONS
from build_sgb_score_short_continue_fixture import bank as prior_bank
from check_sgb_score_short_pair_reference import observe,contract,agree
from check_sgb_score_short_continue_reference import contract as prior_contract
from check_sgb_score_gate_reference import PULSES
from schedule_sgb_score import schedule
from sgb_score_short_continue_tests import trace as prior_trace


def trace(case,art):
    rows=[];following=10000+68*5500;old_stop=10000+76*5500;new_stop=10000+72*5500
    for line in prior_trace(case,art,8).splitlines()[1:]:
        fields=line.split(',');t,a,v=(int(fields[i]) for i in (2,4,5))
        if case=='continue-note' and (t,a,v)==(following+PULSES[96,art,8]*2048,92,4):t=following+8192
        elif t>=old_stop:t+=new_stop-old_stop
        fields[2]=str(t);rows.append((t,','.join(fields)))
    rows.sort(key=lambda row:row[0])
    return 'kind,master_clock,spc_cycle,pcm_sample,address,value\n'+'\n'.join(row for _,row in rows)+'\n'


def references():
    result=[]
    for c in CASES:
        for a in ARTICULATIONS:
            r=observe(io.StringIO(trace(c,a)),c,a)
            result.extend({**copy.deepcopy(r),'case':c,'model':m,'fixture_sha256':hashlib.sha256(build(a,c)).hexdigest()} for m in ('sgb','sgb2'))
    return result


class ShortPairTests(unittest.TestCase):
    def test_owned_reproducibility_and_only_two_duration_changes(self):
        hashes=set()
        for c in CASES:
            for a in ARTICULATIONS:
                b=bank(a,c);before=prior_bank(a,8,c)
                self.assertEqual(len(b),2048);self.assertEqual([i for i,(x,y) in enumerate(zip(b,before)) if x!=y],[0x6C2,0x6E2])
                self.assertEqual(build(a,c),build(a,c));hashes.add(hashlib.sha256(build(a,c)).hexdigest())
        self.assertEqual(len(hashes),4)
        for args in ((True,'continue-note'),(31,'continue-note'),(63,'unknown')):
            with self.assertRaises(ValueError):bank(*args)

    def test_independent_scheduler_proves_two_short_events_and_stop(self):
        for c in CASES:
            for a in ARTICULATIONS:
                b=bank(a,c);s=schedule(b,int.from_bytes(b[:2],'little'),inherit_timing=True,end_priority=True,boundary_events=True)
                es=[e for e in s['events'] if e['kind'] in ('note','rest')]
                self.assertEqual(s['ticks'],72);self.assertEqual(len(es),10)
                self.assertEqual([p['start_tick'] for p in s['patterns']],[0,32,64])
                self.assertEqual([p['channels'] for p in s['patterns']],[[2,3],[3],[2,3]])
                self.assertEqual([(e['tick'],e['duration']) for e in es[6:]],[(64,4),(64,4),(68,4),(68,4)])
                self.assertTrue(all(not e.get('boundary_event',False) for e in es))
                self.assertEqual([e['articulation'] for e in es],[127]*6+[a]*4)

    def test_complete_matrix_retains_separate_timer_releases(self):
        refs=references();self.assertEqual(len(agree(refs)),4)
        for r in refs:
            self.assertEqual(r['return_duration'],4);self.assertEqual(r['continuation_duration'],4)
            self.assertEqual(r['unreleased_final_voices'],[]);self.assertEqual(r['voices_pending_at_stop'],[])
            self.assertEqual(len(r['unreleased_retriggers']),1)
            self.assertEqual(r['note_gates'][5]['gate_spc_cycles'],8192)
            if r['case']=='continue-note':self.assertEqual(r['note_gates'][6]['gate_spc_cycles'],8192)

    def test_preceding_contract_and_general_gate_table_stay_closed(self):
        for r in references():
            with self.assertRaises(ValueError):prior_contract(r,r['case'])
        for a in ARTICULATIONS:self.assertNotIn((96,a,4),PULSES)
        with self.assertRaises(ValueError):prior_contract(references()[0],'continue-note',short_follow=1)

    def test_duration_gate_source_setup_and_tail_forgeries_fail(self):
        for mutation in ('duration','gate','following-too-short','return-too-long','release-kind','release-id','volume','order','held','tail','bool'):
            r=copy.deepcopy(references()[0])
            if mutation=='duration':r['continuation_duration']=8
            elif mutation=='gate':r['note_gates'][-1]['gate_spc_cycles']=16000
            elif mutation=='following-too-short':r['note_gates'][-1]['gate_spc_cycles']=7426
            elif mutation=='return-too-long':r['note_gates'][5]['gate_spc_cycles']=9994
            elif mutation=='release-kind':r['note_gates'][-1]['release_kind']='final-stop'
            elif mutation=='release-id':r['note_gates'][-1]['onset_index']=4
            elif mutation=='volume':r['continuation_voice_writes'][-1]['value']=7
            elif mutation=='order':r['continuation_control_offsets_spc_cycles'].reverse()
            elif mutation=='held':r['voices_pending_at_stop']=[2]
            elif mutation=='tail':r['post_stop_observation_spc_cycles']=1
            elif mutation=='bool':r['continuation_duration']=True
            with self.subTest(mutation=mutation),self.assertRaises(ValueError):contract(r,r['case'])

    def test_model_hash_completeness_and_intermodel_drift_fail(self):
        refs=references()
        for mutation in ('missing','duplicate','hash','model','timing'):
            r=copy.deepcopy(refs)
            if mutation=='missing':r.pop()
            elif mutation=='duplicate':r[1]=copy.deepcopy(r[0])
            elif mutation=='hash':r[0]['fixture_sha256']='0'*64
            elif mutation=='model':r[0]['model']='unknown'
            elif mutation=='timing':r[0]['note_gates'][0]['gate_spc_cycles']+=2049
            with self.subTest(mutation=mutation),self.assertRaises(ValueError):agree(r)

    def test_trace_header_bytes_rows_order_and_values_remain_bounded(self):
        good=trace('continue-note',63)
        for bad in (good.replace('spc_cycle','other',1),good.replace('D,0,0,0,61,0','D,0,0,0,128,0',1),good.replace('D,0,0,0,61,0','D,0,0,0,61,256',1),good+'D,0,1,0,61,0\n',good+'D,0,5000000,0,61,0\n'*32768,'x'*(16*1024*1024+1)):
            with self.assertRaises(ValueError):observe(io.StringIO(bad),'continue-note')


if __name__=='__main__':unittest.main()
