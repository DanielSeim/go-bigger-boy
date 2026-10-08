#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Owned continuing-score geometry and strict opaque observation contracts."""
import copy,hashlib,io
from pathlib import Path
import sys,unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'scripts'))
from build_sgb_score_short_continue_fixture import bank,build,CASES,DURATIONS,ARTICULATIONS
from build_sgb_score_short_return_fixture import bank as prior_bank
from check_sgb_score_short_continue_reference import expected,observe,contract,agree
from check_sgb_score_gate_reference import PULSES
from schedule_sgb_score import schedule


def trace(case,art,duration):
    rows=[(0,61,0)]
    for i,e in enumerate(expected(case)):
        c=10000+(i*16 if i<5 else 68)*5500
        for v in e['voices']:
            if i in (4,5):
                pitch=v['pitch'];volume=v['volumes'][0]
                rows.extend(((c-1600,34,pitch&255),(c-1564,35,pitch>>8),(c-651,32,volume),(c-509,33,volume)))
            else:
                values=(*v['volumes'],v['pitch']&255,v['pitch']>>8,2,143,111,184)
                rows.extend((c-700+20*r,16*v['voice']+r,value) for r,value in enumerate(values))
            if (i,v['voice'])!=(1,2):
                pulse=4 if i==4 else PULSES[(96,art,duration)] if i==5 else PULSES[(96,127,16)]
                rows.append((c+pulse*2048,92,1<<v['voice']))
        if i in (4,5):rows.extend(((c-156,92,0),(c-39,92,0)))
        rows.append((c,76,e['mask']))
    stop=10000+(68+duration)*5500
    rows.extend(((stop,92,255),(stop+117,92,0),(stop+156,76,0),(stop+1000,76,0),(stop+400000,61,0)))
    rows.sort(key=lambda row:row[0])
    return 'kind,master_clock,spc_cycle,pcm_sample,address,value\n'+''.join(f'D,0,{c},0,{a},{v}\n' for c,a,v in rows)


def references():
    result=[]
    for c in CASES:
        for a in ARTICULATIONS:
            for d in DURATIONS:
                r=observe(io.StringIO(trace(c,a,d)),c,a,d);contract(r,c)
                result.extend({**copy.deepcopy(r),'case':c,'model':m,'fixture_sha256':hashlib.sha256(build(a,d,c)).hexdigest()} for m in ('sgb','sgb2'))
    return result


class ShortContinueTests(unittest.TestCase):
    def test_owned_bank_changes_only_continuing_channel_three(self):
        hashes=set()
        for c in CASES:
            for a in ARTICULATIONS:
                for d in DURATIONS:
                    b=bank(a,d,c);before=prior_bank(a,d,'direct-note' if c=='continue-note' else 'direct-rest')
                    self.assertEqual(len(b),2048);self.assertEqual(b[:0x6C2],before[:0x6C2]);self.assertEqual(b[0x6C6:],before[0x6C6:])
                    self.assertEqual(b[0x6C2:0x6C6],bytes((d,a,0xC9,0)))
                    self.assertEqual(build(a,d,c),build(a,d,c));hashes.add(hashlib.sha256(build(a,d,c)).hexdigest())
        self.assertEqual(len(hashes),8)
        for args in ((True,8,'continue-note'),(63,True,'continue-note'),(31,8,'continue-note'),(63,4,'continue-note'),(63,8,'unknown')):
            with self.assertRaises(ValueError):bank(*args)

    def test_independent_scheduler_proves_ordinary_progression(self):
        for c in CASES:
            for a in ARTICULATIONS:
                for d in DURATIONS:
                    b=bank(a,d,c);s=schedule(b,int.from_bytes(b[:2],'little'),inherit_timing=True,end_priority=True,boundary_events=True)
                    es=[e for e in s['events'] if e['kind'] in ('note','rest')]
                    self.assertEqual(s['ticks'],68+d);self.assertEqual(len(es),10)
                    self.assertEqual([p['start_tick'] for p in s['patterns']],[0,32,64])
                    self.assertEqual([p['channels'] for p in s['patterns']],[[2,3],[3],[2,3]])
                    self.assertEqual([(e['tick'],e['duration']) for e in es[6:]],[(64,4),(64,4),(68,d),(68,d)])
                    self.assertTrue(all(not e.get('boundary_event',False) for e in es))
                    self.assertEqual([e['articulation'] for e in es],[127]*6+[a]*4)

    def test_complete_synthetic_matrix_and_release_ledger(self):
        refs=references();self.assertEqual(len(agree(refs)),8)
        for r in refs:
            self.assertEqual(r['unreleased_final_voices'],[]);self.assertEqual(r['voices_pending_at_stop'],[])
            self.assertEqual(len(r['note_gates']),7 if r['case']=='continue-note' else 6)
            self.assertEqual(r['note_gates'][5]['gate_spc_cycles'],8192)

    def test_forgeries_of_deferred_volume_pitch_and_controls_fail(self):
        for field,value in (('continuation_voice_writes',[]),('continuation_control_writes',[]),('final_volumes',[[7,7],[1,1]]),('return_duration',8),('continuation_duration',4),('return_articulation',True),('post_stop_observation_spc_cycles',1),('unreleased_final_voices',[2])):
            r=copy.deepcopy(references()[0]);r[field]=value
            with self.subTest(field=field),self.assertRaises(ValueError):contract(r,r['case'])
        r=references()[0];r['continuation_voice_writes'].reverse()
        with self.assertRaises(ValueError):contract(r,r['case'])

    def test_forged_gate_retrigger_and_raw_progression_fail(self):
        for mutation in ('gate','release-kind','identity','retrigger','late-short','late-continuation','order','bool'):
            r=copy.deepcopy(references()[0])
            if mutation=='gate':r['note_gates'][5]['gate_spc_cycles']=16000
            elif mutation=='release-kind':r['note_gates'][-1]['release_kind']='final-stop'
            elif mutation=='identity':r['note_gates'][-1]['onset_index']=4
            elif mutation=='retrigger':r['unreleased_retriggers'][0]['interval_spc_cycles']+=1
            elif mutation=='late-short':r['onset_intervals_spc_cycles'][-1]=8000
            elif mutation=='late-continuation':r['final_stop_interval_spc_cycles']=42000
            elif mutation=='order':r['continuation_voice_offsets_spc_cycles'].reverse()
            elif mutation=='bool':r['final_volumes'][0][0]=True
            with self.subTest(mutation=mutation),self.assertRaises(ValueError):contract(r,r['case'])

    def test_model_hash_completeness_and_intermodel_timing_fail_closed(self):
        refs=references()
        for mutation in ('missing','duplicate','hash','model','timing'):
            r=copy.deepcopy(refs)
            if mutation=='missing':r.pop()
            elif mutation=='duplicate':r[1]=copy.deepcopy(r[0])
            elif mutation=='hash':r[0]['fixture_sha256']='0'*64
            elif mutation=='model':r[0]['model']='unknown'
            elif mutation=='timing':r[0]['note_gates'][0]['gate_spc_cycles']+=2049
            with self.subTest(mutation=mutation),self.assertRaises(ValueError):agree(r)

    def test_bounded_trace_header_order_rows_and_values(self):
        good=trace('continue-note',63,8)
        for bad in (good.replace('spc_cycle','other',1),good.replace('D,0,0,0,61,0','D,0,0,0,128,0',1),good.replace('D,0,0,0,61,0','D,0,0,0,61,256',1),good+'D,0,1,0,61,0\n',good+'D,0,5000000,0,61,0\n'*32768,'x'*(16*1024*1024+1)):
            with self.assertRaises(ValueError):observe(io.StringIO(bad),'continue-note')


if __name__=='__main__':unittest.main()
