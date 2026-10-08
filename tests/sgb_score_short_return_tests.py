#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Owned short direct returns and bounded opaque observation contracts."""
import copy,hashlib,io
from pathlib import Path
import sys,unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'scripts'))
from build_sgb_score_short_return_fixture import bank,build,CASES,DURATIONS,ARTICULATIONS
from build_sgb_score_direct_return_fixture import bank as prior_bank
from check_sgb_score_short_return_reference import observe,contract,agree
from check_sgb_score_direct_return_reference import contract as prior_contract
from check_sgb_score_gate_reference import PULSES
from schedule_sgb_score import schedule
from sgb_score_direct_return_tests import trace as prior_trace


def trace(c,a,d):
    rows=[];ret=10000+64*5500;old_stop=10000+(64+d)*5500-156;new_stop=10000+68*5500-156
    for line in prior_trace(c,a,d).splitlines()[1:]:
        fields=line.split(',');t,r,v=(int(fields[i]) for i in (2,4,5))
        if (t,r,v)==(ret+PULSES[(96,a,d)]*2048,92,4):t=ret+4*2048
        elif t>=old_stop-500:t+=new_stop-old_stop
        fields[2]=str(t);rows.append((t,','.join(fields)))
    rows.sort(key=lambda row:row[0])
    return 'kind,master_clock,spc_cycle,pcm_sample,address,value\n'+'\n'.join(row for _,row in rows)+'\n'


def references():
    refs=[]
    for c in CASES:
        for a in ARTICULATIONS:
            for d in DURATIONS:
                r=observe(io.StringIO(trace(c,a,d)),c,a,d)
                refs.extend({**copy.deepcopy(r),'case':c,'model':m,'fixture_sha256':hashlib.sha256(build(a,d,c)).hexdigest()} for m in ('sgb','sgb2'))
    return refs


class ShortReturnTests(unittest.TestCase):
    def test_owned_reproducibility_and_known_pending_duration_bounds(self):
        hashes=set()
        for c in CASES:
            for a in ARTICULATIONS:
                for d in DURATIONS:
                    b=bank(a,d,c);self.assertEqual(len(b),2048);self.assertEqual(build(a,d,c),build(a,d,c))
                    before=prior_bank(a,d,c);self.assertEqual([i for i,(x,y) in enumerate(zip(b,before)) if x!=y],[0x6BD,0x6DD])
                    self.assertEqual(b[0x6DD],4);self.assertEqual(b[0x6E2],d)
                    hashes.add(hashlib.sha256(build(a,d,c)).hexdigest())
        self.assertEqual(len(hashes),8)
        for args in ((True,8,'direct-note'),(63,True,'direct-note'),(31,8,'direct-note'),(63,4,'direct-note'),(63,8,'unknown')):
            with self.assertRaises(ValueError):bank(*args)

    def test_symbolic_four_tick_return_and_independent_final_duration(self):
        for c in CASES:
            for a in ARTICULATIONS:
                for d in DURATIONS:
                    b=bank(a,d,c);s=schedule(b,int.from_bytes(b[:2],'little'),inherit_timing=True,end_priority=True,boundary_events=True)
                    es=[e for e in s['events'] if e['kind'] in ('note','rest')]
                    self.assertEqual(s['ticks'],68);self.assertEqual(len(es),9)
                    self.assertEqual([e['articulation'] for e in es],[127]*6+[a]*3)
                    self.assertEqual([(e['tick'],e['duration'],e['kind']) for e in es[6:8]],[(64,4,'note'),(64,4,'rest')])
                    self.assertEqual((es[-1]['tick'],es[-1]['duration'],es[-1]['boundary_event']),(68,d,True))

    def test_complete_synthetic_matrix_retains_raw_retrigger_and_real_release(self):
        refs=references();self.assertEqual(len(agree(refs)),8)
        for r in refs:
            self.assertEqual(r['return_duration'],4);self.assertIn(r['boundary_duration'],DURATIONS)
            self.assertEqual(r['unreleased_retriggers'][0]['interval_spc_cycles'],sum(r['onset_intervals_spc_cycles'][1:4]))
            self.assertEqual(r['note_gates'][-1]['gate_spc_cycles'],8192)
            self.assertLess(r['note_gates'][-1]['gate_spc_cycles'],r['final_stop_interval_spc_cycles'])
            self.assertEqual(r['voices_pending_at_stop'],[])
            self.assertEqual(r['unreleased_final_voices'],[2] if r['case']=='direct-note' else [])

    def test_preceding_contract_does_not_admit_a_new_note_profile(self):
        r=references()[0]
        with self.assertRaises(ValueError):prior_contract(r,r['case'])
        self.assertNotIn((96,63,4),PULSES);self.assertNotIn((96,127,4),PULSES)

    def test_short_gate_source_duration_control_and_tail_forgeries_rejected(self):
        for mutation in ('gate','source','return-duration','pending-duration','art','pulse','pitch','volume','held','tail'):
            r=copy.deepcopy(references()[0])
            if mutation=='gate':r['note_gates'][-1]['gate_spc_cycles']=16000
            elif mutation=='source':r['note_gates'][-1]['release_kind']='final-stop'
            elif mutation=='return-duration':r['return_duration']=8
            elif mutation=='pending-duration':r['boundary_duration']=4
            elif mutation=='art':r['return_articulation']=True
            elif mutation=='pulse':r['return_control_writes'][0]['value']=4
            elif mutation=='pitch':r['return_voice_writes'][0]['value']=8
            elif mutation=='volume':r['post_return_volume_writes']=[{'address':32,'value':1}]
            elif mutation=='held':r['voices_pending_at_stop']=[2]
            elif mutation=='tail':r['post_stop_observation_spc_cycles']=1
            with self.subTest(mutation=mutation),self.assertRaises(ValueError):contract(r,r['case'])

    def test_unreleased_retrigger_and_final_onset_bind_to_raw_timing(self):
        for field in ('interval_spc_cycles','previous_onset_index','onset_index'):
            r=references()[0];r['unreleased_retriggers'][0][field]+=1
            with self.assertRaises(ValueError):contract(r,r['case'])
        r=references()[0];r['onset_intervals_spc_cycles'][-1]+=1
        with self.assertRaises(ValueError):contract(r,r['case'])

    def test_matrix_model_hash_completeness_and_drift_rejected(self):
        refs=references()
        with self.assertRaises(ValueError):agree(refs[:-1])
        with self.assertRaises(ValueError):agree(refs[:-1]+[refs[0]])
        for field,value in (('model','unknown'),('fixture_sha256','0'*64),('boundary_duration',True)):
            changed=copy.deepcopy(refs);changed[0][field]=value
            with self.assertRaises(ValueError):agree(changed)
        changed=copy.deepcopy(refs);changed[1]['note_gates'][0]['gate_spc_cycles']+=3000
        with self.assertRaisesRegex(ValueError,'allowance'):agree(changed)

    def test_header_byte_row_order_and_numeric_bounds(self):
        header='kind,master_clock,spc_cycle,pcm_sample,address,value\n'
        for text in ('bad header\n','x'*(16*1024*1024+1),header+'D,0,2,0,61,0\nD,0,1,0,61,0\n',header+'D,0,0,0,128,0\n',header+'D,0,0,0,61,1.0\n'):
            with self.assertRaises(ValueError):observe(io.StringIO(text),'direct-note')
        with self.assertRaisesRegex(ValueError,'row bound'):observe(io.StringIO(header+'D,0,0,0,61,0\n'*32768),'direct-note')


if __name__=='__main__':unittest.main()
